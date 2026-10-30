// Bogestv0 MIXER — движок v0 (этап 2): микрофон -> программное усиление -> наушники.
// Консольная программа без драйвера. Звук через WASAPI (miniaudio).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "third_party/miniaudio.h"

#include "dsp.h"
#include "serve.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

static const char* VERSION = "0.3.0";

struct Engine {
    std::atomic<float> gain_lin{1.0f};
    bg::Meter in_meter;
    bg::Meter out_meter;
    std::atomic<unsigned long long> callbacks{0};
};

// Один вызов = один блок. Здесь нельзя выделять память, блокироваться и печатать.
static void on_audio(ma_device* dev, void* out, const void* in, ma_uint32 frames) {
    Engine* e = static_cast<Engine*>(dev->pUserData);
    const float* src = static_cast<const float*>(in);
    float* dst = static_cast<float*>(out);
    const size_t ch = dev->playback.channels;
    const size_t n = static_cast<size_t>(frames) * ch;
    const float g = e->gain_lin.load(std::memory_order_relaxed);

    if (!src) { std::memset(dst, 0, n * sizeof(float)); return; }
    e->in_meter.feed(src, frames, dev->capture.channels);
    for (size_t i = 0; i < n; ++i) dst[i] = bg::soft_limit(src[i] * g);
    e->out_meter.feed(dst, frames, ch);
    e->callbacks.fetch_add(1, std::memory_order_relaxed);
}

static void usage() {
    std::printf(
        "Bogestv0 MIXER engine %s\n\n"
        "  engine.exe --list                    список устройств\n"
        "  engine.exe --in N --out M [опции]    микрофон N -> наушники M\n"
        "  engine.exe --serve                   режим микшера для окна (команды в stdin)\n\n"
        "Опции:\n"
        "  --gain ДБ      усиление 0..30 (по умолчанию 0)\n"
        "  --period N     размер блока: 128, 256, 512 (по умолчанию 128)\n"
        "  --rate Гц      44100 или 48000 (по умолчанию 48000)\n"
        "  --exclusive    эксклюзивный режим микрофона (меньше задержка)\n"
        "  --exclusive-out эксклюзивный режим выхода (сильно меньше задержка, закройте Voicemeeter)\n"
        "  --selftest     проверка обработки без звуковых устройств\n"
        "  --fake-driver [секунд]  имитация драйвера (этап 6): тон в Input 2, слушает Output 1; запускать при работающем микшере\n\n"
        "Клавиши: + / - усиление, q выход.\n", VERSION);
}


// ---------- проверка обработки микрофона (этап 4) ----------
static int selftest_mic() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok    " : "ОШИБКА", what); if (!ok) ++bad; };
    const size_t N = 48000 * 4, B = 128;
    uint32_t rng = 2463534242u;
    auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return static_cast<float>(rng & 0xFFFFFF) / 8388608.0f - 1.0f; };
    auto tone = [&](float hz, float dbfs) {
        std::vector<float> v(N * 2);
        const float a = bg::db_to_lin(dbfs);
        for (size_t i = 0; i < N; ++i) v[i * 2] = v[i * 2 + 1] = a * std::sin(2.0f * 3.14159265f * hz * static_cast<float>(i) / 48000.0f);
        return v;
    };
    auto noise = [&](float dbfs) {
        std::vector<float> v(N * 2);
        const float a = bg::db_to_lin(dbfs) * 1.732f;   // равномерный шум: RMS = пик / √3
        for (size_t i = 0; i < N; ++i) v[i * 2] = v[i * 2 + 1] = a * rnd();
        return v;
    };
    auto run = [&](bg::MicChain& c, const std::vector<float>& in) {
        std::vector<float> out(in);
        for (size_t o = 0; o < N; o += B) c.process(out.data() + o * 2, std::min(B, N - o));
        return out;
    };
    // Уровень (дБFS по RMS) участка [from, to) в отсчётах.
    auto level = [&](const std::vector<float>& v, size_t from, size_t to) {
        double acc = 0.0;
        for (size_t i = from; i < to; ++i) acc += static_cast<double>(v[i * 2]) * v[i * 2];
        return 10.0f * std::log10(static_cast<float>(acc / static_cast<double>(to - from)) + 1e-20f);
    };
    auto neutral = [&](bg::MicChain& c) {
        c.p.gain_db = 0.0f; c.p.gate_db = -80.0f; c.p.hpf_hz = 0.0f; c.p.comp_ratio = 1.0f; c.p.ns_mode = 0; c.p.ns_amt = 100.0f;
    };
    const size_t T0 = 48000, T1 = N;   // смотрим последние 3 секунды

    {   // без настроек микрофон не меняется
        bg::MicChain c; neutral(c);
        auto in = tone(440.0f, -12.0f);
        auto out = run(c, in);
        float dmax = 0.0f;
        for (size_t i = 0; i < in.size(); ++i) dmax = std::max(dmax, std::fabs(out[i] - in[i]));
        check(dmax < 1e-6f, "микрофон без настроек проходит без изменений");
    }
    {   // усиление
        bg::MicChain c; neutral(c); c.p.gain_db = 20.0f;
        auto out = run(c, tone(1000.0f, -40.0f));
        check(std::fabs(level(out, T0, T1) - level(tone(1000.0f, -40.0f), T0, T1) - 20.0f) < 0.2f, "усиление +20 дБ даёт ровно +20 дБ");
    }
    {   // защита от перегрузки
        bg::MicChain c; neutral(c); c.p.gain_db = 30.0f;
        auto out = run(c, tone(300.0f, -6.0f));
        float peak = 0.0f; bool finite = true;
        for (float x : out) { peak = std::max(peak, std::fabs(x)); finite = finite && std::isfinite(x); }
        check(peak <= 1.0f && finite, "усиление +30 дБ на громком сигнале не выходит за ±1.0");
    }
    {   // фильтр низких частот
        bg::MicChain c; neutral(c); c.p.hpf_hz = 100.0f;
        auto lo_in = tone(40.0f, -20.0f), mid_in = tone(1000.0f, -20.0f);
        const float low = level(run(c, lo_in), T0, T1) - level(lo_in, T0, T1);
        bg::MicChain d; neutral(d); d.p.hpf_hz = 100.0f;
        const float mid = level(run(d, mid_in), T0, T1) - level(mid_in, T0, T1);
        std::printf("       фильтр 100 Гц: 40 Гц %.1f дБ, 1 кГц %.2f дБ\n", low, mid);
        check(low < -20.0f, "фильтр 100 Гц: 40 Гц подавлено больше чем на 20 дБ");
        check(std::fabs(mid) < 0.3f, "фильтр 100 Гц: 1 кГц проходит без изменений");
    }
    {   // гейт
        bg::MicChain c; neutral(c); c.p.gate_db = -45.0f;
        const float quiet = level(run(c, noise(-60.0f)), T0, T1);
        bg::MicChain d; neutral(d); d.p.gate_db = -45.0f;
        const float loud = level(run(d, tone(500.0f, -20.0f)), T0, T1);
        check(quiet < -60.0f - 30.0f, "гейт -45 дБ закрывает шум -60 дБ (тише на 30 дБ и больше)");
        check(std::fabs(loud - (-20.0f - 3.0103f)) < 0.3f, "гейт не трогает громкий сигнал");
    }
    {   // компрессор
        bg::MicChain c; neutral(c); c.p.comp_ratio = 4.0f;
        const float a = level(run(c, tone(1000.0f, -30.0f)), T0, T1);
        bg::MicChain d; neutral(d); d.p.comp_ratio = 4.0f;
        const float b = level(run(d, tone(1000.0f, -6.0f)), T0, T1);
        check((b - a) < 24.0f - 6.0f, "компрессор 4:1 сжимает разницу между тихим и громким больше чем на 6 дБ");
    }
    {   // шумоподавление: фон должен стать заметно тише
        bg::MicChain c; neutral(c); c.p.ns_mode = 3; c.p.ns_amt = 100.0f;
        auto in = noise(-35.0f);
        auto out = run(c, in);
        const float drop = level(in, T0, T1) - level(out, T0, T1);
        std::printf("       шум -35 дБ, «Максимум»: тише на %.1f дБ\n", drop);
        check(drop > 15.0f, "шумоподавление «Максимум» опускает постоянный шум больше чем на 15 дБ");
        bg::MicChain m; neutral(m); m.p.ns_mode = 1; m.p.ns_amt = 50.0f;
        const float drop1 = level(in, T0, T1) - level(run(m, in), T0, T1);
        std::printf("       шум -35 дБ, «Мягкое» 50%%: тише на %.1f дБ\n", drop1);
        check(drop1 > 3.0f && drop1 < drop, "«Мягкое» слабее, чем «Максимум», но работает");
        const float fr = c.raw_floor.floor_db.load(), fp = c.proc_floor.floor_db.load();
        check(fr > -50.0f && fp < fr - 10.0f, "оценка фона в паузах: после подавления ниже, чем до");
    }
    {   // при силе 0 % звук просто задержан на 10 мс: смесь не гребенчатит
        bg::MicChain c; neutral(c); c.p.ns_mode = 1; c.p.ns_amt = 0.0f;
        auto in = tone(700.0f, -15.0f);
        auto out = run(c, in);
        float dmax = 0.0f;
        for (size_t i = 6000; i < N; ++i) dmax = std::max(dmax, std::fabs(out[i * 2] - in[(i - 480) * 2]));
        check(dmax < 1e-5f, "сила 0 %: звук идёт с задержкой ровно 480 отсчётов (10 мс)");
    }
    {   // переключение режимов на ходу не ломает звук
        bg::MicChain c; neutral(c);
        auto in = tone(500.0f, -20.0f);
        std::vector<float> out(in);
        bool finite = true; float jump = 0.0f, prev = 0.0f;
        for (size_t o = 0; o < N; o += B) {
            const size_t blk = o / B;
            c.p.ns_mode = static_cast<int>((blk / 150) % 4);
            c.process(out.data() + o * 2, std::min(B, N - o));
        }
        for (size_t i = 0; i < N; ++i) { finite = finite && std::isfinite(out[i * 2]); jump = std::max(jump, std::fabs(out[i * 2] - prev)); prev = out[i * 2]; }
        check(finite && jump < 0.2f, "переключение шумодава на ходу без щелчков и NaN");
    }
    return bad;
}

// ---------- проверка шин и переподключения устройств (этап 5), на «нулевом» бэкенде miniaudio ----------
static int selftest_devices() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok    " : "ОШИБКА", what); if (!ok) ++bad; };
    ma_backend nb[] = {ma_backend_null};
    ma_context ctx;
    ma_context_config cc = ma_context_config_init();
    if (ma_context_init(nb, 1, &cc, &ctx) != MA_SUCCESS) { std::printf("пропущено: нулевой звуковой бэкенд недоступен\n"); return 0; }
    {
        bg::Mixer m(&ctx, nullptr);   // без общей памяти: она проверяется отдельно
        // Фейдеры и Mute шин: границы и ошибки.
        check(m.command("set bus 0 gain -6").empty() && std::fabs(m.buses[0].gain_db.load() + 6.0f) < 1e-6f, "шина: фейдер ставится");
        m.command("set bus 1 gain 99");
        m.command("set bus 2 gain -99");
        check(m.buses[1].gain_db.load() == 12.0f && m.buses[2].gain_db.load() == -48.0f, "шина: фейдер ограничен -48…+12 дБ");
        m.command("set bus 3 mute 1");
        check(m.buses[3].mute.load() && !m.buses[4].mute.load(), "шина: Mute включается только у своей шины");
        check(!m.command("set bus 7 gain 0").empty() && !m.command("set bus 0 bass 3").empty(), "шина: неверный номер и неизвестное свойство дают ошибку");

        const std::string dev = m.debug_name(false);
        if (dev.empty()) { std::printf("пропущено: нет устройств-пустышек\n"); }
        else {
            check(m.command("set dev a1 " + dev).empty() && m.slot_ok(2), "устройство A1 открылось (ведущее)");
            check(m.command("set dev a2 " + dev).empty() && m.slot_ok(2) && m.slot_ok(3), "устройство A2 открылось");

            // Обычное устройство остановилось (выдернули наушники): открывается само, остальные не трогаем.
            const void* master_before = m.debug_dev(2);
            m.debug_stop(3);
            check(!m.debug_started(3), "(проверка самой проверки) после остановки устройство A2 действительно не работает");
            std::string err = m.watchdog();
            check(err.empty() && m.slot_ok(3), "сторож: остановившееся устройство A2 открыто заново");
            check(m.slot_ok(2) && m.debug_dev(2) == master_before, "сторож: ведущее устройство при этом не пересобиралось");

            // Остановилось ведущее: тракт собирается заново.
            m.debug_stop(2);
            check(!m.debug_started(2), "(проверка самой проверки) после остановки ведущее устройство действительно не работает");
            err = m.watchdog();
            check(err.empty() && m.slot_ok(2) && m.slot_ok(3), "сторож: остановившееся ведущее устройство вернулось вместе с трактом");

            // Назначенного устройства нет в системе: ждём, ничего не пересобираем и не шумим.
            m.command("set dev a3 Такого устройства нет");
            const void* before = m.debug_dev(2);
            bool quiet = true;
            for (int i = 0; i < 3; ++i) quiet = quiet && m.watchdog().empty();
            check(quiet && !m.slot_ok(4) && m.slot_ok(2) && m.debug_dev(2) == before, "сторож: отсутствующее устройство A3 ждёт, остальной звук не прерывается");
            m.command("set dev a3 -");
        }
        m.command("set dev a1 -");
        m.command("set dev a2 -");
    }
    ma_context_uninit(&ctx);
    return bad;
}

// ---------- проверка общей памяти и обмена с драйвером (этап 6.1) ----------
// Без драйвера: «драйвер» в тестах это второе отображение той же памяти и поток, который пишет и читает кольца.
static int selftest_shm() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok    " : "ОШИБКА", what); if (!ok) ++bad; };
    const std::string nm = "Bogestv0MixerTest" + std::to_string(static_cast<unsigned long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007ull));
    std::string err;
    {
        auto eng = bg::Shm::open(nm, bg::Shm::Role::Engine, err);
        check(eng != nullptr, "общая память создаётся");
        if (eng) std::printf("      (%s)\n", eng->where().c_str()); else std::printf("      %s\n", err.c_str());
        if (!eng) return bad + 1;
        check(bg::Shm::open(nm + "Нет", bg::Shm::Role::Peer, err) == nullptr, "драйвер без движка память не открывает");
        auto drv = bg::Shm::open(nm, bg::Shm::Role::Peer, err);
        check(drv != nullptr, "драйвер открывает готовую память");
        if (!drv) return bad + 1;
        const bg::ShmHeader* h = drv->header();
        check(h->rate == 48000 && h->channels == 2 && h->in_rings == 6 && h->out_rings == 8 && h->ring_frames == bg::kShmRingFrames,
              "заголовок: 48 кГц, 2 канала, 6 входов и 2 выхода");
        check(sizeof(bg::ShmHeader) == 256 && sizeof(bg::ShmRingCtl) == 128, "размеры заголовка и управляющего блока зафиксированы");

        // --- кольцо: порядок, нехватка, переполнение, переход через границу uint32 ---
        bg::ShmRing e = eng->in(1), d = drv->in(1);   // одно и то же кольцо с двух сторон
        std::vector<float> a(2000 * 2), o(2000 * 2, 9.0f);
        for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<float>(i) * 0.001f;
        check(d.push(a.data(), 100) == 100 && e.available() == 100, "кольцо: записали 100 кадров");
        check(e.pop(o.data(), 60) == 60 && std::fabs(o[2 * 59] - a[2 * 59]) < 1e-9f && std::fabs(o[2 * 59 + 1] - a[2 * 59 + 1]) < 1e-9f, "кольцо: кадры идут по порядку, L и R не путаются");
        check(e.pop(o.data(), 80) == 40 && o[2 * 40] == 0.0f && o[2 * 79 + 1] == 0.0f && e.ctl->starved.load() == 40, "кольцо: нехватка добивается тишиной и считается");
        std::vector<float> big(9000 * 2, 0.1f);
        check(d.push(big.data(), 9000) == bg::kShmRingFrames && d.ctl->drops.load() == 9000 - bg::kShmRingFrames, "кольцо: переполнение отбрасывает лишнее и считает");
        e.discard();
        check(e.available() == 0, "кольцо: discard() выбрасывает накопленное");
        e.ctl->write_pos.store(0xFFFFFFF0u); e.ctl->read_pos.store(0xFFFFFFF0u);   // позиции у границы uint32
        check(d.push(a.data(), 100) == 100 && e.available() == 100, "кольцо: запись через границу uint32");
        std::fill(o.begin(), o.end(), 0.0f);
        check(e.pop(o.data(), 100) == 100 && std::fabs(o[2 * 99] - a[2 * 99]) < 1e-9f && std::fabs(o[0] - a[0]) < 1e-9f, "кольцо: чтение через границу uint32");
        d.push(big.data(), 3000);
        e.pop(o.data(), 128, 960);
        check(e.available() == 960 - 128, "кольцо: запас обрезается до заданной задержки (остаётся 960 кадров минус прочитанные)");
        e.discard();
        e.ctl->write_pos.store(12345u); e.ctl->read_pos.store(1u);   // испорченные позиции (разность больше кольца)
        check(e.pop(o.data(), 64) == 0 && e.available() == 0, "кольцо: испорченные позиции не приводят к чтению мусора");
        e.ctl->write_pos.store(0); e.ctl->read_pos.store(0);

        // --- поток-писатель и поток-читатель: все кадры в порядке, без потерь ---
        {
            bg::ShmRing w = eng->in(2), r = drv->in(2);
            const uint32_t total = 400000;
            std::atomic<bool> ordered{true};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            std::thread prod([&] {
                uint32_t sent = 0;
                std::vector<float> blk(256 * 2);
                while (sent < total && std::chrono::steady_clock::now() < deadline) {   // читатель сдался: не зависаем
                    const uint32_t n = std::min<uint32_t>(256, total - sent);
                    if (bg::kShmRingFrames - w.available() < n) { std::this_thread::yield(); continue; }   // нет места: ждём, блок целиком
                    for (uint32_t i = 0; i < n; ++i) { blk[i * 2] = static_cast<float>(sent + i); blk[i * 2 + 1] = -static_cast<float>(sent + i); }
                    sent += w.push(blk.data(), n);
                }
            });
            uint32_t got = 0;
            std::vector<float> blk(300 * 2);
            const auto t0 = std::chrono::steady_clock::now();
            while (got < total && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(50)) {
                const uint32_t m = r.pop(blk.data(), 300);
                for (uint32_t i = 0; i < m; ++i)
                    if (blk[i * 2] != static_cast<float>(got + i) || blk[i * 2 + 1] != -static_cast<float>(got + i)) ordered = false;
                got += m;
                if (!m) std::this_thread::yield();
            }
            prod.join();
            check(got == total && ordered.load(), "два потока: 400000 кадров дошли по порядку без потерь");
        }

        // --- второй движок, повторный запуск ---
        eng->header()->engine_beat.store(0);
        std::atomic<bool> beating{true};
        std::thread beat([&] { while (beating) { eng->header()->engine_beat.fetch_add(1); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } });
        std::string err2;
        auto second = bg::Shm::open(nm, bg::Shm::Role::Engine, err2);
        beating = false; beat.join();
        check(second == nullptr && !err2.empty(), "второй движок при работающем первом получает отказ");
        eng->in(3).ctl->write_pos.store(500);   // накопилось, пока движок не читал
        eng.reset();                             // движок закрылся: драйвер держит память
        check(drv->header()->engine_pid == 0, "после закрытия движка engine_pid = 0");
        auto again = bg::Shm::open(nm, bg::Shm::Role::Engine, err2);
        check(again != nullptr && again->header()->engine_pid != 0 && again->in(3).available() == 0,
              "перезапуск движка: память и заголовок на месте, старый звук входов выброшен");
    }
    bg::Shm::unlink_name(nm);
    return bad;
}

// Движок целиком на «нулевом» звуковом устройстве и «драйвер» в потоке: V2 из драйвера идёт на B1 и обратно к драйверу.
static int selftest_driver_mixer() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok    " : "ОШИБКА", what); if (!ok) ++bad; };
    ma_backend nb[] = {ma_backend_null};
    ma_context ctx;
    ma_context_config cc = ma_context_config_init();
    if (ma_context_init(nb, 1, &cc, &ctx) != MA_SUCCESS) { std::printf("пропущено: нулевой звуковой бэкенд недоступен\n"); return 0; }
    const std::string nm = "Bogestv0MixerTestM" + std::to_string(static_cast<unsigned long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007ull));
    {
        bg::Mixer m(&ctx, nm.c_str());
        std::string dev = m.debug_name(false);
        if (!m.shm_ready() || dev.empty()) { std::printf("пропущено: нет общей памяти или устройств-пустышек\n"); }
        else {
            std::string err;
            auto drv = bg::Shm::open(nm, bg::Shm::Role::Peer, err);
            check(drv != nullptr, "драйвер подключился к памяти движка");
            check(m.command("set dev a1 " + dev).empty() && m.slot_ok(2), "ведущее устройство A1 открылось");
            m.command("set strip 3 route 3 1");   // V2 («Музыка») -> B1
            check(m.state_json().find("\"drv\":{\"shm\":1,\"via\":0,\"found\":0,\"alive\":0,\"in\":[0,0,0,0,0,0],\"out\":[0,0,0,0,0,0,0,0],\"diag\":{\"rate\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"starved\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"drops\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"trim\":[0,0,0,0,0,0]}}") != std::string::npos, "состояние: драйвера пока нет, диагностика колец нулевая");

            // «Драйвер»: открывает Input 2 и Output 1, пишет постоянный сигнал в Input 2 и читает Output 1.
            bg::ShmRing in2 = drv->in(1), out1 = drv->out(0);
            out1.discard(); out1.set_active(true);
            std::atomic<bool> stop{false};
            std::thread th([&] {
                std::vector<float> blk(480 * 2);
                for (uint32_t i = 0; i < 480; ++i) { blk[i * 2] = 0.5f; blk[i * 2 + 1] = -0.25f; }
                in2.set_active(true);
                while (!stop) {
                    in2.push(blk.data(), 480);
                    drv->header()->driver_beat.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
            bool heard = false;
            std::vector<float> o(480 * 2);
            for (int i = 0; i < 100 && !heard; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const uint32_t got = out1.pop(o.data(), 480);
                for (uint32_t f = 0; f < got; ++f) if (std::fabs(o[f * 2] - 0.5f) < 1e-3f && std::fabs(o[f * 2 + 1] + 0.25f) < 1e-3f) heard = true;
            }
            check(heard, "звук из Input 2 драйвера пришёл на B1 и вернулся в Output 1 (L=0.5, R=-0.25)");
            // Выход дорожки Out 2 (V2): чистый звук до фейдера и Mute. Дорожка заглушена, а в OBS звук идёт.
            bg::ShmRing tap2 = drv->out(bg::kShmBusRings + 1);
            tap2.discard(); tap2.set_active(true);
            m.command("set strip 3 mute 1");
            bool heard_tap = false;
            for (int i = 0; i < 100 && !heard_tap; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const uint32_t got = tap2.pop(o.data(), 480);
                for (uint32_t f = 0; f < got; ++f) if (std::fabs(o[f * 2] - 0.5f) < 1e-3f && std::fabs(o[f * 2 + 1] + 0.25f) < 1e-3f) heard_tap = true;
            }
            check(heard_tap, "выход дорожки Out 2 отдаёт звук V2 до фейдера и Mute (L=0.5, R=-0.25)");
            m.command("set strip 3 mute 0");
            check(drv->header()->engine_beat.load() > 0, "движок считает блоки: engine_beat растёт");
            m.poll_driver();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));   // драйвер успевает сделать такт между опросами
            m.poll_driver();
            check(m.driver_alive() && m.state_json().find("\"in\":[0,1,0,0,0,0],\"out\":[1,0,0,1,0,0,0,0]") != std::string::npos, "состояние: драйвер жив, открыты Input 2 и Output 1");

            // Тест-источник важнее драйвера: пока он включён, звук из драйвера не берётся.
            m.command("set strip 3 src sine");
            m.command("set strip 3 gain 0");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            out1.discard();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            // Синус одинаков в L и R, а звук драйвера нет (L=0.5, R=-0.25): если драйвер просочился, разница заметна.
            bool only_sine = true, any = false;
            for (uint32_t got = out1.pop(o.data(), 480), f = 0; f < got; ++f) { any = true; if (std::fabs(o[f * 2] - o[f * 2 + 1]) > 1e-4f) only_sine = false; }
            check(any && only_sine, "тест-источник на дорожке заглушает звук драйвера");
            m.command("set strip 3 src off");

            // Драйвер закрыл поток: дорожка замолкает, движок в Output 1 больше не пишет.
            in2.set_active(false);
            out1.set_active(false);
            tap2.set_active(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint32_t w_before = out1.ctl->write_pos.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            check(out1.ctl->write_pos.load() == w_before, "пока Output 1 не открыт драйвером, движок в него не пишет");
            stop = true;
            th.join();

            // Драйвер пропал (счётчик стоит): через проверку сторожа он «не жив».
            m.poll_driver();
            m.poll_driver();
            check(!m.driver_alive(), "счётчик драйвера стоит: драйвер считается пропавшим");
            m.command("set dev a1 -");
        }
    }
    ma_context_uninit(&ctx);
    bg::Shm::unlink_name(nm);
    return bad;
}

// Имитация драйвера для проверки на Windows без самого драйвера: `engine.exe --fake-driver [секунд]`.
// Запускать, пока работает микшер (окно или `--serve`). Играет тон 440 Гц в «Bogestv0 Input 2» и слушает «Bogestv0 Output 1».
static int fake_driver(int seconds) {
    std::string err;
    auto shm = bg::Shm::open(bg::kShmDefaultName, bg::Shm::Role::Peer, err);
    if (!shm) { std::fprintf(stderr, "%s. Сначала запустите микшер (окно программы).\n", err.c_str()); return 1; }
    std::printf("Имитация драйвера подключена (%s). %d с.\n", shm->where().c_str(), seconds);
    std::printf("Input 2 (дорожка «Музыка») получает тон 440 Гц; B1 слушаем в Output 1.\n");
    std::printf("Чтобы услышать тон: на «Микшере» у «Музыка» выключите тест-источник и включите маршрут на A1 или A2.\n\n");
    bg::ShmRing in2 = shm->in(1), out1 = shm->out(0);
    out1.discard(); out1.set_active(true); in2.set_active(true);
    double phase = 0.0;
    std::vector<float> blk(480 * 2), rd(480 * 2);
    float peak = 0.0f;
    auto next = std::chrono::steady_clock::now();
    for (int tick = 0; tick < seconds * 100; ++tick) {
        for (int i = 0; i < 480; ++i) {
            blk[i * 2] = blk[i * 2 + 1] = static_cast<float>(std::sin(phase)) * 0.3f;
            phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
        }
        in2.push(blk.data(), 480);
        shm->header()->driver_beat.fetch_add(1, std::memory_order_relaxed);
        const uint32_t got = out1.pop(rd.data(), 480);
        for (uint32_t i = 0; i < got * 2; ++i) peak = std::max(peak, std::fabs(rd[i]));
        if (tick % 100 == 99) {
            std::printf("engine_beat %u | из B1 пришло, пик %.1f дБ | потеряно кадров: вход %u, выход %u\n", shm->header()->engine_beat.load(),
                        bg::lin_to_db(peak), in2.ctl->drops.load(), out1.ctl->starved.load());
            peak = 0.0f;
        }
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
    in2.set_active(false); out1.set_active(false);
    return 0;
}

static int selftest() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok    " : "ОШИБКА", what); if (!ok) ++bad; };
    check(std::fabs(bg::db_to_lin(0.0f) - 1.0f) < 1e-6f, "0 дБ = x1");
    check(std::fabs(bg::db_to_lin(20.0f) - 10.0f) < 1e-4f, "+20 дБ = x10");
    check(std::fabs(bg::lin_to_db(0.5f) + 6.0206f) < 1e-3f, "0.5 = -6.02 дБ");
    check(std::fabs(bg::soft_limit(0.5f) - 0.5f) < 1e-6f, "тихий сигнал не меняется");
    bool bounded = true, mono = true;
    float prev = -2.0f;
    for (float x = 0.0f; x <= 40.0f; x += 0.01f) {
        float y = bg::soft_limit(x);
        if (y > 1.0f) bounded = false;
        if (y < prev - 1e-6f) mono = false;
        prev = y;
    }
    check(bounded, "после ограничителя не больше 1.0 даже при x40");
    check(mono, "ограничитель не ломает форму (растёт монотонно)");
    check(bg::soft_limit(-3.0f) < 0.0f && bg::soft_limit(-3.0f) >= -1.0f, "отрицательные значения симметричны");
    bg::Meter m;
    float buf[8] = {0.25f, -0.5f, 0.1f, 0.0f, -0.75f, 0.3f, 0.0f, 0.0f};
    m.feed(buf, 4, 2);
    check(std::fabs(m.peak[0].load() - 0.75f) < 1e-6f && std::fabs(m.peak[1].load() - 0.5f) < 1e-6f, "метр: пики L=0.75 R=0.5");
    float v = m.read(0);
    check(std::fabs(v - 0.75f) < 1e-6f && std::fabs(m.peak[0].load() - 0.6f) < 1e-6f, "метр: плавный спад");
    bad += selftest_mic();
    bad += selftest_devices();
    bad += selftest_shm();
    bad += selftest_driver_mixer();
    std::printf(bad ? "\nЕсть ошибки: %d\n" : "\nВсе проверки пройдены.\n", bad);
    return bad ? 1 : 0;
}

struct DevList {
    std::vector<ma_device_info> cap, play;
};

static bool enumerate(ma_context& ctx, DevList& d) {
    ma_device_info *pp, *cp;
    ma_uint32 pc, cc;
    if (ma_context_get_devices(&ctx, &pp, &pc, &cp, &cc) != MA_SUCCESS) return false;
    d.play.assign(pp, pp + pc);
    d.cap.assign(cp, cp + cc);
    return true;
}

static void print_bar(float lin, int width) {
    float db = bg::lin_to_db(lin);
    int n = static_cast<int>(std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f)) * width + 0.5f);
    for (int i = 0; i < width; ++i) std::putchar(i < n ? '#' : '.');
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    int in_idx = -1, out_idx = -1, period = 128, rate = 48000;
    float gain_db = 0.0f;
    bool serve = false;
    bool list = false, exclusive = false, exclusive_out = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "Не хватает значения для %s\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "--list") list = true;
        else if (a == "--serve") serve = true;
        else if (a == "--in") in_idx = std::atoi(next("--in"));
        else if (a == "--out") out_idx = std::atoi(next("--out"));
        else if (a == "--gain") gain_db = static_cast<float>(std::atof(next("--gain")));
        else if (a == "--period") period = std::atoi(next("--period"));
        else if (a == "--rate") rate = std::atoi(next("--rate"));
        else if (a == "--exclusive") exclusive = true;
        else if (a == "--exclusive-out") exclusive_out = true;
        else if (a == "--selftest") return selftest();
        else if (a == "--fake-driver") return fake_driver(i + 1 < argc && std::atoi(argv[i + 1]) > 0 ? std::atoi(argv[i + 1]) : 15);
        else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }
    if (!list && !serve && (in_idx < 0 || out_idx < 0)) { usage(); return 2; }
    if (period != 128 && period != 256 && period != 512) { std::fprintf(stderr, "--period: 128, 256 или 512\n"); return 2; }
    if (rate != 44100 && rate != 48000) { std::fprintf(stderr, "--rate: 44100 или 48000\n"); return 2; }
    gain_db = std::max(0.0f, std::min(30.0f, gain_db));

    ma_backend backends[] = {ma_backend_wasapi};
    ma_context ctx;
    ma_context_config cc = ma_context_config_init();
    ma_result r = ma_context_init(backends, 1, &cc, &ctx);
#ifndef _WIN32
    if (r != MA_SUCCESS) r = ma_context_init(nullptr, 0, &cc, &ctx);  // для проверки на не-Windows
#endif
    if (r != MA_SUCCESS) { std::fprintf(stderr, "Не удалось открыть звуковую систему (код %d)\n", r); return 1; }

    if (serve) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        int rc = bg::run_server(ctx);
        ma_context_uninit(&ctx);
        return rc;
    }

    DevList dl;
    if (!enumerate(ctx, dl)) { std::fprintf(stderr, "Не удалось получить список устройств\n"); return 1; }

    if (list) {
        std::printf("Микрофоны (--in):\n");
        for (size_t i = 0; i < dl.cap.size(); ++i) std::printf("  %zu  %s%s\n", i, dl.cap[i].name, dl.cap[i].isDefault ? "  [по умолчанию]" : "");
        std::printf("Выходы (--out):\n");
        for (size_t i = 0; i < dl.play.size(); ++i) std::printf("  %zu  %s%s\n", i, dl.play[i].name, dl.play[i].isDefault ? "  [по умолчанию]" : "");
        ma_context_uninit(&ctx);
        return 0;
    }
    if (in_idx >= static_cast<int>(dl.cap.size()) || out_idx >= static_cast<int>(dl.play.size())) {
        std::fprintf(stderr, "Нет устройства с таким номером. Посмотрите --list.\n");
        return 2;
    }

    Engine eng;
    eng.gain_lin = bg::db_to_lin(gain_db);

    // Пробуем настройки от самых быстрых к самым мягким и показываем, какие подошли.
    struct Attempt { ma_performance_profile prof; ma_uint32 period; const char* name; };
    const Attempt attempts[] = {
        {ma_performance_profile_low_latency, (ma_uint32)period, "малая задержка"},
        {ma_performance_profile_conservative, (ma_uint32)period, "обычный режим"},
        {ma_performance_profile_conservative, (ma_uint32)period * 2, "увеличенный блок"},
        {ma_performance_profile_conservative, 0, "блок по умолчанию"},
    };
    auto make_cfg = [&](const Attempt& at, ma_device_type type) {
        ma_device_config cfg = ma_device_config_init(type);
        cfg.sampleRate = static_cast<ma_uint32>(rate);
        cfg.periodSizeInFrames = at.period;
        cfg.periods = 2;
        cfg.performanceProfile = at.prof;
        cfg.noPreSilencedOutputBuffer = MA_TRUE;
        cfg.dataCallback = on_audio;
        cfg.pUserData = &eng;
        cfg.capture.pDeviceID = &dl.cap[in_idx].id;
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 2;
        cfg.capture.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
        cfg.playback.pDeviceID = &dl.play[out_idx].id;
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.playback.shareMode = exclusive_out ? ma_share_mode_exclusive : ma_share_mode_shared;
        return cfg;
    };

    ma_device dev;
    bool opened = false;
    for (const Attempt& at : attempts) {
        ma_device_config cfg = make_cfg(at, ma_device_type_duplex);
        ma_result dr = ma_device_init(&ctx, &cfg, &dev);
        if (dr == MA_SUCCESS) { std::printf("Режим: %s\n", at.name); opened = true; break; }
        std::fprintf(stderr, "Режим «%s» не открылся: %s (код %d)\n", at.name, ma_result_description(dr), dr);
    }
    if (!opened) {
        std::fprintf(stderr, "\nПроверяю устройства по отдельности:\n");
        {
            ma_device_config c = make_cfg(attempts[1], ma_device_type_capture);
            ma_device d; ma_result x = ma_device_init(&ctx, &c, &d);
            std::fprintf(stderr, "  микрофон %d (%s): %s\n", in_idx, dl.cap[in_idx].name, x == MA_SUCCESS ? "открывается" : ma_result_description(x));
            if (x == MA_SUCCESS) ma_device_uninit(&d);
        }
        {
            ma_device_config c = make_cfg(attempts[1], ma_device_type_playback);
            ma_device d; ma_result x = ma_device_init(&ctx, &c, &d);
            std::fprintf(stderr, "  выход %d (%s): %s\n", out_idx, dl.play[out_idx].name, x == MA_SUCCESS ? "открывается" : ma_result_description(x));
            if (x == MA_SUCCESS) ma_device_uninit(&d);
        }
        std::fprintf(stderr, "Если оба открываются по отдельности, а вместе нет, пришлите этот текст целиком.\n");
        ma_context_uninit(&ctx);
        return 1;
    }
    if (ma_device_start(&dev) != MA_SUCCESS) { std::fprintf(stderr, "Не удалось запустить поток\n"); ma_device_uninit(&dev); ma_context_uninit(&ctx); return 1; }

    const double rate_hz = dev.sampleRate;
    const double frames_path = static_cast<double>(dev.capture.internalPeriodSizeInFrames) * dev.capture.internalPeriods +
                               static_cast<double>(dev.playback.internalPeriodSizeInFrames) * dev.playback.internalPeriods;
    const double est_ms = frames_path / (dev.capture.internalSampleRate ? dev.capture.internalSampleRate : rate_hz) * 1000.0;

    std::printf("Bogestv0 MIXER engine %s\n", VERSION);
    std::printf("Микрофон: %s\nВыход:    %s\n", dev.capture.name, dev.playback.name);
    std::printf("%u Гц, блок %u смп (устройство: %u x %u / %u x %u), %s\n", dev.sampleRate, (unsigned)period,
                dev.capture.internalPeriodSizeInFrames, dev.capture.internalPeriods,
                dev.playback.internalPeriodSizeInFrames, dev.playback.internalPeriods,
                exclusive ? (exclusive_out ? "эксклюзив: микрофон и выход" : "эксклюзивный микрофон") : (exclusive_out ? "эксклюзивный выход" : "общий режим"));
    std::printf("Оценка задержки (буферы входа и выхода): около %.1f мс. Реальную может добавить драйвер звуковой карты.\n", est_ms);
    std::printf("ВНИМАНИЕ: пользуйтесь наушниками, иначе микрофон будет свистеть от колонок.\n");
    std::printf("Клавиши: + / - усиление, q выход.\n\n");

    bool run = true;
    while (run) {
#ifdef _WIN32
        while (_kbhit()) {
            int k = _getch();
            if (k == 'q' || k == 'Q') run = false;
            if (k == '+' || k == '=') gain_db = std::min(30.0f, gain_db + 1.0f);
            if (k == '-' || k == '_') gain_db = std::max(0.0f, gain_db - 1.0f);
            eng.gain_lin = bg::db_to_lin(gain_db);
        }
#endif
        float il = eng.in_meter.read(0), ir = eng.in_meter.read(1);
        bool clip = eng.out_meter.clipped.exchange(false);
        std::printf("\rвход L ");
        print_bar(il, 20);
        std::printf(" R ");
        print_bar(ir, 20);
        std::printf(" %6.1f дБ | усиление +%2.0f дБ | %s   ", bg::lin_to_db(std::max(il, ir)), gain_db, clip ? "ПЕРЕГРУЗ" : "        ");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    std::printf("\n");
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    return 0;
}
