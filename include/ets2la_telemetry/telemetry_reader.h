#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "protocol.h"
#include "telemetry_data.h"

namespace ets2la_telemetry {

inline uint64_t qpc_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return uint64_t(q.QuadPart);
}

struct TelemetryInfo {
    uint64_t frame_index = 0;
    uint64_t timestamp_qpc = 0;
    uint64_t publish_qpc = 0;
};

// Owning copy of a snapshot (`telemetry.truck.speed`, `telemetry.trailers[0].connected`, ...).
// Reuse one instance across calls, it is about 21 KB.
struct Telemetry : TelemetryInfo, TelemetryData {
    bool valid = false;  // True after at least one successful read, false for default initialized data.
};

// Non-owning view straight into shared memory. Keeps the mapping alive, but the
// writer may overwrite the slot after kSlotCount-1 newer snapshots: call valid()
// after you are done reading and discard the result if it returns false.
class TelemetryView : public TelemetryInfo {
public:
    const TelemetryData* data = nullptr;
    bool valid() const {
        if (!slot_) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot_->seq.load(std::memory_order_relaxed) == seq_;
    }

private:
    friend class TelemetryReader;
    std::shared_ptr<void> keepalive_;
    const SlotHeader* slot_ = nullptr;
    uint64_t seq_ = 0;
};

struct ReaderOptions {
    uint32_t spin_us = 0;
};

// Snapshot of the plugin state and the timing statistics.
struct TelemetryStats {
    uint64_t latest_frame = 0;            // Index of the newest snapshot.
    uint64_t frames_published = 0;        // Snapshots published since the plugin loaded.
    bool active = false;                  // The game currently runs the telemetry API.
    bool paused = false;                  // The game is paused (menu, map, loading).
    uint32_t writer_pid = 0;              // Process id of the game that loaded the plugin.
    double frame_interval_ms = 0.0;       // Time between the last two game frames.
    double publish_cost_us = 0.0;         // Time the plugin needed to publish the last snapshot.
    double since_last_publish_ms = 0.0;   // Time since the last snapshot was published.
    uint32_t readers = 0;                 // Readers (this one included) that recently sent a sign of life.
};

// Lock-free reader for the `ets2la_telemetry.dll` shared memory.
//
// Any number of readers can run at the same time, every one of them always gets the newest snapshot.
// The delay of a snapshot is reported by age_ms()/delivery_ms() and stats().
class TelemetryReader {
public:
    explicit TelemetryReader(ReaderOptions options = {}) : options_(options) {
        connect();
    }
    ~TelemetryReader() {
        disconnect();
    }
    TelemetryReader(const TelemetryReader&) = delete;
    TelemetryReader& operator=(const TelemetryReader&) = delete;

    bool connected() const {
        return ctl_ != nullptr;
    }
    const std::string& last_error() const {
        return lastError_;
    }

    // Blocks until a snapshot newer than the previous one returned by this reader
    // exists (or timeout) and copies it into `out`.
    bool get_telemetry(Telemetry& out, uint32_t timeout_ms = 1000) {
        return acquire(&out, nullptr, true, timeout_ms);
    }

    // Non-blocking: newest snapshot available right now, copied into `out`.
    bool get_latest_telemetry(Telemetry& out) {
        return acquire(&out, nullptr, false, 0);
    }

    // Blocks until a snapshot newer than the previous one returned by this reader
    // exists (or timeout) and returns a view into shared memory. The caller must
    // call valid() on the returned view after it is done reading to ensure that
    // the writer did not overwrite the slot in the meantime.
    bool get_telemetry_view(TelemetryView& out, uint32_t timeout_ms = 1000) {
        return acquire(nullptr, &out, true, timeout_ms);
    }

    // Non-blocking: newest snapshot available right now, returns a view into shared
    // memory. The caller must call valid() on the returned view after it is done
    // reading to ensure that the writer did not overwrite the slot in the meantime.
    bool get_latest_telemetry_view(TelemetryView& out) {
        return acquire(nullptr, &out, false, 0);
    }

    double ticks_to_ms(uint64_t ticks) const {
        return freq_ ? double(ticks) * 1000.0 / double(freq_) : 0.0;
    }
    // Time since the game started this frame, in milliseconds.
    double age_ms(const TelemetryInfo& t) const {
        return ticks_to_ms(qpc_now() - t.timestamp_qpc);
    }
    // Time since the plugin published this snapshot, in milliseconds (the delay the shared memory adds).
    double delivery_ms(const TelemetryInfo& t) const {
        return ticks_to_ms(qpc_now() - t.publish_qpc);
    }

    TelemetryStats stats() const {
        TelemetryStats s;
        if (!ctl_) {
            return s;
        }
        s.latest_frame = ctl_->latest_frame.load(std::memory_order_relaxed);
        s.frames_published = ctl_->frames_published.load(std::memory_order_relaxed);
        s.active = ctl_->active.load(std::memory_order_relaxed) != 0;
        s.paused = ctl_->paused.load(std::memory_order_relaxed) != 0;
        s.writer_pid = ctl_->writer_pid;
        s.frame_interval_ms = ticks_to_ms(ctl_->frame_interval_ticks.load(std::memory_order_relaxed));
        s.publish_cost_us = ticks_to_ms(ctl_->publish_cost_ticks.load(std::memory_order_relaxed)) * 1000.0;
        const uint64_t last = ctl_->last_publish_qpc.load(std::memory_order_relaxed);
        s.since_last_publish_ms = last ? ticks_to_ms(qpc_now() - last) : 0.0;
        const uint64_t now = GetTickCount64();
        for (uint32_t i = 0; i < kMaxReaders; ++i) {
            const auto& r = ctl_->readers[i];
            if (r.id.load(std::memory_order_relaxed) != 0 && now - r.heartbeat_ms.load(std::memory_order_relaxed) < kReaderTimeoutMs) {
                ++s.readers;
            }
        }
        return s;
    }

    bool connect() {
        disconnect();
        ctlMap_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kControlName);
        if (!ctlMap_) {
            return false;
        }
        ctl_ = static_cast<ControlBlock*>(MapViewOfFile(ctlMap_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kControlBytes));
        if (!ctl_) {
            return fail("MapViewOfFile(control) failed");
        }
        if (ctl_->magic != kControlMagic) {
            disconnect();
            return fail("telemetry DLL not ready");
        }
        if (ctl_->version != kProtocolVersion) {
            disconnect();
            return fail("telemetry DLL protocol version mismatch");
        }
        for (int i = 0; i < 2; ++i) {
            events_[i] = OpenEventW(SYNCHRONIZE, FALSE, kEventNames[i]);
            if (!events_[i]) {
                disconnect();
                return fail("OpenEvent failed");
            }
        }
        freq_ = ctl_->qpc_frequency;
        if (!ensure_mapping()) {
            disconnect();
            return false;
        }
        register_reader();
        last_index_ = ctl_->latest_frame.load(std::memory_order_acquire);
        lastError_.clear();
        return true;
    }

private:
    struct Mapping {
        HANDLE handle = nullptr;
        void* base = nullptr;
        const DataHeader* hdr = nullptr;
        ~Mapping() {
            if (base) {
                UnmapViewOfFile(base);
            }
            if (handle) {
                CloseHandle(handle);
            }
        }
    };

    bool fail(const char* msg) {
        lastError_ = msg;
        return false;
    }

    void disconnect() {
        unregister_reader();
        map_.reset();
        if (ctl_) {
            UnmapViewOfFile(ctl_);
            ctl_ = nullptr;
        }
        if (ctlMap_) {
            CloseHandle(ctlMap_);
            ctlMap_ = nullptr;
        }
        for (auto& e : events_) {
            if (e) {
                CloseHandle(e);
                e = nullptr;
            }
        }
    }

    void register_reader() {
        static std::atomic<uint32_t> counter{0};
        myId_ = (uint64_t(GetCurrentProcessId()) << 32) | (counter.fetch_add(1) + 1);
        const uint64_t now = GetTickCount64();
        const uint32_t start = GetCurrentProcessId() % kMaxReaders;
        entry_ = &ctl_->readers[start];
        for (uint32_t i = 0; i < kMaxReaders; ++i) {
            ReaderEntry* e = &ctl_->readers[(start + i) % kMaxReaders];
            const uint64_t id = e->id.load(std::memory_order_relaxed);
            if (id == 0 || now - e->heartbeat_ms.load(std::memory_order_relaxed) > kReaderTimeoutMs * 2) {
                entry_ = e;
                break;
            }
        }
        heartbeat();
    }
    void heartbeat() {
        if (!entry_) return;
        entry_->id.store(myId_, std::memory_order_relaxed);
        entry_->heartbeat_ms.store(GetTickCount64(), std::memory_order_release);
    }
    void unregister_reader() {
        if (entry_ && ctl_ && entry_->id.load(std::memory_order_relaxed) == myId_) {
            entry_->heartbeat_ms.store(0, std::memory_order_relaxed);
            entry_->id.store(0, std::memory_order_release);
        }
        entry_ = nullptr;
    }

    bool ensure_mapping() {
        if (map_) {
            return true;
        }
        auto m = std::make_shared<Mapping>();
        m->handle = OpenFileMappingW(FILE_MAP_READ, FALSE, kDataName);
        if (!m->handle) {
            return false;
        }
        m->base = MapViewOfFile(m->handle, FILE_MAP_READ, 0, 0, 0);
        if (!m->base) {
            return fail("MapViewOfFile(data) failed");
        }
        m->hdr = static_cast<const DataHeader*>(m->base);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m->hdr->magic != kDataMagic ||
            m->hdr->slot_count == 0 ||
            m->hdr->version != kProtocolVersion) {
            return fail("data mapping header invalid");
        }
        if (m->hdr->data_size != sizeof(TelemetryData)) {
            return fail("telemetry DLL data layout mismatch (rebuild your client against the same version)");
        }
        map_ = std::move(m);
        return true;
    }

    bool snapshot(uint64_t minExclusive, TelemetryView& out) {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t latest = ctl_->latest_frame.load(std::memory_order_acquire);
            if (latest == 0 || latest <= minExclusive) {
                return false;
            }
            if (!ensure_mapping()) {
                return false;
            }

            const DataHeader* h = map_->hdr;
            const uint8_t* base = static_cast<const uint8_t*>(map_->base);
            const uint8_t* slotBase = base + h->slots_offset + (latest % h->slot_count) * h->slot_stride;
            const SlotHeader* slot = reinterpret_cast<const SlotHeader*>(slotBase);

            const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
            if ((s1 & 1) != 0 || slot->frame_index != latest) {
                YieldProcessor();
                continue;
            }

            out.frame_index = latest;
            out.timestamp_qpc = slot->timestamp_qpc;
            out.publish_qpc = slot->publish_qpc;
            out.data = reinterpret_cast<const TelemetryData*>(slotBase + kSlotHeaderBytes);
            out.slot_ = slot;
            out.seq_ = s1;
            out.keepalive_ = map_;

            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot->seq.load(std::memory_order_relaxed) != s1) continue;
            return true;
        }
        return false;
    }

    bool wait_for_newer(uint64_t deadline) {
        if (options_.spin_us) {
            const uint64_t spinEnd = qpc_now() + uint64_t(options_.spin_us) * freq_ / 1000000ull;
            while (qpc_now() < spinEnd) {
                if (ctl_->latest_frame.load(std::memory_order_acquire) > last_index_) {
                    return true;
                }
                YieldProcessor();
            }
        }
        while (true) {
            if (ctl_->latest_frame.load(std::memory_order_acquire) > last_index_) {
                return true;
            }
            const uint64_t now = qpc_now();
            if (now >= deadline) {
                return false;
            }
            const uint64_t remainingMs = ((deadline - now) * 1000 + freq_ - 1) / freq_;
            const DWORD slice = DWORD(remainingMs < 5 ? remainingMs : 5);
            WaitForSingleObject(events_[(last_index_ + 1) & 1], slice ? slice : 1);
            heartbeat();
        }
    }

    static bool copy_view(const TelemetryView& v, Telemetry& out) {
        TelemetryData data;
        std::memcpy(&data, v.data, sizeof(TelemetryData));
        if (!v.valid()) {
            return false;
        }
        static_cast<TelemetryInfo&>(out) = v;
        static_cast<TelemetryData&>(out) = data;
        out.valid = true;
        return true;
    }

    bool acquire(Telemetry* copyOut, TelemetryView* viewOut, bool onlyNew, uint32_t timeoutMs) {
        if (!ctl_) {
            const DWORD now = GetTickCount();
            if (now - lastReconnectTick_ < 250) {
                Sleep(timeoutMs < 50 ? timeoutMs : 50);
                return false;
            }
            lastReconnectTick_ = now;
            if (!connect()) {
                Sleep(timeoutMs < 50 ? timeoutMs : 50);
                return false;
            }
        }
        heartbeat();
        const uint64_t t0 = qpc_now();
        const uint64_t deadline = t0 + uint64_t(onlyNew ? timeoutMs : 5) * freq_ / 1000;
        while (true) {
            if (onlyNew && !wait_for_newer(deadline)) {
                return false;
            }
            TelemetryView v;
            if (snapshot(onlyNew ? last_index_ : 0, v)) {
                if (viewOut) {
                    last_index_ = v.frame_index;
                    *viewOut = std::move(v);
                    return true;
                }
                if (copy_view(v, *copyOut)) {
                    last_index_ = v.frame_index;
                    return true;
                }
            }
            if (qpc_now() >= deadline) {
                return false;
            }
            YieldProcessor();
        }
    }

    ReaderOptions options_;
    std::string lastError_;
    HANDLE ctlMap_ = nullptr;
    ControlBlock* ctl_ = nullptr;
    HANDLE events_[2] = {nullptr, nullptr};
    ReaderEntry* entry_ = nullptr;
    uint64_t myId_ = 0;
    uint64_t freq_ = 0;
    uint64_t last_index_ = 0;
    std::shared_ptr<Mapping> map_;
    DWORD lastReconnectTick_ = 0;
};

}