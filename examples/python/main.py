from ets2la_telemetry import TelemetryReader

reader = TelemetryReader()

while True:
    telemetry = reader.get_telemetry()
    if telemetry is None:
        continue

    print(f"speed {telemetry.truck.speed * 3.6:6.1f} km/h  rpm {telemetry.truck.engine_rpm:5.0f}  age {telemetry.age_ms():.2f} ms")