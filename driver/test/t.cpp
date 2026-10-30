#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <vector>
#include "shm.h"
#include <memory>
#include <string>
#include <algorithm>
#include "bogestvbridge.cpp"      // тестируем сам файл драйвера (подключаем целиком, чтобы видеть static)
static int bad = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("НЕ ПРОШЛО: %s\n", m); ++bad; } } while (0)
int main() {
    static_assert(BG_TOTAL == bg::kShmTotalBytes, "размер");
    static_assert(BG_STRIDE == bg::kShmRingStride, "шаг");
    DRIVER_OBJECT drv; memset(&drv, 0, sizeof drv);
    CHECK(BogestvBridgeInit(&drv) == 0, "init");
    // раскладка: смотрим тем же кодом, что у движка
    bg::ShmHeader* h = (bg::ShmHeader*)g_Mem;
    CHECK(h->magic.load() == bg::kShmMagic && h->version == bg::kShmVersion && h->ring_frames == bg::kShmRingFrames && h->ring_stride == bg::kShmRingStride, "заголовок читается движком");
    CHECK(h->in_rings == 6 && h->out_rings == 2 && h->rate == 48000 && h->channels == 2, "поля заголовка");
    // имена
    CHECK(BogestvRingFor(L"WaveSpeaker", FALSE) == 0 && BogestvRingFor(L"WaveInput6", FALSE) == 5 && BogestvRingFor(L"WaveMicIn", TRUE) == 6 && BogestvRingFor(L"WaveMicArray1", TRUE) == 7, "имена -> кольца");
    CHECK(BogestvRingFor(L"WaveSpeaker", TRUE) == -1 && BogestvRingFor(L"WaveMicIn", FALSE) == -1 && BogestvRingFor(L"WaveMicArray2", TRUE) == -1, "петля и чужое не берём");

    auto ring = [&](int i) { bg::ShmRing r; unsigned char* p = (unsigned char*)g_Mem + bg::kShmHeaderBytes + bg::kShmRingStride * i; r.ctl = (bg::ShmRingCtl*)p; r.data = (float*)(p + bg::kShmCtlBytes); r.cap = bg::kShmRingFrames; return r; };

    // воспроизведение: PCM16 стерео -> кольцо 1 -> читает «движок»
    BG_FMT f16 = {2, 16, 4, 48000, FALSE};
    BogestvStreamRun(1, FALSE, TRUE, &f16);
    CHECK(ring(1).active() && h->reserved[1] == 48000, "active=1 и частота");
    const ULONG N = 1000, DMA = 4096 * 4;
    std::vector<int16_t> dma(DMA / 2, 0);
    for (ULONG i = 0; i < DMA / 4; ++i) { dma[i*2] = (int16_t)(i * 7 - 8000); dma[i*2+1] = (int16_t)(-(int)i * 5); }
    // кадры 3000..4000 пересекают границу буфера (4096 кадров)? нет; берём 3500..4500: заворот
    BogestvRender(1, (BYTE*)dma.data(), DMA, 3500, N, &f16);
    bg::ShmRing e = ring(1);
    CHECK(e.available() == N, "в кольце N кадров");
    std::vector<float> got(N * 2);
    e.pop(got.data(), N);
    bool ok = true;
    for (ULONG i = 0; i < N; ++i) {
        ULONG fr = (3500 + i) % (DMA / 4);
        float l = dma[fr*2] / 32768.0f, r = dma[fr*2+1] / 32768.0f;
        if (std::fabs(got[i*2] - l) > 1e-6f || std::fabs(got[i*2+1] - r) > 1e-6f) { ok = false; break; }
    }
    CHECK(ok, "PCM16 через границу буфера дошёл без искажений");
    BogestvStreamRun(1, FALSE, FALSE, NULL);
    CHECK(!ring(1).active() && h->reserved[1] == 0, "active=0 после остановки");

    // PCM24 и float32, моно
    BG_FMT f24 = {2, 24, 6, 48000, FALSE};
    uint8_t b24[6 * 4];
    for (int i = 0; i < 4; ++i) { int32_t v = (i + 1) * 1000000 * (i % 2 ? -1 : 1); b24[i*6]=v&255; b24[i*6+1]=(v>>8)&255; b24[i*6+2]=(v>>16)&255; int32_t w = v/2; b24[i*6+3]=w&255; b24[i*6+4]=(w>>8)&255; b24[i*6+5]=(w>>16)&255; }
    BogestvRender(2, b24, sizeof b24, 0, 4, &f24);
    float o[8]; ring(2).pop(o, 4);
    CHECK(std::fabs(o[0] - 1000000 / 8388608.0f) < 1e-6f && std::fabs(o[2] + 2000000 / 8388608.0f) < 1e-6f && std::fabs(o[3] + 1000000 / 8388608.0f) < 1e-6f, "PCM24 со знаком");
    BG_FMT fm = {1, 32, 4, 48000, TRUE};
    float mono[3] = {0.25f, -0.5f, 1.0f};
    BogestvRender(3, (BYTE*)mono, sizeof mono, 0, 3, &fm);
    float om[6]; ring(3).pop(om, 3);
    CHECK(om[0] == 0.25f && om[1] == 0.25f && om[4] == 1.0f && om[5] == 1.0f, "моно float -> обе стороны");

    // запись: движок кладёт в кольцо 6 -> драйвер заполняет DMA
    BG_FMT c16 = {2, 16, 4, 48000, FALSE};
    BogestvStreamRun(6, TRUE, TRUE, &c16);
    bg::ShmRing out1 = ring(6);
    CHECK(out1.active(), "Output: active=1");
    std::vector<float> src(500 * 2);
    for (int i = 0; i < 500; ++i) { src[i*2] = 0.5f * std::sin(i * 0.1f); src[i*2+1] = -0.25f; }
    out1.push(src.data(), 500);
    std::vector<int16_t> cap(DMA / 2, 0x7777);
    BogestvCapture(6, (BYTE*)cap.data(), DMA, 100, 500, &c16);
    ok = true;
    for (int i = 0; i < 500; ++i) {
        ULONG fr = (100 + i) % (DMA / 4);
        if (std::abs(cap[fr*2] - (int)(src[i*2] * 32767.0f)) > 1 || std::abs(cap[fr*2+1] - (int)(src[i*2+1] * 32767.0f)) > 1) { ok = false; break; }
    }
    CHECK(ok, "запись: кадры из кольца в DMA");
    CHECK(cap[(100 + 500) % (DMA/4) * 2] == 0x7777, "за пределами порции DMA не тронут");
    // нехватка: тишина и счётчик
    BogestvCapture(6, (BYTE*)cap.data(), DMA, 0, 64, &c16);
    CHECK(cap[0] == 0 && cap[1] == 0 && out1.ctl->starved.load() >= 64, "нехватка = тишина, starved растёт");
    // запас больше 4096+n обрезается
    std::vector<float> big(6000 * 2, 0.1f); out1.push(big.data(), 6000);
    BogestvCapture(6, (BYTE*)cap.data(), DMA, 0, 100, &c16);
    CHECK(out1.available() == 4096 - 100, "запас обрезан до 4096 кадров, затем прочитано 100");
    // клип
    BG_FMT cf = {2, 16, 4, 48000, FALSE}; float hot[2] = {3.0f, -3.0f}; out1.discard(); out1.push(hot, 1);
    int16_t one[2]; BogestvCapture(6, (BYTE*)one, 4, 0, 1, &cf);
    CHECK(one[0] == 32767 && one[1] == -32767, "клип без переполнения");
    // повторный вход: при запуске Output выбрасывает старое
    out1.push(big.data(), 100); BogestvStreamRun(6, TRUE, FALSE, NULL); BogestvStreamRun(6, TRUE, TRUE, &c16);
    CHECK(out1.available() == 0, "Output: при запуске старое выброшено");
    CHECK(h->driver_beat.load() > 0, "driver_beat растёт");

    // IOCTL
    IRP irp; memset(&irp, 0, sizeof irp); BOGESTV_MAP_OUT mo; irp.AssociatedIrp.SystemBuffer = &mo;
    irp.sp.MajorFunction = IRP_MJ_DEVICE_CONTROL; irp.sp.Parameters.DeviceIoControl.IoControlCode = IOCTL_BOGESTV_MAP_SHM; irp.sp.Parameters.DeviceIoControl.OutputBufferLength = sizeof mo;
    FILE_OBJECT* fo1 = (FILE_OBJECT*)0x10; irp.sp.FileObject = fo1;
    CHECK(BogestvDispatch(g_Dev, &irp) == 0 && mo.base == 0x10000 && mo.total_bytes == bg::kShmTotalBytes && mo.version == 1, "IOCTL отдаёт адрес");
    irp.sp.FileObject = (FILE_OBJECT*)0x20;
    CHECK(BogestvDispatch(g_Dev, &irp) == STATUS_DEVICE_BUSY, "второй процесс получает «занято»");
    irp.sp.MajorFunction = IRP_MJ_CLEANUP; irp.sp.FileObject = (FILE_OBJECT*)0x20; BogestvDispatch(g_Dev, &irp);
    CHECK(g_OwnerFile == fo1, "чужое закрытие отображение не снимает");
    irp.sp.FileObject = fo1; BogestvDispatch(g_Dev, &irp);
    CHECK(g_OwnerFile == NULL, "закрыли дескриптор: отображение снято");
    irp.sp.MajorFunction = IRP_MJ_DEVICE_CONTROL; irp.sp.FileObject = (FILE_OBJECT*)0x30; irp.sp.Parameters.DeviceIoControl.OutputBufferLength = 4;
    CHECK(BogestvDispatch(g_Dev, &irp) == STATUS_BUFFER_TOO_SMALL, "малый буфер");
    BogestvBridgeUnload();
    std::printf(bad ? "ОШИБОК: %d\n" : "Все проверки моста пройдены\n", bad);
    return bad;
}
