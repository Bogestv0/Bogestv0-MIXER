#pragma once
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cstddef>
#include <cstdlib>
typedef uint32_t ULONG; typedef int32_t LONG; typedef uint8_t BYTE; typedef uint8_t UCHAR; typedef void* PVOID; typedef size_t SIZE_T; typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR; typedef int16_t SHORT; typedef bool BOOLEAN; typedef const wchar_t* PCWSTR; typedef int32_t NTSTATUS; typedef uint16_t WORD;
#define VOID void
#define TRUE true
#define FALSE false
#define NULL 0
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#define C_ASSERT(e) static_assert(e,"")
#define DECLSPEC_ALIGN(n) alignas(n)
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define PAGED_CODE()
#define DPF(a,b)
#define D_ERROR 0
#define D_TERSE 0
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010)
#define POOL_FLAG_NON_PAGED 1
#define DO_DEVICE_INITIALIZING 0x80
#define DO_BUFFERED_IO 4
#define IRP_MJ_CREATE 0
#define IRP_MJ_CLOSE 2
#define IRP_MJ_DEVICE_CONTROL 14
#define IRP_MJ_CLEANUP 18
#define IRP_MJ_MAXIMUM_FUNCTION 27
#define FILE_DEVICE_UNKNOWN 0x22
#define FILE_DEVICE_SECURE_OPEN 0x100
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0
#define CTL_CODE(a,b,c,d) (((a)<<16)|((d)<<14)|((b)<<2)|(c))
#define IO_NO_INCREMENT 0
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#define UserMode 1
#define MmCached 1
#define NormalPagePriority 16
#define MdlMappingNoExecute 0x40000000
#define __try if(true)
#define __except(x) else
#define EXCEPTION_EXECUTE_HANDLER 1
#define RTL_CONSTANT_STRING(s) {0,0,(wchar_t*)s}
struct UNICODE_STRING { uint16_t Length, Max; wchar_t* Buffer; };
struct GUID { int x; };
inline bool IsEqualGUIDAligned(const GUID&, const GUID&) { return false; }
static GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
struct WAVEFORMATEX { WORD wFormatTag, nChannels; ULONG nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; };
struct WAVEFORMATEXTENSIBLE { WAVEFORMATEX Format; union { WORD wValidBitsPerSample; } Samples; ULONG dwChannelMask; GUID SubFormat; };
struct KFLOATING_SAVE { int x; };
inline NTSTATUS KeSaveFloatingPointState(KFLOATING_SAVE*) { return 0; }
inline void KeRestoreFloatingPointState(KFLOATING_SAVE*) {}
inline void KeMemoryBarrier() { __sync_synchronize(); }
inline LONG InterlockedAdd(volatile LONG* p, LONG v) { return __sync_add_and_fetch(p, v); }
inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p, 1); }
struct DRIVER_OBJECT; typedef DRIVER_OBJECT* PDRIVER_OBJECT;
struct IRP; typedef IRP* PIRP; struct DEVICE_OBJECT; typedef DEVICE_OBJECT* PDEVICE_OBJECT; struct FILE_OBJECT; typedef FILE_OBJECT* PFILE_OBJECT;
typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef NTSTATUS DRIVER_DISPATCH(PDEVICE_OBJECT, PIRP);
struct DRIVER_OBJECT { PDRIVER_DISPATCH MajorFunction[28]; };
struct DEVICE_OBJECT { ULONG Flags; };
struct IO_STACK_LOCATION { UCHAR MajorFunction; PFILE_OBJECT FileObject; struct { struct { ULONG OutputBufferLength, IoControlCode; } DeviceIoControl; } Parameters; };
typedef IO_STACK_LOCATION* PIO_STACK_LOCATION;
struct IRP { struct { NTSTATUS Status; ULONG_PTR Information; } IoStatus; struct { void* SystemBuffer; } AssociatedIrp; IO_STACK_LOCATION sp; };
inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP i) { return &i->sp; }
inline void IoCompleteRequest(PIRP, int) {}
struct MDL { int x; }; typedef MDL* PMDL; typedef void* PEPROCESS; typedef void* PRKPROCESS; 
inline PVOID ExAllocatePool2(int, SIZE_T n, ULONG) { return calloc(1, n); }
inline void ExFreePoolWithTag(PVOID p, ULONG) { free(p); }
inline PMDL IoAllocateMdl(PVOID, ULONG, bool, bool, PIRP) { return new MDL; }
inline void MmBuildMdlForNonPagedPool(PMDL) {}
inline void IoFreeMdl(PMDL m) { delete m; }
static PVOID g_fakeUser = (PVOID)0x10000;
inline PVOID MmMapLockedPagesSpecifyCache(PMDL, int, int, PVOID, bool, int) { return g_fakeUser; }
inline void MmUnmapLockedPages(PVOID, PMDL) {}
inline PEPROCESS PsGetCurrentProcess() { return (PEPROCESS)1; }
inline void ObReferenceObject(PEPROCESS) {} inline void ObDereferenceObject(PEPROCESS) {}
#define NTKERNELAPI
extern "C" inline void KeStackAttachProcess(PRKPROCESS, PVOID) {} extern "C" inline void KeUnstackDetachProcess(PVOID) {}
static DEVICE_OBJECT g_fakeDev;
inline NTSTATUS IoCreateDeviceSecure(PDRIVER_OBJECT, ULONG, UNICODE_STRING*, ULONG, ULONG, bool, UNICODE_STRING*, void*, PDEVICE_OBJECT* d) { *d = &g_fakeDev; return 0; }
inline NTSTATUS IoCreateSymbolicLink(UNICODE_STRING*, UNICODE_STRING*) { return 0; }
inline void IoDeleteSymbolicLink(UNICODE_STRING*) {} inline void IoDeleteDevice(PDEVICE_OBJECT) {}
