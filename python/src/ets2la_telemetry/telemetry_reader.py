from __future__ import annotations

import ctypes
import struct
import time
from dataclasses import dataclass
from typing import Optional

from ._data import TelemetryData

CONTROL_MAGIC = 0x32545445
DATA_MAGIC = 0x324D5445
CONTROL_NAME = "Local\\ETS2LA_Telemetry_Control"
DATA_NAME = "Local\\ETS2LA_Telemetry_Data"
EVENT_NAMES = ("Local\\ETS2LA_Telemetry_Frame0", "Local\\ETS2LA_Telemetry_Frame1")

CONTROL_BYTES = 4096
SLOT_HEADER_BYTES = 64
MAX_READERS = 32
READER_TIMEOUT_MS = 10000

# Bump whenever ControlBlock/DataHeader/TelemetryData layout or semantics change in any way.
PROTOCOL_VERSION = 1

CTL_MAGIC = 0
CTL_VERSION = 4
CTL_SLOT_COUNT = 8
CTL_MAX_READERS = 12
CTL_QPC_FREQ = 16
CTL_WRITER_PID = 24
CTL_ACTIVE = 64
CTL_PAUSED = 68
CTL_LATEST = 72
CTL_PUBLISHED = 80
CTL_LAST_PUBLISH_QPC = 88
CTL_FRAME_INTERVAL = 96
CTL_PUBLISH_COST = 104
CTL_READERS = 128
READER_STRIDE = 64

# DataHeader: magic, version, slot_count, data_size, slot_stride, slots_offset, total_size
DATA_HEADER_FMT = "<IIIIQQQ"

# SlotHeader: seq, frame_index, timestamp_qpc, publish_qpc
SLOT_FMT = "<QQQQ"

SYNCHRONIZE = 0x00100000
FILE_MAP_WRITE = 0x0002
FILE_MAP_READ = 0x0004

DATA_SIZE = ctypes.sizeof(TelemetryData)


def _load_kernel32():
    try:
        k = ctypes.WinDLL("kernel32", use_last_error=True)
    except (AttributeError, OSError):
        return None
    from ctypes import wintypes as wt

    k.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
    k.OpenFileMappingW.restype = wt.HANDLE
    k.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
    k.MapViewOfFile.restype = ctypes.c_void_p
    k.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    k.UnmapViewOfFile.restype = wt.BOOL
    k.OpenEventW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
    k.OpenEventW.restype = wt.HANDLE
    k.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    k.WaitForSingleObject.restype = wt.DWORD
    k.CloseHandle.argtypes = [wt.HANDLE]
    k.CloseHandle.restype = wt.BOOL
    k.GetTickCount64.argtypes = []
    k.GetTickCount64.restype = ctypes.c_uint64
    k.QueryPerformanceCounter.argtypes = [ctypes.POINTER(ctypes.c_int64)]
    k.QueryPerformanceCounter.restype = wt.BOOL
    k.GetCurrentProcessId.argtypes = []
    k.GetCurrentProcessId.restype = wt.DWORD
    return k


kernel32 = _load_kernel32()


@dataclass(frozen=True)
class TelemetryStats:
    """Snapshot of the plugin state and the timing statistics."""

    latest_frame: int
    """Index of the newest snapshot."""
    frames_published: int
    """Snapshots published since the plugin loaded."""
    active: bool
    """The game currently runs the telemetry API."""
    paused: bool
    """The game is paused (menu, map, loading)."""
    writer_pid: int
    """Process id of the game that loaded the plugin."""
    frame_interval_ms: float
    """Time between the last two game frames."""
    publish_cost_us: float
    """Time the plugin needed to publish the last snapshot, in microseconds."""
    since_last_publish_ms: float
    """Time since the last snapshot was published."""
    readers: int
    """Readers (this one included) that recently sent a sign of life."""


class _Guard:
    def __init__(self, handle, addr):
        self.handle = handle
        self.addr = addr

    def __del__(self):
        try:
            if self.addr:
                kernel32.UnmapViewOfFile(self.addr)
            if self.handle:
                kernel32.CloseHandle(self.handle)
        except Exception:
            pass


class _DataMapping:
    def __init__(self, handle, addr):
        self.guard = _Guard(handle, addr)
        self.addr = addr
        head = (ctypes.c_ubyte * 4096).from_address(addr)
        (magic, version, self.slot_count, data_size, self.slot_stride, self.slots_offset,
         self.total_size) = struct.unpack_from(DATA_HEADER_FMT, head, 0)
        if magic != DATA_MAGIC or version != PROTOCOL_VERSION or self.slot_count == 0:
            raise OSError("data mapping header invalid")
        if data_size != DATA_SIZE:
            raise OSError("telemetry DLL data layout mismatch (install the same version of the client)")
        self.buf = (ctypes.c_ubyte * self.total_size).from_address(addr)
        self.buf._guard = self.guard


class Telemetry(TelemetryData):
    """One snapshot of the game (`telemetry.truck.speed`, `telemetry.trailers[0].connected`, ...)."""

    valid: bool = False
    """True after at least one successful read, false for default initialized data."""
    frame_index: int
    """Number of the snapshot, increases by one per game frame."""
    timestamp_qpc: int
    """QueryPerformanceCounter time when the game started this frame."""
    publish_qpc: int
    """QueryPerformanceCounter time when the plugin finished writing the snapshot."""

    _check = None
    _reader = None

    def __init__(self):
        super().__init__()
        self.valid = False
        self._check = None
        self._reader = None

    def is_valid(self) -> bool:
        """Check whether a zero-copy snapshot is still readable."""
        return self.valid and (True if self._check is None else self._check())

    def age_ms(self) -> float:
        """Milliseconds since the game started this frame."""
        return self._reader.ticks_to_ms(self._reader.qpc_now() - self.timestamp_qpc)

    def delivery_ms(self) -> float:
        """Milliseconds since the plugin published this snapshot (the delay the shared memory adds)."""
        return self._reader.ticks_to_ms(self._reader.qpc_now() - self.publish_qpc)


class TelemetryReader:
    """Lock-free reader for the ``ets2la_telemetry.dll`` shared memory.

    Any number of readers can run at the same time, every one of them always gets the newest snapshot.
    The delay of a snapshot is reported by age_ms()/delivery_ms() and stats().

    The constructor tries to connect once, and if that fails, `connected()` stays
    False and get_telemetry() / get_latest_telemetry() silently keep retrying the
    connection in the background until the telemetry DLL shows up.
    """

    def __init__(self):
        if kernel32 is None:
            raise OSError("ets2la_telemetry only works on Windows")
        self._qpc = ctypes.c_int64()
        self._ctl_handle = None
        self._ctl_addr = 0
        self._events = [None, None]
        self._map: Optional[_DataMapping] = None
        self._entry_addr = 0
        self._my_id = 0
        self._last_index = 0
        self._freq = 0
        self._last_reconnect = 0.0
        self.last_error = ""
        self._connect()

    def connected(self) -> bool:
        """True while the telemetry DLL is attached."""
        return self._ctl_addr != 0

    def get_telemetry(self, timeout_ms: int = 1000, copy: bool = True) -> Optional[Telemetry]:
        """Block until a snapshot newer than the last one returned exists.

        Returns None on timeout. copy=False returns a zero-copy view into
        shared memory (check telemetry.is_valid() after using it).
        """
        return self._acquire(True, timeout_ms, copy)

    def get_latest_telemetry(self, copy: bool = True) -> Optional[Telemetry]:
        """Non-blocking: the newest snapshot right now (may repeat the last one)."""
        return self._acquire(False, 0, copy)

    def stats(self) -> Optional[TelemetryStats]:
        """Plugin state and timing, or None while not connected."""
        if not self.connected():
            return None
        u64 = lambda off: ctypes.c_uint64.from_address(self._ctl_addr + off).value
        u32 = lambda off: ctypes.c_uint32.from_address(self._ctl_addr + off).value
        now = kernel32.GetTickCount64()
        readers = 0
        for i in range(MAX_READERS):
            base = CTL_READERS + i * READER_STRIDE
            if u64(base) != 0 and now - u64(base + 8) < READER_TIMEOUT_MS:
                readers += 1
        last = u64(CTL_LAST_PUBLISH_QPC)
        return TelemetryStats(
            latest_frame=u64(CTL_LATEST),
            frames_published=u64(CTL_PUBLISHED),
            active=bool(u32(CTL_ACTIVE)),
            paused=bool(u32(CTL_PAUSED)),
            writer_pid=u32(CTL_WRITER_PID),
            frame_interval_ms=self.ticks_to_ms(u64(CTL_FRAME_INTERVAL)),
            publish_cost_us=self.ticks_to_ms(u64(CTL_PUBLISH_COST)) * 1000.0,
            since_last_publish_ms=self.ticks_to_ms(self.qpc_now() - last) if last else 0.0,
            readers=readers,
        )

    def qpc_now(self) -> int:
        kernel32.QueryPerformanceCounter(ctypes.byref(self._qpc))
        return self._qpc.value

    def ticks_to_ms(self, ticks: int) -> float:
        return ticks * 1000.0 / self._freq if self._freq else 0.0

    def close(self):
        self._unregister()
        self._map = None
        if self._ctl_addr:
            kernel32.UnmapViewOfFile(self._ctl_addr)
            self._ctl_addr = 0
        if self._ctl_handle:
            kernel32.CloseHandle(self._ctl_handle)
            self._ctl_handle = None
        for i, e in enumerate(self._events):
            if e:
                kernel32.CloseHandle(e)
                self._events[i] = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def _fail(self, msg: str) -> bool:
        self.last_error = msg
        return False

    def _connect(self) -> bool:
        self.close()
        h = kernel32.OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, False, CONTROL_NAME)
        if not h:
            return False
        addr = kernel32.MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, CONTROL_BYTES)
        if not addr:
            kernel32.CloseHandle(h)
            return self._fail("MapViewOfFile(control) failed")
        magic = ctypes.c_uint32.from_address(addr + CTL_MAGIC).value
        if magic != CONTROL_MAGIC:
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("telemetry DLL not ready")
        version = ctypes.c_uint32.from_address(addr + CTL_VERSION).value
        if version != PROTOCOL_VERSION:
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("telemetry DLL protocol version mismatch")
        events = [kernel32.OpenEventW(SYNCHRONIZE, False, n) for n in EVENT_NAMES]
        if not all(events):
            for e in events:
                if e:
                    kernel32.CloseHandle(e)
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("OpenEvent failed")

        self._ctl_handle, self._ctl_addr, self._events = h, addr, events
        self._c_latest = ctypes.c_uint64.from_address(addr + CTL_LATEST)
        self._freq = ctypes.c_uint64.from_address(addr + CTL_QPC_FREQ).value
        if self._ensure_mapping() is None:
            self.close()
            return False
        self._register()
        self._last_index = self._c_latest.value
        self.last_error = ""
        return True

    _counter = 0

    def _register(self):
        TelemetryReader._counter += 1
        pid = kernel32.GetCurrentProcessId()
        self._my_id = (pid << 32) | (TelemetryReader._counter & 0xFFFFFFFF)
        now = kernel32.GetTickCount64()
        start = pid % MAX_READERS
        chosen = start
        for i in range(MAX_READERS):
            idx = (start + i) % MAX_READERS
            base = self._ctl_addr + CTL_READERS + idx * READER_STRIDE
            rid = ctypes.c_uint64.from_address(base).value
            hb = ctypes.c_uint64.from_address(base + 8).value
            if rid == 0 or now - hb > READER_TIMEOUT_MS * 2:
                chosen = idx
                break
        self._entry_addr = self._ctl_addr + CTL_READERS + chosen * READER_STRIDE
        self._c_hb_id = ctypes.c_uint64.from_address(self._entry_addr)
        self._c_hb_ms = ctypes.c_uint64.from_address(self._entry_addr + 8)
        self._heartbeat()

    def _heartbeat(self):
        if self._entry_addr:
            self._c_hb_id.value = self._my_id
            self._c_hb_ms.value = kernel32.GetTickCount64()

    def _unregister(self):
        if self._entry_addr and self._ctl_addr and self._c_hb_id.value == self._my_id:
            self._c_hb_ms.value = 0
            self._c_hb_id.value = 0
        self._entry_addr = 0

    def _ensure_mapping(self) -> Optional[_DataMapping]:
        m = self._map
        if m is not None:
            return m
        h = kernel32.OpenFileMappingW(FILE_MAP_READ, False, DATA_NAME)
        if not h:
            return None
        addr = kernel32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)
        if not addr:
            kernel32.CloseHandle(h)
            self._fail("MapViewOfFile(data) failed")
            return None
        try:
            m = _DataMapping(h, addr)
        except OSError as e:
            _Guard(h, addr)
            self._fail(str(e))
            return None
        self._map = m
        return m

    def _snapshot(self, min_exclusive: int):
        for _ in range(16):
            latest = self._c_latest.value
            if latest == 0 or latest <= min_exclusive:
                return None
            m = self._ensure_mapping()
            if m is None:
                return None
            slot_off = m.slots_offset + (latest % m.slot_count) * m.slot_stride
            s1, idx, ts, pub = struct.unpack_from(SLOT_FMT, m.buf, slot_off)
            if (s1 & 1) or idx != latest:
                continue
            return m, slot_off, s1, idx, ts, pub
        return None

    def _wait_newer(self, deadline: float) -> bool:
        ev = self._events[(self._last_index + 1) & 1]
        while True:
            if self._c_latest.value > self._last_index:
                return True
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                return False
            kernel32.WaitForSingleObject(ev, max(1, min(5, int(remaining * 1000 + 0.999))))
            self._heartbeat()

    def _make_telemetry(self, m, slot_off, s1, idx, ts, pub, copy):
        off = slot_off + SLOT_HEADER_BYTES
        seq_view = ctypes.c_uint64.from_address(m.addr + slot_off)

        def still_valid():
            return seq_view.value == s1

        if copy:
            t = Telemetry.from_buffer_copy(m.buf, off)
            if not still_valid():
                return None
        else:
            t = Telemetry.from_buffer(m.buf, off)
        t.frame_index, t.timestamp_qpc, t.publish_qpc = idx, ts, pub
        t.valid = True
        t._check = None if copy else still_valid
        t._reader = self
        return t

    def _acquire(self, only_new: bool, timeout_ms: int, copy: bool) -> Optional[Telemetry]:
        if not self.connected():
            now = time.perf_counter()
            if now - self._last_reconnect >= 0.25:
                self._last_reconnect = now
                self._connect()
            if not self.connected():
                time.sleep(min(timeout_ms, 50) / 1000.0)
                return None
        self._heartbeat()
        deadline = time.perf_counter() + (timeout_ms if only_new else 5) / 1000.0
        while True:
            if only_new and not self._wait_newer(deadline):
                return None
            snap = self._snapshot(self._last_index if only_new else 0)
            if snap is not None:
                telemetry = self._make_telemetry(*snap, copy)
                if telemetry is not None:
                    self._last_index = telemetry.frame_index
                    return telemetry
            elif not only_new:
                return None
            if time.perf_counter() >= deadline:
                return None
            time.sleep(0)