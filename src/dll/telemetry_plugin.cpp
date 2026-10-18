#include "telemetry_plugin.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "bindings.h"
#include "shared_memory.h"

namespace ets2la_telemetry {
namespace {

constexpr scs_u32_t kGameEts2 = 1;
constexpr scs_u32_t kGameAts = 2;

enum class Kind : uint8_t { Bool, S32, U32, S64, U64, F32, F64, Vec3f, Vec3d, Euler, PlacementF, PlacementD, Str };

template <class T> struct KindOf;
template <> struct KindOf<bool> { static constexpr Kind kind = Kind::Bool; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_bool; };
template <> struct KindOf<int32_t> { static constexpr Kind kind = Kind::S32; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_s32; };
template <> struct KindOf<uint32_t> { static constexpr Kind kind = Kind::U32; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_u32; };
template <> struct KindOf<int64_t> { static constexpr Kind kind = Kind::S64; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_s64; };
template <> struct KindOf<uint64_t> { static constexpr Kind kind = Kind::U64; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_u64; };
template <> struct KindOf<float> { static constexpr Kind kind = Kind::F32; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_float; };
template <> struct KindOf<double> { static constexpr Kind kind = Kind::F64; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_double; };
template <> struct KindOf<Vec3f> { static constexpr Kind kind = Kind::Vec3f; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_fvector; };
template <> struct KindOf<Vec3d> { static constexpr Kind kind = Kind::Vec3d; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_dvector; };
template <> struct KindOf<Euler> { static constexpr Kind kind = Kind::Euler; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_euler; };
template <> struct KindOf<PlacementF> { static constexpr Kind kind = Kind::PlacementF; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_fplacement; };
template <> struct KindOf<PlacementD> { static constexpr Kind kind = Kind::PlacementD; static constexpr scs_value_type_t scs = SCS_VALUE_TYPE_dplacement; };

inline uint64_t qpc_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return uint64_t(q.QuadPart);
}

size_t kind_size(Kind k) {
    switch (k) {
        case Kind::Bool: return sizeof(bool);
        case Kind::S32: return sizeof(int32_t);
        case Kind::U32: return sizeof(uint32_t);
        case Kind::S64: return sizeof(int64_t);
        case Kind::U64: return sizeof(uint64_t);
        case Kind::F32: return sizeof(float);
        case Kind::F64: return sizeof(double);
        case Kind::Vec3f: return sizeof(Vec3f);
        case Kind::Vec3d: return sizeof(Vec3d);
        case Kind::Euler: return sizeof(Euler);
        case Kind::PlacementF: return sizeof(PlacementF);
        case Kind::PlacementD: return sizeof(PlacementD);
        case Kind::Str: return 0;
    }
    return 0;
}


inline void to_vec(Vec3f& d, const scs_value_fvector_t& s) { d.x = s.x; d.y = s.y; d.z = s.z; }
inline void to_vec(Vec3d& d, const scs_value_dvector_t& s) { d.x = s.x; d.y = s.y; d.z = s.z; }
inline void to_vec(Vec3f& d, const scs_value_dvector_t& s) { d.x = float(s.x); d.y = float(s.y); d.z = float(s.z); }
inline void to_vec(Vec3d& d, const scs_value_fvector_t& s) { d.x = s.x; d.y = s.y; d.z = s.z; }
inline void to_euler(Euler& d, const scs_value_euler_t& s) { d.heading = s.heading; d.pitch = s.pitch; d.roll = s.roll; }

inline void store_value(bool* d, const scs_value_t* v) { *d = v && v->value_bool != 0; }
inline void store_value(int32_t* d, const scs_value_t* v) { *d = v ? v->value_s32 : 0; }
inline void store_value(uint32_t* d, const scs_value_t* v) { *d = v ? v->value_u32 : 0u; }
inline void store_value(int64_t* d, const scs_value_t* v) { *d = v ? v->value_s64 : 0; }
inline void store_value(uint64_t* d, const scs_value_t* v) { *d = v ? v->value_u64 : 0u; }
inline void store_value(float* d, const scs_value_t* v) { *d = v ? v->value_float : 0.0f; }
inline void store_value(double* d, const scs_value_t* v) { *d = v ? v->value_double : 0.0; }
inline void store_value(Vec3f* d, const scs_value_t* v) {
    if (v) to_vec(*d, v->value_fvector); else std::memset(d, 0, sizeof(*d));
}
inline void store_value(Vec3d* d, const scs_value_t* v) {
    if (v) to_vec(*d, v->value_dvector); else std::memset(d, 0, sizeof(*d));
}
inline void store_value(Euler* d, const scs_value_t* v) {
    if (v) to_euler(*d, v->value_euler); else std::memset(d, 0, sizeof(*d));
}
inline void store_value(PlacementF* d, const scs_value_t* v) {
    if (v) { to_vec(d->position, v->value_fplacement.position); to_euler(d->orientation, v->value_fplacement.orientation); }
    else std::memset(d, 0, sizeof(*d));
}
inline void store_value(PlacementD* d, const scs_value_t* v) {
    if (v) { to_vec(d->position, v->value_dplacement.position); to_euler(d->orientation, v->value_dplacement.orientation); }
    else std::memset(d, 0, sizeof(*d));
}

template <class T>
void SCSAPIFUNC on_channel(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t context) {
    store_value(static_cast<T*>(context), value);
}

struct Entry {
    Kind kind = Kind::U32;
    uint8_t* base = nullptr;
    size_t stride = 0;
    size_t count = 1;
    size_t cap = 0;
};

struct Group {
    std::unordered_map<std::string, Entry> attrs;
    bool* present = nullptr;
    uint32_t* counter = nullptr;
    uint32_t* counterTime = nullptr;
};

bool assign(const Entry& e, void* dst, const scs_value_t& v) {
    switch (e.kind) {
        case Kind::Bool:
            if (v.type != SCS_VALUE_TYPE_bool) return false;
            *static_cast<bool*>(dst) = v.value_bool != 0;
            return true;
        case Kind::S32:
            if (v.type == SCS_VALUE_TYPE_s32) *static_cast<int32_t*>(dst) = v.value_s32;
            else if (v.type == SCS_VALUE_TYPE_u32) *static_cast<int32_t*>(dst) = int32_t(v.value_u32);
            else return false;
            return true;
        case Kind::U32:
            if (v.type == SCS_VALUE_TYPE_u32) *static_cast<uint32_t*>(dst) = v.value_u32;
            else if (v.type == SCS_VALUE_TYPE_u64) *static_cast<uint32_t*>(dst) = uint32_t(v.value_u64);
            else if (v.type == SCS_VALUE_TYPE_s32) *static_cast<uint32_t*>(dst) = uint32_t(v.value_s32);
            else return false;
            return true;
        case Kind::S64:
            if (v.type == SCS_VALUE_TYPE_s64) *static_cast<int64_t*>(dst) = v.value_s64;
            else if (v.type == SCS_VALUE_TYPE_s32) *static_cast<int64_t*>(dst) = v.value_s32;
            else if (v.type == SCS_VALUE_TYPE_u32) *static_cast<int64_t*>(dst) = v.value_u32;
            else return false;
            return true;
        case Kind::U64:
            if (v.type == SCS_VALUE_TYPE_u64) *static_cast<uint64_t*>(dst) = v.value_u64;
            else if (v.type == SCS_VALUE_TYPE_u32) *static_cast<uint64_t*>(dst) = v.value_u32;
            else return false;
            return true;
        case Kind::F32:
            if (v.type == SCS_VALUE_TYPE_float) *static_cast<float*>(dst) = v.value_float;
            else if (v.type == SCS_VALUE_TYPE_double) *static_cast<float*>(dst) = float(v.value_double);
            else return false;
            return true;
        case Kind::F64:
            if (v.type == SCS_VALUE_TYPE_double) *static_cast<double*>(dst) = v.value_double;
            else if (v.type == SCS_VALUE_TYPE_float) *static_cast<double*>(dst) = v.value_float;
            else return false;
            return true;
        case Kind::Vec3f:
            if (v.type == SCS_VALUE_TYPE_fvector) to_vec(*static_cast<Vec3f*>(dst), v.value_fvector);
            else if (v.type == SCS_VALUE_TYPE_dvector) to_vec(*static_cast<Vec3f*>(dst), v.value_dvector);
            else return false;
            return true;
        case Kind::Vec3d:
            if (v.type == SCS_VALUE_TYPE_dvector) to_vec(*static_cast<Vec3d*>(dst), v.value_dvector);
            else if (v.type == SCS_VALUE_TYPE_fvector) to_vec(*static_cast<Vec3d*>(dst), v.value_fvector);
            else return false;
            return true;
        case Kind::Euler:
            if (v.type != SCS_VALUE_TYPE_euler) return false;
            to_euler(*static_cast<Euler*>(dst), v.value_euler);
            return true;
        case Kind::PlacementF:
            if (v.type != SCS_VALUE_TYPE_fplacement) return false;
            store_value(static_cast<PlacementF*>(dst), &v);
            return true;
        case Kind::PlacementD:
            if (v.type != SCS_VALUE_TYPE_dplacement) return false;
            store_value(static_cast<PlacementD*>(dst), &v);
            return true;
        case Kind::Str: {
            if (v.type != SCS_VALUE_TYPE_string || e.cap == 0) return false;
            char* out = static_cast<char*>(dst);
            std::memset(out, 0, e.cap);
            if (v.value_string) {
                const size_t len = std::strlen(v.value_string);
                std::memcpy(out, v.value_string, len < e.cap - 1 ? len : e.cap - 1);
            }
            return true;
        }
    }
    return false;
}

class TelemetryPlugin {
public:
    bool start(const scs_telemetry_init_params_v100_t& params, scs_u32_t apiVersion) {
        stop();
        if (!shared_.init()) {
            scs_logging::writef(SCS_LOG_TYPE_error, "shared memory failed: %s", shared_.last_error().c_str());
            return false;
        }
        params_ = &params;
        std::memset(&data_, 0, sizeof(data_));
        groups_.clear();
        names_.clear();
        warned_.clear();
        gameStarted_ = false;
        lastFrameQpc_ = 0;
        frameQpc_ = 0;
        frameIntervalTicks_ = 0;
        channelsOk_ = channelsFailed_ = 0;

        Game& g = data_.game;
        g.paused = true;
        g.api_major = (apiVersion >> 16);
        g.api_minor = (apiVersion & 0xFFFFu);
        g.version_major = (params.common.game_version >> 16);
        g.version_minor = (params.common.game_version & 0xFFFFu);
        const char* gameId = params.common.game_id ? params.common.game_id : "";
        if (std::strcmp(gameId, "eut2") == 0) {
            g.id = kGameEts2;
        } else if (std::strcmp(gameId, "ats") == 0) {
            g.id = kGameAts;
        } else {
            g.id = 0;
            scs_logging::writef(SCS_LOG_TYPE_warning, "unknown game id \"%s\", values may be missing", gameId);
        }
        legacyTrailer_ = (g.id == kGameEts2 && g.version_minor < 14) || (g.id == kGameAts && g.version_minor < 1);

        if (params.register_for_event(SCS_TELEMETRY_EVENT_frame_start, &TelemetryPlugin::on_frame_start, this) != SCS_RESULT_ok ||
            params.register_for_event(SCS_TELEMETRY_EVENT_frame_end, &TelemetryPlugin::on_frame_end, this) != SCS_RESULT_ok ||
            params.register_for_event(SCS_TELEMETRY_EVENT_paused, &TelemetryPlugin::on_pause, this) != SCS_RESULT_ok ||
            params.register_for_event(SCS_TELEMETRY_EVENT_started, &TelemetryPlugin::on_pause, this) != SCS_RESULT_ok) {
            scs_logging::write(SCS_LOG_TYPE_error, "unable to register the frame events");
            shared_.shutdown();
            return false;
        }
        params.register_for_event(SCS_TELEMETRY_EVENT_configuration, &TelemetryPlugin::on_configuration, this);
        if (params.register_for_event(SCS_TELEMETRY_EVENT_gameplay, &TelemetryPlugin::on_gameplay, this) != SCS_RESULT_ok) {
            scs_logging::write(SCS_LOG_TYPE_warning, "gameplay events are not supported by this game version");
        }

        Registry reg{this};
        bind_config_Truck(reg, "truck", &data_.truck, 1);
        bind_config_Job(reg, "job", &data_.job, 1);
        bind_config_Substances(reg, "substances", &data_.substances, 1);
        for (uint32_t i = 0; i < kTrailerMax; ++i) {
            bind_config_Trailer(reg, "trailer." + std::to_string(i), &data_.trailers[i], 1);
        }
        if (legacyTrailer_) {
            bind_config_Trailer(reg, "trailer", &data_.trailers[0], 1);
        }
        bind_config_JobDeliveredEvent(reg, data_.events.job_delivered);
        bind_config_JobCancelledEvent(reg, data_.events.job_cancelled);
        bind_config_FinedEvent(reg, data_.events.fined);
        bind_config_TollgateEvent(reg, data_.events.tollgate);
        bind_config_FerryEvent(reg, data_.events.ferry);
        bind_config_TrainEvent(reg, data_.events.train);

        bind_channels_Game(reg, data_.game, "");
        bind_channels_Truck(reg, data_.truck, "truck.");
        bind_channels_Job(reg, data_.job, "job.");
        if (legacyTrailer_) {
            bind_channels_Trailer(reg, data_.trailers[0], "trailer.");
        } else {
            for (uint32_t i = 0; i < kTrailerMax; ++i) {
                bind_channels_Trailer(reg, data_.trailers[i], "trailer." + std::to_string(i) + ".");
            }
        }

        shared_.set_active(true);
        scs_logging::writef(SCS_LOG_TYPE_message, "registered %u channels (%u unavailable), game %u, telemetry api %u.%u, protocol v%u",
                            channelsOk_, channelsFailed_, g.id, g.api_major, g.api_minor, kProtocolVersion);
        running_ = true;
        return true;
    }

    void stop() {
        running_ = false;
        shared_.shutdown();
        params_ = nullptr;
    }

private:
    struct Registry {
        TelemetryPlugin* self;

        template <class T>
        void channel(const std::string& name, uint32_t index, T* dst) {
            self->register_channel(name, index, KindOf<T>::scs, &on_channel<T>, dst);
        }

        template <class T>
        void attr(const std::string& id, const char* name, T* base, size_t stride, size_t count) {
            Entry e;
            e.kind = KindOf<T>::kind;
            e.base = reinterpret_cast<uint8_t*>(base);
            e.stride = stride ? stride : sizeof(T);
            e.count = count;
            self->groups_[id].attrs[name] = e;
        }

        void attr_str(const std::string& id, const char* name, char* base, size_t cap, size_t stride, size_t count) {
            Entry e;
            e.kind = Kind::Str;
            e.base = reinterpret_cast<uint8_t*>(base);
            e.stride = stride ? stride : cap;
            e.count = count;
            e.cap = cap;
            self->groups_[id].attrs[name] = e;
        }

        void present(const std::string& id, bool* base) {
            self->groups_[id].present = base;
        }

        void counter(const std::string& id, uint32_t* count, uint32_t* time) {
            Group& g = self->groups_[id];
            g.counter = count;
            g.counterTime = time;
        }
    };

    void register_channel(const std::string& name, uint32_t index, scs_value_type_t type,
                          scs_telemetry_channel_callback_t cb, void* dst) {
        names_.push_back(name);
        const scs_result_t r = params_->register_for_channel(names_.back().c_str(), index, type,
                                                             SCS_TELEMETRY_CHANNEL_FLAG_no_value, cb, dst);
        if (r == SCS_RESULT_ok) {
            ++channelsOk_;
        } else {
            ++channelsFailed_;
        }
    }

    static void SCSAPIFUNC on_frame_start(const scs_event_t, const void* const info, const scs_context_t ctx) {
        static_cast<TelemetryPlugin*>(ctx)->frame_start(*static_cast<const scs_telemetry_frame_start_t*>(info));
    }
    static void SCSAPIFUNC on_frame_end(const scs_event_t, const void*, const scs_context_t ctx) {
        static_cast<TelemetryPlugin*>(ctx)->frame_end();
    }
    static void SCSAPIFUNC on_pause(const scs_event_t event, const void*, const scs_context_t ctx) {
        static_cast<TelemetryPlugin*>(ctx)->data_.game.paused = (event == SCS_TELEMETRY_EVENT_paused);
    }
    static void SCSAPIFUNC on_configuration(const scs_event_t, const void* const info, const scs_context_t ctx) {
        static_cast<TelemetryPlugin*>(ctx)->configuration(*static_cast<const scs_telemetry_configuration_t*>(info));
    }
    static void SCSAPIFUNC on_gameplay(const scs_event_t, const void* const info, const scs_context_t ctx) {
        static_cast<TelemetryPlugin*>(ctx)->gameplay(*static_cast<const scs_telemetry_gameplay_event_t*>(info));
    }

    void frame_start(const scs_telemetry_frame_start_t& info) {
        Game& g = data_.game;
        if (!gameStarted_) {
            lastRender_ = info.render_time;
            lastSimulation_ = info.simulation_time;
            lastPaused_ = info.paused_simulation_time;
            gameStarted_ = true;
        }
        if (info.flags & SCS_TELEMETRY_FRAME_START_FLAG_timer_restart) {
            lastRender_ = lastSimulation_ = lastPaused_ = 0;
            ++g.timer_restarts;
        }
        g.render_time_us += info.render_time - lastRender_;
        g.simulation_time_us += info.simulation_time - lastSimulation_;
        g.paused_simulation_time_us += info.paused_simulation_time - lastPaused_;
        lastRender_ = info.render_time;
        lastSimulation_ = info.simulation_time;
        lastPaused_ = info.paused_simulation_time;

        frameQpc_ = qpc_now();
        frameIntervalTicks_ = lastFrameQpc_ ? frameQpc_ - lastFrameQpc_ : 0;
        lastFrameQpc_ = frameQpc_;
    }

    void frame_end() {
        data_.truck.cruise_control_enabled = data_.truck.cruise_control_speed > 0.0f;
        if (data_.job.present) {
            data_.job.delivery_time_remaining_minutes =
                int32_t(data_.job.delivery_time_minutes) - int32_t(data_.game.game_time_minutes);
        } else {
            data_.job.delivery_time_remaining_minutes = 0;
        }
        shared_.publish(data_, frameQpc_ ? frameQpc_ : qpc_now(), frameIntervalTicks_, data_.game.paused);
    }

    void configuration(const scs_telemetry_configuration_t& info) {
        ++data_.game.config_changes;
        const std::string id = info.id ? info.id : "";
        auto it = groups_.find(id);
        if (it == groups_.end()) {
            warn_once("configuration", id);
            return;
        }
        Group& group = it->second;
        const bool wasPresent = group.present && *group.present;
        clear(group);
        const bool empty = !info.attributes || !info.attributes->name;
        if (group.present) {
            *group.present = !empty;
        }
        uint32_t substanceIds = 0;
        for (const scs_named_value_t* a = info.attributes; a && a->name; ++a) {
            apply(group, id, *a);
            if (id == "substances" && std::strcmp(a->name, "id") == 0) {
                ++substanceIds;
            }
        }
        if (id == "substances") {
            data_.substances.count = substanceIds;
        } else if (id == "job") {
            data_.job.started_game_time_minutes = (!empty && !wasPresent) ? data_.game.game_time_minutes
                                                  : (empty ? 0u : data_.job.started_game_time_minutes);
        }
    }

    void gameplay(const scs_telemetry_gameplay_event_t& info) {
        const std::string id = info.id ? info.id : "";
        auto it = groups_.find(id);
        if (it == groups_.end()) {
            warn_once("gameplay event", id);
            return;
        }
        Group& group = it->second;
        clear(group);
        for (const scs_named_value_t* a = info.attributes; a && a->name; ++a) {
            apply(group, id, *a);
        }
        if (group.counter) {
            ++*group.counter;
        }
        if (group.counterTime) {
            *group.counterTime = data_.game.game_time_minutes;
        }
    }

    static void clear(Group& group) {
        for (auto& kv : group.attrs) {
            const Entry& e = kv.second;
            const size_t bytes = e.kind == Kind::Str ? e.cap : kind_size(e.kind);
            for (size_t i = 0; i < e.count; ++i) {
                std::memset(e.base + i * e.stride, 0, bytes);
            }
        }
    }

    void apply(Group& group, const std::string& id, const scs_named_value_t& a) {
        auto it = group.attrs.find(a.name);
        if (it == group.attrs.end()) {
            warn_once(("attribute of " + id).c_str(), a.name);
            return;
        }
        const Entry& e = it->second;
        size_t index = 0;
        if (e.count > 1) {
            if (a.index >= e.count) {
                return;
            }
            index = a.index;
        } else if (a.index != SCS_U32_NIL && a.index != 0) {
            return;
        }
        if (!assign(e, e.base + index * e.stride, a.value)) {
            warn_once(("value type of " + id).c_str(), a.name);
        }
    }

    void warn_once(const char* what, const std::string& name) {
        const std::string key = std::string(what) + "/" + name;
        if (warned_.emplace(key, true).second) {
            scs_logging::writef(SCS_LOG_TYPE_warning, "ignoring unknown %s \"%s\"", what, name.c_str());
        }
    }

    SharedTelemetryWriter shared_;
    const scs_telemetry_init_params_v100_t* params_ = nullptr;
    TelemetryData data_{};
    std::unordered_map<std::string, Group> groups_;
    std::deque<std::string> names_;
    std::unordered_map<std::string, bool> warned_;
    bool running_ = false;
    bool legacyTrailer_ = false;
    bool gameStarted_ = false;
    uint64_t lastRender_ = 0, lastSimulation_ = 0, lastPaused_ = 0;
    uint64_t frameQpc_ = 0, lastFrameQpc_ = 0, frameIntervalTicks_ = 0;
    uint32_t channelsOk_ = 0, channelsFailed_ = 0;
};

TelemetryPlugin g_plugin;

}
}

bool StartTelemetry(const scs_telemetry_init_params_v100_t& params, scs_u32_t apiVersion) {
    return ets2la_telemetry::g_plugin.start(params, apiVersion);
}

void StopTelemetry() {
    ets2la_telemetry::g_plugin.stop();
}