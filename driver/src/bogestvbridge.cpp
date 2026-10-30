// Мост между потоками SysVAD и общей памятью движка (этап 6.4).
//
// Что здесь происходит:
//  * При загрузке драйвер выделяет невыгружаемую память по раскладке engine/shm.h (заголовок + 14 колец) и создаёт управляющее
//    устройство \Device\Bogestv0Mixer (в Windows оно видно как \\.\Bogestv0Mixer).
//  * Движок вызывает IOCTL_BOGESTV_MAP_SHM: память отображается в его процесс (MmMapLockedPagesSpecifyCache). Закрыл дескриптор, и
//    отображение снимается в IRP_MJ_CLEANUP.
//  * Потоки воспроизведения (Input 1..6) переписывают звук из буфера DMA в кольца 0..5; потоки записи (шины B1, B2 и выходы дорожек Out 1..6)
//    заполняют буфер DMA звуком из колец 6..13. Формат колец: float32, 2 канала, 48 кГц. Преобразование из формата потока (PCM 16/24/32, float)
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
    ULONG reserved[52];            // reserved[i] = частота, с которой Windows открыла кольцо i (0 = закрыто); пишет драйвер
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

static BG_RING GetRing(LONG i)
{
    BG_RING r;
    BYTE* p = (BYTE*)g_Mem + BG_HDR_BYTES + (SIZE_T)BG_STRIDE * (SIZE_T)i;
    r.ctl = (BG_RING_CTL*)p;
    r.data = (float*)(p + BG_CTL_BYTES);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Кольца (те же правила, что в ShmRing из shm.h; x64: хранилища упорядочены, нужны барьеры компилятора и полный барьер на границах)
// ---------------------------------------------------------------------------------------------------------------------
static ULONG RingPush(BG_RING* r, const float* in, ULONG n)
{
    const ULONG w = r->ctl->write_pos;
    KeMemoryBarrier();
    const ULONG rd = r->ctl->read_pos;
    ULONG used = w - rd;
    if (used > BG_RING_FRAMES) used = BG_RING_FRAMES;
    const ULONG space = BG_RING_FRAMES - used;
    const ULONG m = n < space ? n : space;
    for (ULONG i = 0; i < m; ++i) {
        const ULONG idx = (w + i) & (BG_RING_FRAMES - 1);
        r->data[idx * 2]     = in[i * 2];
        r->data[idx * 2 + 1] = in[i * 2 + 1];
    }
    KeMemoryBarrier();
    r->ctl->write_pos = w + m;
    if (m < n) InterlockedAdd((volatile LONG*)&r->ctl->drops, (LONG)(n - m));
    return m;
}

// Читает до n кадров, остальное дополняет тишиной. trim: если накопилось больше trim + n, самое старое выбрасывается.
static VOID RingPop(BG_RING* r, float* out, ULONG n, ULONG trim)
{
    ULONG rd = r->ctl->read_pos;
    KeMemoryBarrier();
    const ULONG w = r->ctl->write_pos;
    ULONG av = w - rd;
    if (av > BG_RING_FRAMES) { rd = w; av = 0; }
    if (trim && av > trim + n) { rd += av - trim; av = trim; }
    const ULONG m = n < av ? n : av;
    for (ULONG i = 0; i < m; ++i) {
        const ULONG idx = (rd + i) & (BG_RING_FRAMES - 1);
        out[i * 2]     = r->data[idx * 2];
        out[i * 2 + 1] = r->data[idx * 2 + 1];
    }
    for (ULONG i = m * 2; i < n * 2; ++i) out[i] = 0.0f;
    KeMemoryBarrier();
    r->ctl->read_pos = rd + m;
    if (m < n) InterlockedAdd((volatile LONG*)&r->ctl->starved, (LONG)(n - m));
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
VOID BogestvStreamRun(_In_ LONG Ring_, _In_ BOOLEAN Capture, _In_ BOOLEAN Run, _In_opt_ const BG_FMT* Fmt)
{
    if (!g_Mem || Ring_ < 0 || Ring_ >= BG_RINGS) return;
    BG_RING r = GetRing(Ring_);
    if (Run) {
        if (Capture) {                                   // Output: выбросить старое, потом active = 1
            r.ctl->read_pos = r.ctl->write_pos;
        }
        g_Hdr->reserved[Ring_] = Fmt ? Fmt->Rate : 0;
        KeMemoryBarrier();
        r.ctl->active = 1;
    } else {
        r.ctl->active = 0;
        g_Hdr->reserved[Ring_] = 0;
    }
    KeMemoryBarrier();
}

VOID BogestvRender(_In_ LONG Ring_, _In_ const BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt)
{
    if (!g_Mem || Ring_ < 0 || Ring_ >= BG_RINGS || !FmtSupported(Fmt) || DmaSize == 0) return;
    InterlockedIncrement((volatile LONG*)&g_Hdr->driver_beat);
    KFLOATING_SAVE fs;
    if (!NT_SUCCESS(KeSaveFloatingPointState(&fs))) return;
    BG_RING r = GetRing(Ring_);
    float tmp[256 * 2];
    const ULONG ba = Fmt->BlockAlign;
    while (Frames > 0) {
        const ULONG off = (ULONG)((Frame0 * ba) % DmaSize);
        ULONG n = (DmaSize - off) / ba;                  // кадров до конца буфера
        if (n > Frames) n = Frames;
        if (n > 256) n = 256;
        if (n == 0) break;
        for (ULONG i = 0; i < n; ++i) FrameToFloat(Dma + off + (SIZE_T)i * ba, Fmt, &tmp[i * 2], &tmp[i * 2 + 1]);
        RingPush(&r, tmp, n);
        Frame0 += n; Frames -= n;
    }
    KeRestoreFloatingPointState(&fs);
}

VOID BogestvCapture(_In_ LONG Ring_, _Inout_ BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt)
{
    if (!g_Mem || Ring_ < 0 || Ring_ >= BG_RINGS || !FmtSupported(Fmt) || DmaSize == 0) return;
    InterlockedIncrement((volatile LONG*)&g_Hdr->driver_beat);
    KFLOATING_SAVE fs;
    if (!NT_SUCCESS(KeSaveFloatingPointState(&fs))) return;
    BG_RING r = GetRing(Ring_);
    float tmp[256 * 2];
    const ULONG ba = Fmt->BlockAlign;
    while (Frames > 0) {
        const ULONG off = (ULONG)((Frame0 * ba) % DmaSize);
        ULONG n = (DmaSize - off) / ba;
        if (n > Frames) n = Frames;
        if (n > 256) n = 256;
        if (n == 0) break;
        RingPop(&r, tmp, n, 4096);                       // запас не больше ~85 мс: часы разные, лишнее выбрасываем
        for (ULONG i = 0; i < n; ++i) FloatToFrame(Dma + off + (SIZE_T)i * ba, Fmt, tmp[i * 2], tmp[i * 2 + 1]);
        Frame0 += n; Frames -= n;
    }
    KeRestoreFloatingPointState(&fs);
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
