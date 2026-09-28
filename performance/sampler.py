"""A sampling profiler for riscv_doom.exe, reported as a histogram.

Every interval it suspends the emulator's CPU thread, reads the host
instruction pointer out of its register context, and resumes it. Each
address is mapped to the function containing it with the executable's own
symbol table (nm), and the samples are counted per function. The histogram is
where the host's time goes, which is the question an optimization has to
start from -- a count of guest instructions would say what the guest did, not
what it cost.

No install, no rebuild: the emulator is an ordinary optimized build (MinGW
keeps its symbols, and it loads at its fixed image base), and the sampler is
ctypes over the Win32 debugging calls. Functions the compiler inlined count
towards the function they were inlined into.
"""
from __future__ import annotations

import bisect
import ctypes
import ctypes.wintypes as wt
import shutil
import subprocess
import threading
import time
from collections import Counter
from pathlib import Path

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
winmm = ctypes.WinDLL("winmm")

TH32CS_SNAPTHREAD = 0x00000004
THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_SIZE = 1232          # sizeof(CONTEXT) on x64
CONTEXT_FLAGS_OFFSET = 0x30
CONTEXT_RIP_OFFSET = 0xF8


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG),
                ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE),
                ("szModule", ctypes.c_wchar * 256), ("szExePath", ctypes.c_wchar * 260)]


TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010

kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.OpenThread.restype = wt.HANDLE
kernel32.SuspendThread.restype = wt.DWORD
kernel32.ResumeThread.restype = wt.DWORD


def threads_of(pid: int) -> list[int]:
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    found = []
    ok = kernel32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            found.append(entry.th32ThreadID)
        ok = kernel32.Thread32Next(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return found


def cpu_time(handle) -> int:
    c, e, k, u = (wt.FILETIME() for _ in range(4))
    if not kernel32.GetThreadTimes(handle, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
        return 0
    return ((k.dwHighDateTime << 32) | k.dwLowDateTime) + ((u.dwHighDateTime << 32) | u.dwLowDateTime)


def modules_of(pid: int) -> list[tuple[int, int, str]]:
    """(load address, size, file name) of every module loaded in the process."""
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    entry = MODULEENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    found = []
    ok = kernel32.Module32FirstW(snap, ctypes.byref(entry))
    while ok:
        found.append((entry.modBaseAddr or 0, entry.modBaseSize, entry.szModule))
        ok = kernel32.Module32NextW(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return found


def preferred_base(exe: Path) -> int:
    """The ImageBase the PE header asks for, which nm's addresses assume."""
    data = exe.read_bytes()[:4096]
    pe = int.from_bytes(data[0x3C:0x40], "little")
    opt = pe + 24
    magic = int.from_bytes(data[opt:opt + 2], "little")
    if magic == 0x20B:                      # PE32+
        return int.from_bytes(data[opt + 24:opt + 32], "little")
    return int.from_bytes(data[opt + 28:opt + 32], "little")


def busiest_thread(pid: int, window: float = 1.0):
    """The thread that used the most CPU over `window` seconds: the CPU thread."""
    access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION
    handles = {}
    for tid in threads_of(pid):
        h = kernel32.OpenThread(access, False, tid)
        if h:
            handles[tid] = h
    before = {tid: cpu_time(h) for tid, h in handles.items()}
    time.sleep(window)
    after = {tid: cpu_time(h) for tid, h in handles.items()}
    best = max(handles, key=lambda t: after[t] - before[t], default=None)
    for tid, h in handles.items():
        if tid != best:
            kernel32.CloseHandle(h)
    return best, handles.get(best)


class Symbols:
    def __init__(self, exe: Path):
        nm = shutil.which("nm") or "nm"
        out = subprocess.run([nm, "-C", "--defined-only", str(exe)], capture_output=True, text=True, check=True).stdout
        rows = []
        for line in out.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[1] in ("T", "t"):
                rows.append((int(parts[0], 16), parts[2]))
        rows.sort()
        self.addrs = [a for a, _ in rows]
        self.names = [n for _, n in rows]

    def name(self, addr: int) -> str:
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return f"?? {addr:#x}"
        name = self.names[i]
        # Clones of one function ([clone .constprop.3], .isra, .part) are the
        # same source function to anyone reading the histogram.
        cut = name.find(" [clone ")
        return name[:cut] if cut >= 0 else name


class Sampler:
    """Samples one thread of a running process until stop() is called."""

    def __init__(self, pid: int, exe: Path, interval: float = 0.002):
        self.pid, self.exe, self.interval = pid, exe, interval
        self.rips: Counter[int] = Counter()
        self.samples = 0
        self.modules: list[tuple[int, int, str]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join()

    def _run(self):
        winmm.timeBeginPeriod(1)
        try:
            # The CPU thread is started by the emulator after it has loaded its
            # images, so choosing at launch finds only the main thread, which
            # then spends the run waiting. Choose once some thread is busy.
            tid, handle = None, None
            while not self._stop.is_set():
                self._stop.wait(0.5)
                tid, handle = busiest_thread(self.pid, 0.5)
                if handle and cpu_time(handle) > 0:
                    break
            if not handle:
                return
            # Windows may load the executable away from its preferred base
            # (address-space layout randomization), so where every module
            # actually sits is taken from the process itself.
            self.modules = modules_of(self.pid)
            buf = (ctypes.c_char * (CONTEXT_SIZE + 64))()
            # CONTEXT must be 16-byte aligned for GetThreadContext.
            base = ctypes.addressof(buf)
            off = (-base) % 16
            ctx = ctypes.c_void_p(base + off)
            flags_at = base + off + CONTEXT_FLAGS_OFFSET
            rip_at = base + off + CONTEXT_RIP_OFFSET
            while not self._stop.is_set():
                if kernel32.SuspendThread(handle) == 0xFFFFFFFF:
                    break
                ctypes.c_uint32.from_address(flags_at).value = CONTEXT_CONTROL
                ok = kernel32.GetThreadContext(handle, ctx)
                kernel32.ResumeThread(handle)
                if ok:
                    self.rips[ctypes.c_uint64.from_address(rip_at).value] += 1
                    self.samples += 1
                time.sleep(self.interval)
            kernel32.CloseHandle(handle)
        finally:
            winmm.timeEndPeriod(1)

    def histogram(self) -> list[tuple[str, int]]:
        syms = Symbols(self.exe)
        want = self.exe.name.lower()
        own = next(((b, s) for b, s, name in self.modules if name.lower() == want), None)
        shift = (own[0] - preferred_base(self.exe)) if own else 0
        per_fn: Counter[str] = Counter()
        for rip, n in self.rips.items():
            if own and own[0] <= rip < own[0] + own[1]:
                per_fn[syms.name(rip - shift)] += n
                continue
            # Outside the emulator: a system or SDL library. Named by module,
            # since their internals are not this project's to optimize.
            where = next((name for b, s, name in self.modules if b <= rip < b + s), None)
            per_fn[f"[{where}]" if where else f"[unknown {rip:#x}]"] += n
        return per_fn.most_common()


def render(hist: list[tuple[str, int]], total: int, top: int = 40, width: int = 50) -> str:
    lines = [f"{'samples':>8} {'share':>6}  function", ""]
    if not total:
        return "no samples\n"
    peak = hist[0][1] if hist else 1
    for name, n in hist[:top]:
        bar = "#" * max(1, round(width * n / peak))
        lines.append(f"{n:>8} {100 * n / total:>5.1f}%  {bar}  {name}")
    rest = sum(n for _, n in hist[top:])
    if rest:
        lines.append(f"{rest:>8} {100 * rest / total:>5.1f}%  (the other {len(hist) - top} functions)")
    lines.append("")
    lines.append(f"{total} samples")
    return "\n".join(lines) + "\n"
