// Саундпад (этап 7): короткие звуки, которые играют на выбранные шины. Плюс чтение файлов по пути в UTF-8.
// Звук целиком декодируется в память (48 кГц, стерео, float) при загрузке, поэтому в аудио-потоке нет ни чтения файлов, ни выделения памяти.
#pragma once

#include "third_party/miniaudio.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bg {

constexpr int kPads = 16;
constexpr size_t kPadMaxFrames = 48000u * 20u;   // длиннее 20 секунд это уже не «звук на кнопке»

// fopen с путём в UTF-8. В Windows обычный fopen читает путь в ANSI-кодировке, и кириллица (например, имя пользователя) ломается.
inline FILE* fopen_u8(const std::string& path, const char* mode) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::vector<wchar_t> w(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    std::wstring m;
    for (const char* p = mode; *p; ++p) m += static_cast<wchar_t>(*p);
    return _wfopen(w.data(), m.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// Читает файл целиком. Возвращает false, если не открылся или слишком большой (max_bytes).
inline bool read_file_u8(const std::string& path, std::vector<unsigned char>& out, size_t max_bytes) {
    FILE* f = fopen_u8(path, "rb");
    if (!f) return false;
    out.clear();
    unsigned char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) {
        if (out.size() + got > max_bytes) { std::fclose(f); out.clear(); return false; }
        out.insert(out.end(), buf, buf + got);
    }
    std::fclose(f);
    return true;
}

// Один звук саундпада. data/frames/playing пишет окно (поток команд), pos и конец воспроизведения пишет аудио-поток.
struct Pad {
    std::atomic<float*> data{nullptr};
    std::atomic<size_t> frames{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> busy{false};      // аудио-поток сейчас читает data: заменять и освобождать буфер нельзя
    std::atomic<size_t> pos{0};

    // Освобождает звук. Порядок важен: сначала playing = false, потом ждём, пока аудио-поток отпустит буфер (busy), потом free.
    // Аудио-поток ставит busy ДО проверки playing, поэтому читать освобождённую память он не может.
    void unload() {
        playing.store(false);
        frames.store(0);
        for (int i = 0; i < 400 && busy.load(); ++i) {
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, nullptr);
#endif
        }
        float* old = data.exchange(nullptr);
        if (old) ma_free(old, nullptr);
        pos.store(0);
    }
    ~Pad() { unload(); }
};

}  // namespace bg
