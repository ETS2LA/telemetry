#pragma once

#include "scs_telemetry_api.h"

bool StartTelemetry(const scs_telemetry_init_params_v100_t& params, scs_u32_t apiVersion);
void StopTelemetry();