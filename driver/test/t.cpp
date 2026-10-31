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
    CHECK(h->in_rings == 6 && h->out_rings == 8 && h->rate == 48000 && h->channels == 2, "поля заголовка");
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
    std::vector<float> pad(1000 * 2, 0.0f);
    out1.push(src.data(), 500);
    out1.push(pad.data(), 1000);                      // запас: порция 500 + 720 кадров должны накопиться
    std::vector<int16_t> cap(DMA / 2, 0x7777);
    BogestvCapture(6, (BYTE*)cap.data(), DMA, 100, 500, &c16);
    ok = true;
    for (int i = 0; i < 500; ++i) {
        ULONG fr = (100 + i) % (DMA / 4);
        if (std::abs(cap[fr*2] - (int)(src[i*2] * 32767.0f)) > 1 || std::abs(cap[fr*2+1] - (int)(src[i*2+1] * 32767.0f)) > 1) { ok = false; break; }
    }
    CHECK(ok, "запись: кадры из кольца в DMA");
    CHECK(cap[(100 + 500) % (DMA/4) * 2] == 0x7777, "за пределами порции DMA не тронут");
    // нехватка: кольцо опустело -> один раз тишина, счётчик растёт, запас после этого больше
    out1.discard();
    std::vector<int16_t> cap2(DMA / 2, 0x7777);
    const ULONG prime0 = g_Prime[6];
    BogestvCapture(6, (BYTE*)cap2.data(), DMA, 0, 64, &c16);
    CHECK(cap2[0] == 0 && cap2[1] == 0 && out1.ctl->starved.load() >= 64, "нехватка = тишина, starved растёт");
    CHECK(!g_Primed[6] && g_Prime[6] == prime0 + BG_PRIME_STEP, "после опустошения запас растёт, ждём набора");
    // пока запас не набран, звук не отдаётся (кольцо не трогаем), а когда набрался, идёт без тишины
    std::vector<float> part(300 * 2, 0.2f); out1.push(part.data(), 300);
    cap2.assign(DMA / 2, 0x7777);
    BogestvCapture(6, (BYTE*)cap2.data(), DMA, 0, 64, &c16);
    CHECK(cap2[0] == 0 && out1.available() == 300, "запас не набран: тишина, звук остаётся в кольце");
    std::vector<float> more(1200 * 2, 0.2f); out1.push(more.data(), 1200);
    BogestvCapture(6, (BYTE*)cap2.data(), DMA, 0, 64, &c16);
    CHECK(g_Primed[6] && cap2[0] != 0 && cap2[127] != 0 && out1.available() == 1500 - 64, "запас набран: звук пошёл с первого кадра");
    // запас больше нормы (порция + запас + 2400) обрезается до нормы
    out1.discard(); BogestvStreamRun(6, TRUE, FALSE, NULL); BogestvStreamRun(6, TRUE, TRUE, &c16);
    std::vector<float> big(6000 * 2, 0.1f); out1.push(big.data(), 6000);
    BogestvCapture(6, (BYTE*)cap2.data(), DMA, 0, 100, &c16);
    CHECK(out1.available() == BG_PRIME_BASE, "лишний запас обрезан до порции + запаса, затем прочитана порция");
    // клип
    BG_FMT cf = {2, 16, 4, 48000, FALSE};
    BogestvStreamRun(6, TRUE, FALSE, NULL); BogestvStreamRun(6, TRUE, TRUE, &cf);
    std::vector<float> hot(1000 * 2); for (int i = 0; i < 1000; ++i) { hot[i*2] = 3.0f; hot[i*2+1] = -3.0f; }
    out1.push(hot.data(), 1000);
    int16_t one[2]; BogestvCapture(6, (BYTE*)one, 4, 0, 1, &cf);
    CHECK(one[0] == 32767 && one[1] == -32767, "клип без переполнения");
    // повторный вход: при запуске Output выбрасывает старое
    out1.push(big.data(), 100); BogestvStreamRun(6, TRUE, FALSE, NULL); BogestvStreamRun(6, TRUE, TRUE, &c16);
    CHECK(out1.available() == 0, "Output: при запуске старое выброшено");
    CHECK(h->driver_beat.load() > 0, "driver_beat растёт");

    // Имитация выходного пути: движок кладёт по 128 кадров, Windows забирает порции по ~10 мс с дрожанием; раз в 5 с движок
    // замолкает на 40 мс (всплеск нагрузки). Без запаса (до 0.5.4) порции с нехваткой шли постоянно, с запасом остаются только провалы.
    {
        BG_FMT sf = {2, 16, 4, 48000, FALSE};
        auto sim = [&](double jit_ms, bool stall, unsigned* underruns, double* gap_pct) {
            BogestvStreamRun(7, TRUE, FALSE, NULL); BogestvStreamRun(7, TRUE, TRUE, &sf);
            bg::ShmRing rg = ring(7); rg.ctl->starved = 0;
            std::vector<float> blk(128 * 2, 0.3f); std::vector<int16_t> dm(2048 * 2);
            uint32_t seed = 7; auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / 16777216.0; };
            double tm = 0, np = 0, nl = 0.0037, stall_end = -1, next_stall = 5.0; unsigned pulls = 0, bad = 0; ULONGLONG fr = 0;
            while (tm < 60.0) {
                if (stall && tm >= next_stall) { stall_end = tm + 0.040; next_stall += 5.0; }
                if (np <= nl) {
                    tm = np;
                    if (tm < stall_end) { np = stall_end; continue; }
                    rg.push(blk.data(), 128); np += 128 / 48000.0;
                } else {
                    tm = nl; const ULONG before = rg.ctl->starved.load();
                    BogestvCapture(7, (BYTE*)dm.data(), 2048 * 4, fr, 480, &sf); fr += 480;
                    ++pulls; if (rg.ctl->starved.load() > before) ++bad;
                    nl += 0.010 + (rnd() - 0.5) * jit_ms * 1e-3;
                }
            }
            *underruns = bad; *gap_pct = 100.0 * bad / pulls;
        };
        unsigned u; double g;
        sim(2.0, false, &u, &g);
        std::printf("      (выход, дрожание ±1 мс: порций с нехваткой %u (%.2f %%); без запаса было ~1,2 %%)\n", u, g);
        CHECK(u <= 3, "выход: дрожание ±1 мс: не больше трёх порций с нехваткой (пока запас растёт)");
        sim(6.0, false, &u, &g);
        std::printf("      (выход, дрожание ±3 мс: порций с нехваткой %u (%.2f %%); без запаса было ~1,8 %%)\n", u, g);
        CHECK(u <= 8, "выход: дрожание ±3 мс: запас вырос и провалы прекратились");
        sim(2.0, true, &u, &g);
        std::printf("      (выход, раз в 5 с провал движка на 40 мс: порций с нехваткой %u из ~6000)\n", u);
        CHECK(u <= 12, "выход: провал движка на 40 мс даёт одну тишину, а не хрип на каждой порции");
    }

    // IOCTL
    IRP irp; memset(&irp, 0, sizeof irp); BOGESTV_MAP_OUT mo; irp.AssociatedIrp.SystemBuffer = &mo;
    irp.sp.MajorFunction = IRP_MJ_DEVICE_CONTROL; irp.sp.Parameters.DeviceIoControl.IoControlCode = IOCTL_BOGESTV_MAP_SHM; irp.sp.Parameters.DeviceIoControl.OutputBufferLength = sizeof mo;
    FILE_OBJECT* fo1 = (FILE_OBJECT*)0x10; irp.sp.FileObject = fo1;
    CHECK(BogestvDispatch(g_Dev, &irp) == 0 && mo.base == 0x10000 && mo.total_bytes == bg::kShmTotalBytes && mo.version == 2, "IOCTL отдаёт адрес");
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
