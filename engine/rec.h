// Запись выходов в WAV (этап 7): 24 бита, стерео, 48 кГц.
// Аудио-поток только кладёт готовый звук шины в кольцо (feed). Файлы пишет отдельный поток, поэтому диск не может подвесить звук.
// Режимы: раздельно (файл на каждую выбранную шину) или один общий (сумма выбранных шин).
#pragma once

#include "pad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bg {

constexpr int kRecBuses = 5;                    // A1 A2 A3 B1 B2
constexpr size_t kRecRingFrames = 1u << 17;     // ~2,7 с запаса на случай, если диск замешкался
constexpr uint64_t kWavMaxData = 3900000000ull; // WAV держит размер в 32 битах: у предела останавливаем запись

// Запись WAV 24 бита стерео. Размеры в заголовке обновляются раз в секунду: если программа упадёт, файл всё равно откроется.
struct WavWriter {
    FILE* f = nullptr;
    uint64_t bytes = 0;     // размер данных
    uint64_t frames = 0;

    static void put32(unsigned char* p, uint32_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
    static void put16(unsigned char* p, uint16_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }

    bool open(const std::string& path) {
        f = fopen_u8(path, "wb");
        if (!f) return false;
        bytes = 0; frames = 0;
        write_header();
        return true;
    }
    void write_header() {
        unsigned char h[44];
        std::memcpy(h, "RIFF", 4); put32(h + 4, static_cast<uint32_t>(36 + bytes));
        std::memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2);
        put32(h + 24, 48000); put32(h + 28, 48000 * 6); put16(h + 32, 6); put16(h + 34, 24);
        std::memcpy(h + 36, "data", 4); put32(h + 40, static_cast<uint32_t>(bytes));
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(h, 1, 44, f);
        std::fseek(f, 0, SEEK_END);
    }
    // x: стерео float; значения за ±1 обрезаются.
    bool write(const float* x, size_t n) {
        if (!f) return false;
        if (bytes + n * 6 > kWavMaxData) return false;
        std::vector<unsigned char> b(n * 6);
        for (size_t i = 0; i < n * 2; ++i) {
            const float c = std::max(-1.0f, std::min(1.0f, x[i]));
            const int32_t v = static_cast<int32_t>(std::lrintf(c * 8388607.0f));
            b[i * 3] = v & 255; b[i * 3 + 1] = (v >> 8) & 255; b[i * 3 + 2] = (v >> 16) & 255;
        }
        if (std::fwrite(b.data(), 1, b.size(), f) != b.size()) return false;
        bytes += b.size(); frames += n;
        return true;
    }
    void close() {
        if (!f) return;
        write_header();
        std::fclose(f);
        f = nullptr;
    }
};

class Recorder {
public:
    Recorder() { for (auto& r : rings_) r.buf.assign(kRecRingFrames * 2, 0.0f); }
    ~Recorder() { stop(); }

    bool active() const { return active_.load(); }
    // Поток записи закончился сам (ошибка диска), но ещё не прибран: вызвать stop().
    bool needs_reap() const { return !active_.load() && th_.joinable(); }
    double seconds() const { return static_cast<double>(frames_.load()) / 48000.0; }
    uint32_t dropped() const { return dropped_.load(); }
    std::string last_error() const { std::lock_guard<std::mutex> lk(mu_); return err_; }
    std::vector<std::string> files() const { std::lock_guard<std::mutex> lk(mu_); return paths_; }

    // Аудио-поток: готовый звук шины bus (после лимитера). Если шина не записывается, почти ничего не стоит.
    void feed(int bus, const float* a, size_t n) {
        if (!(mask_.load(std::memory_order_relaxed) & (1 << bus))) return;
        Ring& r = rings_[bus];
        const uint64_t w = r.w.load(std::memory_order_relaxed), rd = r.r.load(std::memory_order_acquire);
        if (kRecRingFrames - (w - rd) < n) { dropped_.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed); r.lost.store(true); return; }
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = static_cast<size_t>((w + i) & (kRecRingFrames - 1)) * 2;
            r.buf[idx] = a[i * 2]; r.buf[idx + 1] = a[i * 2 + 1];
        }
        r.w.store(w + n, std::memory_order_release);
    }

    // mask: биты шин A1..B2; mixed: один общий файл. dir: папка (уже существует); base: начало имени файла (без расширения).
    // Возвращает пустую строку или текст ошибки.
    std::string start(int mask, bool mixed, const std::string& dir, const std::string& base) {
        stop();
        mask &= (1 << kRecBuses) - 1;
        if (!mask) return "запись: выберите хотя бы один выход";
        static const char* tags[kRecBuses] = {"A1", "A2", "A3", "B1", "B2"};
        std::vector<std::string> paths;
        std::vector<int> owner;   // какой шине принадлежит файл (-1 = общий)
        const std::string sep = dir.empty() || dir.back() == '/' || dir.back() == '\\' ? "" : "/";
        if (mixed) { paths.push_back(dir + sep + base + " · общий.wav"); owner.push_back(-1); }
        else for (int k = 0; k < kRecBuses; ++k) if (mask & (1 << k)) { paths.push_back(dir + sep + base + " · " + tags[k] + ".wav"); owner.push_back(k); }
        std::vector<WavWriter> w(paths.size());
        for (size_t i = 0; i < paths.size(); ++i) {
            if (!w[i].open(paths[i])) {
                for (auto& x : w) x.close();
                return "не удалось создать файл: " + paths[i] + " (проверьте папку записи)";
            }
        }
        for (auto& r : rings_) { r.r.store(r.w.load()); r.lost.store(false); }
        { std::lock_guard<std::mutex> lk(mu_); paths_ = paths; err_.clear(); }
        owner_ = owner; mixed_ = mixed; frames_ = 0; dropped_ = 0;
        writers_ = std::move(w);
        run_mask_ = mask;
        stopping_ = false;
        active_ = true;
        mask_.store(mask);   // с этого момента аудио-поток начинает класть звук в кольца
        th_ = std::thread([this] { loop(); });
        return "";
    }

    void stop() {
        if (!active_.load() && !th_.joinable()) return;
        mask_.store(0);
        stopping_ = true;
        if (th_.joinable()) th_.join();
        for (auto& x : writers_) x.close();
        writers_.clear();
        active_ = false;
    }

private:
    struct Ring {
        std::vector<float> buf;
        std::atomic<uint64_t> w{0}, r{0};
        std::atomic<bool> lost{false};
    };
    Ring rings_[kRecBuses];
    std::atomic<int> mask_{0};
    std::atomic<bool> active_{false}, stopping_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint32_t> dropped_{0};
    mutable std::mutex mu_;
    std::string err_;
    std::vector<std::string> paths_;
    std::vector<WavWriter> writers_;
    std::vector<int> owner_;
    bool mixed_ = false;
    int run_mask_ = 0;
    std::thread th_;

    void fail(const std::string& m) { std::lock_guard<std::mutex> lk(mu_); if (err_.empty()) err_ = m; }

    // Забирает из колец всё, что накопилось, и пишет в файлы. Возвращает, сколько кадров записано (для общего файла: общих).
    void drain() {
        std::vector<float> tmp(4800 * 2), mixbuf(4800 * 2);
        for (int guard = 0; guard < 64; ++guard) {
            bool any = false;
            if (mixed_) {
                uint64_t avail = ~0ull;
                for (int k = 0; k < kRecBuses; ++k) if (run_mask_ & (1 << k)) avail = std::min<uint64_t>(avail, rings_[k].w.load(std::memory_order_acquire) - rings_[k].r.load(std::memory_order_relaxed));
                const size_t n = static_cast<size_t>(std::min<uint64_t>(avail, 4800));
                if (n) {
                    std::fill(mixbuf.begin(), mixbuf.begin() + n * 2, 0.0f);
                    for (int k = 0; k < kRecBuses; ++k) {
                        if (!(run_mask_ & (1 << k))) continue;
                        Ring& r = rings_[k];
                        const uint64_t rd = r.r.load(std::memory_order_relaxed);
                        for (size_t i = 0; i < n; ++i) { const size_t idx = static_cast<size_t>((rd + i) & (kRecRingFrames - 1)) * 2; mixbuf[i * 2] += r.buf[idx]; mixbuf[i * 2 + 1] += r.buf[idx + 1]; }
                        r.r.store(rd + n, std::memory_order_release);
                    }
                    if (!writers_[0].write(mixbuf.data(), n)) { fail("запись остановлена: диск не принимает данные или файл достиг предела"); mask_.store(0); stopping_ = true; return; }
                    frames_.fetch_add(n);
                    any = true;
                }
            } else {
                for (size_t i = 0; i < writers_.size(); ++i) {
                    Ring& r = rings_[owner_[i]];
                    const uint64_t rd = r.r.load(std::memory_order_relaxed), avail = r.w.load(std::memory_order_acquire) - rd;
                    const size_t n = static_cast<size_t>(std::min<uint64_t>(avail, 4800));
                    if (!n) continue;
                    for (size_t j = 0; j < n; ++j) { const size_t idx = static_cast<size_t>((rd + j) & (kRecRingFrames - 1)) * 2; tmp[j * 2] = r.buf[idx]; tmp[j * 2 + 1] = r.buf[idx + 1]; }
                    r.r.store(rd + n, std::memory_order_release);
                    if (!writers_[i].write(tmp.data(), n)) { fail("запись остановлена: диск не принимает данные или файл достиг предела"); mask_.store(0); stopping_ = true; return; }
                    if (i == 0) frames_.fetch_add(n);
                    any = true;
                }
            }
            if (!any) break;
        }
    }

    void loop() {
        auto last_hdr = std::chrono::steady_clock::now();
        while (!stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            drain();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_hdr > std::chrono::seconds(1)) {
                for (auto& x : writers_) { x.write_header(); std::fflush(x.f); }
                last_hdr = now;
            }
        }
        drain();   // остаток после остановки
        for (auto& x : writers_) x.close();   // файлы закрываются здесь, даже если запись остановилась сама (ошибка диска)
        active_ = false;
    }
};

}  // namespace bg
