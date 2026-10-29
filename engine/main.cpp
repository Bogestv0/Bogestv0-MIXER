// Bogestv0 MIXER — движок v0 (этап 2): микрофон -> программное усиление -> наушники.
// Консольная программа без драйвера. Звук через WASAPI (miniaudio).
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "third_party/miniaudio.h"

#include "dsp.h"

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
#endif

static const char* VERSION = "0.0.2";

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
        "  engine.exe --in N --out M [опции]    микрофон N -> наушники M\n\n"
        "Опции:\n"
        "  --gain ДБ      усиление 0..30 (по умолчанию 0)\n"
        "  --period N     размер блока: 128, 256, 512 (по умолчанию 128)\n"
        "  --rate Гц      44100 или 48000 (по умолчанию 48000)\n"
        "  --exclusive    эксклюзивный режим микрофона (меньше задержка)\n"
        "  --selftest     проверка обработки без звуковых устройств\n\n"
        "Клавиши: + / - усиление, q выход.\n", VERSION);
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
    bool list = false, exclusive = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "Не хватает значения для %s\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "--list") list = true;
        else if (a == "--in") in_idx = std::atoi(next("--in"));
        else if (a == "--out") out_idx = std::atoi(next("--out"));
        else if (a == "--gain") gain_db = static_cast<float>(std::atof(next("--gain")));
        else if (a == "--period") period = std::atoi(next("--period"));
        else if (a == "--rate") rate = std::atoi(next("--rate"));
        else if (a == "--exclusive") exclusive = true;
        else if (a == "--selftest") return selftest();
        else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }
    if (!list && (in_idx < 0 || out_idx < 0)) { usage(); return 2; }
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

    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.sampleRate = static_cast<ma_uint32>(rate);
    cfg.periodSizeInFrames = static_cast<ma_uint32>(period);
    cfg.periods = 2;
    cfg.performanceProfile = ma_performance_profile_low_latency;
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
    cfg.wasapi.noAutoConvertSRC = MA_FALSE;  // ресемплинг делает система, если устройство на другой частоте

    ma_device dev;
    if (ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS) {
        std::fprintf(stderr, "Не удалось открыть устройства. Для --exclusive закройте программы, занявшие микрофон.\n");
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
                exclusive ? "эксклюзивный микрофон" : "общий режим");
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
