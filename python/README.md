# ets2la_telemetry

Lock-free reader for the `ets2la_telemetry.dll` shared memory.

## Installation

```powershell
pip install ./python
```

## Usage

```python
from ets2la_telemetry import TelemetryReader

reader = TelemetryReader()
telemetry = reader.get_telemetry(timeout_ms=1000)   # next new snapshot, or None
if telemetry is not None and telemetry.valid:
    speed = telemetry.truck.speed * 3.6              # km/h
    brand = telemetry.truck.brand.decode()           # strings are bytes
```

* `get_telemetry(copy=False)` returns a zero-copy view into shared memory. Check `telemetry.is_valid()` after use, if it is `False` the writer lapped you, discard.
* `get_latest_telemetry()` is non-blocking.
* `telemetry.age_ms()` is the time since the game started the frame, `telemetry.delivery_ms()` the delay the shared memory added.
* `reader.stats()` shows published frames, frame interval and publish cost.
* `reader.connected()` reports whether the telemetry DLL is currently attached.