#!/usr/bin/env python3
"""A core in Vitis HLS's emulation, lock-stepped against DoomV in its process.

What corun.py --lockstep and lockstep_lib.py run as the device under test: a
Vitis HLS component -- a core, or the stand-in for one's retirement port in
lockstep_lib/vitis -- in one of Vitis's two emulations:

  sw-emu   C simulation: the component's C++ compiled natively with its
           testbench, built once, then run once per program.
  hw-emu   C/RTL co-simulation: synthesised once, then the generated Verilog
           run in XSim per program, the testbench on what the RTL produced.

The component is a folder with an hls_config.cfg (paths relative to it; the
part, clock and the flags that link DoomV's library are added here). Its
testbench links doomv_lockstep.dll (make lockstep-lib) and keeps to this
contract, all through the environment:

  DOOMV_LS_ARGS    DoomV's arguments, for doomv_ls_open_line: the machine,
                   -lockstep-strict, -cycle-clock, -lockstep-record
  DOOMV_LS_ELF     the program to run
  DOOMV_LS_LIMIT   at most this many steps
  DOOMV_LS_TRACE   for the stand-in only: the records it replays

It hands every record the core retires to DoomV, prints doomv_ls_message on
a mismatch (DoomV's "lockstep: MISMATCH ..." report), and returns 0 only if
every record matched.
"""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import threading

ROOT = pathlib.Path(__file__).resolve().parents[2]
LIB = ROOT / "build" / "lockstep-lib"
STAND_IN = ROOT / "tools" / "verification" / "lockstep_lib" / "vitis"
# The KV260's part; any part Vitis has will do for a component this size.
PART = "xck26-sfvc784-2LV-c"
PATH_KEYS = ("syn.file", "tb.file", "syn.cflags_file")


def vitis_gxx():
    """Vitis's own MinGW g++: VITIS_GXX, else the newest installation's."""
    if os.environ.get("VITIS_GXX"):
        return pathlib.Path(os.environ["VITIS_GXX"])
    for base in ("Z:/FPGA", "C:/Xilinx", "C:/AMD"):
        # The newest release, then the newest MinGW in it: 10.0.0 is newer than 8.3.0.
        def version(p):
            return [[int(x) if x.isdigit() else 0 for x in part.split(".")] for part in (p.parts[-8], p.parts[-5])]
        for gxx in sorted(pathlib.Path(base).glob("*/Vitis/tps/mingw/*/win64.o/nt/bin/g++.exe"), key=version, reverse=True):
            return gxx
    return None


def vitis_root():
    gxx = vitis_gxx()
    if not gxx:
        return None
    for parent in gxx.parents:
        if parent.name == "Vitis" and (parent / "bin").exists():
            return parent
    return None


def fwd(path) -> str:
    return str(path).replace("\\", "/")


def clean_dir(path: pathlib.Path):
    """An empty folder. Vitis leaves read-only files behind, which rmtree
    cannot remove as they are."""
    def writable(func, p, _):
        os.chmod(p, 0o666)
        func(p)
    if path.exists():
        shutil.rmtree(path, onerror=writable)
    path.mkdir(parents=True)


def component_config(component: pathlib.Path, out: pathlib.Path, clock_ns: float = 10.0):
    """The component's hls_config.cfg, its paths made absolute, with the part
    and clock if it names none, and the flags that link the testbench with
    DoomV's library. Vitis's Tcl does not survive spaces in paths; the
    checkout's are assumed to have none."""
    top, hls = [], []
    section = top
    tb_cflags = [f"-I{fwd(LIB)}", f"-I{fwd(ROOT / 'tools/verification/lockstep_lib')}"]
    ldflags = [f"-L{fwd(LIB)}", "-ldoomv_lockstep"]
    keys = set()
    for line in (component / "hls_config.cfg").read_text().splitlines():
        if line.strip().lower() == "[hls]":
            section = hls
            continue
        key, eq, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if eq:
            keys.add(key)
            if key in PATH_KEYS:
                p = pathlib.Path(value)
                line = f"{key}={fwd(p if p.is_absolute() else component / p)}"
            elif key == "tb.cflags":
                tb_cflags.insert(0, value)
                continue
            elif key == "csim.ldflags":
                ldflags.insert(0, value)
                continue
        section.append(line)
    if "part" not in keys:
        top.insert(0, f"part={PART}")
    if "clock" not in keys:
        hls.append(f"clock={clock_ns:.3f}ns")
    hls += ["tb.cflags=" + " ".join(tb_cflags), "csim.ldflags=" + " ".join(ldflags)]
    out.write_text("\n".join(top + ["", "[hls]"] + hls) + "\n")


class VitisDut:
    """One component, prepared once for one emulation, then run per program."""

    def __init__(self, emu: str, component: pathlib.Path, work: pathlib.Path, timeout: int):
        assert emu in ("sw-emu", "hw-emu")
        self.emu, self.component, self.work, self.timeout = emu, component, work, timeout
        self.vitis = vitis_root()
        self.cfg = work / "hls_config.cfg"
        self.comp_dir = work / "component"
        self.csim_exe = None
        self.csim_path = []
        self.lock = threading.Lock()   # co-simulation works in the component's folder: one at a time
        self.error = ""

    def _tool(self, tool, args, env=None, log=None):
        cmd = ["cmd", "/c", str(self.vitis / "bin" / tool)] + args + ["--config", str(self.cfg),
                                                                       "--work_dir", str(self.comp_dir)]
        r = subprocess.run(cmd, cwd=self.work, capture_output=True, text=True, errors="replace",
                           timeout=self.timeout * 4, env=env)
        out = (r.stdout or "") + (r.stderr or "")
        if log:
            log.write_text(out)
        return r.returncode, out

    def prepare(self) -> bool:
        """Build what every program's run shares. False, with self.error, if it cannot."""
        if not self.vitis:
            self.error = "no Vitis installation (VITIS_GXX, or one under Z:/FPGA, C:/Xilinx or C:/AMD)"
            return False
        r = subprocess.run(["make", "lockstep-lib"], cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            self.error = "make lockstep-lib failed: " + (r.stdout + r.stderr)[-1500:]
            return False
        clean_dir(self.work)
        component_config(self.component, self.cfg)
        if self.emu == "hw-emu":
            code, out = self._tool("v++.bat", ["-c", "--mode", "hls"], log=self.work / "synthesis.log")
            if code:
                self.error = "synthesis failed: see " + str(self.work / "synthesis.log")
                return False
            return True
        # C simulation once, to build its program: with no program to run the
        # testbench stops at once, and the program it built is run per test.
        self._tool("vitis-run.bat", ["--mode", "hls", "--csim"], env=self._env({}), log=self.work / "csim-build.log")
        build = self.comp_dir / "hls" / "csim" / "build"
        if not (build / "csim.exe").exists():
            self.error = "C simulation built no program: see " + str(self.work / "csim-build.log")
            return False
        self.csim_exe = build / "csim.exe"
        # The PATH Vitis gives it (its MinGW runtime, its simulation libraries).
        tcl = (build / "run_sim.tcl").read_text(errors="replace") if (build / "run_sim.tcl").exists() else ""
        self.csim_path = re.findall(r'set ::env\(PATH\) "([^"$;]+);\$::env\(PATH\)"', tcl)
        self.csim_path += re.findall(r'set ::env\(PATH\) "\$::env\(PATH\);([^"]+)"', tcl)
        return True

    def _env(self, values: dict) -> dict:
        env = dict(os.environ)
        for k in ("DOOMV_LS_ARGS", "DOOMV_LS_ELF", "DOOMV_LS_LIMIT", "DOOMV_LS_TRACE"):
            env.pop(k, None)
        env.update(values)
        env["PATH"] = os.pathsep.join([str(LIB)] + [p.replace("/", "\\") for p in self.csim_path] + [env["PATH"]])
        return env

    def run(self, values: dict, out: pathlib.Path) -> tuple[int | None, str]:
        """One program: (exit status, None on a timeout; what it printed)."""
        env = self._env(values)
        try:
            if self.emu == "sw-emu":
                r = subprocess.run([str(self.csim_exe)], cwd=out, capture_output=True, text=True, errors="replace",
                                   timeout=self.timeout, env=env)
                return r.returncode, (r.stdout or "") + (r.stderr or "")
            with self.lock:
                code, text = self._tool("vitis-run.bat", ["--mode", "hls", "--cosim"], env=env)
            # Its log has a blank line after every line the testbench prints.
            text = re.sub(r"\n[ \t\r]*\n", "\n", text)
            # The testbench runs twice, on the C++ and then on what the RTL
            # produced; co-simulation passes only if both returned 0.
            if code == 0 and "C/RTL co-simulation finished: PASS" not in text:
                code = 1
            return code, text
        except subprocess.TimeoutExpired:
            return None, "timed out"


def quote(args) -> str:
    """Arguments as one string for doomv_ls_open_line."""
    return " ".join(f'"{a}"' if " " in a else a for a in args)
