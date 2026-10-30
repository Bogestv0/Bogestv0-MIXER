// Обработка микрофона (этап 4): фильтр низких -> усиление -> шумоподавление (RNNoise) -> гейт -> компрессор -> ограничитель.
// Всё работает в аудио-потоке, поэтому здесь нет выделения памяти, блокировок и печати.
// Память под нейросеть и буферы выделяется заранее, в конструкторе.
#pragma once

#include "dsp.h"
#include "third_party/rnnoise/include/rnnoise.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace bg {

constexpr float kPi = 3.14159265358979f;
constexpr float kRate = 48000.0f;

// Коэффициент сглаживания для заданного времени (в секундах).
inline float smooth_coef(float seconds) { return 1.0f - std::exp(-1.0f / (seconds * kRate)); }

// ---------- фильтр низких частот ----------
// Один квадратичный фильтр (стерео). Два подряд с разной добротностью дают фильтр Баттерворта 4-го порядка (24 дБ на октаву).
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};

    void highpass(float fc, float q) {
        const float w = 2.0f * kPi * fc / kRate, c = std::cos(w), s = std::sin(w), alpha = s / (2.0f * q);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f + c) / 2.0f / a0;
        b1 = -(1.0f + c) / a0;
        b2 = (1.0f + c) / 2.0f / a0;
        a1 = -2.0f * c / a0;
        a2 = (1.0f - alpha) / a0;
    }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }
    void process(float* x, size_t n) {
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                const float in = x[i * 2 + c];
                const float out = b0 * in + z1[c];
                z1[c] = b1 * in - a1 * out + z2[c];
                z2[c] = b2 * in - a2 * out;
                x[i * 2 + c] = out;
            }
        // На тишине состояние фильтра затухает до денормализованных чисел, а на них процессор замедляется.
        for (int c = 0; c < 2; ++c) {
            if (std::fabs(z1[c]) < 1e-15f) z1[c] = 0.0f;
            if (std::fabs(z2[c]) < 1e-15f) z2[c] = 0.0f;
        }
    }
};

// ---------- оценка шума в паузах ----------
// Берёт самый тихий отрезок за последние ~3 секунды: это и есть фон в паузах между словами.
struct FloorTracker {
    static constexpr int kBins = 30;                 // 30 отрезков по 100 мс
    static constexpr size_t kBinLen = 4800;
    double acc = 0.0;
    size_t cnt = 0;
    float bins[kBins];
    int bi = 0, nb = 0;
    std::atomic<float> floor_db{-120.0f};

    FloorTracker() { for (float& b : bins) b = 0.0f; }
    void reset() { acc = 0.0; cnt = 0; bi = 0; nb = 0; floor_db.store(-120.0f, std::memory_order_relaxed); }
    void feed(const float* x, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const float l = x[i * 2], r = x[i * 2 + 1];
            acc += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
            if (++cnt >= kBinLen) {
                const float db = 10.0f * std::log10(static_cast<float>(acc / static_cast<double>(cnt)) + 1e-12f);
                bins[bi] = db;
                bi = (bi + 1) % kBins;
                if (nb < kBins) ++nb;
                float m = bins[0];
                for (int k = 1; k < nb; ++k) m = std::min(m, bins[k]);
                floor_db.store(m, std::memory_order_relaxed);
                acc = 0.0;
                cnt = 0;
            }
        }
    }
};

// ---------- шумоподавление ----------
// RNNoise работает кадрами по 480 отсчётов (10 мс, 48 кГц, моно). Накопление кадра даёт задержку 10 мс.
// Исходный звук проходит через такую же задержку, чтобы «сухой» и «обработанный» сигналы не гребенчатили при смешивании.
// Режимы: 1 «Мягкое», 2 «Сильное», 3 «Максимум». Сила (0..1) — доля обработанного звука в смеси.
// В «Сильном» и «Максимуме» к тому же приглушаются кадры, где нейросеть не слышит голоса.
class Denoiser {
public:
    static constexpr size_t kFrame = 480;
    static constexpr size_t kCap = 8192;       // степень двойки; больше, чем 480 + самый большой блок
    static constexpr size_t kMask = kCap - 1;

    Denoiser() : st_(rnnoise_create(nullptr)), dry_(kCap * 2, 0.0f), wet_(kCap, 0.0f), vad_(kCap, 1.0f) {
        std::memset(in_, 0, sizeof in_);
        for (int m = 0; m < 4; ++m) att_[m] = 0.0f;
        att_[2] = 6.0f;    // дБ приглушения «пауз» в режиме «Сильное»
        att_[3] = 20.0f;   // и в режиме «Максимум»
    }
    ~Denoiser() { if (st_) rnnoise_destroy(st_); }
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    bool running() const { return running_; }
    void stop() { running_ = false; mix_ = 0.0f; }

    // io: стерео, на месте. mode 0..3, w 0..1.
    void process(float* io, size_t n, int mode, float w) {
        if (!running_) {
            if (mode <= 0) return;
            start();
        }
        mode = std::max(0, std::min(3, mode));
        w = std::max(0.0f, std::min(1.0f, w));
        const float mix_target = mode > 0 ? 1.0f : 0.0f;
        const float mix_step = mode > 0 ? 1.0f / 1440.0f : 1.0f / 480.0f;   // включение 30 мс, выключение 10 мс
        const float att = att_[mode];

        // 1) Накопление кадров и нейросеть.
        for (size_t i = 0; i < n; ++i) {
            const float l = io[i * 2], r = io[i * 2 + 1];
            const size_t di = (dwr_ & kMask) * 2;
            dry_[di] = l;
            dry_[di + 1] = r;
            ++dwr_;
            float m = (l + r) * 0.5f * 32768.0f;
            in_[nin_++] = std::max(-32768.0f, std::min(32767.0f, m));
            if (nin_ == kFrame) {
                float out[kFrame];
                const float vad = rnnoise_process_frame(st_, out, in_);
                for (size_t k = 0; k < kFrame; ++k) {
                    const size_t wi = (wwr_ + k) & kMask;
                    wet_[wi] = out[k] * (1.0f / 32768.0f);
                    vad_[wi] = vad;
                }
                wwr_ += kFrame;
                nin_ = 0;
            }
        }

        // 2) Смесь. Отсчёт rd_ отстаёт от входа ровно на один кадр.
        const float a_fast = smooth_coef(0.003f), a_slow = smooth_coef(0.060f);
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = rd_ & kMask;
            ++rd_;
            const float vad = vad_[idx];
            if (vad != last_vad_ || att != last_att_) {
                last_vad_ = vad;
                last_att_ = att;
                const float k = std::max(0.0f, std::min(1.0f, (0.6f - vad) / 0.4f));   // 0 = голос, 1 = точно не голос
                vg_target_ = att > 0.0f ? db_to_lin(-att * k) : 1.0f;
            }
            vg_ += (vg_target_ - vg_) * (vg_target_ < vg_ ? a_slow : a_fast);

            const float wet = wet_[idx] * w;
            const float pl = (dry_[idx * 2] * (1.0f - w) + wet) * vg_;
            const float pr = (dry_[idx * 2 + 1] * (1.0f - w) + wet) * vg_;
            if (mix_ < mix_target) mix_ = std::min(mix_target, mix_ + mix_step);
            else if (mix_ > mix_target) mix_ = std::max(mix_target, mix_ - mix_step);
            io[i * 2] = io[i * 2] * (1.0f - mix_) + pl * mix_;
            io[i * 2 + 1] = io[i * 2 + 1] * (1.0f - mix_) + pr * mix_;
        }
        if (mode == 0 && mix_ <= 0.0f) running_ = false;
    }

private:
    void start() {
        std::fill(dry_.begin(), dry_.end(), 0.0f);
        std::fill(wet_.begin(), wet_.end(), 0.0f);
        std::fill(vad_.begin(), vad_.end(), 1.0f);
        nin_ = 0;
        rd_ = 0;
        dwr_ = wwr_ = kFrame;
        mix_ = 0.0f;
        vg_ = vg_target_ = 1.0f;
        last_vad_ = -1.0f;
        last_att_ = -1.0f;
        running_ = true;
    }

    DenoiseState* st_;
    std::vector<float> dry_, wet_, vad_;
    float in_[kFrame];
    float att_[4];
    size_t nin_ = 0, rd_ = 0, dwr_ = kFrame, wwr_ = kFrame;
    float mix_ = 0.0f, vg_ = 1.0f, vg_target_ = 1.0f, last_vad_ = -1.0f, last_att_ = -1.0f;
    bool running_ = false;
};

// ---------- гейт ----------
// Закрывает микрофон, когда сигнал тише порога. Открывается быстро, закрывается плавно, после слова держится ~120 мс.
struct Gate {
    float env = 0.0f, gain = 1.0f;
    int hold = 0;
    bool open = true;
    void reset() { env = 0.0f; gain = 1.0f; hold = 0; open = true; }

    // thr_db <= -79: гейт выключен.
    void process(float* x, size_t n, float thr_db) {
        const bool off = thr_db <= -79.0f;
        const float thr = db_to_lin(thr_db), close_thr = thr * 0.63f;   // гистерезис 4 дБ
        const float env_rel = std::exp(-1.0f / (0.010f * kRate));
        const float a_att = smooth_coef(0.0005f), a_rel = smooth_coef(0.080f);
        const float floor_gain = 0.001f;                                 // -60 дБ
        const int hold_len = static_cast<int>(0.120f * kRate);
        for (size_t i = 0; i < n; ++i) {
            const float a = std::max(std::fabs(x[i * 2]), std::fabs(x[i * 2 + 1]));
            env = a > env ? a : env * env_rel;
            if (off) open = true;
            else if (env > thr) { open = true; hold = hold_len; }
            else if (env < close_thr) { if (hold > 0) --hold; else open = false; }
            const float target = open ? 1.0f : floor_gain;
            gain += (target - gain) * (target > gain ? a_att : a_rel);
            if (gain != 1.0f) { x[i * 2] *= gain; x[i * 2 + 1] *= gain; }
        }
        if (std::fabs(gain - 1.0f) < 1e-7f && open) gain = 1.0f;
    }
};

// ---------- компрессор ----------
// Порог −20 дБFS, мягкое колено 6 дБ, атака 8 мс, спад 150 мс. Выравнивание громкости подбирается автоматически
// (половина «потерянной» громкости возвращается), перегрузку в конце ловит ограничитель.
struct Compressor {
    static constexpr float kThr = -20.0f, kKnee = 6.0f;
    float gr_db = 0.0f, makeup_db = 0.0f;
    void reset() { gr_db = 0.0f; makeup_db = 0.0f; }

    void process(float* x, size_t n, float ratio) {
        ratio = std::max(1.0f, ratio);
        const float inv = 1.0f / ratio;
        const float makeup_t = ratio > 1.04f ? 0.5f * (-kThr) * (1.0f - inv) : 0.0f;
        const float a_att = smooth_coef(0.008f), a_rel = smooth_coef(0.150f), a_mk = smooth_coef(0.050f);
        if (ratio <= 1.04f && std::fabs(gr_db) < 1e-4f && std::fabs(makeup_db) < 1e-4f) { gr_db = 0.0f; makeup_db = 0.0f; return; }
        for (size_t i = 0; i < n; ++i) {
            const float lvl = std::max(std::fabs(x[i * 2]), std::fabs(x[i * 2 + 1]));
            const float lvl_db = 20.0f * std::log10(lvl + 1e-9f);
            const float over = lvl_db - kThr;
            float out_db;
            if (2.0f * over < -kKnee) out_db = lvl_db;
            else if (2.0f * std::fabs(over) <= kKnee) { const float t = over + kKnee * 0.5f; out_db = lvl_db + (inv - 1.0f) * t * t / (2.0f * kKnee); }
            else out_db = kThr + over * inv;
            const float target = out_db - lvl_db;   // <= 0
            gr_db += (target - gr_db) * (target < gr_db ? a_att : a_rel);
            makeup_db += (makeup_t - makeup_db) * a_mk;
            const float g = db_to_lin(gr_db + makeup_db);
            x[i * 2] *= g;
            x[i * 2 + 1] *= g;
        }
    }
};

// ---------- параметры и вся цепочка ----------
struct MicParams {
    std::atomic<float> gain_db{0.0f};       // 0..30
    std::atomic<float> gate_db{-80.0f};     // -80..-20, -80 = выкл
    std::atomic<float> hpf_hz{0.0f};        // 20..300, <25 = выкл
    std::atomic<float> comp_ratio{1.0f};    // 1..10, 1 = выкл
    std::atomic<float> ns_amt{100.0f};      // 0..100 %
    std::atomic<int> ns_mode{0};            // 0 выкл, 1 мягкое, 2 сильное, 3 максимум
};

class MicChain {
public:
    MicParams p;
    FloorTracker raw_floor;    // фон до подавления (после фильтра и усиления)
    FloorTracker proc_floor;   // фон после подавления и гейта

    // Только пока аудио остановлено.
    void reset() {
        hp_[0].reset(); hp_[1].reset();
        dn_.stop();
        gate_.reset(); comp_.reset();
        raw_floor.reset(); proc_floor.reset();
        cur_gain_ = 1.0f;
        cur_hpf_ = -1.0f;
    }

    // x: стерео, на месте, n <= 4096.
    void process(float* x, size_t n) {
        const float hpf = p.hpf_hz.load(std::memory_order_relaxed);
        if (hpf != cur_hpf_) {
            cur_hpf_ = hpf;
            if (hpf >= 25.0f) { const float f = std::min(hpf, 400.0f); hp_[0].highpass(f, 0.5412f); hp_[1].highpass(f, 1.3066f); }
        }
        if (hpf >= 25.0f) { hp_[0].process(x, n); hp_[1].process(x, n); }

        const float target = db_to_lin(std::max(0.0f, std::min(30.0f, p.gain_db.load(std::memory_order_relaxed))));
        if (cur_gain_ != target) {
            const float step = (target - cur_gain_) / static_cast<float>(n);
            float g = cur_gain_;
            for (size_t i = 0; i < n; ++i) { g += step; x[i * 2] *= g; x[i * 2 + 1] *= g; }
            cur_gain_ = target;
        } else if (target != 1.0f) {
            for (size_t i = 0; i < n * 2; ++i) x[i] *= target;
        }
        raw_floor.feed(x, n);

        dn_.process(x, n, p.ns_mode.load(std::memory_order_relaxed), p.ns_amt.load(std::memory_order_relaxed) * 0.01f);
        gate_.process(x, n, p.gate_db.load(std::memory_order_relaxed));
        proc_floor.feed(x, n);

        comp_.process(x, n, p.comp_ratio.load(std::memory_order_relaxed));
        for (size_t i = 0; i < n * 2; ++i) x[i] = soft_limit(x[i]);   // защита от перегрузки при большом усилении
    }

private:
    Biquad hp_[2];
    Denoiser dn_;
    Gate gate_;
    Compressor comp_;
    float cur_gain_ = 1.0f;
    float cur_hpf_ = -1.0f;
};

}  // namespace bg
