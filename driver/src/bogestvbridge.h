// Мост между потоками SysVAD и общей памятью движка (этап 6.4). Реализация: bogestvbridge.cpp.
#pragma once

typedef struct _BG_FMT {
    ULONG    Channels;
    ULONG    Bits;          // бит в контейнере: 16, 24, 32
    ULONG    BlockAlign;
    ULONG    Rate;
    BOOLEAN  IsFloat;
} BG_FMT;

// Создаёт общую память и управляющее устройство. Вызывать из DriverEntry после PcInitializeAdapterDriver.
NTSTATUS BogestvBridgeInit(_In_ PDRIVER_OBJECT DriverObject);
// Убирает управляющее устройство и память. Вызывать из DriverUnload.
VOID BogestvBridgeUnload();

// Номер кольца по имени волнового подустройства; -1, если поток не наш. capture: TRUE для записи («Output»).
LONG BogestvRingFor(_In_ PCWSTR WaveName, _In_ BOOLEAN Capture);
VOID BogestvFillFmt(_Out_ BG_FMT* Fmt, _In_ const WAVEFORMATEXTENSIBLE* Wfx);
// Состояние одного потока Windows на кольце (0.5.5). На одном устройстве Windows может открыть несколько потоков сразу (по таблицам
// пинов SysVAD: динамики и наушники до 6, HDMI/SPDIF и Input 5/6 до 2, запись до 5), и у каждого свои позиции, поэтому состояние живёт
// в самом потоке, а не в кольце. Лежит внутри CMiniportWaveRTStream; все поля трогает только мост под замком кольца.
typedef struct _BG_STREAM {
    LONG     Ring;          // номер кольца; -1 = поток не наш
    BOOLEAN  Capture;       // TRUE: запись (читает кольцо), FALSE: воспроизведение (пишет в кольцо)
    BOOLEAN  Running;       // зарегистрирован на кольце (KSSTATE_RUN)
    ULONG    Pos;           // воспроизведение: куда этот поток пишет следующий кадр; запись: откуда читает (его курсор)
    BOOLEAN  Primed;        // запись: запас набран, звук идёт
    ULONG    Prime;         // запись: текущий запас (кадры)
} BG_STREAM;

// Привязывает состояние потока к кольцу (при создании потока). Ring < 0 = не наш поток.
VOID BogestvStreamInit(_Out_ BG_STREAM* S, _In_ LONG Ring, _In_ BOOLEAN Capture);
// Поток запущен (Run = TRUE) или остановлен. Можно звать сколько угодно раз подряд (SetState приходит на каждое состояние и из деструктора):
// поток регистрируется один раз, кольцо считается открытым, пока запущен хотя бы один поток.
VOID BogestvStreamRun(_Inout_ BG_STREAM* S, _In_ BOOLEAN Run, _In_opt_ const BG_FMT* Fmt);
// Воспроизведение: переписать кадры [Frame0, Frame0 + Frames) из буфера DMA в кольцо. Звук нескольких потоков одного кольца складывается.
VOID BogestvRender(_Inout_ BG_STREAM* S, _In_ const BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt);
// Запись: заполнить кадры [Frame0, Frame0 + Frames) буфера DMA звуком из кольца (нехватка = тишина). Каждый поток читает кольцо своим курсором,
// все получают один и тот же звук.
VOID BogestvCapture(_Inout_ BG_STREAM* S, _Inout_ BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt);
