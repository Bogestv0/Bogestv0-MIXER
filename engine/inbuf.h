// Чтение кольца с запасом и подстройкой под чужие часы (этап 6.6: часть A, а в 0.5.2 и остальные кольца движка).
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
// Читатель работает с любым кольцом «один пишет, один читает», у которого есть view() / commit() / on_silence() (ShmRing в shm.h и
// Ring в serve.h). Запас для каждого кольца свой: у входа драйвера 25 мс (Windows отдаёт порции по таймеру), у микрофонов и
// вторичных выходов 10 мс (обе стороны железные, дрожание меньше, а задержка важнее).
//
// Интерполяция кубическая (Catmull–Rom): линейная при медленно ползущей дробной части давала бы «дрожание» верхних частот.
//
// 0.5.3, часть 1: пересчёт частоты. Драйвер не пересчитывает частоту: Windows может открыть устройство на 44,1 кГц, и кадры в
// кольце тогда идут с этой частотой (драйвер пишет её в заголовок: reserved[i]). Читатель знает частоту источника и берёт за
// базовую скорость чтения src/48000 (44 100 Гц → 0,91875 кадра на выходной кадр). Подстройка под часы идёт поверх неё. Если
// источник быстрее 48 кГц (96 кГц и выше), перед прореживанием берётся среднее по шагу, чтобы верх не заворачивался в слышимый диапазон.
//
// 0.5.3, часть 2: запас сам подстраивается. Фиксированные 10 мс (микрофоны, A2) не хватали, когда Windows отдаёт и забирает порции
// по 10 мс: одна опоздавшая порция съедала весь запас, и читатель опустошался сотни раз. Теперь цель запаса ходит между базой
// (configure) и потолком kMaxTarget:
//   * не ниже блока, который читают (n + kBlockMargin): иначе опустошение гарантировано;
//   * после опустошения сразу растёт на недостающее плюс запас;
//   * раз в kWindowSec смотрит, сколько кадров оставалось сверх нужного перед худшим чтением окна («слабина»). Меньше kSlackLo → цель
//     растёт; больше kSlackHi несколько окон подряд → цель медленно снижается к базе. Потолок, пол и гистерезис не дают цели гулять.
#pragma once

#include "shm.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace bg {

constexpr uint32_t kInTargetFrames = 1200;   // 25 мс при 48 кГц: вход драйвера
constexpr uint32_t kHwTargetFrames = 512;    // ~10,7 мс: микрофоны и вторичные выходы (начальная цель; дальше растёт по необходимости)
constexpr uint32_t kMaxTarget = 4800;        // потолок цели запаса, 100 мс

class InReader {
public:
    // Счётчики для окна (с запуска движка). Запас и цель указаны в кадрах 48 кГц, даже если источник идёт на другой частоте.
    std::atomic<uint32_t> fill{0};      // средний запас сейчас
    std::atomic<uint32_t> under{0};     // сколько раз кольцо опустело (после начала игры)
    std::atomic<uint32_t> trim{0};      // сколько кадров выброшено сверху (в кадрах источника)
    std::atomic<uint32_t> opens{0};     // сколько раз поток открывался
    std::atomic<uint32_t> closes{0};    // сколько раз поток закрывался
    std::atomic<int32_t> adj_ppm{0};    // текущая подстройка скорости в миллионных долях
    std::atomic<uint32_t> tgt{kInTargetFrames};   // текущая цель запаса (адаптивная)
    std::atomic<uint32_t> grown{0};     // сколько раз цель увеличивали
    std::atomic<uint32_t> src_hz{kShmRate};   // частота источника, с которой читаем сейчас

    // Начальная цель запаса (кадры 48 кГц). Вызывать до начала работы потока. adaptive = false отключает рост и снижение цели
    // (так читал движок 0.5.2; нужно для сравнения в самопроверке).
    void configure(uint32_t target, bool adaptive = true) {
        base_ = target; tgt48_ = target; adaptive_ = adaptive;
        tgt.store(target, std::memory_order_relaxed);
    }
    // Из другого потока: «кольцо очищено или пересоздано, начни заново». Выполнится в начале следующего read().
    void request_reset() { reset_req_.store(true, std::memory_order_release); }

    // Поток только что открылся: старое не нужно, ждём запас.
    template <class R> void open(R& rg) {
        rg.commit(rg.view().w);
        restart();
        opens.fetch_add(1, std::memory_order_relaxed);
    }
    void closed() { closes.fetch_add(1, std::memory_order_relaxed); fill.store(0, std::memory_order_relaxed); }

    // Отдаёт ровно n кадров 48 кГц. Только аудио-поток. hz = частота кадров в кольце (0 = 48 000).
    template <class R> void read(R& rg, float* out, uint32_t n, uint32_t hz = kShmRate) {
        if (reset_req_.exchange(false, std::memory_order_acq_rel)) restart();
        if (hz < kMinHz || hz > kMaxHz) hz = kShmRate;
        if (hz != hz_) {   // Windows открыла поток на другой частоте: кадры в кольце теперь другие, начинаем заново
            hz_ = hz; k_ = static_cast<double>(hz) / static_cast<double>(kShmRate);
            src_hz.store(hz, std::memory_order_relaxed);
            restart();
        }
        if (adaptive_) {   // цель не ниже блока: читаем n кадров за раз, и после этого должен остаться запас на опоздание порции
            if (n > n_max_) n_max_ = n;
            const uint32_t fl = std::min(kMaxTarget, std::max(base_, n_max_ + kBlockMargin));
            if (tgt48_ < fl) { tgt48_ = fl; grown.fetch_add(1, std::memory_order_relaxed); }
        }
        tgt.store(tgt48_, std::memory_order_relaxed);

        const RingView v = rg.view();
        uint32_t r = v.r;
        uint32_t av = v.w - v.r;
        if (av > v.cap) { rg.commit(v.w); r = v.w; av = 0; restart(); }   // испорченные позиции: начинаем с чистого
        const uint32_t mask = v.cap - 1;
        const uint32_t target = static_cast<uint32_t>(std::lround(static_cast<double>(tgt48_) * k_));   // цель в кадрах источника
        const uint32_t max_av = std::min(std::max(target * 4, target + 4 * n), v.cap / 4 * 3);

        if (!primed_) {
            if (av < target) { silence(rg, out, n); return; }
            // Накопили запас. Небольшой избыток оставляем (регулятор сам его уберёт); если накопилось сильно больше цели
            // (поток стоял), лишнее старое выбрасываем.
            if (av > 2 * target) { trim.fetch_add(av - target, std::memory_order_relaxed); r += av - target; av = target; rg.commit(r); }
            primed_ = true; underran_ = false;
            frac_ = 0.0; fill_avg_ = static_cast<double>(av); integ_ = 0.0; adj_ = 0.0;
        } else if (av > max_av) {
            trim.fetch_add(av - target, std::memory_order_relaxed);
            r += av - target; av = target;
            rg.commit(r);
        }

        const double ratio = k_ * (1.0 + adj_);                           // кадров источника на один выходной кадр
        const int taps = ratio > 1.0001 ? std::min(8, static_cast<int>(std::ceil(ratio))) : 1;   // >1: усредняем по шагу (см. шапку)
        const double sub = ratio / taps;
        const double endp = frac_ + static_cast<double>(n - 1) * ratio + static_cast<double>(taps - 1) * sub;   // позиция последнего нужного кадра
        const uint32_t last = static_cast<uint32_t>(endp) + 2;            // кубической интерполяции нужен ещё кадр впереди
        if (last >= av) {   // не хватает данных: замолкаем один раз, данные остаются в кольце
            under.fetch_add(1, std::memory_order_relaxed);
            if (adaptive_) {   // цель мала для таких порций: растим на недостающее плюс запас и ждём, пока наберётся
                const double lack = static_cast<double>(last - av + 1) / k_;
                tgt48_ = std::min(kMaxTarget, tgt48_ + std::max(kBumpMin, static_cast<uint32_t>(lack) + kSlackMid));
                grown.fetch_add(1, std::memory_order_relaxed);
                tgt.store(tgt48_, std::memory_order_relaxed);
            }
            primed_ = false; underran_ = true; frac_ = 0.0;
            win_t_ = 0.0; win_min_ = 1e9; calm_ = 0;
            silence(rg, out, n);
            return;
        }
        const double slack = static_cast<double>(av - last) / k_;   // сколько кадров 48 кГц осталось сверх нужного перед этим чтением

        double p = frac_;
        const float inv = 1.0f / static_cast<float>(taps);
        for (uint32_t i = 0; i < n; ++i) {
            float acc0 = 0.0f, acc1 = 0.0f;
            for (int j = 0; j < taps; ++j) {
                const double q = p + static_cast<double>(j) * sub;
                const uint32_t k = static_cast<uint32_t>(q);
                const float t = static_cast<float>(q - k);
                const float* y0 = v.data + ((r + k - 1) & mask) * 2;   // кадр перед позицией (на самом старте это старый мусор, но t = 0, его вес нулевой)
                const float* y1 = v.data + ((r + k) & mask) * 2;
                const float* y2 = v.data + ((r + k + 1) & mask) * 2;
                const float* y3 = v.data + ((r + k + 2) & mask) * 2;
                acc0 += y1[0] + 0.5f * t * (y2[0] - y0[0] + t * (2.0f * y0[0] - 5.0f * y1[0] + 4.0f * y2[0] - y3[0] + t * (3.0f * (y1[0] - y2[0]) + y3[0] - y0[0])));
                acc1 += y1[1] + 0.5f * t * (y2[1] - y0[1] + t * (2.0f * y0[1] - 5.0f * y1[1] + 4.0f * y2[1] - y3[1] + t * (3.0f * (y1[1] - y2[1]) + y3[1] - y0[1])));
            }
            out[i * 2] = taps == 1 ? acc0 : acc0 * inv;
            out[i * 2 + 1] = taps == 1 ? acc1 : acc1 * inv;
            p += ratio;
        }
        const uint32_t used = static_cast<uint32_t>(p);
        frac_ = p - used;
        rg.commit(r + used);

        // Регулятор: средний запас (за ~0,5 с) тянем к цели. Пропорциональная часть быстро, интегральная убирает постоянный сдвиг
        // от разницы часов. Положительная ошибка (запаса много) = читаем чуть быстрее. Считаем в кадрах 48 кГц.
        const double dt = static_cast<double>(n) / static_cast<double>(kShmRate);
        fill_avg_ += std::min(1.0, dt / 0.5) * (static_cast<double>(av - used) - fill_avg_);
        const double err = (fill_avg_ - static_cast<double>(target)) / k_;
        integ_ = std::max(-kMaxAdj, std::min(kMaxAdj, integ_ + kKi * err * dt));
        adj_ = std::max(-kMaxAdj, std::min(kMaxAdj, kKp * err + integ_));
        fill.store(static_cast<uint32_t>(fill_avg_ / k_ + 0.5), std::memory_order_relaxed);
        adj_ppm.store(static_cast<int32_t>(std::lround(adj_ * 1e6)), std::memory_order_relaxed);

        // Подстройка цели запаса по «слабине» худшего чтения окна.
        if (adaptive_) {
            win_min_ = std::min(win_min_, slack);
            win_t_ += dt;
            if (win_t_ >= kWindowSec) {
                const uint32_t fl = std::min(kMaxTarget, std::max(base_, n_max_ + kBlockMargin));
                if (win_min_ < static_cast<double>(kSlackLo)) {
                    tgt48_ = std::min(kMaxTarget, tgt48_ + static_cast<uint32_t>(static_cast<double>(kSlackMid) - win_min_));
                    grown.fetch_add(1, std::memory_order_relaxed);
                    calm_ = 0;
                } else if (win_min_ > static_cast<double>(kSlackHi)) {
                    if (++calm_ >= kCalmWindows && tgt48_ > fl)
                        tgt48_ = std::max(fl, tgt48_ - std::min(kShrinkStep, static_cast<uint32_t>(win_min_ - static_cast<double>(kSlackMid))));
                } else {
                    calm_ = 0;
                }
                win_t_ = 0.0; win_min_ = 1e9;
            }
        }
    }

private:
    static constexpr double kMaxAdj = 0.003;    // ±0,3 %
    static constexpr double kKp = 1.0e-5;       // на кадр ошибки запаса
    static constexpr double kKi = 1.0e-6;       // на кадр ошибки в секунду
    static constexpr uint32_t kMinHz = 8000, kMaxHz = 192000;   // вне диапазона считаем, что частота не задана (48 000)
    static constexpr uint32_t kBlockMargin = 96;     // цель не ниже блока + 2 мс
    static constexpr uint32_t kBumpMin = 192;        // минимальный рост цели после опустошения (4 мс)
    static constexpr uint32_t kSlackLo = 96;         // слабина меньше 2 мс: цель растёт
    static constexpr uint32_t kSlackMid = 192;       // к этой слабине (4 мс) стремимся
    static constexpr uint32_t kSlackHi = 480;        // слабина больше 10 мс: цель можно снижать
    static constexpr uint32_t kShrinkStep = 96;      // снижение цели за окно (2 мс)
    static constexpr double kWindowSec = 2.0;        // окно измерения слабины
    static constexpr int kCalmWindows = 5;           // столько окон подряд «лишнего запаса» до начала снижения (10 с)

    uint32_t base_ = kInTargetFrames, tgt48_ = kInTargetFrames, n_max_ = 0;
    bool adaptive_ = true;
    std::atomic<bool> reset_req_{false};
    uint32_t hz_ = kShmRate;
    double k_ = 1.0;           // кадров источника на кадр 48 кГц при скорости 1
    bool primed_ = false;
    bool underran_ = false;    // кольцо опустело и ещё не набралось: тишина считается нехваткой
    double frac_ = 0.0, fill_avg_ = 0.0, integ_ = 0.0, adj_ = 0.0;
    double win_t_ = 0.0, win_min_ = 1e9;
    int calm_ = 0;

    void restart() { primed_ = false; underran_ = false; frac_ = 0.0; fill_avg_ = 0.0; integ_ = 0.0; adj_ = 0.0;
                     win_t_ = 0.0; win_min_ = 1e9; calm_ = 0;
                     fill.store(0, std::memory_order_relaxed); adj_ppm.store(0, std::memory_order_relaxed); }
    template <class R> void silence(R& rg, float* out, uint32_t n) {
        for (uint32_t i = 0; i < n * 2; ++i) out[i] = 0.0f;
        if (underran_) rg.on_silence(n);   // начальное ожидание запаса нехваткой не считаем
    }
};

}  // namespace bg
