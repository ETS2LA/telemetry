# ets2la_telemetry

In-process telemetry for Euro Truck Simulator 2 / American Truck Simulator.
It registers with the game's telemetry SDK and publishes the truck, trailers, job and game state to any number of C++ / Python clients through shared memory.

Every value, its type and what it tells, see `docs/TELEMETRY.md`.

## Build

Requires CMake 3.15+ and MSVC (Visual Studio 2019+).

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Outputs `ets2la_telemetry.dll` (put it in `<game>/bin/win_x64/plugins/`).

Python client: `pip install ./python` (see `python/README.md`).

## Reading telemetry

```python
from ets2la_telemetry import TelemetryReader

reader = TelemetryReader()
telemetry = reader.get_telemetry()
print(telemetry.truck.speed * 3.6)
```

```cpp
#include "ets2la_telemetry/telemetry_reader.h"

ets2la_telemetry::TelemetryReader reader;
ets2la_telemetry::Telemetry telemetry;
if (reader.get_telemetry(telemetry)) {}
```

* `get_telemetry` blocks for a snapshot newer than the last one this reader got. First call after connecting waits for a fresh snapshot.
* `get_latest_telemetry` is non-blocking.
* Zero-copy views stay valid for ~3 newer snapshots (ring of 4). Finish (or copy) within about 2 frame times, then check `valid()`.
* `age_ms()` is the time since the game started the frame, `delivery_ms()` the delay the shared memory added, `stats()` shows frame interval and publish cost.