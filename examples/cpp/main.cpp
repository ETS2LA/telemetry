#include "ets2la_telemetry/telemetry_reader.h"
#include <cstdio>


int main() {
    ets2la_telemetry::TelemetryReader reader;
    ets2la_telemetry::Telemetry telemetry;

    while (true) {
        if (!reader.get_telemetry(telemetry)) {
            continue;
        }

        std::printf("speed %6.1f km/h  rpm %5.0f  age %.2f ms\n",
            telemetry.truck.speed * 3.6f,
            telemetry.truck.engine_rpm,
            reader.age_ms(telemetry)
        );
    }
    return 0;
}