// Мост между потоками SysVAD и общей памятью движка (этап 6.4).
//
// Что здесь происходит:
//  * При загрузке драйвер выделяет невыгружаемую память по раскладке engine/shm.h (заголовок + 14 колец) и создаёт управляющее
//    устройство \Device\Bogestv0Mixer (в Windows оно видно как \\.\Bogestv0Mixer).
//  * Движок вызывает IOCTL_BOGESTV_MAP_SHM: память отображается в его процесс (MmMapLockedPagesSpecifyCache). Закрыл дескриптор, и
//    отображение снимается в IRP_MJ_CLEANUP.
//  * Потоки воспроизведения (Input 1..6) переписывают звук из буфера DMA в кольца 0..5; потоки записи (шины B1, B2 и выходы дорожек Out 1..6)
//    заполняют буфер DMA звуком из колец 6..13. На одном кольце может быть несколько потоков Windows (0.5.5): воспроизведение складывается, запись читается у каждого потока своим курсором. Формат колец: float32, 2 канала, 48 кГц. Преобразование из формата потока (PCM 16/24/32, float)
//    делается здесь; пересчёт частоты не делается (если у устройства в Windows не 48 кГц, звук будет в другой высоте: этап 6.6).
//  * Правила обмена (позиции, active, beat) те же, что в shm.h. Все функции потоков вызываются из таймера SysVAD (DISPATCH_LEVEL).
//
// Раскладку нужно держать в точном соответствии с engine/shm.h (размеры проверяются C_ASSERT).
#include "sysvad.h"

// ntifs.h здесь подключать нельзя (конфликтует с типами sysvad.h/portcls: PEPROCESS, PETHREAD и др.), поэтому две нужные функции
// объявлены вручную. Настоящая структура KAPC_STATE занимает 48 байт на x64, берём буфер с запасом (64 байта).
extern "C" {
NTKERNELAPI VOID KeStackAttachProcess(_Inout_ PRKPROCESS Process, _Out_ PVOID ApcState);
NTKERNELAPI VOID KeUnstackDetachProcess(_In_ PVOID ApcState);
}
#include <wdmsec.h>
#include "bogestvbridge.h"
#include "bogestv_ioctl.h"

#define BG_TAG          'bgvB'
#define BG_IN_RINGS     6
#define BG_OUT_RINGS    8                 // B1, B2 и шесть выходов дорожек (Out 1..6)
#define BG_RINGS        (BG_IN_RINGS + BG_OUT_RINGS)
#define BG_RING_FRAMES  8192
#define BG_RATE         48000
#define BG_HDR_BYTES    256
#define BG_CTL_BYTES    128
#define BG_STRIDE       (BG_CTL_BYTES + BG_RING_FRAMES * 2 * 4)
#define BG_TOTAL        (BG_HDR_BYTES + BG_STRIDE * BG_RINGS)
#define BG_MAGIC        0x58474642u
#define BG_VERSION      2u

typedef struct _BG_SHM_HDR {
    volatile ULONG magic;
    ULONG version, header_bytes, ring_stride, in_rings, out_rings, rate, ring_frames, channels;
    ULONG engine_pid;
    volatile ULONG engine_beat;
    volatile ULONG driver_beat;
    ULONG reserved[52];            // пишет драйвер: [i] = частота, с которой Windows открыла кольцо i (0 = закрыто); [14 + i] = запущено потоков на кольце i; [28 + i] = пик потоков на кольце i
} BG_SHM_HDR;
C_ASSERT(sizeof(BG_SHM_HDR) == BG_HDR_BYTES);

typedef struct _BG_RING_CTL {
    DECLSPEC_ALIGN(64) volatile ULONG write_pos;
    volatile ULONG drops;
    DECLSPEC_ALIGN(64) volatile ULONG read_pos;
    volatile ULONG active;
    volatile ULONG starved;
} BG_RING_CTL;
C_ASSERT(sizeof(BG_RING_CTL) == BG_CTL_BYTES);
C_ASSERT(FIELD_OFFSET(BG_RING_CTL, read_pos) == 64);

typedef struct _BG_RING {
    BG_RING_CTL* ctl;
    float*       data;
} BG_RING;

static PVOID            g_Mem = NULL;            // ядро: невыгружаемая память
static PMDL             g_Mdl = NULL;
static BG_SHM_HDR*      g_Hdr = NULL;
static PDEVICE_OBJECT   g_Dev = NULL;
static UNICODE_STRING   g_Sym = RTL_CONSTANT_STRING(L"\\DosDevices\\Bogestv0Mixer");
static PDRIVER_DISPATCH g_Old[IRP_MJ_MAXIMUM_FUNCTION + 1];

// Владелец отображения.
static PFILE_OBJECT     g_OwnerFile = NULL;
static PEPROCESS        g_OwnerProc = NULL;
static PVOID            g_UserVa = NULL;

// Запас на стороне записи (0.5.4). Драйвер забирает звук из колец выходов порциями по таймеру Windows (~10 мс), а движок кладёт его
// блоками по своим часам. Без запаса кольцо перед забором почти пусто, любое опоздание движка даёт кусок тишины (хрип), и запас сам
// уже не набирается: обе стороны идут с одной скоростью. Поэтому: начинаем отдавать звук, только когда накоплено порция + запас; если
// кольцо опустело, молчим один раз и ждём, пока запас снова наберётся, а сам запас после каждого опустошения растёт.
// С 0.5.5 запас (Primed / Prime) свой у каждого потока записи: потоков на кольце может быть несколько, у каждого свой курсор чтения.
#define BG_PRIME_BASE   720               // 15 мс при 48 кГц: начальный запас сверх порции
#define BG_PRIME_STEP   480               // рост запаса после опустошения (10 мс)
#define BG_PRIME_MAX    2880              // потолок запаса (60 мс)
#define BG_TRIM_EXTRA   2400              // если накопилось больше порции + запаса + это (50 мс), лишнее старое выбрасывается

// Несколько потоков на одном кольце (0.5.5). Windows может открыть на устройстве больше одного потока (разные приложения или режимы
// обработки, см. MAX_*_STREAMS в таблицах пинов SysVAD). Правила:
//  * воспроизведение: потоки СКЛАДЫВАЮТСЯ. Кольцо знает «край» (write_pos); каждый поток пишет со своей позиции Pos: кадры ниже края
//    суммируются с тем, что уже лежит, кадры на краю и выше дописываются и сдвигают край. Один поток всегда пишет ровно на край, как раньше;
//  * запись: каждый поток читает СВОИМ курсором (Pos), общий read_pos кольца = самый отстающий из курсоров, поэтому все читатели
//    получают один и тот же звук, а движок видит свободное место по самому медленному;
//  * кольцо активно (active = 1), пока запущен хотя бы один поток; счётчики потоков идут в заголовок (BG_HDR_STREAMS / BG_HDR_PEAK);
//  * всё состояние кольца и потоков меняется под замком кольца (спин-блокировка, до DISPATCH_LEVEL).
#define BG_MAX_STREAMS  8                 // потоков на кольцо (в таблицах пинов максимум 6)
#define BG_MIX_WINDOW   4800              // насколько (кадры, 100 мс) поток может отставать от края или опережать его, дальше он прыгает на край
#define BG_HDR_STREAMS  14                // reserved[14 + i] = сколько потоков запущено на кольце i сейчас
#define BG_HDR_PEAK     28                // reserved[28 + i] = наибольшее число потоков на кольце i одновременно (с загрузки драйвера)
static KSPIN_LOCK       g_Lock[BG_RINGS];
static BG_STREAM*       g_Run[BG_RINGS][BG_MAX_STREAMS];   // запущенные потоки кольца
static ULONG            g_Count[BG_RINGS];                 // сколько их

static BG_RING GetRing(LONG i)
{
    BG_RING r;
    BYTE* p = (BYTE*)g_Mem + BG_HDR_BYTES + (SIZE_T)BG_STRIDE * (SIZE_T)i;
    r.ctl = (BG_RING_CTL*)p;
    r.data = (float*)(p + BG_CTL_BYTES);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Кольца (позиции те же, что в ShmRing из shm.h; x64: хранилища упорядочены, нужны барьеры компилятора и полный барьер на границах)
// ---------------------------------------------------------------------------------------------------------------------
// Читатель (запись): общий read_pos = самый отстающий курсор запущенных потоков. Только вперёд. Вызывать под замком кольца.
static VOID RecalcReadPos(LONG ring)
{
    BG_RING r = GetRing(ring);
    const ULONG w = r.ctl->write_pos;
    ULONG maxav = 0;
    BOOLEAN any = FALSE;
    for (ULONG i = 0; i < g_Count[ring]; ++i) {
        const BG_STREAM* s = g_Run[ring][i];
        if (!s->Capture) continue;
        ULONG av = w - s->Pos;
        if (av > BG_RING_FRAMES) av = BG_RING_FRAMES;
        if (!any || av > maxav) maxav = av;
        any = TRUE;
    }
    if (!any) return;
    const ULONG want = w - maxav;
    if ((LONG)(want - r.ctl->read_pos) > 0) {
        KeMemoryBarrier();
        r.ctl->read_pos = want;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Формат потока
// ---------------------------------------------------------------------------------------------------------------------
LONG BogestvRingFor(_In_ PCWSTR WaveName, _In_ BOOLEAN Capture)
{
    if (!g_Mem || !WaveName) return -1;
    static const struct { PCWSTR name; LONG ring; BOOLEAN capture; } kMap[] = {
        { L"WaveSpeaker",          0, FALSE },   // Input 1
        { L"WaveSpeakerHeadphone", 1, FALSE },   // Input 2
        { L"WaveHdmi",             2, FALSE },   // Input 3
        { L"WaveSpdif",            3, FALSE },   // Input 4
        { L"WaveInput5",           4, FALSE },   // Input 5
        { L"WaveInput6",           5, FALSE },   // Input 6
        { L"WaveMicIn",            6, TRUE  },   // Mix B1 Out (шина B1)
        { L"WaveMicArray1",        7, TRUE  },   // Mix B2 Out (шина B2)
        { L"WaveOut1",             8, TRUE  },   // Games Out (дорожка V1 до фейдера)
        { L"WaveOut2",             9, TRUE  },   // Music Out (V2)
        { L"WaveOut3",            10, TRUE  },   // Discord Out (V3)
        { L"WaveOut4",            11, TRUE  },   // Browser 1 Out (V4)
        { L"WaveOut5",            12, TRUE  },   // Donations Out (V5)
        { L"WaveOut6",            13, TRUE  },   // Browser 2 Out (V6)
    };
    for (ULONG i = 0; i < ARRAYSIZE(kMap); ++i)
        if (kMap[i].capture == Capture && wcscmp(kMap[i].name, WaveName) == 0) return kMap[i].ring;
    return -1;
}

VOID BogestvFillFmt(_Out_ BG_FMT* Fmt, _In_ const WAVEFORMATEXTENSIBLE* Wfx)
{
    Fmt->Channels = Wfx->Format.nChannels;
    Fmt->Bits = Wfx->Format.wBitsPerSample;
    Fmt->BlockAlign = Wfx->Format.nBlockAlign;
    Fmt->Rate = Wfx->Format.nSamplesPerSec;
    Fmt->IsFloat = FALSE;
    if (Wfx->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) Fmt->IsFloat = TRUE;
    else if (Wfx->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUIDAligned(Wfx->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) Fmt->IsFloat = TRUE;
}

static BOOLEAN FmtSupported(const BG_FMT* f)
{
    if (!f || f->Channels == 0 || f->BlockAlign == 0) return FALSE;
    if (f->IsFloat) return f->Bits == 32;
    return f->Bits == 16 || f->Bits == 24 || f->Bits == 32;
}

// Кадр из байтов потока в пару float.
static VOID FrameToFloat(const BYTE* p, const BG_FMT* f, float* l, float* r)
{
    const ULONG step = f->Bits / 8;
    float v[2] = { 0.0f, 0.0f };
    for (ULONG c = 0; c < 2 && c < f->Channels; ++c) {
        const BYTE* s = p + step * c;
        if (f->IsFloat) {
            v[c] = *(const float*)s;
        } else if (f->Bits == 16) {
            v[c] = (float)(*(const SHORT*)s) * (1.0f / 32768.0f);
        } else if (f->Bits == 24) {
            const LONG x = ((LONG)s[0] << 8 | (LONG)s[1] << 16 | (LONG)s[2] << 24) >> 8;
            v[c] = (float)x * (1.0f / 8388608.0f);
        } else {
            v[c] = (float)(*(const LONG*)s) * (1.0f / 2147483648.0f);
        }
    }
    if (f->Channels == 1) v[1] = v[0];
    *l = v[0]; *r = v[1];
}

static VOID FloatToFrame(BYTE* p, const BG_FMT* f, float l, float r)
{
    const ULONG step = f->Bits / 8;
    if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
    if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
    float v[2] = { l, r };
    if (f->Channels == 1) v[0] = 0.5f * (l + r);
    for (ULONG c = 0; c < f->Channels; ++c) {
        BYTE* d = p + step * c;
        const float x = c < 2 ? v[c] : 0.0f;
        if (f->IsFloat) {
            *(float*)d = x;
        } else if (f->Bits == 16) {
            *(SHORT*)d = (SHORT)(x * 32767.0f);
        } else if (f->Bits == 24) {
            const LONG y = (LONG)(x * 8388607.0f);
            d[0] = (BYTE)(y); d[1] = (BYTE)(y >> 8); d[2] = (BYTE)(y >> 16);
        } else {
            *(LONG*)d = (LONG)((double)x * 2147483647.0);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Потоки
// ---------------------------------------------------------------------------------------------------------------------
VOID BogestvStreamInit(_Out_ BG_STREAM* S, _In_ LONG Ring, _In_ BOOLEAN Capture)
{
    S->Ring = (Ring >= 0 && Ring < BG_RINGS) ? Ring : -1;
    S->Capture = Capture;
    S->Running = FALSE;
    S->Pos = 0;
    S->Primed = FALSE;
    S->Prime = BG_PRIME_BASE;
}

VOID BogestvStreamRun(_Inout_ BG_STREAM* S, _In_ BOOLEAN Run, _In_opt_ const BG_FMT* Fmt)
{
    if (!g_Mem || !S || S->Ring < 0 || S->Ring >= BG_RINGS) return;
    const LONG ring = S->Ring;
    BG_RING r = GetRing(ring);
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock[ring], &irql);
    if (Run) {
        if (!S->Running) {
            if (g_Count[ring] >= BG_MAX_STREAMS) {           // места нет: поток не регистрируем, его вызовы ничего не делают
                KeReleaseSpinLock(&g_Lock[ring], irql);
                return;
            }
            const ULONG w = r.ctl->write_pos;
            if (g_Count[ring] == 0 && S->Capture) {          // Output: первый поток выбрасывает старое, потом active = 1
                r.ctl->read_pos = w;
            }
            S->Pos = w;                                      // запись: читаем с самого нового; воспроизведение: пишем с края
            S->Primed = FALSE;                               // запись: ждём запас заново
            S->Prime = BG_PRIME_BASE;
            g_Run[ring][g_Count[ring]++] = S;
            S->Running = TRUE;
            g_Hdr->reserved[BG_HDR_STREAMS + ring] = g_Count[ring];
            if (g_Count[ring] > g_Hdr->reserved[BG_HDR_PEAK + ring]) g_Hdr->reserved[BG_HDR_PEAK + ring] = g_Count[ring];
        }
        g_Hdr->reserved[ring] = Fmt ? Fmt->Rate : 0;
        KeMemoryBarrier();
        r.ctl->active = 1;
    } else {
        if (S->Running) {
            for (ULONG i = 0; i < g_Count[ring]; ++i) {
                if (g_Run[ring][i] == S) {
                    g_Run[ring][i] = g_Run[ring][g_Count[ring] - 1];
                    g_Run[ring][g_Count[ring] - 1] = NULL;
                    --g_Count[ring];
                    break;
                }
            }
            S->Running = FALSE;
            g_Hdr->reserved[BG_HDR_STREAMS + ring] = g_Count[ring];
            if (S->Capture) RecalcReadPos(ring);             // остановленный читатель больше не держит кольцо
        }
        if (g_Count[ring] == 0) {                            // кольцо закрыто, когда остановился последний поток
            r.ctl->active = 0;
            g_Hdr->reserved[ring] = 0;
        }
    }
    KeMemoryBarrier();
    KeReleaseSpinLock(&g_Lock[ring], irql);
}

VOID BogestvRender(_Inout_ BG_STREAM* S, _In_ const BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt)
{
    if (!g_Mem || !S || S->Ring < 0 || S->Ring >= BG_RINGS || !FmtSupported(Fmt) || DmaSize == 0) return;
    InterlockedIncrement((volatile LONG*)&g_Hdr->driver_beat);
    const LONG ring = S->Ring;
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock[ring], &irql);
    if (!S->Running) {                                       // поток не запущен (или не влез в реестр): в кольцо не пишет
        KeReleaseSpinLock(&g_Lock[ring], irql);
        return;
    }
    KFLOATING_SAVE fs;
    if (!NT_SUCCESS(KeSaveFloatingPointState(&fs))) {
        KeReleaseSpinLock(&g_Lock[ring], irql);
        return;
    }
    BG_RING r = GetRing(ring);
    ULONG w = r.ctl->write_pos;                              // край кольца: дальше него ещё никто не писал
    const ULONG rd = r.ctl->read_pos;
    ULONG pos = S->Pos;
    ULONG lost = 0;
    // Слишком далеко от края (отстал или убежал вперёд): складывать уже не с чем, прыгаем на край.
    if ((LONG)(w - pos) > BG_MIX_WINDOW || (LONG)(pos - w) > BG_MIX_WINDOW) pos = w;

    float tmp[256 * 2];
    const ULONG ba = Fmt->BlockAlign;
    while (Frames > 0) {
        const ULONG off = (ULONG)((Frame0 * ba) % DmaSize);
        ULONG n = (DmaSize - off) / ba;                  // кадров до конца буфера
        if (n > Frames) n = Frames;
        if (n > 256) n = 256;
        if (n == 0) break;
        for (ULONG i = 0; i < n; ++i) FrameToFloat(Dma + off + (SIZE_T)i * ba, Fmt, &tmp[i * 2], &tmp[i * 2 + 1]);
        for (ULONG i = 0; i < n; ++i) {
            if ((LONG)(pos - w) < 0) {                   // ниже края: там уже лежит звук другого потока
                if ((LONG)(pos - rd) >= 0) {             // движок это ещё не прочитал: складываем
                    float* c = &r.data[(pos & (BG_RING_FRAMES - 1)) * 2];
                    c[0] += tmp[i * 2];
                    c[1] += tmp[i * 2 + 1];
                } else {
                    ++lost;                              // движок уже забрал этот кадр: опоздали
                }
                ++pos;
                continue;
            }
            // На краю или выше: дописываем (небольшой зазор до края заполняем тишиной).
            BOOLEAN full = FALSE;
            while ((LONG)(pos - w) > 0) {
                if (w - rd >= BG_RING_FRAMES) { full = TRUE; break; }
                float* z = &r.data[(w & (BG_RING_FRAMES - 1)) * 2];
                z[0] = 0.0f; z[1] = 0.0f;
                ++w;
            }
            if (full || w - rd >= BG_RING_FRAMES) {      // кольцо полно: кадр теряем, поток остаётся на краю
                ++lost;
                pos = w;
                continue;
            }
            float* c = &r.data[(w & (BG_RING_FRAMES - 1)) * 2];
            c[0] = tmp[i * 2];
            c[1] = tmp[i * 2 + 1];
            ++w;
            pos = w;
        }
        Frame0 += n; Frames -= n;
    }
    KeMemoryBarrier();
    r.ctl->write_pos = w;                                // данные записаны, теперь публикуем край
    S->Pos = pos;
    if (lost) InterlockedAdd((volatile LONG*)&r.ctl->drops, (LONG)lost);
    KeRestoreFloatingPointState(&fs);
    KeReleaseSpinLock(&g_Lock[ring], irql);
}

VOID BogestvCapture(_Inout_ BG_STREAM* S, _Inout_ BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt)
{
    if (!g_Mem || !S || S->Ring < 0 || S->Ring >= BG_RINGS || !FmtSupported(Fmt) || DmaSize == 0) return;
    InterlockedIncrement((volatile LONG*)&g_Hdr->driver_beat);
    const LONG ring = S->Ring;
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock[ring], &irql);
    KFLOATING_SAVE fs;
    if (!NT_SUCCESS(KeSaveFloatingPointState(&fs))) {
        KeReleaseSpinLock(&g_Lock[ring], irql);
        return;
    }
    BG_RING r = GetRing(ring);
    float tmp[256 * 2];
    const ULONG ba = Fmt->BlockAlign;

    // Запас (см. выше). Решение одно на всю порцию, а не на каждый кусок по 256 кадров. Считаем от СВОЕГО курсора потока.
    BOOLEAN hold = !S->Running;                          // TRUE: в этот раз отдаём тишину, кольцо не трогаем
    if (S->Running) {
        ULONG prime = S->Prime;
        if (prime < BG_PRIME_BASE) prime = BG_PRIME_BASE;
        const ULONG w = r.ctl->write_pos;
        ULONG av = w - S->Pos;
        if (av > BG_RING_FRAMES) { S->Pos = w; av = 0; S->Primed = FALSE; }
        if (!S->Primed) {
            if (av >= Frames + prime) S->Primed = TRUE;
            else hold = TRUE;                            // начальное ожидание нехваткой не считаем
        } else if (av < Frames) {                        // кольцо опустело: молчим один раз, запас растёт
            S->Primed = FALSE;
            S->Prime = (prime + BG_PRIME_STEP > BG_PRIME_MAX) ? BG_PRIME_MAX : prime + BG_PRIME_STEP;
            InterlockedAdd((volatile LONG*)&r.ctl->starved, (LONG)Frames);
            hold = TRUE;
        }
        if (!hold && av > Frames + prime + BG_TRIM_EXTRA) {   // часы разные: запас вырос, лишнее старое выбрасываем до нормы
            S->Pos += av - (Frames + prime);
        }
    }
    if (hold) {
        for (ULONG i = 0; i < 256 * 2; ++i) tmp[i] = 0.0f;
    }
    while (Frames > 0) {
        const ULONG off = (ULONG)((Frame0 * ba) % DmaSize);
        ULONG n = (DmaSize - off) / ba;
        if (n > Frames) n = Frames;
        if (n > 256) n = 256;
        if (n == 0) break;
        if (!hold) {                                     // до этого места проверено: в кольце не меньше Frames кадров от курсора потока
            for (ULONG i = 0; i < n; ++i) {
                const ULONG idx = (S->Pos + i) & (BG_RING_FRAMES - 1);
                tmp[i * 2]     = r.data[idx * 2];
                tmp[i * 2 + 1] = r.data[idx * 2 + 1];
            }
            S->Pos += n;
        }
        for (ULONG i = 0; i < n; ++i) FloatToFrame(Dma + off + (SIZE_T)i * ba, Fmt, tmp[i * 2], tmp[i * 2 + 1]);
        Frame0 += n; Frames -= n;
    }
    if (S->Running) RecalcReadPos(ring);                 // освободили место: общий read_pos = самый отстающий читатель
    KeRestoreFloatingPointState(&fs);
    KeReleaseSpinLock(&g_Lock[ring], irql);
}

// ---------------------------------------------------------------------------------------------------------------------
// Управляющее устройство
// ---------------------------------------------------------------------------------------------------------------------
static VOID UnmapOwner()
{
    if (!g_OwnerProc) return;
    ULONG_PTR apc[8];                                    // под KAPC_STATE (см. выше)
    const BOOLEAN attach = (PsGetCurrentProcess() != g_OwnerProc);
    if (attach) KeStackAttachProcess((PRKPROCESS)g_OwnerProc, &apc);
    if (g_UserVa) MmUnmapLockedPages(g_UserVa, g_Mdl);
    if (attach) KeUnstackDetachProcess(&apc);
    ObDereferenceObject(g_OwnerProc);
    g_UserVa = NULL; g_OwnerProc = NULL; g_OwnerFile = NULL;
    if (g_Hdr) g_Hdr->engine_pid = 0;
}

static NTSTATUS Complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

extern "C" DRIVER_DISPATCH BogestvDispatch;
extern "C" NTSTATUS BogestvDispatch(_In_ PDEVICE_OBJECT Dev, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (Dev != g_Dev) {                                  // не наше устройство: отдаём PortCls
        PDRIVER_DISPATCH old = g_Old[sp->MajorFunction];
        if (old) return old(Dev, Irp);
        return Complete(Irp, STATUS_NOT_SUPPORTED, 0);
    }
    switch (sp->MajorFunction) {
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        return Complete(Irp, STATUS_SUCCESS, 0);
    case IRP_MJ_CLEANUP:
        if (sp->FileObject == g_OwnerFile) UnmapOwner();
        return Complete(Irp, STATUS_SUCCESS, 0);
    case IRP_MJ_DEVICE_CONTROL:
        if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_BOGESTV_MAP_SHM) {
            if (sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(BOGESTV_MAP_OUT)) return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
            if (g_OwnerFile) return Complete(Irp, STATUS_DEVICE_BUSY, 0);
            PVOID va = NULL;
            __try {
                va = MmMapLockedPagesSpecifyCache(g_Mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                va = NULL;
            }
            if (!va) return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
            g_UserVa = va;
            g_OwnerProc = PsGetCurrentProcess();
            ObReferenceObject(g_OwnerProc);
            g_OwnerFile = sp->FileObject;
            BOGESTV_MAP_OUT* out = (BOGESTV_MAP_OUT*)Irp->AssociatedIrp.SystemBuffer;
            out->base = (unsigned long long)(ULONG_PTR)va;
            out->total_bytes = BG_TOTAL;
            out->version = BG_VERSION;
            return Complete(Irp, STATUS_SUCCESS, sizeof(BOGESTV_MAP_OUT));
        }
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    default:
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

NTSTATUS BogestvBridgeInit(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    g_Mem = ExAllocatePool2(POOL_FLAG_NON_PAGED, BG_TOTAL, BG_TAG);
    if (!g_Mem) return STATUS_INSUFFICIENT_RESOURCES;
    g_Mdl = IoAllocateMdl(g_Mem, BG_TOTAL, FALSE, FALSE, NULL);
    if (!g_Mdl) { ExFreePoolWithTag(g_Mem, BG_TAG); g_Mem = NULL; return STATUS_INSUFFICIENT_RESOURCES; }
    MmBuildMdlForNonPagedPool(g_Mdl);

    for (ULONG i = 0; i < BG_RINGS; ++i) {
        KeInitializeSpinLock(&g_Lock[i]);
        g_Count[i] = 0;
        for (ULONG j = 0; j < BG_MAX_STREAMS; ++j) g_Run[i][j] = NULL;
    }
    g_Hdr = (BG_SHM_HDR*)g_Mem;
    g_Hdr->version = BG_VERSION;
    g_Hdr->header_bytes = BG_HDR_BYTES;
    g_Hdr->ring_stride = BG_STRIDE;
    g_Hdr->in_rings = BG_IN_RINGS;
    g_Hdr->out_rings = BG_OUT_RINGS;
    g_Hdr->rate = BG_RATE;
    g_Hdr->ring_frames = BG_RING_FRAMES;
    g_Hdr->channels = 2;
    KeMemoryBarrier();
    g_Hdr->magic = BG_MAGIC;                             // последним: по нему заголовок считается готовым

    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\Bogestv0Mixer");
    // Система и администраторы: всё; вошедший пользователь (движок работает от него): чтение и запись.
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    NTSTATUS st = IoCreateDeviceSecure(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &sddl, NULL, &g_Dev);
    if (NT_SUCCESS(st)) st = IoCreateSymbolicLink(&g_Sym, &name);
    if (!NT_SUCCESS(st)) {
        DPF(D_ERROR, ("[Bogestv] control device failed 0x%x", st));
        if (g_Dev) { IoDeleteDevice(g_Dev); g_Dev = NULL; }
        // Память оставляем: потоки всё равно будут работать, только движок её не получит.
        return st;
    }
    g_Dev->Flags &= ~DO_DEVICE_INITIALIZING;
    g_Dev->Flags |= DO_BUFFERED_IO;

    // Перехват: наши IRP обрабатываем сами, остальное уходит прежним обработчикам (PortCls).
    const UCHAR majors[] = { IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_CLEANUP, IRP_MJ_DEVICE_CONTROL };
    for (ULONG i = 0; i < ARRAYSIZE(majors); ++i) {
        g_Old[majors[i]] = DriverObject->MajorFunction[majors[i]];
        DriverObject->MajorFunction[majors[i]] = BogestvDispatch;
    }
    DPF(D_TERSE, ("[Bogestv] bridge ready, %u bytes", (ULONG)BG_TOTAL));
    return STATUS_SUCCESS;
}

VOID BogestvBridgeUnload()
{
    PAGED_CODE();
    if (g_OwnerFile) UnmapOwner();
    if (g_Dev) { IoDeleteSymbolicLink(&g_Sym); IoDeleteDevice(g_Dev); g_Dev = NULL; }
    if (g_Mdl) { IoFreeMdl(g_Mdl); g_Mdl = NULL; }
    if (g_Mem) { PVOID m = g_Mem; g_Mem = NULL; g_Hdr = NULL; ExFreePoolWithTag(m, BG_TAG); }
}
