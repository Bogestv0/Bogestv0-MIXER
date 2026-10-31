// Режим --serve (этап 3): микшер, которым управляет окно через строки в stdin/stdout.
//
// Дорожки (8): 0,1 = HW1, HW2 (микрофоны), 2..7 = V1..V6 (виртуальные; пока тест-источники).
// Шины (5): 0..2 = A1..A3, 3..4 = B1, B2.
// Слоты устройств (7): hw1 hw2 a1 a2 a3 b1 b2.
//
// Команды (по одной в строке):
//   get | devices | quit
//   set strip I gain ДБ | mute 0/1 | solo 0/1 | route K 0/1 | src off/sine/noise/file | file ПУТЬ
//   set bus K gain ДБ | mute 0/1
//   set mic I gain 0..30 | gate -80..-20 (-80 = выкл) | hpf 20..300 (20 = выкл) | comp 1..10 (1 = выкл)
//                | ns 0..3 (выкл / мягкое / сильное / максимум) | nsamt 0..100    (I = 0 или 1, микрофон)
//   set dev СЛОТ ИМЯ_УСТРОЙСТВА   (пустое имя или "-" отключает слот)
// Ответы: строки JSON (hello, devices, state, m, err).
// Драйвер (этап 6.1): если в общей памяти (shm.h) драйвер держит поток на «Bogestv0 Input N», дорожка V(N) берёт звук оттуда
// (когда её тест-источник выключен). Шины B1 и B2 дополнительно пишутся в «Bogestv0 Output 1/2», пока драйвер открыл поток.
// Устройства (этап 5): сторож раз в ~2 секунды проверяет назначенные устройства. Выдернули наушники, и слот освободился;
// вставили, и он открылся сам, а остальной звук не прерывается. Если пропало ведущее устройство (первая рабочая шина A/B),
// тракт собирается заново.
#pragma once

#include "dsp.h"
#include "inbuf.h"
#include "mic.h"
#include "shm.h"
#include "third_party/miniaudio.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bg {

constexpr int kStrips = 8;
constexpr int kBuses = 5;
constexpr int kSlots = 7;
constexpr int kMics = 2;
constexpr size_t kMaxBlock = 4096;  // самый большой блок за один проход
static const char* kSlotNames[kSlots] = {"hw1", "hw2", "a1", "a2", "a3", "b1", "b2"};
static const char* kEngineVersion = "0.3.0";

// Кольцо «один пишет, один читает» для стерео-кадров. Связывает потоки разных устройств.
struct Ring {
    std::vector<float> buf;
    size_t cap = 0;
    std::atomic<size_t> r{0}, w{0};

    void init(size_t frames) { cap = frames; buf.assign(frames * 2, 0.0f); r = 0; w = 0; }   // frames: степень двойки

    // Для читателя с запасом (inbuf.h). Позиции r и w у кольца 64-битные, читателю нужны младшие 32 бита: размер кольца степень двойки.
    RingView view() const {
        return RingView{buf.data(), static_cast<uint32_t>(cap), static_cast<uint32_t>(r.load(std::memory_order_relaxed)),
                        static_cast<uint32_t>(w.load(std::memory_order_acquire))};
    }
    void commit(uint32_t new_read_pos) {
        const size_t cur = r.load(std::memory_order_relaxed);
        r.store(cur + static_cast<uint32_t>(new_read_pos - static_cast<uint32_t>(cur)), std::memory_order_release);
    }
    void on_silence(uint32_t) {}

    // Выбросить накопленное. Вызывать, только когда читающее устройство остановлено.
    void clear() { r.store(w.load(std::memory_order_acquire), std::memory_order_release); }

    void push(const float* in, size_t n) {
        size_t wv = w.load(std::memory_order_relaxed), rv = r.load(std::memory_order_acquire);
        size_t space = cap - (wv - rv);
        if (n > space) n = space;  // переполнение: лишнее отбрасываем
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (wv + i) % cap;
            buf[idx * 2] = in[i * 2];
            buf[idx * 2 + 1] = in[i * 2 + 1];
        }
        w.store(wv + n, std::memory_order_release);
    }

    // Читает ровно n кадров (нехватку дополняет тишиной). Держит задержку в пределах keep кадров:
    // устройства идут от разных часов, и запас не должен расти бесконечно.
    void pop(float* out, size_t n) {
        size_t rv = r.load(std::memory_order_relaxed), wv = w.load(std::memory_order_acquire);
        size_t av = wv - rv;
        size_t keep = std::max<size_t>(n * 3, 960);
        if (av > keep + n) { rv += av - keep; av = keep; }
        size_t m = std::min(n, av);
        for (size_t i = 0; i < m; ++i) {
            size_t idx = (rv + i) % cap;
            out[i * 2] = buf[idx * 2];
            out[i * 2 + 1] = buf[idx * 2 + 1];
        }
        for (size_t i = m * 2; i < n * 2; ++i) out[i] = 0.0f;
        r.store(rv + m, std::memory_order_release);
    }
};

enum Source { SRC_OFF = 0, SRC_SINE = 1, SRC_NOISE = 2, SRC_FILE = 3 };

struct Strip {
    bool hw = false;
    std::atomic<float> gain_db{0.0f};
    std::atomic<bool> mute{false}, solo{false};
    std::atomic<int> route{0};  // битовая маска: A1 A2 A3 B1 B2
    std::atomic<int> src{SRC_OFF};
    Meter meter;
    Ring ring;
    float cur_gain = 1.0f;     // только аудио-поток
    double phase = 0.0;
    uint32_t rng = 12345u;
    std::atomic<float*> fdata{nullptr};
    std::atomic<size_t> fframes{0};
    size_t fpos = 0;
};

struct Bus {
    std::atomic<float> gain_db{0.0f};
    std::atomic<bool> mute{false};
    Meter meter;
    Ring ring;
    float cur_gain = 1.0f;
};

class Mixer;
struct Role { Mixer* m; int slot; };

inline std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o;
}

inline float gain_from_db(float db) { return db <= -47.9f ? 0.0f : db_to_lin(db); }

class Mixer {
public:
    Strip strips[kStrips];
    Bus buses[kBuses];
    MicChain mic[kMics];
    std::atomic<bool> dirty{true};

    // shm_name = nullptr отключает общую память (для проверок).
    explicit Mixer(ma_context* c, const char* shm_name = kShmDefaultName) : ctx_(c) {
        if (shm_name) {
            shm_ = Shm::open(shm_name, Shm::Role::Engine, shm_err_);
            if (shm_) {
                for (int i = 0; i < kShmInRings; ++i) shm_in_[i] = shm_->in(i);
                for (int i = 0; i < kShmOutRings; ++i) shm_out_[i] = shm_->out(i);
            }
        }
        for (int i = 0; i < kStrips; ++i) { strips[i].hw = i < 2; strips[i].ring.init(8192); }
        for (int i = 0; i < kMics; ++i) mic_rd_[i].configure(kHwTargetFrames);
        for (int k = 0; k < kBuses; ++k) bus_rd_[k].configure(kHwTargetFrames);
        for (int k = 0; k < kBuses; ++k) buses[k].ring.init(8192);
        for (int s = 0; s < kSlots; ++s) { roles_[s] = {this, s}; ok_[s] = false; }
        acc_.assign(kBuses, std::vector<float>(kMaxBlock * 2));
        tmp_.assign(kMaxBlock * 2, 0.0f);
        refresh_devices();
    }
    ~Mixer() { close_all(); }

    // ---------- устройства ----------
    void refresh_devices() {
        std::lock_guard<std::mutex> lk(dev_mu_);
        refresh_locked();
    }

    // Список устройств изменился с прошлой отправки окну (вставили или выдернули устройство).
    bool take_devices_dirty() { return devices_dirty_.exchange(false); }

    std::string devices_json() const {
        std::lock_guard<std::mutex> lk(dev_mu_);
        std::ostringstream o;
        o << "{\"t\":\"devices\",\"cap\":[";
        for (size_t i = 0; i < cap_.size(); ++i)
            o << (i ? "," : "") << "{\"name\":\"" << json_escape(cap_[i].name) << "\",\"def\":" << (cap_[i].isDefault ? 1 : 0) << "}";
        o << "],\"play\":[";
        for (size_t i = 0; i < play_.size(); ++i)
            o << (i ? "," : "") << "{\"name\":\"" << json_escape(play_[i].name) << "\",\"def\":" << (play_[i].isDefault ? 1 : 0) << "}";
        o << "]}";
        return o.str();
    }

    int slot_index(const std::string& n) const {
        for (int s = 0; s < kSlots; ++s) if (n == kSlotNames[s]) return s;
        return -1;
    }

    // Присваивает устройство слоту (по точному имени или по номеру) и пересобирает звуковой тракт.
    std::string set_device(int slot, const std::string& what) {
        std::lock_guard<std::mutex> lk(dev_mu_);
        std::string name = what;
        if (name == "-") name.clear();
        if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
            size_t idx = std::stoul(name);
            const auto& list = slot < 2 ? cap_ : play_;
            name = idx < list.size() ? list[idx].name : "";
        }
        names_[slot] = name;
        return rebuild();
    }

    // Сторож устройств. Вызывать примерно раз в 2 секунды из отдельного потока. Возвращает текст ошибки или пустую строку.
    std::string watchdog() {
        using clock = std::chrono::steady_clock;
        std::lock_guard<std::mutex> lk(dev_mu_);
        poll_driver();
        refresh_locked();
        const auto now = clock::now();
        bool changed = false, master_lost = false;
        std::string err;

        // 1) Устройства, которые остановились или пропали из системы. «Пропало из списка» считаем только со второго раза подряд:
        //    один сбойный опрос Windows не должен гасить рабочее устройство.
        for (int s = 0; s < kSlots; ++s) {
            if (!dev_[s]) continue;
            const bool started = ma_device_get_state(dev_[s].get()) == ma_device_state_started;
            const bool present = find_id(s, names_[s]) != nullptr;
            miss_[s] = present ? 0 : miss_[s] + 1;
            if (started && miss_[s] < 2) continue;
            changed = true;
            if (master_ >= 0 && s == 2 + master_) master_lost = true;
            else { close_slot(s); fails_[s] = 0; retry_at_[s] = now; }
        }

        if (master_lost) {
            err = rebuild();
            if (master_ < 0) rebuild_at_ = now + std::chrono::seconds(2);
        } else if (master_ < 0) {
            // Нет ведущего устройства: ждём, пока появится любое назначенное устройство выхода.
            bool any = false;
            for (int s = 2; s < kSlots; ++s) any = any || (!names_[s].empty() && find_id(s, names_[s]));
            if (any && now >= rebuild_at_) {
                err = rebuild();
                changed = true;
                rebuild_fails_ = master_ < 0 ? std::min(rebuild_fails_ + 1, 10) : 0;
                rebuild_at_ = now + std::chrono::seconds(std::min(30, 2 * (rebuild_fails_ + 1)));
            }
        } else {
            // Остальные назначенные устройства, которые сейчас закрыты, а в системе уже есть: открываем только их.
            for (int s = 0; s < kSlots; ++s) {
                if (dev_[s] || names_[s].empty() || !find_id(s, names_[s]) || now < retry_at_[s]) continue;
                std::string e;
                open_slot(s, false, e);
                if (ok_[s]) {
                    changed = true; fails_[s] = 0;
                    if (s >= 2) { buses[s - 2].ring.clear(); bus_rd_[s - 2].request_reset(); }
                    else mic_rd_[s].request_reset();
                }
                else if (fails_[s] == 1) err += e;   // об ошибке говорим один раз, дальше тихо пробуем реже
            }
        }
        if (changed) dirty = true;
        return err;
    }

    // ---------- драйвер (общая память) ----------
    bool shm_ready() const { return shm_ != nullptr; }
    const std::string& shm_error() const { return shm_err_; }
    bool driver_alive() const { return drv_alive_.load(); }
    Shm* shm() const { return shm_.get(); }
    // Драйвер найден в системе (устройство Bogestv0Mixer есть), даже если память от него получить не удалось.
    bool driver_found() const { return shm_ ? shm_->driver_found() : driver_found_; }
    bool via_driver() const { return shm_ && shm_->via_driver(); }
    const std::string& driver_note() const { return shm_ ? shm_->driver_note() : driver_note_; }

    // Сторож: драйвер жив, если его счётчик сдвинулся с прошлой проверки. Меняется набор открытых потоков: окно получит новое состояние.
    void poll_driver() {
        if (!shm_) return;
        const uint32_t beat = shm_->header()->driver_beat.load(std::memory_order_relaxed);
        const bool alive = beat != last_driver_beat_;
        last_driver_beat_ = beat;
        drv_alive_ = alive;
        unsigned mask = alive ? 1u : 0u;
        for (int i = 0; i < kShmInRings; ++i) if (shm_in_[i].active()) mask |= 2u << i;
        for (int i = 0; i < kShmOutRings; ++i) if (shm_out_[i].active()) mask |= 2u << (kShmInRings + i);
        if (mask != drv_mask_.exchange(mask)) dirty = true;
    }

    // Для самопроверки без звуковых устройств.
    bool slot_ok(int s) const { return ok_[s]; }
    const void* debug_dev(int s) const { return dev_[s].get(); }
    bool debug_started(int s) const { return dev_[s] && ma_device_get_state(dev_[s].get()) == ma_device_state_started; }
    void debug_stop(int s) { if (dev_[s]) ma_device_stop(dev_[s].get()); }
    std::string debug_name(bool capture) const {
        std::lock_guard<std::mutex> lk(dev_mu_);
        const auto& l = capture ? cap_ : play_;
        return l.empty() ? std::string() : std::string(l[0].name);
    }

    // ---------- состояние ----------
    std::string state_json() const {
        std::lock_guard<std::mutex> lk(dev_mu_);
        std::ostringstream o;
        o << "{\"t\":\"state\",\"v\":\"" << kEngineVersion << "\",\"running\":" << (master_ >= 0 ? 1 : 0)
          << ",\"master\":" << (master_ >= 0 ? std::string("\"") + kSlotNames[2 + master_] + "\"" : std::string("null"))
          << ",\"rate\":48000,\"ms\":" << latency_ms_ << ",\"strips\":[";
        for (int i = 0; i < kStrips; ++i) {
            const Strip& s = strips[i];
            o << (i ? "," : "") << "{\"g\":" << s.gain_db.load() << ",\"m\":" << (s.mute.load() ? 1 : 0)
              << ",\"s\":" << (s.solo.load() ? 1 : 0) << ",\"r\":" << s.route.load() << ",\"src\":" << s.src.load() << "}";
        }
        o << "],\"mic\":[";
        for (int i = 0; i < kMics; ++i) {
            const MicParams& m = mic[i].p;
            o << (i ? "," : "") << "{\"g\":" << m.gain_db.load() << ",\"gate\":" << m.gate_db.load() << ",\"hpf\":" << m.hpf_hz.load()
              << ",\"comp\":" << m.comp_ratio.load() << ",\"ns\":" << m.ns_mode.load() << ",\"amt\":" << m.ns_amt.load() << "}";
        }
        o << "],\"buses\":[";
        for (int k = 0; k < kBuses; ++k)
            o << (k ? "," : "") << "{\"g\":" << buses[k].gain_db.load() << ",\"m\":" << (buses[k].mute.load() ? 1 : 0) << "}";
        o << "],\"dev\":{";
        for (int s = 0; s < kSlots; ++s) o << (s ? "," : "") << "\"" << kSlotNames[s] << "\":\"" << json_escape(names_[s]) << "\"";
        o << "},\"ok\":{";
        for (int s = 0; s < kSlots; ++s) o << (s ? "," : "") << "\"" << kSlotNames[s] << "\":" << (ok_[s] ? 1 : 0);
        o << "},\"drv\":{\"shm\":" << (shm_ ? 1 : 0) << ",\"via\":" << (shm_ && shm_->via_driver() ? 1 : 0) << ",\"found\":" << (driver_found() ? 1 : 0) << ",\"alive\":" << (drv_alive_.load() ? 1 : 0) << ",\"in\":[";
        const unsigned mk = drv_mask_.load();
        for (int i = 0; i < kShmInRings; ++i) o << (i ? "," : "") << ((mk >> (1 + i)) & 1u);
        o << "],\"out\":[";
        for (int i = 0; i < kShmOutRings; ++i) o << (i ? "," : "") << ((mk >> (1 + kShmInRings + i)) & 1u);
        // Диагностика колец (с запуска движка): rate = частота, с которой Windows открыла поток (0 = закрыт); starved = кадров заполнено
        // тишиной (читающий не дождался данных); drops = кадров отброшено (писавший не нашёл места); trim = кадров выброшено движком сверху.
        // Только для входов V1..V6: fill = средний запас сейчас (кадры), under = сколько раз кольцо опустело, adj = подстройка скорости (ppm),
        // opens / closes = сколько раз Windows открывала и закрывала поток.
        o << "],\"diag\":{\"rate\":[";
        for (int i = 0; i < kShmRings; ++i) o << (i ? "," : "") << (shm_ ? shm_->header()->reserved[i] : 0u);
        o << "],\"starved\":[";
        for (int i = 0; i < kShmRings; ++i) o << (i ? "," : "") << (shm_ ? shm_->ring(i).ctl->starved.load(std::memory_order_relaxed) : 0u);
        o << "],\"drops\":[";
        for (int i = 0; i < kShmRings; ++i) o << (i ? "," : "") << (shm_ ? shm_->ring(i).ctl->drops.load(std::memory_order_relaxed) : 0u);
        o << "],\"trim\":[";
        for (int i = 0; i < kShmInRings; ++i) o << (i ? "," : "") << in_rd_[i].trim.load(std::memory_order_relaxed);
        o << "],\"fill\":[";
        for (int i = 0; i < kShmInRings; ++i) o << (i ? "," : "") << in_rd_[i].fill.load(std::memory_order_relaxed);
        o << "],\"under\":[";
        for (int i = 0; i < kShmInRings; ++i) o << (i ? "," : "") << in_rd_[i].under.load(std::memory_order_relaxed);
        o << "],\"adj\":[";
        for (int i = 0; i < kShmInRings; ++i) o << (i ? "," : "") << in_rd_[i].adj_ppm.load(std::memory_order_relaxed);
        o << "],\"opens\":[";
        for (int i = 0; i < kShmInRings; ++i) o << (i ? "," : "") << in_rd_[i].opens.load(std::memory_order_relaxed);
        o << "],\"closes\":[";
        for (int i = 0; i < kShmInRings; ++i) o << (i ? "," : "") << in_rd_[i].closes.load(std::memory_order_relaxed);
        // Остальные кольца движка (читатель с запасом ~10 мс): HW1, HW2 и A1..B2 (у ведущего устройства читателя нет, там нули).
        o << "]},\"hw\":{\"fill\":[";
        for (int i = 0; i < kMics; ++i) o << (i ? "," : "") << mic_rd_[i].fill.load(std::memory_order_relaxed);
        for (int k = 0; k < kBuses; ++k) o << "," << bus_rd_[k].fill.load(std::memory_order_relaxed);
        o << "],\"under\":[";
        for (int i = 0; i < kMics; ++i) o << (i ? "," : "") << mic_rd_[i].under.load(std::memory_order_relaxed);
        for (int k = 0; k < kBuses; ++k) o << "," << bus_rd_[k].under.load(std::memory_order_relaxed);
        o << "],\"adj\":[";
        for (int i = 0; i < kMics; ++i) o << (i ? "," : "") << mic_rd_[i].adj_ppm.load(std::memory_order_relaxed);
        for (int k = 0; k < kBuses; ++k) o << "," << bus_rd_[k].adj_ppm.load(std::memory_order_relaxed);
        o << "]}}}";
        return o.str();
    }

    std::string meters_json() {
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(3);
        o << "{\"t\":\"m\",\"s\":[";
        for (int i = 0; i < kStrips; ++i) o << (i ? "," : "") << "[" << strips[i].meter.read(0) << "," << strips[i].meter.read(1) << "]";
        o << "],\"b\":[";
        for (int k = 0; k < kBuses; ++k) o << (k ? "," : "") << "[" << buses[k].meter.read(0) << "," << buses[k].meter.read(1) << "]";
        o << "],\"n\":[";
        o.precision(1);
        for (int i = 0; i < kMics; ++i)
            o << (i ? "," : "") << "[" << mic[i].raw_floor.floor_db.load() << "," << mic[i].proc_floor.floor_db.load() << "]";
        o << "]}";
        return o.str();
    }

    // ---------- команды ----------
    // Возвращает пустую строку или текст ошибки.
    std::string command(const std::string& line) {
        std::istringstream in(line);
        std::string a, b;
        in >> a;
        if (a != "set") return "неизвестная команда: " + a;
        in >> b;
        if (b == "strip") {
            int i; std::string what;
            if (!(in >> i >> what) || i < 0 || i >= kStrips) return "дорожка: неверный номер";
            Strip& s = strips[i];
            if (what == "gain") { float v; if (!(in >> v)) return "gain: нужно число"; s.gain_db = std::max(-48.0f, std::min(12.0f, v)); }
            else if (what == "mute") { int v; if (!(in >> v)) return "mute: 0 или 1"; s.mute = v != 0; }
            else if (what == "solo") {
                int v; if (!(in >> v)) return "solo: 0 или 1";
                if (v) for (int j = 0; j < kStrips; ++j) strips[j].solo = (j == i);  // эксклюзивный solo
                else s.solo = false;
            }
            else if (what == "route") {
                int k, v; if (!(in >> k >> v) || k < 0 || k >= kBuses) return "route: номер шины 0..4 и 0/1";
                int m = s.route.load();
                s.route = v ? (m | (1 << k)) : (m & ~(1 << k));
            }
            else if (what == "src") {
                std::string v; in >> v;
                if (s.hw) return "у микрофона нет тест-источника";
                s.src = v == "sine" ? SRC_SINE : v == "noise" ? SRC_NOISE : v == "file" ? SRC_FILE : SRC_OFF;
            }
            else if (what == "file") {
                std::string path; std::getline(in, path);
                if (!path.empty() && path[0] == ' ') path.erase(0, 1);
                return load_file(s, path);
            }
            else return "дорожка: неизвестное свойство " + what;
        } else if (b == "mic") {
            int i; std::string what; float v;
            if (!(in >> i >> what >> v) || i < 0 || i >= kMics) return "микрофон: номер 0..1, свойство и число";
            if (!std::isfinite(v)) return "микрофон: нужно число";
            MicParams& m = mic[i].p;
            if (what == "gain") m.gain_db = std::max(0.0f, std::min(30.0f, v));
            else if (what == "gate") m.gate_db = std::max(-80.0f, std::min(-20.0f, v));
            else if (what == "hpf") m.hpf_hz = v < 25.0f ? 0.0f : std::min(300.0f, v);
            else if (what == "comp") m.comp_ratio = std::max(1.0f, std::min(10.0f, v));
            else if (what == "ns") m.ns_mode = std::max(0, std::min(3, static_cast<int>(v)));
            else if (what == "nsamt") m.ns_amt = std::max(0.0f, std::min(100.0f, v));
            else return "микрофон: неизвестное свойство " + what;
        } else if (b == "bus") {
            int k; std::string what;
            if (!(in >> k >> what) || k < 0 || k >= kBuses) return "шина: неверный номер";
            Bus& bus = buses[k];
            if (what == "gain") { float v; if (!(in >> v)) return "gain: нужно число"; bus.gain_db = std::max(-48.0f, std::min(12.0f, v)); }
            else if (what == "mute") { int v; if (!(in >> v)) return "mute: 0 или 1"; bus.mute = v != 0; }
            else return "шина: неизвестное свойство " + what;
        } else if (b == "dev") {
            std::string slot; in >> slot;
            int s = slot_index(slot);
            if (s < 0) return "слот: " + slot;
            std::string name; std::getline(in, name);
            if (!name.empty() && name[0] == ' ') name.erase(0, 1);
            return set_device(s, name);
        } else return "неизвестный раздел: " + b;
        dirty = true;
        return "";
    }

private:
    ma_context* ctx_;
    std::vector<ma_device_info> cap_, play_;
    std::string names_[kSlots];
    std::atomic<bool> ok_[kSlots];
    std::unique_ptr<ma_device> dev_[kSlots];
    Role roles_[kSlots];
    int master_ = -1;
    double latency_ms_ = 0.0;
    mutable std::mutex dev_mu_;
    std::atomic<bool> devices_dirty_{false};
    std::chrono::steady_clock::time_point retry_at_[kSlots], rebuild_at_;
    int fails_[kSlots] = {}, miss_[kSlots] = {}, rebuild_fails_ = 0;
    std::vector<std::vector<float>> acc_;
    std::vector<float> tmp_;
    std::unique_ptr<Shm> shm_;
    std::string shm_err_;
    bool driver_found_ = false;
    std::string driver_note_;
    ShmRing shm_in_[kShmInRings], shm_out_[kShmOutRings];
    InReader mic_rd_[kMics];                       // микрофоны HW1, HW2: кольцо захвата -> главный проход
    InReader bus_rd_[kBuses];                      // вторичные выходы A2, A3, B1, B2: главный проход -> кольцо -> устройство
    InReader in_rd_[kShmInRings];                  // чтение колец входов с запасом (счётчики для диагностики внутри)
    bool drv_was_active_[kShmInRings] = {};        // только аудио-поток
    uint32_t last_driver_beat_ = 0;                // только сторож
    std::atomic<bool> drv_alive_{false};
    std::atomic<unsigned> drv_mask_{0};            // бит 0: драйвер жив; 1..6: входы V1..V6; 7..8: выходы B1, B2

    std::string load_file(Strip& s, const std::string& path) {
        ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, 48000);
        ma_uint64 frames = 0;
        void* data = nullptr;
        if (ma_decode_file(path.c_str(), &dc, &frames, &data) != MA_SUCCESS || !data) return "не удалось прочитать файл: " + path;
        s.fframes = 0;  // старый буфер намеренно не освобождаем: поток мог его ещё читать (только для тестов)
        s.fdata = static_cast<float*>(data);
        s.fpos = 0;
        s.fframes = static_cast<size_t>(frames);
        s.src = SRC_FILE;
        return "";
    }

    void refresh_locked() {
        ma_device_info *pp, *cp;
        ma_uint32 pc, cc;
        if (ma_context_get_devices(ctx_, &pp, &pc, &cp, &cc) != MA_SUCCESS) return;
        auto same = [](const std::vector<ma_device_info>& old, const ma_device_info* now, ma_uint32 n) {
            if (old.size() != n) return false;
            for (ma_uint32 i = 0; i < n; ++i) if (std::strcmp(old[i].name, now[i].name) != 0) return false;
            return true;
        };
        const bool changed = !same(play_, pp, pc) || !same(cap_, cp, cc);
        play_.assign(pp, pp + pc);
        cap_.assign(cp, cp + cc);
        if (changed) devices_dirty_ = true;
    }

    void close_slot(int s) {
        ok_[s] = false;
        if (dev_[s]) { ma_device_uninit(dev_[s].get()); dev_[s].reset(); }
    }

    void close_all() {
        for (int s = 0; s < kSlots; ++s) if (dev_[s]) { ma_device_uninit(dev_[s].get()); dev_[s].reset(); }
        master_ = -1;
    }

    // Открывает устройство слота. При неудаче дописывает причину в err и назначает следующую попытку сторожу.
    void open_slot(int s, bool is_master, std::string& err) {
        ma_device_type type = s < 2 ? ma_device_type_capture : ma_device_type_playback;
        const ma_device_id* id = find_id(s, names_[s]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            ma_device_config cfg = ma_device_config_init(type);
            cfg.sampleRate = 48000;
            cfg.periodSizeInFrames = is_master ? 128 : 256;
            cfg.periods = 2;
            cfg.performanceProfile = (is_master && attempt == 0) ? ma_performance_profile_low_latency : ma_performance_profile_conservative;
            cfg.noPreSilencedOutputBuffer = MA_TRUE;
            cfg.dataCallback = is_master ? &Mixer::cb_master : (s < 2 ? &Mixer::cb_capture : &Mixer::cb_bus);
            cfg.pUserData = &roles_[s];
            if (type == ma_device_type_capture) {
                cfg.capture.pDeviceID = id; cfg.capture.format = ma_format_f32; cfg.capture.channels = 2;
            } else {
                cfg.playback.pDeviceID = id; cfg.playback.format = ma_format_f32; cfg.playback.channels = 2;
            }
            auto d = std::make_unique<ma_device>();
            if (ma_device_init(ctx_, &cfg, d.get()) != MA_SUCCESS) continue;
            if (ma_device_start(d.get()) != MA_SUCCESS) { ma_device_uninit(d.get()); continue; }
            if (is_master) {
                latency_ms_ = d->playback.internalPeriodSizeInFrames * d->playback.internalPeriods * 1000.0 /
                              (d->playback.internalSampleRate ? d->playback.internalSampleRate : 48000);
            }
            dev_[s] = std::move(d);
            ok_[s] = true;
            fails_[s] = 0;
            miss_[s] = 0;
            return;
        }
        err += std::string("не открылось устройство ") + kSlotNames[s] + ": " + names_[s] + "; ";
        fails_[s] = std::min(fails_[s] + 1, 15);
        retry_at_[s] = std::chrono::steady_clock::now() + std::chrono::seconds(std::min(30, 2 * fails_[s]));
    }

    const ma_device_id* find_id(int slot, const std::string& name) {
        const auto& list = slot < 2 ? cap_ : play_;
        for (const auto& d : list) if (name == d.name) return &d.id;
        return nullptr;
    }

    std::string rebuild() {
        close_all();
        std::string err;
        for (int i = 0; i < kStrips; ++i) strips[i].ring.init(8192);
        for (int k = 0; k < kBuses; ++k) buses[k].ring.init(8192);
        for (int i = 0; i < kMics; ++i) mic_rd_[i].request_reset();
        for (int k = 0; k < kBuses; ++k) bus_rd_[k].request_reset();
        for (int s = 0; s < kSlots; ++s) { ok_[s] = false; fails_[s] = 0; miss_[s] = 0; retry_at_[s] = {}; }
        rebuild_fails_ = 0;
        rebuild_at_ = {};
        for (int i = 0; i < kMics; ++i) mic[i].reset();   // звук остановлен: потоки закрыты в close_all()

        // Ведущее устройство: первая выходная шина, у которой есть устройство. Оно задаёт такт всей обработке.
        std::vector<bool> usable(kSlots, false);
        for (int s = 0; s < kSlots; ++s) usable[s] = !names_[s].empty() && find_id(s, names_[s]);
        for (int s = 2; s < kSlots && master_ < 0; ++s) if (usable[s]) master_ = s - 2;

        auto open = [&](int s, bool is_master) { open_slot(s, is_master, err); };

        if (master_ >= 0) open(2 + master_, true);
        if (master_ >= 0 && !ok_[2 + master_]) {  // ведущее не открылось: пробуем следующее
            int failed = master_;
            master_ = -1;
            for (int s = 2 + failed + 1; s < kSlots && master_ < 0; ++s) if (usable[s]) master_ = s - 2;
            if (master_ >= 0) open(2 + master_, true);
            if (master_ >= 0 && !ok_[2 + master_]) master_ = -1;
        }
        for (int s = 0; s < kSlots; ++s) {
            if (!usable[s] || ok_[s] || (master_ >= 0 && s == 2 + master_)) continue;
            if (s >= 2 && master_ < 0) continue;  // без ведущего выходы не нужны
            open(s, false);
        }
        dirty = true;
        return err;
    }

    // ---------- аудио-потоки ----------
    static void cb_capture(ma_device* d, void*, const void* in, ma_uint32 frames) {
        Role* r = static_cast<Role*>(d->pUserData);
        if (in) r->m->strips[r->slot].ring.push(static_cast<const float*>(in), frames);
    }
    static void cb_bus(ma_device* d, void* out, const void*, ma_uint32 frames) {
        Role* r = static_cast<Role*>(d->pUserData);
        r->m->bus_rd_[r->slot - 2].read(r->m->buses[r->slot - 2].ring, static_cast<float*>(out), static_cast<uint32_t>(frames));
    }
    static void cb_master(ma_device* d, void* out, const void*, ma_uint32 frames) {
        Role* r = static_cast<Role*>(d->pUserData);
        r->m->process(static_cast<float*>(out), frames, r->slot - 2);
    }

    void generate(Strip& s, int idx, float* o, size_t n) {
        int src = s.src.load(std::memory_order_relaxed);
        if (src == SRC_SINE) {
            const double step = 2.0 * 3.14159265358979 * (220.0 + 110.0 * idx) / 48000.0;
            for (size_t f = 0; f < n; ++f) {
                float v = static_cast<float>(std::sin(s.phase)) * 0.3f;
                o[f * 2] = o[f * 2 + 1] = v;
                s.phase += step;
                if (s.phase > 6.283185307179586) s.phase -= 6.283185307179586;
            }
        } else if (src == SRC_NOISE) {
            for (size_t f = 0; f < n; ++f) {
                s.rng ^= s.rng << 13; s.rng ^= s.rng >> 17; s.rng ^= s.rng << 5;
                float v = (static_cast<float>(s.rng & 0xFFFF) / 32768.0f - 1.0f) * 0.2f;
                o[f * 2] = o[f * 2 + 1] = v;
            }
        } else if (src == SRC_FILE && s.fframes.load() > 0) {
            const float* data = s.fdata.load();
            const size_t total = s.fframes.load();
            for (size_t f = 0; f < n; ++f) {
                if (s.fpos >= total) s.fpos = 0;
                o[f * 2] = data[s.fpos * 2] * 0.8f;
                o[f * 2 + 1] = data[s.fpos * 2 + 1] * 0.8f;
                ++s.fpos;
            }
        } else {
            std::memset(o, 0, n * 2 * sizeof(float));
        }
    }

    static void ramp(float* x, size_t n, float from, float to) {
        const float step = (to - from) / static_cast<float>(n);
        float g = from;
        for (size_t f = 0; f < n; ++f) { g += step; x[f * 2] *= g; x[f * 2 + 1] *= g; }
    }

    // Главный проход: дорожки -> маршруты -> шины. Вызывается из потока ведущего устройства.
    void process(float* out, size_t frames, int master_bus) {
        bool any_solo = false;
        for (int i = 0; i < kStrips; ++i) any_solo = any_solo || strips[i].solo.load(std::memory_order_relaxed);
        for (size_t done = 0; done < frames;) {
            const size_t n = std::min(kMaxBlock, frames - done);
            for (int k = 0; k < kBuses; ++k) std::memset(acc_[k].data(), 0, n * 2 * sizeof(float));
            for (int i = 0; i < kStrips; ++i) {
                Strip& s = strips[i];
                float* t = tmp_.data();
                if (s.hw) {
                    mic_rd_[i].read(s.ring, t, static_cast<uint32_t>(n));   // запас ~10 мс и подстройка под часы микрофона (inbuf.h)
                    if (i < kMics && ok_[i]) mic[i].process(t, n);   // обработка микрофона
                } else if (shm_ && s.src.load(std::memory_order_relaxed) == SRC_OFF && shm_in_[i - 2].active()) {
                    ShmRing& rg = shm_in_[i - 2];
                    InReader& rd = in_rd_[i - 2];
                    if (!drv_was_active_[i - 2]) { rd.open(rg); drv_was_active_[i - 2] = true; }   // поток только что открылся: старое не нужно, ждём запас
                    rd.read(rg, t, static_cast<uint32_t>(n));   // запас 25 мс и подстройка под часы драйвера (inbuf.h)
                } else {
                    if (i >= 2 && drv_was_active_[i - 2]) {
                        drv_was_active_[i - 2] = false;
                        if (shm_ && !shm_in_[i - 2].active()) in_rd_[i - 2].closed();   // поток закрыт Windows (а не перехвачен тест-источником)
                    }
                    generate(s, i, t, n);
                }
                // Выход дорожки для OBS («Bogestv0 <Дорожка> Out»): чистый звук дорожки V1..V6 до фейдера, Mute и Solo.
                if (shm_ && i >= 2 && i - 2 < kShmTapRings && shm_out_[kShmBusRings + (i - 2)].active())
                    shm_out_[kShmBusRings + (i - 2)].push(t, static_cast<uint32_t>(n));
                const bool silent = s.mute.load(std::memory_order_relaxed) || (any_solo && !s.solo.load(std::memory_order_relaxed));
                const float target = silent ? 0.0f : gain_from_db(s.gain_db.load(std::memory_order_relaxed));
                ramp(t, n, s.cur_gain, target);
                s.cur_gain = target;
                s.meter.feed(t, n, 2);
                const int mask = s.route.load(std::memory_order_relaxed);
                for (int k = 0; k < kBuses; ++k) {
                    if (!(mask & (1 << k))) continue;
                    float* a = acc_[k].data();
                    for (size_t j = 0; j < n * 2; ++j) a[j] += t[j];
                }
            }
            for (int k = 0; k < kBuses; ++k) {
                Bus& b = buses[k];
                float* a = acc_[k].data();
                const float target = b.mute.load(std::memory_order_relaxed) ? 0.0f : gain_from_db(b.gain_db.load(std::memory_order_relaxed));
                ramp(a, n, b.cur_gain, target);
                b.cur_gain = target;
                for (size_t j = 0; j < n * 2; ++j) a[j] = soft_limit(a[j]);  // лимитер выходов
                b.meter.feed(a, n, 2);
                if (k == master_bus) std::memcpy(out + done * 2, a, n * 2 * sizeof(float));
                else if (ok_[2 + k]) b.ring.push(a, n);
                if (shm_ && k >= kBuses - kShmBusRings && shm_out_[k - (kBuses - kShmBusRings)].active())
                    shm_out_[k - (kBuses - kShmBusRings)].push(a, static_cast<uint32_t>(n));   // B1 / B2 -> «Bogestv0 Mix B1/B2 Out»
            }
            if (shm_) shm_->header()->engine_beat.fetch_add(1, std::memory_order_relaxed);
            done += n;
        }
    }
};

// ---------- цикл обмена с окном ----------
inline std::mutex& out_mutex() { static std::mutex m; return m; }
inline void emit(const std::string& line) {
    std::lock_guard<std::mutex> lk(out_mutex());
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
inline void emit_err(const std::string& msg) { emit("{\"t\":\"err\",\"msg\":\"" + json_escape(msg) + "\"}"); }

inline int run_server(ma_context& ctx) {
    Mixer mixer(&ctx);
    std::atomic<bool> quit{false};
    emit(std::string("{\"t\":\"hello\",\"v\":\"") + kEngineVersion + "\"}");
    emit(mixer.devices_json());
    emit(mixer.state_json());
    if (!mixer.shm_ready()) emit_err("общая память драйвера недоступна: " + mixer.shm_error() + " (виртуальные устройства работать не будут)");
    else if (mixer.driver_found() && !mixer.via_driver()) emit_err("драйвер найден, но звук через него не идёт: " + mixer.driver_note() + ". Используется запасная память движка (виртуальные устройства не работают).");

    // Метры ~30 раз в секунду, состояние — когда что-то изменилось.
    std::thread pump([&] {
        while (!quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            emit(mixer.meters_json());
            if (mixer.dirty.exchange(false)) emit(mixer.state_json());
        }
    });

    // Сторож устройств: раз в ~2 секунды проверяет, что назначенные устройства живы, и возвращает вернувшиеся.
    std::thread guard([&] {
        while (!quit) {
            for (int i = 0; i < 20 && !quit; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (quit) break;
            std::string err = mixer.watchdog();
            if (!err.empty()) emit_err(err);
            if (mixer.take_devices_dirty()) emit(mixer.devices_json());
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line == "quit") break;
        if (line == "get") { emit(mixer.state_json()); continue; }
        if (line == "devices") { mixer.refresh_devices(); emit(mixer.devices_json()); mixer.dirty = true; continue; }
        std::string err = mixer.command(line);
        if (!err.empty()) emit_err(err);
    }
    quit = true;  // окно закрылось или прислало quit
    pump.join();
    guard.join();
    return 0;
}

}  // namespace bg
