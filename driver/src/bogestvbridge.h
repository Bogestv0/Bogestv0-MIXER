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
// Поток запущен (Run = TRUE) или остановлен.
VOID BogestvStreamRun(_In_ LONG Ring, _In_ BOOLEAN Capture, _In_ BOOLEAN Run, _In_opt_ const BG_FMT* Fmt);
// Воспроизведение: переписать кадры [Frame0, Frame0 + Frames) из буфера DMA в кольцо.
VOID BogestvRender(_In_ LONG Ring, _In_ const BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt);
// Запись: заполнить кадры [Frame0, Frame0 + Frames) буфера DMA звуком из кольца (нехватка = тишина).
VOID BogestvCapture(_In_ LONG Ring, _Inout_ BYTE* Dma, _In_ ULONG DmaSize, _In_ ULONGLONG Frame0, _In_ ULONG Frames, _In_ const BG_FMT* Fmt);
