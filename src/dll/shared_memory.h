#pragma once

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "../../include/ets2la_telemetry/protocol.h"
#include "../../include/ets2la_telemetry/telemetry_data.h"

namespace ets2la_telemetry {

class SecurityAttrs {
public:
    SecurityAttrs() {
        std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
        std::wstring sid = current_user_sid();
        if (!sid.empty()) {
            sddl += L"(A;;GA;;;" + sid + L")";
        }
        sddl += L"S:(ML;;NW;;;ME)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
            sa_.nLength = sizeof(sa_);
            sa_.lpSecurityDescriptor = sd_;
            sa_.bInheritHandle = FALSE;
            ok_ = true;
        }
    }
    ~SecurityAttrs() {
        if (sd_) {
            LocalFree(sd_);
        }
    }
    SecurityAttrs(const SecurityAttrs&) = delete;
    SecurityAttrs& operator=(const SecurityAttrs&) = delete;
    SECURITY_ATTRIBUTES* get() {
        return ok_ ? &sa_ : nullptr;
    }

private:
    static std::wstring current_user_sid() {
        std::wstring result;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return result;
        }
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        if (needed) {
            std::string buf(needed, '\0');
            if (GetTokenInformation(token, TokenUser, &buf[0], needed, &needed)) {
                auto* user = reinterpret_cast<TOKEN_USER*>(&buf[0]);
                LPWSTR str = nullptr;
                if (ConvertSidToStringSidW(user->User.Sid, &str)) {
                    result = str;
                    LocalFree(str);
                }
            }
        }
        CloseHandle(token);
        return result;
    }

    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    bool ok_ = false;
};

class SharedTelemetryWriter {
public:
    ~SharedTelemetryWriter() {
        shutdown();
    }

    const std::string& last_error() const {
        return lastError_;
    }

    bool init() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ctl_) {
            return true;
        }

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);

        bool existed = false;
        ctlMap_ = create_mapping(kControlName, kControlBytes, &existed);
        if (!ctlMap_) {
            return fail("CreateFileMapping(control) failed");
        }
        ctl_ = static_cast<ControlBlock*>(MapViewOfFile(ctlMap_, FILE_MAP_WRITE, 0, 0, kControlBytes));
        if (!ctl_) {
            return fail("MapViewOfFile(control) failed");
        }

        for (int i = 0; i < 2; ++i) {
            events_[i] = create_event(kEventNames[i]);
            if (!events_[i]) {
                return fail("CreateEvent failed");
            }
        }

        if (!existed || ctl_->magic != kControlMagic || ctl_->version != kProtocolVersion) {
            std::memset(ctl_, 0, sizeof(ControlBlock));
        }
        ctl_->version = kProtocolVersion;
        ctl_->slot_count = kSlotCount;
        ctl_->max_readers = kMaxReaders;
        ctl_->qpc_frequency = uint64_t(freq.QuadPart);
        ctl_->writer_pid = GetCurrentProcessId();
        ctl_->active.store(0, std::memory_order_relaxed);
        ctl_->paused.store(1, std::memory_order_relaxed);
        ctl_->magic = kControlMagic;

        const uint64_t latest = ctl_->latest_frame.load(std::memory_order_relaxed);
        ResetEvent(events_[(latest + 1) & 1]);
        if (latest > 0) {
            SetEvent(events_[latest & 1]);
        } else {
            ResetEvent(events_[latest & 1]);
        }

        return init_data();
    }

    bool publish(const TelemetryData& src, uint64_t timestampQpc, uint64_t frameIntervalTicks, bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ctl_ || !data_) {
            return false;
        }

        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);

        const uint64_t index = ctl_->latest_frame.load(std::memory_order_relaxed) + 1;
        uint8_t* slotBase = data_ + kDataHeaderBytes + (index % kSlotCount) * slotStride_;
        auto* slot = reinterpret_cast<SlotHeader*>(slotBase);
        uint8_t* dst = slotBase + kSlotHeaderBytes;

        const uint64_t s = slot->seq.load(std::memory_order_relaxed);
        slot->seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        slot->frame_index = index;
        slot->timestamp_qpc = timestampQpc;
        std::memcpy(dst, &src, sizeof(TelemetryData));
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        slot->publish_qpc = uint64_t(now.QuadPart);

        slot->seq.store(s + 2, std::memory_order_release);

        ResetEvent(events_[(index + 1) & 1]);
        ctl_->paused.store(paused ? 1u : 0u, std::memory_order_relaxed);
        ctl_->frame_interval_ticks.store(frameIntervalTicks, std::memory_order_relaxed);
        ctl_->publish_cost_ticks.store(uint64_t(now.QuadPart) - uint64_t(start.QuadPart), std::memory_order_relaxed);
        ctl_->latest_frame.store(index, std::memory_order_release);
        ctl_->last_publish_qpc.store(uint64_t(now.QuadPart), std::memory_order_relaxed);
        ctl_->frames_published.fetch_add(1, std::memory_order_relaxed);
        SetEvent(events_[index & 1]);
        return true;
    }

    bool has_active_readers() const {
        if (!ctl_) {
            return false;
        }
        const uint64_t now = GetTickCount64();
        for (uint32_t i = 0; i < kMaxReaders; ++i) {
            const auto& r = ctl_->readers[i];
            if (r.id.load(std::memory_order_relaxed) != 0 && now - r.heartbeat_ms.load(std::memory_order_relaxed) < kReaderTimeoutMs) {
                return true;
            }
        }
        return false;
    }

    void set_active(bool on) {
        if (ctl_) {
            ctl_->active.store(on ? 1u : 0u, std::memory_order_relaxed);
        }
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ctl_) {
            ctl_->active.store(0, std::memory_order_relaxed);
        }
        close_data();
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

private:
    bool fail(const char* msg) { lastError_ = msg; return false; }

    HANDLE create_mapping(const wchar_t* name, uint64_t size, bool* existed) {
        const DWORD hi = DWORD(size >> 32), lo = DWORD(size & 0xFFFFFFFFu);
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, sa_.get(), PAGE_READWRITE, hi, lo, name);
        if (!h && sa_.get()) {
            h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, name);
        }
        if (existed) {
            *existed = (h != nullptr) && GetLastError() == ERROR_ALREADY_EXISTS;
        }
        return h;
    }

    HANDLE create_event(const wchar_t* name) {
        HANDLE h = CreateEventW(sa_.get(), TRUE, FALSE, name);
        if (!h && sa_.get()) {
            h = CreateEventW(nullptr, TRUE, FALSE, name);
        }
        return h;
    }

    void close_data() {
        if (data_) {
            UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (dataMap_) {
            CloseHandle(dataMap_);
            dataMap_ = nullptr;
        }
    }

    bool init_data() {
        const uint64_t stride = slot_stride_for(sizeof(TelemetryData));
        const uint64_t total = kDataHeaderBytes + stride * kSlotCount;

        bool existed = false;
        HANDLE map = create_mapping(kDataName, total, &existed);
        if (!map) {
            return fail("CreateFileMapping failed");
        }

        auto* base = static_cast<uint8_t*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0));
        if (!base) {
            CloseHandle(map);
            return fail("MapViewOfFile failed");
        }
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(base, &info, sizeof(info)) == 0 || info.RegionSize < total) {
            UnmapViewOfFile(base);
            CloseHandle(map);
            return fail("existing data mapping is too small");
        }

        auto* hdr = reinterpret_cast<DataHeader*>(base);
        if (!existed || hdr->magic != kDataMagic || hdr->version != kProtocolVersion) {
            std::memset(base, 0, size_t(total));
            ctl_->latest_frame.store(0, std::memory_order_relaxed);
        }
        hdr->version = kProtocolVersion;
        hdr->slot_count = kSlotCount;
        hdr->data_size = uint32_t(sizeof(TelemetryData));
        hdr->slot_stride = stride;
        hdr->slots_offset = kDataHeaderBytes;
        hdr->total_size = total;
        std::atomic_thread_fence(std::memory_order_release);
        hdr->magic = kDataMagic;

        close_data();
        dataMap_ = map;
        data_ = base;
        slotStride_ = stride;
        return true;
    }

    std::mutex mutex_;
    SecurityAttrs sa_;
    std::string lastError_;

    HANDLE ctlMap_ = nullptr;
    ControlBlock* ctl_ = nullptr;
    HANDLE events_[2] = {nullptr, nullptr};

    HANDLE dataMap_ = nullptr;
    uint8_t* data_ = nullptr;
    uint64_t slotStride_ = 0;
};

}