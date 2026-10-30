// Общий заголовок драйвера и движка (этап 6.4): как движок получает общую память у драйвера.
// Драйвер создаёт управляющее устройство \\.\Bogestv0Mixer. Движок открывает его и один раз вызывает IOCTL_BOGESTV_MAP_SHM:
// драйвер отображает свою общую память (раскладка в engine/shm.h) в адресное пространство вызвавшего процесса и возвращает адрес.
// Отображение живёт, пока открыт дескриптор: закрыли устройство (или процесс завершился), драйвер само убирает отображение.
// Одновременно память может держать только один процесс (второй получит «занято»).
// Файл подключают и драйвер (ядро), и движок (Windows): CTL_CODE берётся из winioctl.h / ntddk.h, подключённых до него.
#pragma once

#define BOGESTV_DEVICE_USER_PATH L"\\\\.\\Bogestv0Mixer"

#define IOCTL_BOGESTV_MAP_SHM CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)
typedef struct _BOGESTV_MAP_OUT {
    unsigned long long base;         // адрес памяти в процессе-вызывающем
    unsigned int total_bytes;        // размер (должен совпасть с kShmTotalBytes)
    unsigned int version;            // kShmVersion
} BOGESTV_MAP_OUT;
#pragma pack(pop)
