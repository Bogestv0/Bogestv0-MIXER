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
#include <type_traits>
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
        check(!m.command("set bus 7 gain 0").empty() && !m.command("set bus 0 pan 3").empty(), "шина: неверный номер и неизвестное свойство дают ошибку");

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

// ---------- чтение входа драйвера с запасом (этап 6.6, этап A) ----------
// Без звуковых устройств и без реального времени: часы считаются виртуальные. «Драйвер» присылает порции по 480 кадров
// с дрожанием и своей частотой часов, «движок» забирает блоки по 128 кадров. На входе синус 440 Гц: по выходу видно
// провалы (тишина посреди звука) и щелчки (скачок больше, чем даёт синус).
struct InSim {
    double clock = 1.0;        // часы драйвера относительно движка (1.001 = на 0,1 % быстрее)
    double jitter_ms = 8.0;    // опоздание порции от 0 до этого значения
    double secs = 60.0;
    double stall_at = -1.0, stall_len = 0.0;   // драйвер замолчал, потом выдал накопленное разом
    uint32_t block = 128;
    uint32_t burst = 480;      // сколько кадров в одной порции
    uint32_t target = bg::kInTargetFrames;
    bool legacy = false;       // читать старым способом (pop без запаса) для сравнения
    bool adaptive = true;      // false = запас фиксированный (как в 0.5.2)
    uint32_t hz = 48000;       // частота кадров в кольце (Windows открыла поток не на 48 кГц)
    double tone = 440.0;       // частота синуса на входе, Гц
    double phase_ms = 0.0;     // сдвиг всех порций относительно блоков движка
    bool sym = false;          // дрожание в обе стороны (±jitter_ms): порция может прийти и раньше, и позже чтения, как в Windows
};
struct InSimRes {
    uint32_t gaps = 0, clicks = 0, under = 0, trim = 0, grown = 0;
    double fill_min = 1e9, fill_max = 0, fill_end = 0;
    int32_t adj_end = 0;
    double freq = 0.0;         // частота синуса на выходе (по переходам через ноль), Гц
    uint32_t tgt_end = 0;      // цель запаса в конце
};
template <class R> static void sim_reset(R& rg) { rg.commit(rg.view().w); }
template <class R> static void sim_legacy_pop(R& rg, float* out, uint32_t n) {
    if constexpr (std::is_same<R, bg::ShmRing>::value) rg.pop(out, n, std::max<uint32_t>(n * 3, 960));
    else rg.pop(out, n);
}
template <class R> static InSimRes run_in_sim(R& rg, const InSim& c) {
    sim_reset(rg);
    bg::InReader rd;
    rd.configure(c.target, c.adaptive);
    rd.open(rg);
    InSimRes res;
    std::vector<float> src(c.burst * 2), out(c.block * 2);
    uint32_t seed = 12345;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / 16777216.0; };
    const double period = c.burst / (static_cast<double>(c.hz) * c.clock);
    uint64_t sent = 0;                  // сколько кадров синуса отдано в кольцо
    double next_at = 0.0, prev_at = 0.0;
    auto arrival = [&](int k) {
        double t = c.phase_ms * 1e-3 + k * period + (k == 0 ? 0.0 : (c.sym ? (rnd() * 2.0 - 1.0) : rnd()) * c.jitter_ms * 1e-3);   // первая порция вовремя: самый невыгодный момент начала
        if (t < 0.0) t = 0.0;
        if (c.stall_at >= 0 && t >= c.stall_at && t < c.stall_at + c.stall_len) t = c.stall_at + c.stall_len;
        return std::max(t, prev_at);   // порции не обгоняют друг друга
    };
    int k = 0;
    next_at = arrival(k);
    float prev = 0.0f;
    uint32_t zero_run = 0, crossings = 0;
    bool started = false;
    uint64_t blocks = static_cast<uint64_t>(c.secs * 48000.0 / c.block);
    for (uint64_t b = 0; b < blocks; ++b) {
        const double te = static_cast<double>(b) * c.block / 48000.0;
        while (next_at <= te) {
            for (uint32_t i = 0; i < c.burst; ++i, ++sent) src[i * 2] = src[i * 2 + 1] = 0.5f * static_cast<float>(std::sin(6.283185307179586 * c.tone * static_cast<double>(sent) / static_cast<double>(c.hz)));
            rg.push(src.data(), c.burst);
            prev_at = next_at; ++k; next_at = arrival(k);
        }
        if (c.legacy) sim_legacy_pop(rg, out.data(), c.block);
        else rd.read(rg, out.data(), c.block, c.hz);
        for (uint32_t i = 0; i < c.block; ++i) {
            const float x = out[i * 2];
            if (te > 1.0) {
                if (prev < 0.0f && x >= 0.0f && prev != 0.0f) ++crossings;
                if (x == 0.0f) { if (++zero_run == 16) ++res.gaps; } else zero_run = 0;
                if (started && x != 0.0f && prev != 0.0f && std::fabs(x - prev) > 0.08f) ++res.clicks;
            }
            if (x != 0.0f) started = true;
            prev = x;
        }
        if (te > c.secs - 20.0 && !c.legacy) {
            const double f = rd.fill.load();
            res.fill_min = std::min(res.fill_min, f); res.fill_max = std::max(res.fill_max, f);
        }
    }
    res.under = rd.under.load(); res.trim = rd.trim.load(); res.grown = rd.grown.load();
    res.fill_end = rd.fill.load(); res.adj_end = rd.adj_ppm.load(); res.tgt_end = rd.tgt.load();
    res.freq = crossings / std::max(1.0, c.secs - 1.0);
    return res;
}

static int selftest_inbuf() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok    " : "ОШИБКА", what); if (!ok) ++bad; };
    const std::string nm = "Bogestv0MixerTestI" + std::to_string(static_cast<unsigned long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007ull));
    std::string err;
    {
        auto shm = bg::Shm::open(nm, bg::Shm::Role::Engine, err);
        if (!shm) { std::printf("пропущено: нет общей памяти (%s)\n", err.c_str()); return 0; }
        const double target = bg::kInTargetFrames;
        bg::ShmRing ring0 = shm->in(0);

        InSim a;
        // Для сравнения: старое чтение (без запаса) на тех же данных. Само по себе дрожание оно переживает (запас накапливается
        // за счёт первых провалов), а расхождение часов нет: при медленном драйвере кольцо опустошается снова и снова,
        // при быстром запас растёт, и лишнее приходится выбрасывать.
        InSim lo = a; lo.clock = 0.999; lo.legacy = true;
        InSim hi = a; hi.clock = 1.001; hi.legacy = true;
        InSimRes old_lo = run_in_sim(ring0, lo), old_hi = run_in_sim(ring0, hi);
        std::printf("      (старое чтение, 60 с: драйвер медленнее на 0,1 %%: провалов %u; быстрее на 0,1 %%: щелчков %u)\n", old_lo.gaps, old_hi.clicks);
        check(old_lo.gaps > 0, "(проверка самой проверки) старое чтение без запаса при расхождении часов даёт провалы звука");

        InSimRes r = run_in_sim(ring0, a);
        check(r.gaps == 0 && r.clicks == 0 && r.under == 0 && r.trim == 0, "дрожание порций до 8 мс: ни провалов, ни щелчков, кольцо ни разу не опустело");
        check(std::fabs(r.fill_min - target) < 250 && std::fabs(r.fill_max - target) < 250, "запас держится у цели 25 мс");
        std::printf("      (запас %.0f…%.0f кадров, цель %.0f)\n", r.fill_min, r.fill_max, target);

        InSim a2 = a; a2.block = 441; a2.secs = 30;
        r = run_in_sim(ring0, a2);
        check(r.gaps == 0 && r.clicks == 0 && r.under == 0, "блок 441 кадр (не кратен порции): тоже чисто");

        InSim f = a; f.clock = 1.001;
        r = run_in_sim(ring0, f);
        check(r.gaps == 0 && r.clicks == 0 && r.under == 0 && r.trim == 0, "драйвер быстрее на 0,1 %: чисто, ничего не выброшено");
        check(std::fabs(r.fill_end - target) < 250 && std::abs(r.adj_end - 1000) < 300, "запас остался у цели, скорость чтения подстроилась на +0,1 %");
        std::printf("      (запас в конце %.0f, подстройка %d ppm)\n", r.fill_end, r.adj_end);

        InSim s = a; s.clock = 0.999;
        r = run_in_sim(ring0, s);
        check(r.gaps == 0 && r.clicks == 0 && r.under == 0, "драйвер медленнее на 0,1 %: чисто, кольцо не опустело");
        check(std::fabs(r.fill_end - target) < 250 && std::abs(r.adj_end + 1000) < 300, "запас остался у цели, скорость чтения подстроилась на −0,1 %");
        std::printf("      (запас в конце %.0f, подстройка %d ppm)\n", r.fill_end, r.adj_end);

        InSim t = a; t.stall_at = 10.0; t.stall_len = 0.1;
        r = run_in_sim(ring0, t);
        check(r.gaps == 1 && r.under == 1, "драйвер замолчал на 100 мс: одна тишина и один подсчёт, а не хрип на каждой порции");
        check(r.clicks <= 2 && std::fabs(r.fill_end - target) < 250, "после паузы звук продолжается чисто, запас вернулся к цели");
        std::printf("      (после паузы выброшено %u кадров, запас в конце %.0f)\n", r.trim, r.fill_end);

        // Открытие потока: старое выбрасывается, звук идёт только после набора запаса; закрытие и открытие считаются.
        bg::ShmRing rg = shm->in(1);
        bg::InReader rd;
        std::vector<float> big(2000 * 2, 0.25f), o(128 * 2, 9.0f);
        rg.push(big.data(), 2000);
        rd.open(rg);
        rd.read(rg, o.data(), 128);
        check(o[0] == 0.0f && o[255] == 0.0f && rg.available() == 0, "открытие потока: накопленное старое выброшено, пока тишина");
        rg.push(big.data(), 1000);
        rd.read(rg, o.data(), 128);
        check(o[0] == 0.0f && rd.under.load() == 0, "ждёт запас (1000 < 1200), это не считается нехваткой");
        rg.push(big.data(), 300);
        rd.read(rg, o.data(), 128);
        check(std::fabs(o[0] - 0.25f) < 1e-6f && std::fabs(o[255] - 0.25f) < 1e-6f, "запас набрался: звук пошёл");
        rd.closed(); rd.open(rg);
        check(rd.opens.load() == 2 && rd.closes.load() == 1, "счётчики открытий и закрытий потока");
    }
    bg::Shm::unlink_name(nm);

    // Те же проверки для внутренних колец движка (микрофоны, вторичные выходы): порции по 256 кадров, дрожание 1 мс,
    // запас ~10 мс, часы железа расходятся до ±0,05 % (USB-микрофон и звуковая карта).
    {
        auto mk = [](double clock, bool legacy) { InSim x; x.clock = clock; x.burst = 256; x.jitter_ms = 1.0; x.target = bg::kHwTargetFrames; x.legacy = legacy; return x; };
        const double tg = bg::kHwTargetFrames;
        bg::Ring ring;
        ring.init(8192);
        for (double clk : {0.9995, 1.0, 1.0005}) {
            ring.init(8192);
            const InSimRes n = run_in_sim(ring, mk(clk, false));
            ring.init(8192);
            const InSimRes o = run_in_sim(ring, mk(clk, true));
            std::printf("      (кольцо микрофона, часы %.4f: без запаса провалов %u, щелчков %u; с запасом провалов %u, щелчков %u, запас %.0f…%.0f, подстройка %d ppm)\n",
                        clk, o.gaps, o.clicks, n.gaps, n.clicks, n.fill_min, n.fill_max, n.adj_end);
            check(n.gaps == 0 && n.clicks == 0 && n.under == 0 && n.trim == 0, "внутреннее кольцо с запасом 10 мс: чисто при расхождении часов");
            check(std::fabs(n.fill_min - tg) < 200 && std::fabs(n.fill_max - tg) < 200, "запас внутреннего кольца держится у цели");
            if (clk < 1.0) check(o.gaps > 0, "(проверка самой проверки) старое чтение внутреннего кольца при медленном источнике даёт провалы");
        }
        // Счётчики позиций идут с переполнением uint32: кольцо должно работать и через границу.
        ring.init(8192);
        ring.r = 0xFFFFFFFFFFFFFF00ull; ring.w = 0xFFFFFFFFFFFFFF00ull;
        InSim x = mk(1.0, false); x.secs = 5;
        const InSimRes n = run_in_sim(ring, x);
        check(n.gaps == 0 && n.clicks == 0 && n.under == 0, "внутреннее кольцо работает через границу счётчика позиций");
    }

    // 0.5.3, часть 1: Windows открыла поток не на 48 кГц, драйвер частоту не пересчитывает. Кадры в кольце идут с частотой потока,
    // движок должен сам привести их к 48 кГц: тон 440 Гц остаётся 440 Гц (раньше он играл бы на 8 % ниже и с треском).
    {
        bg::Ring ring;
        auto mk = [](uint32_t hz, double clock) { InSim x; x.hz = hz; x.burst = hz / 100; x.jitter_ms = 4.0; x.clock = clock; x.secs = 30; return x; };
        for (double clk : {1.0, 0.9995, 1.0005}) {
            ring.init(8192);
            const InSimRes r = run_in_sim(ring, mk(44100, clk));
            std::printf("      (44 100 Гц, часы %.4f: провалов %u, щелчков %u, опустошений %u, тон %.2f Гц, подстройка %d ppm)\n", clk, r.gaps, r.clicks, r.under, r.freq, r.adj_end);
            check(r.gaps == 0 && r.clicks == 0 && r.under == 0, "поток 44 100 Гц: чисто, без провалов и щелчков");
            check(std::fabs(r.freq - 440.0) < 1.5, "поток 44 100 Гц: тон 440 Гц остался 440 Гц");
        }
        ring.init(8192);
        InSim a22 = mk(22050, 1.0);
        const InSimRes r22 = run_in_sim(ring, a22);
        check(r22.gaps == 0 && r22.clicks == 0 && std::fabs(r22.freq - 440.0) < 1.5, "поток 22 050 Гц (растягивается вдвое): чисто, тон на месте");
        ring.init(8192);
        InSim a96 = mk(96000, 1.0); a96.tone = 1000.0;
        const InSimRes r96 = run_in_sim(ring, a96);
        std::printf("      (96 000 Гц, тон 1000 Гц: провалов %u, щелчков %u, тон на выходе %.2f Гц)\n", r96.gaps, r96.clicks, r96.freq);
        check(r96.gaps == 0 && r96.under == 0 && std::fabs(r96.freq - 1000.0) < 3.0, "поток 96 000 Гц (сжимается вдвое): чисто, тон 1000 Гц на месте");

        // Частота потока поменялась на ходу (Windows закрыла и открыла поток иначе): читатель начинает заново, не падает и звук возвращается.
        ring.init(8192);
        bg::InReader rd;
        std::vector<float> src(480 * 2), o(128 * 2);
        auto feed = [&](uint32_t frames) { for (uint32_t i = 0; i < frames; ++i) src[i * 2] = src[i * 2 + 1] = 0.25f; ring.push(src.data(), frames); };
        rd.open(ring);
        for (int i = 0; i < 40; ++i) { feed(441); rd.read(ring, o.data(), 128, 44100); }
        const bool was = rd.src_hz.load() == 44100;
        for (int i = 0; i < 40; ++i) { feed(480); rd.read(ring, o.data(), 128, 48000); }
        check(was && rd.src_hz.load() == 48000 && std::fabs(o[0] - 0.25f) < 1e-4f && std::fabs(o[255] - 0.25f) < 1e-4f, "смена частоты потока на ходу: читатель перестроился, звук идёт");
    }

    // 0.5.3, часть 2: Windows отдаёт и забирает порции по 10 мс (480 кадров). Запас 10 мс (0.5.2) такими порциями опустошался сотнями раз;
    // теперь цель запаса сама подстраивается. Дрожание до ±14 мс (порция приходит раньше или позже чтения) — как у загруженной Windows.
    {
        bg::Ring ring;
        auto mk = [](double clock, double jit, bool adaptive) {
            InSim x; x.block = 480; x.burst = 480; x.jitter_ms = jit; x.clock = clock; x.target = bg::kHwTargetFrames; x.adaptive = adaptive; x.secs = 60; x.phase_ms = 0.0; x.sym = true; return x; };
        for (double jt : {2.0, 5.0, 9.0, 14.0}) {
            ring.init(8192); const InSimRes f = run_in_sim(ring, mk(1.0, jt, false));
            ring.init(8192); const InSimRes n = run_in_sim(ring, mk(1.0, jt, true));
            std::printf("      (порции по 10 мс, опоздание и опережение до ±%.0f мс, 60 с: запас 10 мс фиксированный: опустошений %u, провалов %u; адаптивный: опустошений %u, провалов %u, цель в конце %.1f мс, росла %u раз)\n", jt, f.under, f.gaps, n.under, n.gaps, n.tgt_end / 48.0, n.grown);
        }
        for (double clk : {1.0, 0.9995, 1.0005}) {
            ring.init(8192);
            const InSimRes f = run_in_sim(ring, mk(clk, 14.0, false));
            ring.init(8192);
            const InSimRes n = run_in_sim(ring, mk(clk, 14.0, true));
            std::printf("      (порции по 10 мс, часы %.4f: запас 10 мс фиксированный: опустошений %u, провалов %u; адаптивный: опустошений %u, провалов %u, цель %.1f мс, запас %.1f мс)\n",
                        clk, f.under, f.gaps, n.under, n.gaps, n.tgt_end / 48.0, n.fill_end / 48.0);
            check(f.under > 20, "(проверка самой проверки) фиксированный запас 10 мс при порциях по 10 мс опустошается много раз");
            check(n.under <= 3 && n.gaps <= 3 && n.clicks <= 3, "адаптивный запас: после подстройки не опустошается (не больше трёх случаев при разгоне)");
            check(n.tgt_end >= 480 && n.tgt_end <= 2400, "цель запаса выросла только до нужного, не до потолка");
        }
        // Спокойный источник: цель остаётся на базе, задержку без нужды не растим.
        ring.init(8192);
        InSim calm = mk(1.0, 0.0, true); calm.block = 128; calm.burst = 256; calm.phase_ms = 0.0; calm.secs = 30;
        const InSimRes c0 = run_in_sim(ring, calm);
        check(c0.under == 0 && c0.tgt_end == bg::kHwTargetFrames, "спокойный источник: цель осталась на базе 10 мс");
        // Большой блок читателя поднимает цель сразу: блок 1024 кадра при базе 512 не может работать без роста.
        ring.init(8192);
        InSim big = mk(1.0, 1.0, true); big.block = 1024; big.burst = 1024; big.phase_ms = 0.0; big.secs = 20;
        const InSimRes bg1 = run_in_sim(ring, big);
        check(bg1.tgt_end >= 1024 + 96 && bg1.gaps <= 2, "блок 1024 кадра: цель не ниже блока плюс 2 мс");
        // После всплеска дрожания цель возвращается к базе: задержка не остаётся большой навсегда.
        ring.init(8192);
        InSim spike = mk(1.0, 0.0, true); spike.block = 128; spike.burst = 480; spike.target = bg::kInTargetFrames; spike.stall_at = 5.0; spike.stall_len = 0.2; spike.secs = 60;
        const InSimRes sp = run_in_sim(ring, spike);
        std::printf("      (после паузы 200 мс: цель выросла %u раз, в конце цель %u кадров, база %u)\n", sp.grown, sp.tgt_end, bg::kInTargetFrames);
        check(sp.grown >= 1 && sp.tgt_end <= bg::kInTargetFrames + 96, "после одиночного сбоя цель запаса снова спускается к базе");
    }
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
            {   // состояние остаётся корректным JSON, в нём есть диагностика ведущего устройства (0.5.4)
                const std::string js = m.state_json();
                int depth = 0; bool instr = false, okj = true;
                for (size_t i = 0; i < js.size(); ++i) { const char c = js[i]; if (c == '"' && (i == 0 || js[i - 1] != '\\')) instr = !instr; if (instr) continue; if (c == '{' || c == '[') ++depth; else if (c == '}' || c == ']') { if (--depth < 0) okj = false; } }
                check(okj && depth == 0 && js.find("\"mst\":{\"calls\":") != std::string::npos && js.find("\"rb\":") != std::string::npos, "состояние: скобки сходятся, есть поле mst");
            }
            check(m.state_json().find("\"drv\":{\"shm\":1,\"via\":0,\"found\":0,\"alive\":0,\"in\":[0,0,0,0,0,0],\"out\":[0,0,0,0,0,0,0,0],\"diag\":{\"rate\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"starved\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"drops\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"trim\":[0,0,0,0,0,0],\"fill\":[0,0,0,0,0,0],\"under\":[0,0,0,0,0,0],\"tgt\":[1200,1200,1200,1200,1200,1200],\"adj\":[0,0,0,0,0,0],\"opens\":[0,0,0,0,0,0],\"closes\":[0,0,0,0,0,0],\"streams\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"peak\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0]},\"hw\":{\"fill\":[0,0,0,0,0,0,0],\"under\":[0,0,0,0,0,0,0],\"tgt\":[576,576,512,512,512,512,512],\"adj\":[0,0,0,0,0,0,0]},\"mst\":{\"calls\":") != std::string::npos, "состояние: драйвера пока нет, диагностика колец нулевая (цель микрофонов 576: блок ведущего 480 + 96)");

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
            std::this_thread::sleep_for(std::chrono::milliseconds(100));   // дорожка снова берёт звук драйвера (считается новым открытием)

            // Драйвер закрыл поток: дорожка замолкает, движок в Output 1 больше не пишет.
            in2.set_active(false);
            out1.set_active(false);
            tap2.set_active(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint32_t w_before = out1.ctl->write_pos.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            check(out1.ctl->write_pos.load() == w_before, "пока Output 1 не открыт драйвером, движок в него не пишет");
            check(m.state_json().find("\"opens\":[0,2,0,0,0,0],\"closes\":[0,1,0,0,0,0]") != std::string::npos, "диагностика: Input 2 открывался дважды (второй раз после тест-источника), закрыт один раз");
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

// ---------- проверка эквалайзера, задержки и аудио-параметров (этап 5) ----------
static float eq_gain_db(bg::Eq& e, float hz) {
    // Синус через фильтр, после прогрева меряем размах.
    const size_t n = 48000;
    std::vector<float> x(n * 2);
    for (size_t i = 0; i < n; ++i) x[i * 2] = x[i * 2 + 1] = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * i / 48000.0));
    e.process(x.data(), n);
    float peak = 0.0f;
    for (size_t i = n / 2; i < n; ++i) peak = std::max(peak, std::fabs(x[i * 2]));
    return bg::lin_to_db(peak / 0.25f);
}

static int selftest_eq() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok    " : "ОШИБКА", what); if (!ok) ++bad; };
    {   // выключенный эквалайзер не меняет звук ни на бит
        bg::Eq e;
        std::vector<float> x(512 * 2), y;
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * static_cast<float>(std::sin(0.01 * i));
        y = x;
        e.process(y.data(), 512);
        check(x == y, "бас/ВЧ: на 0 дБ звук проходит без изменений");
    }
    {
        bg::Eq e; e.bass_db = 12.0f;
        const float lo = eq_gain_db(e, 40.0f);
        bg::Eq e2; e2.bass_db = 12.0f;
        const float mid = eq_gain_db(e2, 2000.0f);
        check(lo > 9.0f && lo < 12.6f, "бас +12 дБ: на 40 Гц подъём около +10…12 дБ");
        check(std::fabs(mid) < 0.5f, "бас +12 дБ: на 2 кГц звук не меняется");
    }
    {
        bg::Eq e; e.bass_db = -12.0f;
        const float lo = eq_gain_db(e, 40.0f);
        check(lo < -9.0f && lo > -12.6f, "бас −12 дБ: на 40 Гц спад около −10…−12 дБ");
    }
    {
        bg::Eq e; e.treble_db = 12.0f;
        const float hi = eq_gain_db(e, 16000.0f);
        bg::Eq e2; e2.treble_db = 12.0f;
        const float mid = eq_gain_db(e2, 200.0f);
        check(hi > 9.0f && hi < 12.6f, "ВЧ +12 дБ: на 16 кГц подъём около +10…12 дБ");
        check(std::fabs(mid) < 0.5f, "ВЧ +12 дБ: на 200 Гц звук не меняется");
    }
    {   // включение и выключение на ходу не даёт скачков и «залипшего» звука
        bg::Eq e; e.bass_db = 9.0f;
        std::vector<float> x(4800 * 2, 0.0f);
        e.process(x.data(), 4800);
        e.bass_db = 0.0f;
        std::vector<float> y(480 * 2);
        for (size_t i = 0; i < y.size(); ++i) y[i] = 0.2f * static_cast<float>(std::sin(0.05 * i));
        auto z = y;
        e.process(y.data(), 480);
        check(y == z, "бас: после возврата на 0 дБ обработка снова пропускается целиком");
        bool fin = true;
        bg::Eq e3; e3.bass_db = 12.0f; e3.treble_db = -12.0f;
        std::vector<float> w(48000 * 2);
        uint32_t r = 7;
        for (float& v : w) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; v = (static_cast<float>(r & 0xFFFF) / 32768.0f - 1.0f) * 0.5f; }
        e3.process(w.data(), 48000);
        for (float v : w) if (!std::isfinite(v) || std::fabs(v) > 8.0f) fin = false;
        check(fin, "бас +12 и ВЧ −12 на шуме: числа конечны, без взрыва");
    }
    {   // задержка выхода
        bg::DelayLine d;
        std::vector<float> x(2000 * 2, 0.0f);
        x[0] = 1.0f; x[1] = -1.0f;
        d.frames = 480;
        d.process(x.data(), 1000);
        d.process(x.data() + 2000, 1000);
        bool at = x[480 * 2] == 1.0f && x[480 * 2 + 1] == -1.0f, other = true;
        for (size_t i = 0; i < 2000; ++i) if (i != 480 && (x[i * 2] != 0.0f || x[i * 2 + 1] != 0.0f)) other = false;
        check(at && other, "задержка 10 мс: импульс сдвинулся ровно на 480 кадров, остальное тишина");
        d.frames = 0;
        std::vector<float> y(100 * 2, 0.5f);
        d.process(y.data(), 100);
        bool same = true;
        for (float v : y) same = same && v == 0.5f;
        check(same, "задержка 0 мс: звук идёт напрямую");
    }
    {   // команды и состояние
        ma_backend nb[] = {ma_backend_null};
        ma_context ctx;
        ma_context_config cc = ma_context_config_init();
        if (ma_context_init(nb, 1, &cc, &ctx) != MA_SUCCESS) { std::printf("пропущено: нулевой звуковой бэкенд недоступен\n"); return bad; }
        {
            bg::Mixer m(&ctx, nullptr);
            check(m.command("set strip 3 bass 6").empty() && m.command("set strip 3 treble -4.5").empty(), "команды set strip bass/treble приняты");
            check(m.command("set strip 3 bass 99").empty() && m.strips[3].eq.bass_db.load() == 12.0f, "бас зажат до +12 дБ");
            check(m.command("set strip 3 treble -99").empty() && m.strips[3].eq.treble_db.load() == -12.0f, "ВЧ зажат до −12 дБ");
            check(!m.command("set strip 3 bass abc").empty() && !m.command("set strip 99 bass 1").empty(), "неверные значения отклоняются");
            check(m.command("set bus 1 bass 3").empty() && m.buses[1].eq.bass_db.load() == 3.0f, "команда set bus bass");
            check(m.command("set audio limiter 0").empty() && m.state_json().find("\"lim\":0") != std::string::npos, "лимитер выключается, состояние это показывает");
            check(m.command("set audio limiter 1").empty() && m.state_json().find("\"lim\":1") != std::string::npos, "лимитер включается обратно");
            check(m.command("set audio delay 1 12.5").empty() && m.buses[1].delay.frames.load() == 600, "задержка A2 12,5 мс = 600 кадров");
            check(m.command("set audio delay 9 5").size() > 0 && m.command("set audio delay 0 9999").empty() && m.buses[0].delay.frames.load() == 24000, "задержка: неверный выход отклонён, предел 500 мс");
            check(m.command("set audio buffer 2000").empty() && m.state_json().find("\"buf\":1024") != std::string::npos, "буфер зажат до 1024 кадров");
            check(m.command("set audio reconnect 1").empty() && m.state_json().find("\"rc\":1") != std::string::npos, "режим переподключения хранится");
            check(!m.command("set audio nonsense 1").empty(), "неизвестное аудио-свойство отклонено");
            const std::string js = m.state_json();
            int depth = 0; bool instr = false, okj = true;
            for (size_t i = 0; i < js.size(); ++i) { const char c = js[i]; if (c == '"' && (i == 0 || js[i - 1] != '\\')) instr = !instr; if (instr) continue; if (c == '{' || c == '[') ++depth; else if (c == '}' || c == ']') { if (--depth < 0) okj = false; } }
            check(okj && depth == 0 && js.find("\"eq\":{\"s\":[[") != std::string::npos && js.find("\"info\":{\"hw1\":null") != std::string::npos, "состояние: скобки сходятся, есть eq, aud и info");
        }
        ma_context_uninit(&ctx);
    }
    return bad;
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
    bad += selftest_inbuf();
    bad += selftest_driver_mixer();
    bad += selftest_eq();
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
