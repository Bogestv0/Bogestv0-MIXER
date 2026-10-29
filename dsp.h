// Простая обработка без выделения памяти: безопасно вызывать из аудио-потока.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace bg {

inline float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }
inline float lin_to_db(float lin) { return lin <= 1e-6f ? -120.0f : 20.0f * std::log10(lin); }

// Мягкое ограничение: до порога сигнал не трогаем, выше плавно прижимаем к ±1.
// Защита от перегрузки при программном усилении микрофона.
inline float soft_limit(float x) {
    const float t = 0.89125f;  // -1 дБFS
    const float a = std::fabs(x);
    if (a <= t) return x;
    const float over = (a - t) / (1.0f - t);
    const float y = t + (1.0f - t) * std::tanh(over);
    return x < 0 ? -y : y;
}

// Пиковый метр для двух каналов. Пишет аудио-поток, читает окно (консоль).
struct Meter {
    std::atomic<float> peak[2];
    std::atomic<bool> clipped;
    Meter() { peak[0] = 0.0f; peak[1] = 0.0f; clipped = false; }

    void feed(const float* interleaved, size_t frames, size_t channels) {
        float p[2] = {0.0f, 0.0f};
        for (size_t i = 0; i < frames; ++i)
            for (size_t c = 0; c < 2 && c < channels; ++c)
                p[c] = std::max(p[c], std::fabs(interleaved[i * channels + c]));
        for (int c = 0; c < 2; ++c) {
            float cur = peak[c].load(std::memory_order_relaxed);
            if (p[c] > cur) peak[c].store(p[c], std::memory_order_relaxed);
        }
        if (std::max(p[0], p[1]) >= 0.999f) clipped.store(true, std::memory_order_relaxed);
    }

    // Читает пик и сбрасывает его с плавным спадом (вызывается ~30 раз в секунду).
    float read(int c, float decay = 0.80f) {
        float v = peak[c].load(std::memory_order_relaxed);
        peak[c].store(v * decay, std::memory_order_relaxed);
        return v;
    }
};

}  // namespace bg
