// Эквалайзер «Бас / ВЧ» для дорожки и шины и линия задержки выхода (этап 5).
// Всё работает в аудио-потоке: без выделения памяти и блокировок (память под задержку выделена в конструкторе).
#pragma once

#include "dsp.h"
#include "mic.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace bg {

constexpr float kEqBassHz = 100.0f;     // «Бас»: полка ниже ~100 Гц
constexpr float kEqTrebleHz = 8000.0f;  // «ВЧ»: полка выше ~8 кГц
constexpr float kEqMaxDb = 12.0f;

// Коэффициенты полочных фильтров (RBJ Audio EQ Cookbook, добротность S = 1) в формате Biquad из mic.h.
inline void set_shelf(Biquad& f, bool high, float fc, float gain_db) {
    const float A = std::pow(10.0f, gain_db / 40.0f);
    const float w = 2.0f * kPi * fc / kRate, c = std::cos(w), s = std::sin(w);
    const float alpha = s / 1.41421356f;                 // S = 1
    const float t = 2.0f * std::sqrt(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * c + t);
        b1 = 2 * A * ((A - 1) - (A + 1) * c);
        b2 = A * ((A + 1) - (A - 1) * c - t);
        a0 = (A + 1) + (A - 1) * c + t;
        a1 = -2 * ((A - 1) + (A + 1) * c);
        a2 = (A + 1) + (A - 1) * c - t;
    } else {
        b0 = A * ((A + 1) + (A - 1) * c + t);
        b1 = -2 * A * ((A - 1) + (A + 1) * c);
        b2 = A * ((A + 1) + (A - 1) * c - t);
        a0 = (A + 1) - (A - 1) * c + t;
        a1 = 2 * ((A - 1) - (A + 1) * c);
        a2 = (A + 1) - (A - 1) * c - t;
    }
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
}

// Два полочных фильтра: бас и высокие. При 0 дБ у обоих обработка полностью пропускается.
// Окно меняет bass_db / treble_db (атомарно), аудио-поток пересчитывает коэффициенты раз на блок.
struct Eq {
    std::atomic<float> bass_db{0.0f}, treble_db{0.0f};
    Biquad lo, hi;
    float cur_b = 0.0f, cur_t = 0.0f;   // только аудио-поток

    bool active() const { return std::fabs(bass_db.load(std::memory_order_relaxed)) > 0.05f || std::fabs(treble_db.load(std::memory_order_relaxed)) > 0.05f; }

    void process(float* x, size_t n) {
        float b = bass_db.load(std::memory_order_relaxed), t = treble_db.load(std::memory_order_relaxed);
        b = std::fabs(b) > 0.05f ? b : 0.0f;
        t = std::fabs(t) > 0.05f ? t : 0.0f;
        if (b != cur_b) { if (cur_b == 0.0f) lo.reset(); cur_b = b; if (b != 0.0f) set_shelf(lo, false, kEqBassHz, b); }
        if (t != cur_t) { if (cur_t == 0.0f) hi.reset(); cur_t = t; if (t != 0.0f) set_shelf(hi, true, kEqTrebleHz, t); }
        if (cur_b != 0.0f) lo.process(x, n);
        if (cur_t != 0.0f) hi.process(x, n);
    }
};

// Задержка выхода в кадрах (для выравнивания колонок и Bluetooth-наушников). До 500 мс.
struct DelayLine {
    static constexpr size_t kCap = 32768;            // степень двойки, больше 24 000 кадров (500 мс)
    static constexpr size_t kMask = kCap - 1;
    static constexpr uint32_t kMaxFrames = 24000;
    std::atomic<uint32_t> frames{0};                  // желаемая задержка
    std::vector<float> buf;
    size_t w = 0;
    uint32_t cur = 0;                                 // только аудио-поток

    DelayLine() : buf(kCap * 2, 0.0f) {}

    void process(float* x, size_t n) {
        uint32_t d = std::min(frames.load(std::memory_order_relaxed), kMaxFrames);
        if (d == 0 && cur == 0) return;
        if (cur == 0) std::fill(buf.begin(), buf.end(), 0.0f);   // включаем с чистого листа: старого звука там быть не должно
        cur = d;
        if (d == 0) return;                                       // выключили: дальше звук идёт напрямую
        for (size_t i = 0; i < n; ++i) {
            const size_t wi = (w & kMask) * 2, ri = ((w - d) & kMask) * 2;
            const float l = x[i * 2], r = x[i * 2 + 1];
            x[i * 2] = buf[ri];
            x[i * 2 + 1] = buf[ri + 1];
            buf[wi] = l;
            buf[wi + 1] = r;
            ++w;
        }
    }
};

}  // namespace bg
