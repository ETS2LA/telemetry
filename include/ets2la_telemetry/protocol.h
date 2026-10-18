#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ets2la_telemetry {

constexpr uint32_t kControlMagic = 0x32545445;  // "ETT2"
constexpr uint32_t kDataMagic = 0x324D5445;     // "ETM2"

// Bump whenever ControlBlock/DataHeader/TelemetryData layout or semantics change in any way.
constexpr uint32_t kProtocolVersion = 1;

constexpr uint32_t kSlotCount = 4;              // snapshots kept in the ring
constexpr uint32_t kMaxReaders = 32;            // heartbeat table size
constexpr uint32_t kReaderTimeoutMs = 10000;    // reader considered gone after this

constexpr uint64_t kControlBytes = 4096;
constexpr uint64_t kDataHeaderBytes = 4096;
constexpr uint64_t kSlotHeaderBytes = 64;       // the snapshot starts this far into a slot

inline constexpr const wchar_t* kControlName = L"Local\\ETS2LA_Telemetry_Control";
inline constexpr const wchar_t* kDataName = L"Local\\ETS2LA_Telemetry_Data";
inline constexpr const wchar_t* kEventNames[2] = {
    L"Local\\ETS2LA_Telemetry_Frame0",
    L"Local\\ETS2LA_Telemetry_Frame1",
};

struct ReaderEntry {                        // 64 bytes, one cache line per reader
    std::atomic<uint64_t> id;               // 0 = free, else (pid << 32) | counter
    std::atomic<uint64_t> heartbeat_ms;     // GetTickCount64() at last sign of life
    uint8_t pad[48];
};

struct ControlBlock {
    // cache line 0: written once by the writer
    uint32_t magic;                         //   0
    uint32_t version;                       //   4
    uint32_t slot_count;                    //   8
    uint32_t max_readers;                   //  12
    uint64_t qpc_frequency;                 //  16
    uint32_t writer_pid;                    //  24
    uint32_t pad0;                          //  28
    uint8_t pad1[32];                       //  32
    // cache line 1..: updated per frame by the writer
    std::atomic<uint32_t> active;           //  64  1 while the game runs the telemetry API
    std::atomic<uint32_t> paused;           //  68  1 while the game is paused
    std::atomic<uint64_t> latest_frame;     //  72  index of newest complete snapshot, 0 = none
    std::atomic<uint64_t> frames_published; //  80
    std::atomic<uint64_t> last_publish_qpc; //  88
    std::atomic<uint64_t> frame_interval_ticks; // 96  QPC ticks between the last two game frames
    std::atomic<uint64_t> publish_cost_ticks;   // 104 QPC ticks the writer needed to publish the last snapshot
    uint8_t pad2[16];                       // 112
    ReaderEntry readers[kMaxReaders];       // 128
};

struct DataHeader {                         // lives in the first 4096 bytes
    uint32_t magic;                         //  0
    uint32_t version;                       //  4
    uint32_t slot_count;                    //  8
    uint32_t data_size;                     // 12  sizeof(TelemetryData)
    uint64_t slot_stride;                   // 16  bytes from one slot to the next
    uint64_t slots_offset;                  // 24  == kDataHeaderBytes
    uint64_t total_size;                    // 32
};

struct SlotHeader {                         // 64 bytes, the snapshot follows at +kSlotHeaderBytes
    std::atomic<uint64_t> seq;              //  0  even = stable, odd = being written
    uint64_t frame_index;                   //  8
    uint64_t timestamp_qpc;                 // 16  QPC when the game started the frame
    uint64_t publish_qpc;                   // 24  QPC when the snapshot became readable
    uint8_t pad[32];
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "need lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");
static_assert(sizeof(ReaderEntry) == 64, "ReaderEntry layout");
static_assert(sizeof(SlotHeader) == 64, "SlotHeader layout");
static_assert(offsetof(ControlBlock, version) == 4, "ControlBlock layout");
static_assert(offsetof(ControlBlock, slot_count) == 8, "ControlBlock layout");
static_assert(offsetof(ControlBlock, max_readers) == 12, "ControlBlock layout");
static_assert(offsetof(ControlBlock, qpc_frequency) == 16, "ControlBlock layout");
static_assert(offsetof(ControlBlock, writer_pid) == 24, "ControlBlock layout");
static_assert(offsetof(ControlBlock, active) == 64, "ControlBlock layout");
static_assert(offsetof(ControlBlock, paused) == 68, "ControlBlock layout");
static_assert(offsetof(ControlBlock, latest_frame) == 72, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frames_published) == 80, "ControlBlock layout");
static_assert(offsetof(ControlBlock, last_publish_qpc) == 88, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frame_interval_ticks) == 96, "ControlBlock layout");
static_assert(offsetof(ControlBlock, publish_cost_ticks) == 104, "ControlBlock layout");
static_assert(offsetof(ControlBlock, readers) == 128, "ControlBlock layout");
static_assert(sizeof(ControlBlock) <= kControlBytes, "ControlBlock too large");
static_assert(offsetof(DataHeader, version) == 4, "DataHeader layout");
static_assert(offsetof(DataHeader, slot_count) == 8, "DataHeader layout");
static_assert(offsetof(DataHeader, data_size) == 12, "DataHeader layout");
static_assert(offsetof(DataHeader, slot_stride) == 16, "DataHeader layout");
static_assert(offsetof(DataHeader, slots_offset) == 24, "DataHeader layout");
static_assert(offsetof(DataHeader, total_size) == 32, "DataHeader layout");
static_assert(sizeof(DataHeader) <= kDataHeaderBytes, "DataHeader too large");

inline constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
inline constexpr uint64_t slot_stride_for(uint64_t payload_bytes) {
    return align_up(kSlotHeaderBytes + payload_bytes, 4096);
}

}