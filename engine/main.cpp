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

static const char* VERSION = "0.2.0";

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
        "  --selftest     проверка обработки без звуковых устройств\n\n"
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
