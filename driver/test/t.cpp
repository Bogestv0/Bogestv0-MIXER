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
    static BG_STREAM S[BG_RINGS];                       // по одному потоку на кольцо для старых проверок
    for (int i = 0; i < BG_RINGS; ++i) BogestvStreamInit(&S[i], i, i >= 6);
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
    BogestvStreamRun(&S[1], TRUE, &f16);
    CHECK(ring(1).active() && h->reserved[1] == 48000, "active=1 и частота");
    const ULONG N = 1000, DMA = 4096 * 4;
    std::vector<int16_t> dma(DMA / 2, 0);
    for (ULONG i = 0; i < DMA / 4; ++i) { dma[i*2] = (int16_t)(i * 7 - 8000); dma[i*2+1] = (int16_t)(-(int)i * 5); }
    // кадры 3000..4000 пересекают границу буфера (4096 кадров)? нет; берём 3500..4500: заворот
    BogestvRender(&S[1], (BYTE*)dma.data(), DMA, 3500, N, &f16);
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
    BogestvStreamRun(&S[1], FALSE, NULL);
    CHECK(!ring(1).active() && h->reserved[1] == 0, "active=0 после остановки");

    // PCM24 и float32, моно
    BG_FMT f24 = {2, 24, 6, 48000, FALSE};
    uint8_t b24[6 * 4];
    for (int i = 0; i < 4; ++i) { int32_t v = (i + 1) * 1000000 * (i % 2 ? -1 : 1); b24[i*6]=v&255; b24[i*6+1]=(v>>8)&255; b24[i*6+2]=(v>>16)&255; int32_t w = v/2; b24[i*6+3]=w&255; b24[i*6+4]=(w>>8)&255; b24[i*6+5]=(w>>16)&255; }
    BogestvStreamRun(&S[2], TRUE, &f24);
    BogestvRender(&S[2], b24, sizeof b24, 0, 4, &f24);
    float o[8]; ring(2).pop(o, 4);
    CHECK(std::fabs(o[0] - 1000000 / 8388608.0f) < 1e-6f && std::fabs(o[2] + 2000000 / 8388608.0f) < 1e-6f && std::fabs(o[3] + 1000000 / 8388608.0f) < 1e-6f, "PCM24 со знаком");
    BG_FMT fm = {1, 32, 4, 48000, TRUE};
    float mono[3] = {0.25f, -0.5f, 1.0f};
    BogestvStreamRun(&S[3], TRUE, &fm);
    BogestvRender(&S[3], (BYTE*)mono, sizeof mono, 0, 3, &fm);
    float om[6]; ring(3).pop(om, 3);
    CHECK(om[0] == 0.25f && om[1] == 0.25f && om[4] == 1.0f && om[5] == 1.0f, "моно float -> обе стороны");

    // запись: движок кладёт в кольцо 6 -> драйвер заполняет DMA
    BG_FMT c16 = {2, 16, 4, 48000, FALSE};
    BogestvStreamRun(&S[6], TRUE, &c16);
    bg::ShmRing out1 = ring(6);
    CHECK(out1.active(), "Output: active=1");
    std::vector<float> src(500 * 2);
    for (int i = 0; i < 500; ++i) { src[i*2] = 0.5f * std::sin(i * 0.1f); src[i*2+1] = -0.25f; }
    std::vector<float> pad(1000 * 2, 0.0f);
    out1.push(src.data(), 500);
    out1.push(pad.data(), 1000);                      // запас: порция 500 + 720 кадров должны накопиться
    std::vector<int16_t> cap(DMA / 2, 0x7777);
    BogestvCapture(&S[6], (BYTE*)cap.data(), DMA, 100, 500, &c16);
    ok = true;
    for (int i = 0; i < 500; ++i) {
        ULONG fr = (100 + i) % (DMA / 4);
        if (std::abs(cap[fr*2] - (int)(src[i*2] * 32767.0f)) > 1 || std::abs(cap[fr*2+1] - (int)(src[i*2+1] * 32767.0f)) > 1) { ok = false; break; }
    }
    CHECK(ok, "запись: кадры из кольца в DMA");
    CHECK(cap[(100 + 500) % (DMA/4) * 2] == 0x7777, "за пределами порции DMA не тронут");
    // нехватка: кольцо опустело -> один раз тишина, счётчик растёт, запас после этого больше
    out1.discard(); S[6].Pos = out1.ctl->write_pos.load();     // пустое кольцо: и общий read_pos, и курсор потока на краю
    std::vector<int16_t> cap2(DMA / 2, 0x7777);
    const ULONG prime0 = S[6].Prime;
    BogestvCapture(&S[6], (BYTE*)cap2.data(), DMA, 0, 64, &c16);
    CHECK(cap2[0] == 0 && cap2[1] == 0 && out1.ctl->starved.load() >= 64, "нехватка = тишина, starved растёт");
    CHECK(!S[6].Primed && S[6].Prime == prime0 + BG_PRIME_STEP, "после опустошения запас растёт, ждём набора");
    // пока запас не набран, звук не отдаётся (кольцо не трогаем), а когда набрался, идёт без тишины
    std::vector<float> part(300 * 2, 0.2f); out1.push(part.data(), 300);
    cap2.assign(DMA / 2, 0x7777);
    BogestvCapture(&S[6], (BYTE*)cap2.data(), DMA, 0, 64, &c16);
    CHECK(cap2[0] == 0 && out1.available() == 300, "запас не набран: тишина, звук остаётся в кольце");
    std::vector<float> more(1200 * 2, 0.2f); out1.push(more.data(), 1200);
    BogestvCapture(&S[6], (BYTE*)cap2.data(), DMA, 0, 64, &c16);
    CHECK(S[6].Primed && cap2[0] != 0 && cap2[127] != 0 && out1.available() == 1500 - 64, "запас набран: звук пошёл с первого кадра");
    // запас больше нормы (порция + запас + 2400) обрезается до нормы
    out1.discard(); BogestvStreamRun(&S[6], FALSE, NULL); BogestvStreamRun(&S[6], TRUE, &c16);
    std::vector<float> big(6000 * 2, 0.1f); out1.push(big.data(), 6000);
    BogestvCapture(&S[6], (BYTE*)cap2.data(), DMA, 0, 100, &c16);
    CHECK(out1.available() == BG_PRIME_BASE, "лишний запас обрезан до порции + запаса, затем прочитана порция");
    // клип
    BG_FMT cf = {2, 16, 4, 48000, FALSE};
    BogestvStreamRun(&S[6], FALSE, NULL); BogestvStreamRun(&S[6], TRUE, &cf);
    std::vector<float> hot(1000 * 2); for (int i = 0; i < 1000; ++i) { hot[i*2] = 3.0f; hot[i*2+1] = -3.0f; }
    out1.push(hot.data(), 1000);
    int16_t one[2]; BogestvCapture(&S[6], (BYTE*)one, 4, 0, 1, &cf);
    CHECK(one[0] == 32767 && one[1] == -32767, "клип без переполнения");
    // повторный вход: при запуске Output выбрасывает старое
    out1.push(big.data(), 100); BogestvStreamRun(&S[6], FALSE, NULL); BogestvStreamRun(&S[6], TRUE, &c16);
    CHECK(out1.available() == 0, "Output: при запуске старое выброшено");
    CHECK(h->driver_beat.load() > 0, "driver_beat растёт");

    // Имитация выходного пути: движок кладёт по 128 кадров, Windows забирает порции по ~10 мс с дрожанием; раз в 5 с движок
    // замолкает на 40 мс (всплеск нагрузки). Без запаса (до 0.5.4) порции с нехваткой шли постоянно, с запасом остаются только провалы.
    {
        BG_FMT sf = {2, 16, 4, 48000, FALSE};
        auto sim = [&](double jit_ms, bool stall, unsigned* underruns, double* gap_pct) {
            BogestvStreamRun(&S[7], FALSE, NULL); BogestvStreamRun(&S[7], TRUE, &sf);
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
                    BogestvCapture(&S[7], (BYTE*)dm.data(), 2048 * 4, fr, 480, &sf); fr += 480;
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


    // ============================================================================================================================
    // 0.5.5: несколько потоков Windows на одном кольце (по таблицам пинов SysVAD: воспроизведение до 2–6, запись до 5)
    // ============================================================================================================================
    {
        BG_FMT f = {2, 16, 4, 48000, FALSE};
        const ULONG D = 4096 * 4;

        // --- Воспроизведение: два потока на Input 5 (кольцо 4) складываются, остановка одного не закрывает кольцо ---
        BG_STREAM a, b; BogestvStreamInit(&a, 4, FALSE); BogestvStreamInit(&b, 4, FALSE);
        std::vector<int16_t> dA(D / 2), dB(D / 2);
        for (ULONG i = 0; i < D / 4; ++i) { dA[i*2] = 3000; dA[i*2+1] = -3000; dB[i*2] = 1000; dB[i*2+1] = 500; }
        bg::ShmRing r4 = ring(4); r4.discard(); r4.ctl->drops = 0;
        BogestvStreamRun(&a, TRUE, &f);
        CHECK(r4.active() && h->reserved[BG_HDR_STREAMS + 4] == 1, "1 поток: active=1, счётчик потоков 1");
        ULONGLONG fa = 0, fb = 0; std::vector<float> o(480 * 2);
        // 5 тиков играет только A, потом присоединяется B (позже, чем A), дальше оба; порядок вызовов внутри тика чередуем
        for (int tick = 0; tick < 5; ++tick) { BogestvRender(&a, (BYTE*)dA.data(), D, fa, 480, &f); fa += 480; r4.pop(o.data(), 480); }
        BogestvStreamRun(&b, TRUE, &f);
        CHECK(h->reserved[BG_HDR_STREAMS + 4] == 2 && h->reserved[BG_HDR_PEAK + 4] == 2, "2 потока: счётчик потоков 2, пик 2");
        bool summed = true; ULONG lateSum = 0;
        for (int tick = 0; tick < 100; ++tick) {
            if (tick & 1) { BogestvRender(&b, (BYTE*)dB.data(), D, fb, 480, &f); fb += 480; BogestvRender(&a, (BYTE*)dA.data(), D, fa, 480, &f); fa += 480; }
            else          { BogestvRender(&a, (BYTE*)dA.data(), D, fa, 480, &f); fa += 480; BogestvRender(&b, (BYTE*)dB.data(), D, fb, 480, &f); fb += 480; }
            // движок забирает с отставанием в один тик (как с запасом): к этому моменту оба потока уже сложены
            if (tick >= 1) {
                const ULONG got = r4.pop(o.data(), 480);
                if (got != 480) summed = false;
                for (int i = 0; i < 480 && summed; ++i) {
                    const float el = (3000 + 1000) / 32768.0f, er = (-3000 + 500) / 32768.0f;
                    if (std::fabs(o[i*2] - el) > 1e-6f || std::fabs(o[i*2+1] - er) > 1e-6f) { summed = false; ++lateSum; }
                }
            }
        }
        CHECK(summed, "два потока одного кольца складываются (L: 3000+1000, R: -3000+500), в любом порядке вызовов");
        CHECK(r4.ctl->drops.load() == 0, "два потока не дают переполнения: приток = реальное время, а не вдвое больше");
        BogestvStreamRun(&b, FALSE, NULL);
        CHECK(r4.active() && h->reserved[4] == 48000 && h->reserved[BG_HDR_STREAMS + 4] == 1, "остановился второй поток: кольцо открыто, частота на месте");
        BogestvStreamRun(&b, FALSE, NULL);
        CHECK(r4.active() && h->reserved[BG_HDR_STREAMS + 4] == 1, "повторная остановка того же потока ничего не меняет");
        const ULONG before = r4.ctl->write_pos.load();
        BogestvRender(&b, (BYTE*)dB.data(), D, fb, 480, &f);
        CHECK(r4.ctl->write_pos.load() == before, "остановленный поток в кольцо не пишет");
        BogestvStreamRun(&a, FALSE, NULL);
        CHECK(!r4.active() && h->reserved[4] == 0 && h->reserved[BG_HDR_STREAMS + 4] == 0, "остановился последний поток: кольцо закрыто");

        // --- Воспроизведение: один поток ведёт себя как раньше (пишет на край, переполнение считается) ---
        BG_STREAM s1; BogestvStreamInit(&s1, 5, FALSE); BogestvStreamRun(&s1, TRUE, &f);
        bg::ShmRing r5 = ring(5); r5.discard(); r5.ctl->drops = 0;
        for (int tick = 0; tick < 20; ++tick) BogestvRender(&s1, (BYTE*)dA.data(), D, (ULONGLONG)tick * 480, 480, &f);   // движок не читает
        CHECK(r5.available() == bg::kShmRingFrames && r5.ctl->drops.load() == 20 * 480 - bg::kShmRingFrames, "один поток: кольцо полно, лишнее считается в drops");
        BogestvStreamRun(&s1, FALSE, NULL);

        // --- Запись: два читателя одного кольца (OBS и Discord на «Bogestv0 Mix B1 Out» / Out N) получают один и тот же звук ---
        BG_STREAM c1, c2; BogestvStreamInit(&c1, 8, TRUE); BogestvStreamInit(&c2, 8, TRUE);
        bg::ShmRing r8 = ring(8);
        BogestvStreamRun(&c1, TRUE, &f);
        BogestvStreamRun(&c2, TRUE, &f);
        CHECK(r8.active() && h->reserved[BG_HDR_STREAMS + 8] == 2, "запись: два потока, кольцо открыто");
        std::vector<float> src(480 * 2); ULONGLONG tt = 0;
        std::vector<int16_t> k1(D / 2), k2(D / 2); ULONGLONG g1 = 0, g2 = 0; unsigned snd1 = 0, snd2 = 0, sil1 = 0, sil2 = 0; bool same = true;
        for (int tick = 0; tick < 300; ++tick) {
            for (int i = 0; i < 480; ++i) { float v = 0.5f * std::sin((tt + i) * 0.05f) + 0.01f; src[i*2] = v; src[i*2+1] = v; } tt += 480;
            r8.push(src.data(), 480);
            if (tick < 5) continue;
            BogestvCapture(&c1, (BYTE*)k1.data(), D, g1, 480, &f);
            BogestvCapture(&c2, (BYTE*)k2.data(), D, g2, 480, &f);
            bool n1 = false, n2 = false;
            for (ULONG i = 0; i < 480; ++i) {
                const int16_t x1 = k1[((g1 + i) % (D / 4)) * 2], x2 = k2[((g2 + i) % (D / 4)) * 2];
                if (x1) n1 = true; if (x2) n2 = true;
                if (n1 && n2 && x1 != x2) same = false;
            }
            g1 += 480; g2 += 480; (n1 ? snd1 : sil1)++; (n2 ? snd2 : sil2)++;
        }
        std::printf("      (запись, два читателя: со звуком %u и %u порций, тишины %u и %u, starved %u кадров)\n", snd1, snd2, sil1, sil2, r8.ctl->starved.load());
        CHECK(sil1 == 0 && sil2 == 0 && snd1 == 295 && snd2 == 295, "запись: оба читателя получают звук без провалов (раньше делили кольцо пополам)");
        CHECK(same, "запись: оба читателя слышат один и тот же звук");
        CHECK(r8.ctl->starved.load() == 0, "запись: нехватки нет");
        // общий read_pos = самый отстающий; читатель останавливается, второй идёт дальше без провалов
        BogestvStreamRun(&c1, FALSE, NULL);
        CHECK(r8.active() && h->reserved[BG_HDR_STREAMS + 8] == 1, "запись: остановился один читатель, кольцо открыто");
        sil2 = 0;
        for (int tick = 0; tick < 60; ++tick) {
            for (int i = 0; i < 480; ++i) { float v = 0.5f * std::sin((tt + i) * 0.05f) + 0.01f; src[i*2] = v; src[i*2+1] = v; } tt += 480;
            r8.push(src.data(), 480);
            BogestvCapture(&c2, (BYTE*)k2.data(), D, g2, 480, &f);
            bool n2 = false; for (ULONG i = 0; i < 480; ++i) if (k2[((g2 + i) % (D / 4)) * 2]) { n2 = true; break; } g2 += 480; if (!n2) ++sil2;
        }
        CHECK(sil2 == 0, "запись: после ухода первого читателя второй продолжает без провалов");
        CHECK(r8.ctl->drops.load() == 0, "запись: движок не упирался в полное кольцо");
        // новый читатель присоединяется к идущему: остальных не трогает, сам ждёт свой запас
        BG_STREAM c3; BogestvStreamInit(&c3, 8, TRUE); BogestvStreamRun(&c3, TRUE, &f);
        sil2 = 0; unsigned sil3 = 0, snd3 = 0; ULONGLONG g3 = 0;
        for (int tick = 0; tick < 60; ++tick) {
            for (int i = 0; i < 480; ++i) { float v = 0.5f * std::sin((tt + i) * 0.05f) + 0.01f; src[i*2] = v; src[i*2+1] = v; } tt += 480;
            r8.push(src.data(), 480);
            BogestvCapture(&c2, (BYTE*)k2.data(), D, g2, 480, &f);
            bool n2 = false; for (ULONG i = 0; i < 480; ++i) if (k2[((g2 + i) % (D / 4)) * 2]) { n2 = true; break; } g2 += 480; if (!n2) ++sil2;
            BogestvCapture(&c3, (BYTE*)k1.data(), D, g3, 480, &f);
            bool n3 = false; for (ULONG i = 0; i < 480; ++i) if (k1[((g3 + i) % (D / 4)) * 2]) { n3 = true; break; } g3 += 480; (n3 ? snd3 : sil3)++;
        }
        CHECK(sil2 == 0, "запись: новый читатель не мешает идущему");
        CHECK(snd3 >= 55 && sil3 <= 4, "запись: новый читатель после набора запаса получает звук");
        BogestvStreamRun(&c2, FALSE, NULL); BogestvStreamRun(&c3, FALSE, NULL);
        CHECK(!r8.active(), "запись: все читатели остановлены, кольцо закрыто");

        // --- Реестр: больше BG_MAX_STREAMS потоков не ломают мост ---
        BogestvStreamRun(&S[3], FALSE, NULL);               // поток от старых проверок на этом кольце
        BG_STREAM many[BG_MAX_STREAMS + 2];
        for (int i = 0; i < BG_MAX_STREAMS + 2; ++i) { BogestvStreamInit(&many[i], 3, FALSE); BogestvStreamRun(&many[i], TRUE, &f); }
        CHECK(g_Count[3] == BG_MAX_STREAMS && !many[BG_MAX_STREAMS].Running, "реестр: лишние потоки не регистрируются");
        BogestvRender(&many[BG_MAX_STREAMS + 1], (BYTE*)dA.data(), D, 0, 480, &f);
        for (int i = 0; i < BG_MAX_STREAMS + 2; ++i) BogestvStreamRun(&many[i], FALSE, NULL);
        CHECK(g_Count[3] == 0 && !ring(3).active(), "реестр: после остановки всех кольцо закрыто");
        // --- Повторный запуск потока после паузы ---
        BogestvStreamRun(&a, TRUE, &f); BogestvStreamRun(&a, TRUE, &f);
        CHECK(g_Count[4] == 1, "повторный запуск уже запущенного потока не дублирует его в реестре");
        BogestvStreamRun(&a, FALSE, NULL);
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
