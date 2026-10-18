#pragma once

#include "scs_logging.h"

using scs_context_t = void*;
using scs_value_type_t = scs_u32_t;
using scs_event_t = scs_u32_t;
using scs_timestamp_t = scs_u64_t;

constexpr scs_u32_t SCS_TELEMETRY_VERSION_1_00 = (1u << 16) | 0u;
constexpr scs_u32_t SCS_TELEMETRY_VERSION_1_01 = (1u << 16) | 1u;
constexpr scs_u32_t SCS_U32_NIL = 0xFFFFFFFFu;
constexpr scs_value_type_t SCS_VALUE_TYPE_bool = 1;
constexpr scs_value_type_t SCS_VALUE_TYPE_s32 = 2;
constexpr scs_value_type_t SCS_VALUE_TYPE_u32 = 3;
constexpr scs_value_type_t SCS_VALUE_TYPE_u64 = 4;
constexpr scs_value_type_t SCS_VALUE_TYPE_float = 5;
constexpr scs_value_type_t SCS_VALUE_TYPE_double = 6;
constexpr scs_value_type_t SCS_VALUE_TYPE_fvector = 7;
constexpr scs_value_type_t SCS_VALUE_TYPE_dvector = 8;
constexpr scs_value_type_t SCS_VALUE_TYPE_euler = 9;
constexpr scs_value_type_t SCS_VALUE_TYPE_fplacement = 10;
constexpr scs_value_type_t SCS_VALUE_TYPE_dplacement = 11;
constexpr scs_value_type_t SCS_VALUE_TYPE_string = 12;
constexpr scs_value_type_t SCS_VALUE_TYPE_s64 = 13;
constexpr scs_event_t SCS_TELEMETRY_EVENT_frame_start = 1;
constexpr scs_event_t SCS_TELEMETRY_EVENT_frame_end = 2;
constexpr scs_event_t SCS_TELEMETRY_EVENT_paused = 3;
constexpr scs_event_t SCS_TELEMETRY_EVENT_started = 4;
constexpr scs_event_t SCS_TELEMETRY_EVENT_configuration = 5;
constexpr scs_event_t SCS_TELEMETRY_EVENT_gameplay = 6;
constexpr scs_u32_t SCS_TELEMETRY_FRAME_START_FLAG_timer_restart = 0x00000001;
constexpr scs_u32_t SCS_TELEMETRY_CHANNEL_FLAG_no_value = 0x00000002;

struct scs_value_fvector_t {
    float x;
    float y;
    float z;
};

struct scs_value_dvector_t {
    double x;
    double y;
    double z;
};

struct scs_value_euler_t {
    float heading;
    float pitch;
    float roll;
};

struct scs_value_fplacement_t {
    scs_value_fvector_t position;
    scs_value_euler_t orientation;
};

struct scs_value_dplacement_t {
    scs_value_dvector_t position;
    scs_value_euler_t orientation;
    scs_u32_t _padding;
};

struct scs_value_t {
    scs_value_type_t type;
#if defined(_WIN64)
    scs_u32_t _padding;
#endif
    union {
        scs_u8_t value_bool;
        scs_s32_t value_s32;
        scs_u32_t value_u32;
        scs_u64_t value_u64;
        scs_s64_t value_s64;
        float value_float;
        double value_double;
        scs_value_fvector_t value_fvector;
        scs_value_dvector_t value_dvector;
        scs_value_euler_t value_euler;
        scs_value_fplacement_t value_fplacement;
        scs_value_dplacement_t value_dplacement;
        scs_string_t value_string;
    };
};

struct scs_named_value_t {
    scs_string_t name;
    scs_u32_t index;
#if defined(_WIN64)
    scs_u32_t _padding;
#endif
    scs_value_t value;
};

struct scs_telemetry_frame_start_t {
    scs_u32_t flags;
#if defined(_WIN64)
    scs_u32_t _padding;
#endif
    scs_timestamp_t render_time;
    scs_timestamp_t simulation_time;
    scs_timestamp_t paused_simulation_time;
};

struct scs_telemetry_configuration_t {
    scs_string_t id;
    const scs_named_value_t* attributes;
};

struct scs_telemetry_gameplay_event_t {
    scs_string_t id;
    const scs_named_value_t* attributes;
};

using scs_telemetry_event_callback_t = void (SCSAPIFUNC*)(
    const scs_event_t event,
    const void* const event_info,
    const scs_context_t context
);

using scs_telemetry_channel_callback_t = void (SCSAPIFUNC*)(
    const scs_string_t name,
    const scs_u32_t index,
    const scs_value_t* const value,
    const scs_context_t context
);

using scs_telemetry_register_for_event_t = scs_result_t (SCSAPIFUNC*)(
    const scs_event_t event,
    const scs_telemetry_event_callback_t callback,
    const scs_context_t context
);

using scs_telemetry_unregister_from_event_t = scs_result_t (SCSAPIFUNC*)(const scs_event_t event);

using scs_telemetry_register_for_channel_t = scs_result_t (SCSAPIFUNC*)(
    const scs_string_t name,
    const scs_u32_t index,
    const scs_value_type_t type,
    const scs_u32_t flags,
    const scs_telemetry_channel_callback_t callback,
    const scs_context_t context
);

using scs_telemetry_unregister_from_channel_t = scs_result_t (SCSAPIFUNC*)(
    const scs_string_t name,
    const scs_u32_t index,
    const scs_value_type_t type
);

struct scs_telemetry_init_params_t {
    void method_indicating_this_is_not_a_c_struct(void);
};

struct scs_telemetry_init_params_v100_t : scs_telemetry_init_params_t {
    scs_sdk_init_params_v100_t common;
    scs_telemetry_register_for_event_t register_for_event;
    scs_telemetry_unregister_from_event_t unregister_from_event;
    scs_telemetry_register_for_channel_t register_for_channel;
    scs_telemetry_unregister_from_channel_t unregister_from_channel;
};