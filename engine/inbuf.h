// Чтение кольца входа драйвера с запасом и подстройкой под чужие часы (этап 6.6, этап A).
//
// Зачем. Драйвер кладёт звук в кольцо порциями по таймеру Windows, а движок забирает его блоками по своим часам.
// Часы двух сторон немного расходятся (до ~0,1 %), порции приходят с дрожанием. Если запаса нет, перед каждой порцией
// кольцо почти пусто, любое опоздание даёт кусок тишины (щелчок), а при постоянных опозданиях получается «хрип».
//
// Что делает читатель:
//   * Запас. Начинает играть, только когда накоплено kTargetFrames (25 мс). Если кольцо опустело, замолкает один раз и ждёт,
//     пока запас снова наберётся, вместо тишины на каждой порции.
//   * Подстройка. Следит за средним запасом и чуть меняет скорость чтения (не больше ±0,3 %), чтобы запас держался на цели.
//     Звук при этом не режется и тишина не вставляется. Расхождение часов 0,1 % даёт сдвиг высоты в 1,7 цента, на слух не слышен.
//   * Страховка сверху. Если запас вырос выше kMaxFrames (драйвер выдал много сразу), лишнее старое выбрасывается.
// Всё работает в аудио-потоке без выделения памяти. Счётчики атомарные: их читает окно.
//
// Интерполяция кубическая (Catmull–Rom): линейная при медленно ползущей дробной части давала бы «дрожание» верхних частот.
#pragma once

#include "shm.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace bg {

constexpr uint32_t kInTargetFrames = 1200;   // 25 мс при 48 кГц
constexpr uint32_t kInMaxFrames = 4800;      // 100 мс: выше этого лишнее выбрасывается

class InReader {
public:
    // Счётчики для окна (с запуска движка), единицы указаны в кадрах 48 кГц.
    std::atomic<uint32_t> fill{0};      // средний запас сейчас
    std::atomic<uint32_t> under{0};     // сколько раз кольцо опустело (после начала игры)
    std::atomic<uint32_t> trim{0};      // сколько кадров выброшено сверху
    std::atomic<uint32_t> opens{0};     // сколько раз поток открывался
    std::atomic<uint32_t> closes{0};    // сколько раз поток закрывался
    std::atomic<int32_t> adj_ppm{0};    // текущая подстройка скорости в миллионных долях

    // Поток только что открылся: старое не нужно, ждём запас.
    void open(ShmRing& rg) {
        rg.discard();
        restart();
        opens.fetch_add(1, std::memory_order_relaxed);
    }
    void closed() { closes.fetch_add(1, std::memory_order_relaxed); fill.store(0, std::memory_order_relaxed); }

    // Отдаёт ровно n кадров. Только аудио-поток.
    void read(ShmRing& rg, float* out, uint32_t n) {
        uint32_t r = rg.ctl->read_pos.load(std::memory_order_relaxed);
        const uint32_t w = rg.ctl->write_pos.load(std::memory_order_acquire);
        uint32_t av = w - r;
        if (av > rg.cap) { rg.discard(); r = w; av = 0; restart(); }   // испорченные позиции: начинаем с чистого

        if (!primed_) {
            if (av < kInTargetFrames) { silence(rg, out, n); return; }
            // Накопили запас. Небольшой избыток оставляем (регулятор сам его уберёт); если накопилось сильно больше цели
            // (поток стоял), лишнее старое выбрасываем.
            if (av > 2 * kInTargetFrames) { trim.fetch_add(av - kInTargetFrames, std::memory_order_relaxed); r += av - kInTargetFrames; av = kInTargetFrames; rg.ctl->read_pos.store(r, std::memory_order_release); }
            primed_ = true; underran_ = false;
            frac_ = 0.0; fill_avg_ = static_cast<double>(av); integ_ = 0.0; adj_ = 0.0;
        } else if (av > kInMaxFrames) {
            trim.fetch_add(av - kInTargetFrames, std::memory_order_relaxed);
            r += av - kInTargetFrames; av = kInTargetFrames;
            rg.ctl->read_pos.store(r, std::memory_order_release);
        }

        const double ratio = 1.0 + adj_;
        const double endp = frac_ + static_cast<double>(n - 1) * ratio;   // позиция последнего нужного кадра
        const uint32_t last = static_cast<uint32_t>(endp) + 2;            // кубической интерполяции нужен ещё кадр впереди
        if (last >= av) {   // не хватает данных: замолкаем один раз, данные остаются в кольце
            under.fetch_add(1, std::memory_order_relaxed);
            primed_ = false; underran_ = true; frac_ = 0.0;
            silence(rg, out, n);
            return;
        }

        double p = frac_;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t k = static_cast<uint32_t>(p);
            const float t = static_cast<float>(p - k);
            const float* y0 = rg.data + ((r + k - 1) & (rg.cap - 1)) * 2;   // кадр перед позицией (на самом старте это старый мусор, но t = 0, его вес нулевой)
            const float* y1 = rg.data + ((r + k) & (rg.cap - 1)) * 2;
            const float* y2 = rg.data + ((r + k + 1) & (rg.cap - 1)) * 2;
            const float* y3 = rg.data + ((r + k + 2) & (rg.cap - 1)) * 2;
            for (int c = 0; c < 2; ++c)
                out[i * 2 + c] = y1[c] + 0.5f * t * (y2[c] - y0[c] + t * (2.0f * y0[c] - 5.0f * y1[c] + 4.0f * y2[c] - y3[c] + t * (3.0f * (y1[c] - y2[c]) + y3[c] - y0[c])));
            p += ratio;
        }
        const uint32_t used = static_cast<uint32_t>(p);
        frac_ = p - used;
        rg.ctl->read_pos.store(r + used, std::memory_order_release);

        // Регулятор: средний запас (за ~0,5 с) тянем к цели. Пропорциональная часть быстро, интегральная убирает постоянный сдвиг
        // от разницы часов. Положительная ошибка (запаса много) = читаем чуть быстрее.
        const double dt = static_cast<double>(n) / 48000.0;
        fill_avg_ += std::min(1.0, dt / 0.5) * (static_cast<double>(av - used) - fill_avg_);
        const double err = fill_avg_ - static_cast<double>(kInTargetFrames);
        integ_ = std::max(-kMaxAdj, std::min(kMaxAdj, integ_ + kKi * err * dt));
        adj_ = std::max(-kMaxAdj, std::min(kMaxAdj, kKp * err + integ_));
        fill.store(static_cast<uint32_t>(fill_avg_ + 0.5), std::memory_order_relaxed);
        adj_ppm.store(static_cast<int32_t>(std::lround(adj_ * 1e6)), std::memory_order_relaxed);
    }

private:
    static constexpr double kMaxAdj = 0.003;    // ±0,3 %
    static constexpr double kKp = 1.0e-5;       // на кадр ошибки запаса
    static constexpr double kKi = 1.0e-6;       // на кадр ошибки в секунду

    bool primed_ = false;
    bool underran_ = false;    // кольцо опустело и ещё не набралось: тишина считается нехваткой
    double frac_ = 0.0, fill_avg_ = 0.0, integ_ = 0.0, adj_ = 0.0;

    void restart() { primed_ = false; underran_ = false; frac_ = 0.0; fill_avg_ = 0.0; integ_ = 0.0; adj_ = 0.0;
                     fill.store(0, std::memory_order_relaxed); adj_ppm.store(0, std::memory_order_relaxed); }
    void silence(ShmRing& rg, float* out, uint32_t n) {
        for (uint32_t i = 0; i < n * 2; ++i) out[i] = 0.0f;
        if (underran_) rg.ctl->starved.fetch_add(n, std::memory_order_relaxed);   // начальное ожидание запаса нехваткой не считаем
    }
};

}  // namespace bg
