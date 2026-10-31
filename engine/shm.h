// Общая память движка и драйвера (этап 6.1).
//
// Драйвер Windows (виртуальные устройства) и движок обмениваются звуком через один участок общей памяти:
//   кольца 0..5 = «Bogestv0 Input 1..6» (V1..V6): пишет драйвер (то, что играют программы), читает движок;
//   кольца 6..7 = «Bogestv0 Mix B1/B2 Out» (шины B1, B2): пишет движок (микс шины), читает драйвер (это «микрофон» для OBS и Discord);
//   кольца 8..13 = «Bogestv0 <Дорожка> Out» (Out 1..6): пишет движок (звук дорожки V1..V6 до фейдера и Mute), читает драйвер (для OBS).
//   Версия раскладки 2 (в версии 1 было 8 колец).
// Звук: 32-битные числа с плавающей точкой, 2 канала вперемешку (L R L R ...), частота из заголовка (сейчас 48000).
//
// Правила обмена (их же должен соблюдать драйвер):
//   * В каждом кольце один пишущий и один читающий. Позиции write_pos / read_pos считают кадры с самого начала и
//     переполняются по кругу (uint32). Размер кольца степень двойки, поэтому разность позиций всегда верна.
//   * Пишущий сначала записывает данные, потом публикует write_pos (release). Читающий читает write_pos (acquire),
//     потом данные, потом публикует read_pos (release).
//   * `active` выставляет сторона драйвера, когда у неё открыт поток на этом кольце (Windows открыл устройство).
//     Кольца входов: драйвер ставит active=1 и пишет; движок видит 0->1 и выбрасывает накопленное.
//     Кольца выходов: драйвер ПЕРЕД active=1 ставит read_pos=write_pos (выбрасывает старое). Пока active=0, движок не пишет.
//   * engine_beat растёт с каждым блоком, который обработал движок; driver_beat растёт с каждым периодом драйвера.
//     Если счётчик стоит на месте больше ~2 секунд, другая сторона считается отсутствующей.
//   * Нехватка данных заполняется тишиной, переполнение отбрасывает лишнее. Оба случая считаются в drops / starved.
//
// Откуда берётся память (этап 6.4):
//   * Windows с установленным драйвером: память создаёт ДРАЙВЕР. Движок открывает \\.\Bogestv0Mixer и одним IOCTL получает адрес
//     (см. driver/src/bogestv_ioctl.h). Заголовок и кольца готовит драйвер; reserved[i] = частота, с которой Windows открыла кольцо i;
//     reserved[14 + i] = сколько потоков Windows сейчас запущено на кольце i; reserved[28 + i] = наибольшее их число одновременно (0.5.5).
//     На одном устройстве Windows может открыть несколько потоков: драйвер складывает воспроизведение и раздаёт запись каждому потоку своим курсором.
//     Для движка правила прежние: у кольца один пишущий и один читающий, active = 1, пока запущен хотя бы один поток.
//   * Иначе (нет драйвера, Linux, проверки, --fake-driver): память создаёт сам движок, как в 6.1.
//
// Раскладка памяти (все числа little-endian, как есть на x64):
//   [0, 256)                         заголовок ShmHeader
//   [256 + i * kShmRingStride, ...)  кольцо i: управляющий блок ShmRingCtl (128 байт), затем кадры (кадров * 2 * 4 байта)
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <sddl.h>
#include "../driver/src/bogestv_ioctl.h"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bg {

constexpr uint32_t kShmMagic = 0x58474642u;       // "BFGX"
constexpr uint32_t kShmVersion = 2;
constexpr int kShmInRings = 6;                    // V1..V6
constexpr int kShmBusRings = 2;                   // B1, B2: микс шины
constexpr int kShmTapRings = 6;                   // Out 1..6: чистый звук дорожки V1..V6 до фейдера (для OBS)
constexpr int kShmOutRings = kShmBusRings + kShmTapRings;
constexpr int kShmRings = kShmInRings + kShmOutRings;
constexpr uint32_t kShmRingFrames = 8192;         // степень двойки: ~170 мс при 48 кГц
constexpr uint32_t kShmRate = 48000;
constexpr size_t kShmHeaderBytes = 256;
constexpr size_t kShmCtlBytes = 128;
constexpr size_t kShmRingStride = kShmCtlBytes + static_cast<size_t>(kShmRingFrames) * 2 * sizeof(float);
constexpr size_t kShmTotalBytes = kShmHeaderBytes + kShmRingStride * kShmRings;
static const char* const kShmDefaultName = "Bogestv0MixerAudio";

static_assert(std::atomic<uint32_t>::is_always_lock_free, "нужны атомарные uint32 без блокировок");
static_assert((kShmRingFrames & (kShmRingFrames - 1)) == 0, "размер кольца должен быть степенью двойки");

struct ShmHeader {
    std::atomic<uint32_t> magic;     // ставится последним: по нему видно, что заголовок готов
    uint32_t version;
    uint32_t header_bytes;
    uint32_t ring_stride;            // байт на одно кольцо (управляющий блок + данные)
    uint32_t in_rings;
    uint32_t out_rings;
    uint32_t rate;                   // Гц
    uint32_t ring_frames;
    uint32_t channels;
    uint32_t engine_pid;             // 0 = движок не запущен
    std::atomic<uint32_t> engine_beat;
    std::atomic<uint32_t> driver_beat;
    uint32_t reserved[52];
};
static_assert(sizeof(ShmHeader) == kShmHeaderBytes, "заголовок общей памяти должен быть 256 байт");

struct ShmRingCtl {
    alignas(64) std::atomic<uint32_t> write_pos;
    std::atomic<uint32_t> drops;       // сколько кадров отброшено из-за переполнения (считает пишущий)
    alignas(64) std::atomic<uint32_t> read_pos;
    std::atomic<uint32_t> active;      // 1 = сторона драйвера держит поток открытым
    std::atomic<uint32_t> starved;     // сколько кадров дополнено тишиной (считает читающий)
};
static_assert(sizeof(ShmRingCtl) == kShmCtlBytes, "управляющий блок кольца должен быть 128 байт");

// Вид на кольцо для читателя с запасом (inbuf.h): данные, размер (степень двойки) и позиции чтения / записи
// (счёт кадров с переполнением uint32).
struct RingView { const float* data; uint32_t cap; uint32_t r; uint32_t w; };

// Вид на одно кольцо. Не владеет памятью.
struct ShmRing {
    ShmRingCtl* ctl = nullptr;
    float* data = nullptr;
    uint32_t cap = 0;

    // Пишет до n кадров. Возвращает, сколько записано (остальное отброшено и посчитано в drops).
    uint32_t push(const float* in, uint32_t n) {
        const uint32_t w = ctl->write_pos.load(std::memory_order_relaxed);
        const uint32_t r = ctl->read_pos.load(std::memory_order_acquire);
        uint32_t used = w - r;
        if (used > cap) used = cap;                         // испорченные позиции: считаем кольцо полным
        const uint32_t space = cap - used;
        const uint32_t m = n < space ? n : space;
        for (uint32_t i = 0; i < m; ++i) {
            const uint32_t idx = (w + i) & (cap - 1);
            data[idx * 2] = in[i * 2];
            data[idx * 2 + 1] = in[i * 2 + 1];
        }
        ctl->write_pos.store(w + m, std::memory_order_release);
        if (m < n) ctl->drops.fetch_add(n - m, std::memory_order_relaxed);
        return m;
    }

    // Читает ровно n кадров (нехватку дополняет тишиной). Если trim > 0 и накопилось больше trim + n кадров,
    // самое старое выбрасывается: часы двух сторон расходятся, и запас не должен расти бесконечно.
    // Возвращает, сколько настоящих кадров прочитано.
    // trimmed (необязательно): сюда прибавляется, сколько кадров выброшено из-за trim.
    uint32_t pop(float* out, uint32_t n, uint32_t trim = 0, uint32_t* trimmed = nullptr) {
        uint32_t r = ctl->read_pos.load(std::memory_order_relaxed);
        const uint32_t w = ctl->write_pos.load(std::memory_order_acquire);
        uint32_t av = w - r;
        if (av > cap) { r = w; av = 0; }                    // испорченные позиции: начинаем с чистого
        if (trim && av > trim + n) { if (trimmed) *trimmed += av - trim; r += av - trim; av = trim; }
        const uint32_t m = n < av ? n : av;
        for (uint32_t i = 0; i < m; ++i) {
            const uint32_t idx = (r + i) & (cap - 1);
            out[i * 2] = data[idx * 2];
            out[i * 2 + 1] = data[idx * 2 + 1];
        }
        for (uint32_t i = m * 2; i < n * 2; ++i) out[i] = 0.0f;
        ctl->read_pos.store(r + m, std::memory_order_release);
        if (m < n) ctl->starved.fetch_add(n - m, std::memory_order_relaxed);
        return m;
    }

    // Для читателя с запасом (inbuf.h).
    RingView view() const { return RingView{data, cap, ctl->read_pos.load(std::memory_order_relaxed), ctl->write_pos.load(std::memory_order_acquire)}; }
    void commit(uint32_t new_read_pos) { ctl->read_pos.store(new_read_pos, std::memory_order_release); }
    void on_silence(uint32_t n) { ctl->starved.fetch_add(n, std::memory_order_relaxed); }

    // Выбросить накопленное. Только для читающей стороны.
    void discard() { ctl->read_pos.store(ctl->write_pos.load(std::memory_order_acquire), std::memory_order_release); }
    uint32_t available() const { return ctl->write_pos.load(std::memory_order_acquire) - ctl->read_pos.load(std::memory_order_acquire); }
    bool active() const { return ctl->active.load(std::memory_order_acquire) != 0; }
    void set_active(bool on) { ctl->active.store(on ? 1u : 0u, std::memory_order_release); }
};

// Отображение общей памяти. Движок создаёт (или переиспользует) её, «драйвер» (в тестах и в --fake-driver) открывает готовую.
class Shm {
public:
    enum class Role { Engine, Peer };

    ~Shm() {
        if (hdr_ && role_ == Role::Engine) hdr_->engine_pid = 0;   // чисто закрылись: драйверу видно, что движка нет
        unmap();
    }

    // name: короткое имя без префиксов. В Windows пробуем «Global\имя», затем «Local\имя»; в Linux это /dev/shm/имя.
    // Для роли Engine: если память уже есть и в ней рабочий заголовок, он сохраняется (драйвер мог её уже открыть),
    // а позиции колец входов сбрасываются. Если в ней работает другой движок, возвращает nullptr.
    static std::unique_ptr<Shm> open(const std::string& name, Role role, std::string& err) {
        std::unique_ptr<Shm> s(new Shm());
        s->role_ = role;
        if (!s->map(name, role == Role::Engine, err)) return nullptr;
        ShmHeader* h = s->hdr_;
        if (role == Role::Engine && s->via_driver_) {
            // Заголовок готовит драйвер. Если он не тот, ничего не пишем в память ядра.
            if (h->magic.load(std::memory_order_acquire) != kShmMagic || h->version != kShmVersion ||
                h->ring_frames != kShmRingFrames || h->ring_stride != kShmRingStride) {
                err = "драйвер вернул память с неверной раскладкой (версия драйвера не совпадает с движком)";
                return nullptr;
            }
            const uint32_t b0 = h->engine_beat.load(std::memory_order_relaxed);
            if (h->engine_pid != 0) {   // прежний движок мог упасть, не обнулив pid; драйвер пускает одного, поэтому это след
                sleep_ms(150);
                if (h->engine_beat.load(std::memory_order_relaxed) != b0) { err = "общая память уже занята другим движком"; return nullptr; }
            }
            for (int i = 0; i < kShmInRings; ++i) s->ring(i).discard();
            h->engine_pid = current_pid();
        } else if (role == Role::Engine) {
            if (h->magic.load(std::memory_order_acquire) == kShmMagic && h->version == kShmVersion && h->ring_frames == kShmRingFrames) {
                // Память осталась от прежнего движка. Если ею пользуется живой, его счётчик идёт.
                const uint32_t b0 = h->engine_beat.load(std::memory_order_relaxed);
                if (h->engine_pid != 0) {
                    sleep_ms(150);
                    if (h->engine_beat.load(std::memory_order_relaxed) != b0) { err = "общая память уже занята другим движком"; return nullptr; }
                }
                for (int i = 0; i < kShmInRings; ++i) s->ring(i).discard();   // входы читает движок: старый звук не нужен
            } else {
                std::memset(static_cast<void*>(s->base_), 0, kShmTotalBytes);
                h->version = kShmVersion;
                h->header_bytes = static_cast<uint32_t>(kShmHeaderBytes);
                h->ring_stride = static_cast<uint32_t>(kShmRingStride);
                h->in_rings = kShmInRings;
                h->out_rings = kShmOutRings;
                h->rate = kShmRate;
                h->ring_frames = kShmRingFrames;
                h->channels = 2;
                h->magic.store(kShmMagic, std::memory_order_release);
            }
            h->engine_pid = current_pid();
        } else {
            if (h->magic.load(std::memory_order_acquire) != kShmMagic || h->version != kShmVersion ||
                h->ring_frames != kShmRingFrames || h->ring_stride != kShmRingStride) {
                err = "в общей памяти нет заголовка движка (или другая версия)";
                return nullptr;
            }
        }
        return s;
    }

    ShmHeader* header() const { return hdr_; }
    ShmRing ring(int i) const {
        ShmRing r;
        unsigned char* p = base_ + kShmHeaderBytes + kShmRingStride * static_cast<size_t>(i);
        r.ctl = reinterpret_cast<ShmRingCtl*>(p);
        r.data = reinterpret_cast<float*>(p + kShmCtlBytes);
        r.cap = kShmRingFrames;
        return r;
    }
    ShmRing in(int i) const { return ring(i); }                       // V1..V6: пишет драйвер
    ShmRing out(int i) const { return ring(kShmInRings + i); }        // 0,1 = B1, B2; 2..7 = Out 1..6: пишет движок
    const std::string& where() const { return where_; }
    // Память пришла от драйвера (а не создана движком).
    bool via_driver() const { return via_driver_; }
    // Управляющее устройство драйвера нашлось в системе (даже если память взять не удалось).
    bool driver_found() const { return driver_found_; }
    // Если драйвер найден, но память от него не получена: почему (для сообщения в окне).
    const std::string& driver_note() const { return driver_note_; }

    static void sleep_ms(unsigned ms) {
#ifdef _WIN32
        Sleep(ms);
#else
        usleep(ms * 1000u);
#endif
    }

private:
    Shm() = default;
    Shm(const Shm&) = delete;
    Shm& operator=(const Shm&) = delete;

    Role role_ = Role::Engine;
    unsigned char* base_ = nullptr;
    ShmHeader* hdr_ = nullptr;
    std::string where_;
    bool via_driver_ = false;
    bool driver_found_ = false;
    std::string driver_note_;
#ifdef _WIN32
    HANDLE h_ = nullptr;
    HANDLE dev_ = nullptr;     // дескриптор устройства драйвера: пока он открыт, отображение живёт
#else
    int fd_ = -1;
    std::string path_;
#endif

    static uint32_t current_pid() {
#ifdef _WIN32
        return static_cast<uint32_t>(GetCurrentProcessId());
#else
        return static_cast<uint32_t>(getpid());
#endif
    }

#ifdef _WIN32
    // Память от драйвера. true = получили. Если устройства нет, driver_found_ остаётся false.
    bool map_driver() {
        HANDLE d = CreateFileW(BOGESTV_DEVICE_USER_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (d == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
                driver_found_ = true;
                driver_note_ = "устройство драйвера есть, но не открылось (код Windows " + std::to_string(static_cast<unsigned long>(e)) + ")";
            }
            return false;
        }
        driver_found_ = true;
        BOGESTV_MAP_OUT out = {};
        DWORD got = 0;
        if (!DeviceIoControl(d, IOCTL_BOGESTV_MAP_SHM, nullptr, 0, &out, sizeof(out), &got, nullptr)) {
            const DWORD e = GetLastError();
            driver_note_ = (e == ERROR_BUSY || e == ERROR_DEVICE_IN_USE)
                ? "драйвер уже отдал память другому движку"
                : "драйвер не отдал память (код Windows " + std::to_string(static_cast<unsigned long>(e)) + ")";
            CloseHandle(d);
            return false;
        }
        if (got < sizeof(out) || out.base == 0 || out.total_bytes < kShmTotalBytes) {
            driver_note_ = "драйвер вернул память неверного размера";
            CloseHandle(d);
            return false;
        }
        dev_ = d;
        base_ = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(out.base));
        hdr_ = reinterpret_cast<ShmHeader*>(base_);
        where_ = "драйвер (Bogestv0Mixer)";
        via_driver_ = true;
        return true;
    }
    bool map(const std::string& name, bool create, std::string& err) {
        if (create && map_driver()) return true;
        const char* prefixes[] = {"Global\\", "Local\\"};
        DWORD last = 0;
        for (const char* pre : prefixes) {
            std::string full = std::string(pre) + name;
            std::wstring wname(full.begin(), full.end());   // имена только из ASCII
            HANDLE h = nullptr;
            if (create) {
                // Доступ: система, администраторы и владелец процесса. Чужие программы не должны подсовывать звук.
                SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
                PSECURITY_DESCRIPTOR sd = nullptr;
                if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)", SDDL_REVISION_1, &sd, nullptr))
                    sa.lpSecurityDescriptor = sd;
                h = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.lpSecurityDescriptor ? &sa : nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(kShmTotalBytes), wname.c_str());
                if (sd) LocalFree(sd);
            } else {
                h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
            }
            if (!h) { last = GetLastError(); continue; }
            void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, kShmTotalBytes);
            if (!p) { last = GetLastError(); CloseHandle(h); continue; }
            h_ = h;
            base_ = static_cast<unsigned char*>(p);
            hdr_ = reinterpret_cast<ShmHeader*>(base_);
            where_ = full;
            return true;
        }
        err = "не удалось открыть общую память (код Windows " + std::to_string(static_cast<unsigned long>(last)) + ")";
        return false;
    }
    void unmap() {
        if (dev_) CloseHandle(dev_);       // драйвер сам снимает отображение при закрытии дескриптора
        else if (base_) UnmapViewOfFile(base_);
        if (h_) CloseHandle(h_);
        base_ = nullptr; hdr_ = nullptr; h_ = nullptr; dev_ = nullptr;
    }
public:
    static void unlink_name(const std::string&) {}   // в Windows память исчезает сама, когда закрыта последняя ссылка
#else
    bool map(const std::string& name, bool create, std::string& err) {
        path_ = "/" + name;
        int fd = create ? shm_open(path_.c_str(), O_CREAT | O_RDWR, 0600) : shm_open(path_.c_str(), O_RDWR, 0600);
        if (fd < 0) { err = "не удалось открыть общую память " + path_; return false; }
        if (create && ftruncate(fd, static_cast<off_t>(kShmTotalBytes)) != 0) { close(fd); err = "ftruncate не удался"; return false; }
        void* p = mmap(nullptr, kShmTotalBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { close(fd); err = "mmap не удался"; return false; }
        fd_ = fd;
        base_ = static_cast<unsigned char*>(p);
        hdr_ = reinterpret_cast<ShmHeader*>(base_);
        where_ = "/dev/shm" + path_;
        return true;
    }
    void unmap() {
        if (base_) munmap(base_, kShmTotalBytes);
        if (fd_ >= 0) close(fd_);
        base_ = nullptr; hdr_ = nullptr; fd_ = -1;
    }
public:
    // Только для проверок: убрать файл общей памяти из системы (в Windows память исчезает сама, когда закрыта последняя ссылка).
    static void unlink_name(const std::string& name) { shm_unlink(("/" + name).c_str()); }
#endif
};

}  // namespace bg
