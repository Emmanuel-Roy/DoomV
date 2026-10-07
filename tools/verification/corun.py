#!/usr/bin/env python3
"""Run one program on every RISC-V simulator at once and compare them all to Sail.

Sail is the golden reference. DoomV, Spike, Whisper and QEMU run the same
bare-metal ELF, at the same time, each writing its own trace; every trace is
read into the same form -- one step per instruction, with the program counter,
the instruction, whether it trapped, the registers it wrote (x, f, v) and the
bytes it stored -- and walked against Sail's, step by step from the ELF's entry
point. Where a simulator first parts from Sail, the report says where and how,
with what every simulator had at that step.

    python tools/verification/corun.py build/hello.elf
    python tools/verification/corun.py --suite riscv-tests --limit 40
    python tools/verification/corun.py --suite riscv-vector-tests-v256x64 rv64vadd_vv-0 --vlen 256
    python tools/verification/corun.py --snapshot build/desktop/xfce-100G --limit 20000

From a snapshot: DoomV restores it, and the others start from the same
architectural state. DoomV exports it (-export-state: RAM, registers, CSRs),
and this builds one ELF of that RAM plus a short M-mode program that writes
the CSRs, f and v registers and x registers back and returns (mret) to the
snapshot's pc and privilege. Every simulator can load an ELF. Comparing starts
at that pc. Devices are not part of the state: the other simulators have their
own, so a guest that touches one -- Linux's UART, its virtio disks -- parts
from Sail there, and the report says so. The snapshot's command.txt (every
snapshot since this was added has one) says how to start DoomV's machine;
for an older one, give it after `--`.

DoomV is held to Sail more closely than the others: besides the trace, it runs
under -lockstep-strict against Sail's trace, which compares CSRs, traps,
interrupts and the clock as well. If that finds a mismatch, a DoomV snapshot is
taken one step before the instruction that went wrong -- the machine as it
was just before the inciting instruction -- and checked: restored, its first
instruction must be the one the lock-step stopped at. Restore it with

    riscv_doom.exe <the same arguments> -restore=<dir>

(the report prints the exact line), and step it in DoomV's debugger or with gdb.

What is compared, and what is not:
  * x, f and v registers, after every instruction, and memory stores (QEMU's
    log has no stores; it is compared on registers alone).
  * Not CSRs, between the other simulators: each implements its own set of
    optional ones and its own counters, so they differ by design. DoomV's
    CSRs are compared with Sail's by the lock-step.
  * Each simulator is given the ISA asked for (--march); what a simulator does
    not know is dropped, and the report lists what was dropped, since a
    simulator without an extension traps where Sail does not.

Where they run: Sail, Spike, Whisper and QEMU in WSL (Ubuntu), DoomV on
Windows. Spike and Whisper are built under /root/build in WSL (simulators/
spike/build.sh; Whisper with `make` in a copy of simulators/whisper/src),
QEMU is Ubuntu's qemu-system-riscv.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field

ROOT = pathlib.Path(__file__).resolve().parents[2]
SUITES = ROOT / "tools" / "verification" / "tests" / "suites"
sys.path.insert(0, str(SUITES))
import run_suite  # noqa: E402  (Sail's path and config, the suites' march, the ELF symbol reader)

WORK = ROOT / "build" / "corun"
# DOOMV_EXE: another build -- a copy, for a long run while this one is rebuilt.
DOOMV = pathlib.Path(os.environ["DOOMV_EXE"]) if os.environ.get("DOOMV_EXE") else ROOT / "riscv_doom.exe"
WAD = ROOT / "tools" / "doom" / "doombuild" / "DOOM1.WAD"
SPIKE = "/root/build/spike-build/spike"
WHISPER = "/root/build/whisper/build-Linux/whisper"
QEMU = "qemu-system-riscv64"
SAIL_FLAGS = ["--trace-instr", "--trace-gpr", "--trace-fpr", "--trace-vreg", "--trace-csr",
              "--trace-mem", "--trace-exception", "--trace-interrupt"]
SIMS = ("doomv", "spike", "whisper", "qemu")
# What DoomV gives a Linux boot when no -march says otherwise (src/machine.cpp):
# a snapshot of one is of that hart, and the others are given the same.
LINUX_MARCH = ("rv64imafdcv_zicsr_zifencei_zba_zbb_zbs_zicond_zicbom_zicbop_zicboz_zicntr_zihintpause"
               "_zihintntl_zimop_zcmop_zawrs_zfa_zfh_svinval_svnapot_svpbmt_sscofpmf_ssstateen_ssnpm_smnpm")


def wsl(path: pathlib.Path) -> str:
    return run_suite.win_to_wsl(path)


def in_wsl(cmd, timeout, cwd: pathlib.Path | None = None):
    """Run a command in WSL; returns (exit code, output). cwd is a Windows path."""
    pre = ["wsl", "-d", "Ubuntu", "-u", "root"]
    if cwd is not None:
        pre += ["--cd", wsl(cwd)]
    try:
        r = subprocess.run(pre + ["--"] + cmd, capture_output=True, text=True, errors="replace",
                           timeout=timeout, env=run_suite._env())
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        return None, "timed out"


def is_elf64(path: pathlib.Path) -> bool:
    with path.open("rb") as f:
        head = f.read(6)
    return head[:4] == b"\x7fELF" and head[4] == 2 and head[5] == 1


def elf_entry(path: pathlib.Path) -> int:
    data = path.read_bytes()[:0x20]
    return struct.unpack_from("<Q", data, 0x18)[0]


# ---- the common form ------------------------------------------------------------

@dataclass
class Step:
    pc: int
    insn: int | None = None
    text: str = ""                      # disassembly, where the trace has it
    trap: bool = False
    writes: dict = field(default_factory=dict)    # "x5" / "f3" / "v0" -> value
    stores: dict | None = None          # address -> byte; None where the trace has no stores


@dataclass
class Program:
    """What every simulator runs: an ELF for the others, and the arguments that
    start DoomV on the same machine -- from reset, or by restoring a snapshot."""
    name: str
    elf: pathlib.Path
    machine: list                 # DoomV's arguments for the machine, without the executable
    start: list                   # how DoomV starts it: [] or ["-restore=<dir>"]
    entry: int                    # where comparing starts
    tohost: int | None = None
    first_step: int = 0           # DoomV's step count at the entry (a snapshot's)
    ram_mb: int | None = None     # memory the others need, for a snapshot's RAM
    vlen: int = 128
    march: str = ""               # the ISA every simulator is given


@dataclass
class Run:
    name: str
    steps: list = field(default_factory=list)
    error: str = ""
    dropped: list = field(default_factory=list)   # extensions this simulator was not given
    log: pathlib.Path | None = None
    capped: bool = False   # stopped on purpose before the limit (QEMU's log size)
    command: list = field(default_factory=list)


def store_bytes(addr: int, hexval: str, out: dict):
    """A store of `hexval` (as many bytes as it has digits) at addr, little-endian."""
    hexval = hexval.lower().removeprefix("0x")
    if len(hexval) % 2:
        hexval = "0" + hexval
    n = len(hexval) // 2
    v = int(hexval, 16)
    for i in range(n):
        out[addr + i] = (v >> (8 * i)) & 0xFF


# Sail's format, which DoomV's -trace writes too:
#   [87] [U]: 0x0000000080000168 (0x02056007) vle32.v v0, (x10)
#   x5 <- 0x...   f1 <- 0x...   v0 <- 0x...
#   mem[W,0x...] <- 0x...   (RW for an AMO's store, C for cbo.zero)
#   trapping from M to M to handle illegal-instruction ... CSR mepc (0x341) <- 0x...
# A trap is the record's own only when the exception pc it saves is the
# record's pc: a fetch that faults has no record of its own, and its trap
# lines follow the instruction before it (Spike, Whisper and QEMU show no
# step for it either).
RECORD = re.compile(r"^\[(\d+)\] \[[A-Z]+\]: 0x([0-9A-Fa-f]+) \(0x([0-9A-Fa-f]+)\)\s*(.*)$")
WRITE = re.compile(r"^([xfv])(\d+) <- 0x([0-9A-Fa-f]+)")
STORE = re.compile(r"^mem\[(?:W|RW|C),0x([0-9A-Fa-f]+)\] <- 0x([0-9A-Fa-f]+)")
EPC = re.compile(r"^CSR (?:mepc|sepc|vsepc|mnepc) \(0x[0-9A-Fa-f]+\) <- 0x([0-9A-Fa-f]+)")


def parse_sail_format(path: pathlib.Path) -> list:
    steps, cur, trapping = [], None, False
    with path.open(errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = RECORD.match(line)
            if m:
                # The disassembly, without Sail's "symbol+offset" after it.
                text = re.split(r"\s{2,}", m.group(4).strip())[0]
                cur = Step(int(m.group(2), 16), int(m.group(3), 16), text, stores={})
                steps.append(cur)
                trapping = False
                continue
            if cur is None:
                continue
            m = WRITE.match(line)
            if m:
                cur.writes[m.group(1) + str(int(m.group(2)))] = int(m.group(3), 16)
                continue
            m = STORE.match(line)
            if m:
                store_bytes(int(m.group(1), 16), m.group(2), cur.stores)
                continue
            if line.startswith("trapping from"):
                trapping = True
                continue
            m = EPC.match(line)
            if m and trapping:
                trapping = False
                if int(m.group(1), 16) == cur.pc:
                    cur.trap = True
                    cur.writes, cur.stores = {}, {}
    return steps


# Spike, with --log-commits -l:
#   core   0: 0x0000000080000168 (0x02056007) vle32.v v0, (a0)          every instruction
#   core   0: 0 0x0000000080000168 (0x02056007) e32 m1 l4 v0  0x... mem 0x...   one that retired
#   core   0: exception trap_illegal_instruction, epc 0x...
SPIKE_FETCH = re.compile(r"^core\s+\d+: 0x([0-9a-f]+) \(0x([0-9a-f]+)\)\s*(.*)$")
SPIKE_COMMIT = re.compile(r"^core\s+\d+: \d 0x([0-9a-f]+) \(0x([0-9a-f]+)\)(.*)$")


def parse_spike(path: pathlib.Path) -> list:
    steps = []
    with path.open(errors="replace") as f:
        for line in f:
            m = SPIKE_FETCH.match(line)
            if m:
                steps.append(Step(int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip(),
                                  trap=True, stores={}))   # until its commit line says otherwise
                continue
            m = SPIKE_COMMIT.match(line)
            if m and steps and steps[-1].pc == int(m.group(1), 16):
                st = steps[-1]
                st.trap = False
                tok = m.group(3).split()
                i = 0
                while i < len(tok):
                    t = tok[i]
                    if re.fullmatch(r"[xfv]\d+", t) and i + 1 < len(tok):
                        st.writes[t[0] + str(int(t[1:]))] = int(tok[i + 1], 16)
                        i += 2
                    elif t == "mem" and i + 1 < len(tok):
                        # mem <addr> <value> is a store; mem <addr> alone a load
                        if i + 2 < len(tok) and tok[i + 2].startswith("0x") and not re.fullmatch(r"[xfvc]\S*", tok[i + 2]):
                            store_bytes(int(tok[i + 1], 16), tok[i + 2], st.stores)
                            i += 3
                        else:
                            i += 2
                    else:
                        i += 1
    return steps


# Whisper, with --logfile --csvlog:
#   pc, inst, modified regs, source operands, memory, inst info, privilege, trap, disassembly, hartid
#   80000168,2056007,v0=...,x10,8000c400;...,v,m,,vle32.v v0; (x10),0
# Under translation an address is virtual:physical. A store's value is
# written without leading zeros, so its width comes from the instruction.
def store_width(mnemonic: str):
    m = mnemonic.lower()
    if m.startswith("cbo.zero"):
        return 8                     # listed eight bytes to an entry
    if m.startswith("v"):
        return 1                     # vector stores are listed by byte
    for suffix, n in ((".b", 1), (".h", 2), (".w", 4), (".d", 8), (".q", 16)):
        if suffix in m:
            return n
    for name, n in (("sb", 1), ("sh", 2), ("sw", 4), ("sd", 8), ("fsh", 2), ("fsw", 4), ("fsd", 8),
                    ("c.sw", 4), ("c.sd", 8), ("c.swsp", 4), ("c.sdsp", 8), ("c.fsd", 8), ("c.fsdsp", 8),
                    ("c.sb", 1), ("c.sh", 2)):
        if m == name:
            return n
    return None


def parse_whisper(path: pathlib.Path) -> list:
    steps = []
    with path.open(errors="replace") as f:
        f.readline()                 # the header
        for line in f:
            col = line.rstrip("\n").split(",")
            if len(col) < 9:
                continue
            text = col[8].replace(";", ",")
            st = Step(int(col[0].split(":")[0], 16), int(col[1], 16), text, trap=bool(col[7].strip()), stores={})
            for w in filter(None, col[2].split(";")):
                name, _, value = w.partition("=")
                if re.fullmatch(r"[xfv]\d+", name):
                    st.writes[name[0] + str(int(name[1:]))] = int(value, 16)
            width = store_width(text.split()[0] if text.split() else "")
            for m in filter(None, col[4].split(";")):
                addr, eq, value = m.partition("=")
                if eq:
                    addr = addr.split(":")[-1]          # the physical address
                    if width:
                        value = value.rjust(2 * width, "0")
                    store_bytes(int(addr, 16), value, st.stores)
            if st.trap:
                # A fetch that faulted is a record here, with no instruction;
                # Sail and Spike have no step for it.
                if st.insn == 0:
                    continue
                st.writes, st.stores = {}, {}
            steps.append(st)
    return steps


# QEMU, with one instruction per block and -d in_asm,cpu,fpu,vpu,nochain: the
# whole register file before every instruction, and each block's disassembly
# when it is translated -- again, if the code changed. A step's writes are
# what changed by the next dump; a trap shows as the next pc being the trap
# vector with the exception pc pointing back at this instruction.
QEMU_IN = re.compile(r"^0x([0-9a-f]+):\s+([0-9a-f]+)\s+(.*)$")
QEMU_REG = re.compile(r"([xfv]\d+)(?:/\w+)?\s+([0-9a-f]+)")


def parse_qemu(path: pathlib.Path) -> list:
    insn, dumps, cur = {}, [], None
    with path.open(errors="replace") as f:
        for line in f:
            m = QEMU_IN.match(line)
            if m:
                insn[int(m.group(1), 16)] = (int(m.group(2), 16), m.group(3).split("#")[0].strip())
                continue
            s = line.strip()
            if s.startswith("pc "):
                pc = int(s.split()[1], 16)
                cur = {"pc": pc, "_insn": insn.get(pc, (None, ""))}
                dumps.append(cur)
                continue
            if cur is None:
                continue
            if s.startswith(("mepc ", "sepc ", "mtvec ", "stvec ")):
                k, v = s.split()[:2]
                cur[k] = int(v, 16)
                continue
            for name, value in QEMU_REG.findall(line):
                if name[0] in "xfv":
                    cur[name[0] + str(int(name[1:]))] = int(value, 16)
    # A block QEMU translates again (after a store to its page, say) is
    # logged again with nothing executed in between: one step, not two.
    def state(d):
        return {k: v for k, v in d.items() if k != "_insn"}
    merged = []
    for d in dumps:
        if merged and state(merged[-1]) == state(d):
            merged[-1] = d
            continue
        merged.append(d)
    dumps = merged
    steps = []
    for i, d in enumerate(dumps):
        word, text = d["_insn"]
        st = Step(d["pc"], word, text)
        if i + 1 < len(dumps):
            nxt = dumps[i + 1]
            for k, v in nxt.items():
                if k[0] in "xfv" and k[1:].isdigit() and d.get(k) != v:
                    st.writes[k] = v
            vectors = {nxt.get("mtvec", -1) & ~3, nxt.get("stvec", -1) & ~3}
            if nxt["pc"] in vectors and d["pc"] in (nxt.get("mepc"), nxt.get("sepc")) \
                    and (nxt.get("mepc") != d.get("mepc") or nxt.get("sepc") != d.get("sepc")
                         or nxt["pc"] != d["pc"] + 4):
                st.trap = True
                st.writes = {}
                # A fetch that faulted: never translated, so no instruction,
                # and no step in Sail or Spike either.
                if word is None:
                    continue
        steps.append(st)
    return steps


# ---- the ISA each simulator is given --------------------------------------------

def isa_tokens(march: str):
    base, *rest = march.lower().split("_")
    return base, [t for t in dict.fromkeys(rest) if t]


def spike_isa(march: str, vlen: int, elf: pathlib.Path, timeout: int):
    """The march, less what this Spike refuses ("unsupported extension: x")."""
    base, toks = isa_tokens(march)
    dropped = []
    if vlen != 128 and "v" in base[4:]:
        toks.append(f"zvl{vlen}b")
    while True:
        isa = "_".join([base] + toks)
        code, out = in_wsl([SPIKE, f"--isa={isa}", "--instructions=1", wsl(elf)], timeout)
        m = re.search(r"unsupported extension: (\w+)", out or "")
        if not m or m.group(1) not in toks:
            return isa, dropped
        toks.remove(m.group(1))
        dropped.append(m.group(1))


QEMU_LETTERS = {"v": "v", "h": "h"}


def qemu_cpu(march: str, vlen: int, elf: pathlib.Path, timeout: int):
    """-cpu rv64 with the march's extensions turned on, less what QEMU does not have."""
    base, toks = isa_tokens(march)
    props = [f"{QEMU_LETTERS[c]}=true" for c in base[4:] if c in QEMU_LETTERS]
    props += [f"{t}=true" for t in toks]
    # QEMU's rv64 turns Svadu on by itself: it sets a page's A and D bits in
    # hardware, where Sail and Spike fault (Svade) and let the handler set
    # them. Svade, then, unless the march asks for Svadu.
    if "svadu" not in toks:
        props += ["svadu=false", "svade=true"]
    if "v" in base[4:]:
        props += [f"vlen={vlen}", "elen=64"]
    dropped = []
    while True:
        cpu = ",".join(["rv64"] + props)
        # Started paused (-S): it either refuses the CPU at once or sits there
        # until the timeout, which means it took it.
        code, out = in_wsl(["timeout", "3", QEMU, "-machine", "spike", "-cpu", cpu, "-bios", "none",
                            "-kernel", wsl(elf), "-nographic", "-monitor", "none", "-serial", "none", "-S"],
                           timeout)
        bad = re.search(r"Property '[\w.-]*?\.?([\w-]+)' not found|property '([\w-]+)' not found|"
                        r"'([\w-]+)' is not a valid", out or "", re.I)
        if not bad:
            return cpu, dropped
        name = next((g for g in bad.groups() if g), None)
        victim = next((p for p in props if name and p.startswith(name + "=")), None)
        if victim is None:
            return cpu, dropped   # an error that is not about a property: leave it to the real run
        props.remove(victim)
        dropped.append(name)


# ---- running them -------------------------------------------------------------------

def run_sail(prog, out, config_wsl, limit, timeout) -> Run:
    r = Run("sail", log=out / "sail.log")
    # A snapshot's restore program and Sail's boot ROM come first; the limit
    # counts from reset, so they are added to it.
    extra = 4096 if prog.start else 0
    r.command = [run_suite.WSL_SAIL, "--config", config_wsl, "--inst-limit", str(limit + extra)] + SAIL_FLAGS \
        + ["--trace-output", wsl(r.log), wsl(prog.elf)]
    code, text = in_wsl(r.command, timeout)
    (out / "sail.out").write_text(text)
    if not r.log.exists():
        r.error = "no trace: " + text.strip().splitlines()[-1] if text.strip() else "no trace"
        return r
    r.steps = parse_sail_format(r.log)
    return r


def elf_machine(elf, march, vlen, syms):
    """DoomV's arguments for running an ELF from reset."""
    args = ["-ng", str(WAD), str(elf), "-march=" + march]
    if "tohost" in syms:
        args.append("-tohost={:x}".format(syms["tohost"]))
    if vlen != 128:
        args.append(f"-vlen={vlen}")
    return args


def run_doomv(prog, out, limit, timeout) -> Run:
    r = Run("doomv", log=out / "doomv.log")
    r.command = [str(DOOMV)] + prog.machine + prog.start + [f"-trace={r.log}", f"-stopat={prog.first_step + limit}"]
    try:
        p = subprocess.run(r.command, cwd=out, capture_output=True, text=True, errors="replace", timeout=timeout)
        (out / "doomv.out").write_text((p.stdout or "") + (p.stderr or ""))
    except subprocess.TimeoutExpired:
        r.error = "timed out"
    if r.log.exists():
        r.steps = parse_sail_format(r.log)
    elif not r.error:
        r.error = "no trace"
    return r


def run_spike(prog, out, march, limit, timeout) -> Run:
    r = Run("spike", log=out / "spike.log")
    isa, r.dropped = spike_isa(march, prog.vlen, prog.elf, timeout)
    mem = [f"-m{prog.ram_mb}"] if prog.ram_mb else []
    extra = 4096 if prog.start else 0
    # Spike's --instructions does not track its own trace closely (after a
    # restore program it stopped thirty instructions in); it is given room,
    # and the trace is cut to the limit instead.
    r.command = [SPIKE, f"--isa={isa}"] + mem + [f"--instructions={20 * (limit + extra) + 100000}", "--log-commits", "-l",
                                                f"--log={wsl(r.log)}", wsl(prog.elf)]
    code, text = in_wsl(r.command, timeout)
    (out / "spike.out").write_text(text)
    if not r.log.exists():
        r.error = "no trace: " + (text.strip().splitlines() or ["?"])[-1]
        return r
    r.steps = parse_spike(r.log)[:limit + extra + 64]
    return r


def run_whisper(prog, out, march, limit, timeout) -> Run:
    r = Run("whisper", log=out / "whisper.csv")
    base, toks = isa_tokens(march)
    # This Whisper, with H and no hart configuration beyond the ISA, crashes
    # on the first hypervisor CSR write (CsRegs::hyperWrite); without H it
    # runs, and a test that uses H traps where Sail does not, which the
    # report then shows.
    if "h" in base[4:]:
        base = base[:4] + base[4:].replace("h", "")
        r.dropped.append("h")
    # Whisper takes S and U mode as ISA letters; without them it has neither.
    base += "".join(c for c in "su" if c not in base[4:])
    config = out / "whisper.json"
    config.write_text(json.dumps({"vector": {"bytes_per_vec": prog.vlen // 8, "max_bytes_per_elem": 8}}))
    extra = 4096 if prog.start else 0
    r.command = [WHISPER, "--isa", "_".join([base] + toks), "--configfile", wsl(config), "--target", wsl(prog.elf),
                 "--logfile", wsl(r.log), "--csvlog", "--maxinst", str(limit + extra)]
    if prog.ram_mb:
        r.command += ["--memorysize", hex(0x80000000 + prog.ram_mb * 2**20)]
    if prog.tohost is not None:
        r.command += ["--tohostsym", "tohost"]
    code, text = in_wsl(r.command, timeout)
    (out / "whisper.out").write_text(text)
    r.dropped += sorted(set(re.findall(r"Unknown extension: (\w+)", text)))
    if code not in (0, None) and not r.log.exists() or (r.log.exists() and r.log.stat().st_size == 0):
        r.error = f"exit {code}: " + (text.strip().splitlines() or ["?"])[-1]
        return r
    if not r.log.exists():
        r.error = "no trace: " + (text.strip().splitlines() or ["?"])[-1]
        return r
    r.steps = parse_whisper(r.log)
    return r


def run_qemu(prog, out, march, limit, timeout) -> Run:
    r = Run("qemu", log=out / "qemu.log")
    cpu, r.dropped = qemu_cpu(march, prog.vlen, prog.elf, timeout)
    mem = ["-m", f"{prog.ram_mb + 2048}M"] if prog.ram_mb else []
    # From a snapshot, the generic loader: the spike machine starts a -kernel
    # at the bottom of RAM whatever its entry, and the restore program is not
    # there. A test keeps -kernel, which is what wires its tohost to an exit.
    load = ["-device", f"loader,file={wsl(prog.elf)},cpu-num=0"] if prog.start else ["-kernel", wsl(prog.elf)]
    r.command = [QEMU, "-machine", "spike", "-cpu", cpu] + mem + ["-bios", "none"] + load + ["-nographic",
        "-monitor", "none", "-serial", "none", "-accel", "tcg,one-insn-per-tb=on",
        "-d", "in_asm,cpu,fpu,vpu,nochain", "-D", wsl(r.log)]
    # QEMU has no instruction limit. A test ends at its tohost write; anything
    # else is stopped once its log holds about `limit` instructions -- each
    # one is a whole register dump, a few kilobytes.
    cap = (limit + 4096) * (8000 + 70 * prog.vlen // 8) if prog.tohost is None else 0
    # In a script file: wsl.exe would expand $p and $! itself.
    script = " ".join(f"'{a}'" for a in r.command)
    if cap:
        script = "\n".join([
            f"{script} &",
            "p=$!",
            "while kill -0 $p 2>/dev/null; do",
            f"  [ $(stat -c%s '{wsl(r.log)}' 2>/dev/null || echo 0) -gt {cap} ] && kill $p",
            "  sleep 0.2",
            "done",
            "wait $p"])
    (out / "qemu.sh").write_text(script + "\n", newline="\n")
    code, text = in_wsl(["timeout", str(timeout), "bash", wsl(out / "qemu.sh")], timeout + 30)
    (out / "qemu.out").write_text(text)
    if not r.log.exists():
        r.error = "no trace: " + (text.strip().splitlines() or ["?"])[-1]
        return r
    r.capped = bool(cap) and r.log.stat().st_size >= cap
    r.steps = parse_qemu(r.log)
    return r


# ---- comparing ---------------------------------------------------------------------

@dataclass
class Divergence:
    step: int            # steps since the entry point
    pc: int
    what: str            # "pc", "trap", "x5", "store", "ended", ...
    sail: str
    other: str


def sync(steps, entry):
    return next((i for i, s in enumerate(steps) if s.pc == entry), None)


def fmt(v, name=""):
    if v is None:
        return "-"
    if name.startswith("v"):
        return f"0x{v:x}"
    return f"0x{v:016x}"


def compare(sail: Run, other: Run, entry: int, tohost: int | None = None, limit: int | None = None):
    """(steps matched, Divergence or None)."""
    a0, b0 = sync(sail.steps, entry), sync(other.steps, entry)
    if a0 is None or b0 is None:
        return 0, Divergence(0, entry, "start", "never reached the entry point" if a0 is None else "reached it",
                             "never reached the entry point" if b0 is None else "reached it")
    A, B = sail.steps[a0:], other.steps[b0:]
    if limit:
        A, B = A[:limit], B[:limit]      # the same number of instructions for all of them
    sa, sb = {}, {}
    for i in range(min(len(A), len(B))):
        a, b = A[i], B[i]
        if a.pc != b.pc:
            return i, Divergence(i, a.pc, "pc", f"0x{a.pc:x}", f"0x{b.pc:x}")
        if a.insn is not None and b.insn is not None and (a.insn & 0xFFFF if a.insn < 0x10000 else a.insn) \
                != (b.insn & 0xFFFF if b.insn < 0x10000 else b.insn):
            return i, Divergence(i, a.pc, "instruction", f"0x{a.insn:08x}", f"0x{b.insn:08x}")
        if a.trap != b.trap:
            return i, Divergence(i, a.pc, "trap", "traps" if a.trap else "retires", "traps" if b.trap else "retires")
        if a.trap:
            continue
        for name in sorted(set(a.writes) | set(b.writes)):
            if name in ("x0",):
                continue
            prev = sa.get(name)
            va = a.writes.get(name, prev)
            vb = b.writes.get(name, sb.get(name, prev))
            if name[0] == "f" and va is not None and vb is not None and (va >> 32 == 0 or vb >> 32 == 0):
                va, vb = va & 0xFFFFFFFF, vb & 0xFFFFFFFF      # a 32-bit print on one side
            if va is not None and vb is not None and va != vb:
                return i, Divergence(i, a.pc, name, fmt(va, name), fmt(vb, name))
            if va is not None:
                sa[name] = va
            if vb is not None:
                sb[name] = vb
        # By page offset: Spike logs a store's virtual address, the others
        # the physical one, and the two agree within the page. Spike's log
        # has no line for the block a cbo.zero clears: nothing to compare.
        unlogged = other.name == "spike" and a.text.startswith("cbo.zero") and not b.stores
        if not unlogged and a.stores is not None and b.stores is not None and \
                {k & 0xFFF: v for k, v in a.stores.items()} != {k & 0xFFF: v for k, v in b.stores.items()}:
            def show(s):
                return ", ".join(f"[{k:x}]={v:02x}" for k, v in sorted(s.items())[:16]) or "none"
            return i, Divergence(i, a.pc, "store", show(a.stores), show(b.stores))
    if len(B) < len(A) and other.capped:
        return len(B), None              # stopped by us, not by the program
    if len(B) < len(A):
        # Stopping early is a divergence -- unless it was at the program's
        # exit: Whisper stops on the tohost store itself, Sail a step or two
        # after it. Running on past Sail's end is not one either: Spike
        # notices the tohost write a while later.
        n = len(B)
        # Stopped in a wfi: it is waiting for an interrupt, which the spec
        # allows, where Sail's wfi returns after a few ticks of the clock.
        if n and B[n - 1].text.split()[:1] == ["wfi"] or (n and n - 1 < len(A) and A[n - 1].text.startswith("wfi")):
            return n, Divergence(n, B[n - 1].pc, "wfi", "returns from the wfi and runs on",
                                 "waits in the wfi for an interrupt (allowed; the trace stops here)")
        if tohost is not None:
            exit_at = next((i for i, s in enumerate(A)
                            if s.stores and any(tohost <= k < tohost + 8 for k in s.stores)), None)
            if exit_at is not None and n > exit_at:
                return n, None
        last = A[n - 1].pc if n else entry
        return n, Divergence(n, last, "ended", f"{len(A)} steps", f"{len(B)} steps")
    return len(A), None


# ---- DoomV against Sail, strictly, and the snapshot ----------------------------------

def lockstep_and_snapshot(prog, out, limit, sail: Run, timeout):
    """DoomV under -lockstep-strict against Sail's trace; on a mismatch, a snapshot
    one step before the instruction that went wrong, checked by restoring it."""
    result = {"status": "not run"}
    if sail.error or not sail.log or not sail.log.exists():
        result["status"] = "no Sail trace"
        return result
    base = [str(DOOMV)] + prog.machine
    cmd = base + prog.start + [f"-lockstep={sail.log}", "-lockstep-strict", f"-stopat={prog.first_step + limit}"]
    try:
        p = subprocess.run(cmd, cwd=out, capture_output=True, text=True, errors="replace", timeout=timeout)
        text = (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return {"status": "timed out"}
    (out / "doomv-lockstep.txt").write_text(text)
    if "MISMATCH" not in text:
        verdicts = [l for l in text.splitlines() if l.startswith("lockstep: ") and "skipped" not in l]
        result.update(status="match", summary=verdicts[-1] if verdicts else "no lock-step summary")
        return result
    at = text.index("lockstep: MISMATCH")
    detail = text[at:].strip()
    m = re.search(r"\(instruction (\d+)\)", detail)
    pcm = re.search(r"DoomV:\s*\n\s*\[\d+\] \[[A-Z]+\]: 0x([0-9A-Fa-f]+)", detail)
    result.update(status="mismatch", detail=detail)
    if not m:
        return result
    count = int(m.group(1))
    bad_pc = int(pcm.group(1), 16) if pcm else None
    # The lock-step stops with the bad instruction executed: count includes it.
    snap_at = count - 1
    snap = out / "doomv-snapshot"
    shutil.rmtree(snap, ignore_errors=True)
    result["snapshot_step"] = snap_at
    if snap_at <= prog.first_step:
        result["snapshot"] = ("the first instruction went wrong; " +
                              ("start from the snapshot itself" if prog.start else "restart the program"))
        return result
    p = subprocess.run(base + prog.start + [f"-snapshot={snap}", f"-snapshotat={snap_at}", f"-stopat={snap_at + 1}"],
                       cwd=out, capture_output=True, text=True, errors="replace", timeout=timeout)
    if not (snap / "state.bin").exists():
        result["snapshot"] = "not taken: " + ((p.stdout or "") + (p.stderr or "")).strip()[-300:]
        return result
    # Check it: restored, the next instruction is the one the lock-step stopped at.
    check = out / "doomv-snapshot-check.log"
    check.unlink(missing_ok=True)
    subprocess.run(base + [f"-restore={snap}", f"-trace={check}", f"-stopat={snap_at + 1}"],
                   cwd=out, capture_output=True, text=True, errors="replace", timeout=timeout)
    first = parse_sail_format(check)[:1] if check.exists() else []
    result["snapshot"] = str(snap)
    result["restore"] = " ".join(f'"{a}"' if " " in a else a for a in base + [f"-restore={snap}"])
    if first and bad_pc is not None:
        result["verified"] = first[0].pc == bad_pc
        result["next_pc"] = f"0x{first[0].pc:x}"
        result["bad_pc"] = f"0x{bad_pc:x}"
    return result


# ---- from a snapshot: one ELF that puts DoomV's state back --------------------------

# CSRs written at the very end, once nothing more can trap: a trap would
# overwrite them. mtvec last of all -- until then it points at a handler that
# steps over whatever a simulator does not implement.
TAIL_CSRS = (0x34B, 0x34A, 0x340, 0x342, 0x343)   # mtval2, mtinst, mscratch, mcause, mtval


def restore_assembly(state: dict, data_at: int) -> str:
    """The M-mode program that puts the state back and returns into it."""
    csrs = {int(k): int(v, 16) for k, v in state["csrs"].items()}
    lines = [".option norvc", ".text", ".globl _start", "_start:",
             "  la t0, skip", "  csrw mtvec, t0",
             # FS and VS on, to load the f and v registers; mstatus is written last.
             "  li t0, 0x6600", "  csrs mstatus, t0",
             "  la a0, csrvals"]
    # Not mcycle or minstret: every simulator counts its own way, and Spike's
    # --instructions limit is counted in minstret, so writing it ended Spike.
    order = [c for c in csrs if c not in TAIL_CSRS and c not in (0x305, 0x341, 0xB00, 0xB02)]
    data = []

    def word(v):
        data.append(v & (2**64 - 1))
        return (len(data) - 1) * 8

    for c in order:
        lines += [f"  ld t0, {word(csrs[c])}(a0)", f"  csrw {c:#x}, t0"]
    # The CLINT's mtime and this hart's mtimecmp, where a simulator has them
    # (a store nobody answers traps, and is stepped over).
    lines += [f"  ld t0, {word(int(state['mtime'], 16))}(a0)", "  li t1, 0x200bff8", "  sd t0, 0(t1)",
              f"  ld t0, {word(int(state['mtimecmp'], 16))}(a0)", "  li t1, 0x2004000", "  sd t0, 0(t1)"]
    lines += ["  la a1, fregs"] + [f"  fld f{i}, {8 * i}(a1)" for i in range(32)]
    lines += [f"  ld t0, {word(int(state['fcsr'], 16))}(a0)", "  csrw fcsr, t0"]
    if state.get("vector"):
        group = state["vlen"]   # bytes in a group of eight registers: VLEN/8 * 8
        lines += [f"  li t0, {group}", "  vsetvli x0, t0, e8, m8, ta, ma", "  la a1, vregs"]
        for g in (0, 8, 16, 24):
            lines += [f"  vle8.v v{g}, (a1)", "  add a1, a1, t0"]
        lines += [f"  ld t0, {word(int(state['vl'], 16))}(a0)", f"  ld t1, {word(int(state['vtype'], 16))}(a0)",
                  "  vsetvl x0, t0, t1",
                  f"  ld t0, {word(int(state['vstart'], 16))}(a0)", "  csrw vstart, t0",
                  f"  ld t0, {word(int(state['vcsr'], 16))}(a0)", "  csrw vcsr, t0"]
    for c in TAIL_CSRS:
        if c in csrs:
            lines += [f"  ld t0, {word(csrs[c])}(a0)", f"  csrw {c:#x}, t0"]
    # mstatus as mret will leave it: MIE from MPIE, the snapshot's privilege
    # and virtualisation in MPP and MPV. mret then sets MPIE to 1 and MPP to
    # U, which only M-mode code could see, and only after a trap into M has
    # written both again.
    ms = int(state["mstatus"], 16)
    final = ms & ~((1 << 3) | (1 << 7) | (3 << 11) | (1 << 39))
    if ms & (1 << 3):
        final |= 1 << 7
    final |= (state["priv"] & 3) << 11
    if state["virt"]:
        final |= 1 << 39
    lines += [f"  ld t0, {word(csrs.get(0x305, 0))}(a0)", "  csrw mtvec, t0",
              f"  ld t0, {word(int(state['pc'], 16))}(a0)", "  csrw mepc, t0",
              f"  ld t0, {word(final)}(a0)", "  csrw mstatus, t0",
              "  la x31, gprs"]
    xs = [int(v, 16) for v in state["x"]]
    lines += [f"  ld x{i}, {8 * i}(x31)" for i in range(1, 31)] + ["  ld x31, 248(x31)", "  mret"]
    # Whatever traps on the way is stepped over: a CSR or a device a
    # simulator does not have.
    lines += [".balign 4", "skip:", "  csrr t6, mepc", "  addi t6, t6, 4", "  csrw mepc, t6", "  mret"]
    # HTIF's two words: QEMU's spike machine will not start without them,
    # though nothing here writes them.
    lines += [".balign 64", ".globl tohost", "tohost: .dword 0", ".globl fromhost", "fromhost: .dword 0"]
    lines += [".balign 8", "csrvals:"] + [f"  .dword {v:#x}" for v in data]
    lines += ["gprs:"] + [f"  .dword {v:#x}" for v in xs]
    lines += ["fregs:"] + [f"  .dword {int(v, 16):#x}" for v in state["f"]]
    if state.get("vector"):
        lines += [".balign 8", "vregs:"]
        for v in state["v"]:
            lines += ["  .byte " + ", ".join(f"0x{v[i:i + 2]}" for i in range(0, len(v), 2))]
    return "\n".join(lines) + "\n"


def write_elf(path: pathlib.Path, entry: int, segments, symbols: dict):
    """An ELF64 RISC-V executable of these (address, bytes) segments, with a
    symbol table: Spike's loader insists on section headers."""
    ph = [(addr, data) for addr, data in segments if data]
    strtab = b"\0"
    syms = [bytes(24)]
    for name, value in symbols.items():
        syms.append(struct.pack("<IBBHQQ", len(strtab), 0x10, 0, 0xFFF1, value, 8))   # global, absolute
        strtab += name.encode() + b"\0"
    symtab = b"".join(syms)
    shstrtab = b"\0.symtab\0.strtab\0.shstrtab\0"
    data_off = 64 + 56 * len(ph)
    blobs, off = [], data_off
    for _, data in ph:
        blobs.append(off)
        off += len(data)
    sym_off = off
    str_off = sym_off + len(symtab)
    shs_off = str_off + len(strtab)
    sh_off = (shs_off + len(shstrtab) + 7) & ~7
    out = bytearray()
    out += b"ELF" + bytes([2, 1, 1, 0]) + bytes(8)
    out += struct.pack("<HHIQQQIHHHHHH", 2, 243, 1, entry, 64, sh_off, 0x4, 64, 56, len(ph), 64, 4, 3)
    for (addr, data), o in zip(ph, blobs):
        out += struct.pack("<IIQQQQQQ", 1, 7, o, addr, addr, len(data), len(data), 0x1000)
    tail = bytearray(symtab + strtab + shstrtab)
    tail += bytes(sh_off - shs_off - len(shstrtab))
    shdr = bytes(64)
    shdr += struct.pack("<IIQQQQIIQQ", 1, 2, 0, 0, sym_off, len(symtab), 2, 1, 8, 24)            # .symtab
    shdr += struct.pack("<IIQQQQIIQQ", 9, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0)             # .strtab
    shdr += struct.pack("<IIQQQQIIQQ", 17, 3, 0, 0, shs_off, len(shstrtab), 0, 0, 1, 0)          # .shstrtab
    with path.open("wb") as f:
        f.write(out)
        for _, data in ph:
            f.write(data)
        f.write(tail)
        f.write(shdr)


def ram_segments(ram: bytes, base: int, page=4096, gap=16):
    """RAM as segments: runs of pages that are not all zero, joined across
    fewer than `gap` zero pages."""
    zero = bytes(page)
    nonzero = [ram[i:i + page] != zero for i in range(0, len(ram), page)]
    segs, start, last = [], None, None
    for i, nz in enumerate(nonzero):
        if not nz:
            continue
        if start is not None and i - last > gap:
            segs.append((start, last + 1))
            start = None
        if start is None:
            start = i
        last = i
    if start is not None:
        segs.append((start, last + 1))
    return [(base + a * page, ram[a * page:b * page]) for a, b in segs], nonzero


def snapshot_program(snap: pathlib.Path, out: pathlib.Path, args) -> Program:
    # The machine the snapshot is of: its command.txt, or what follows `--`.
    cmd_file = snap / "command.txt"
    machine = args.passthrough or (cmd_file.read_text().split("\n") if cmd_file.exists() else [])
    machine = [a for a in machine if a and not a.startswith(("-gdb", "-snapshot", "-restore=", "-stopat="))]
    if not machine:
        raise RuntimeError(f"{snap} has no command.txt: give DoomV's arguments for its machine after --")
    if not any(a in ("-ng", "-nogui", "-headless", "--headless") for a in machine):
        machine = ["-ng"] + machine
    state_dir = out / "state"
    p = subprocess.run([str(DOOMV)] + machine + [f"-restore={snap}", f"-export-state={state_dir}"],
                       cwd=out, capture_output=True, text=True, errors="replace", timeout=args.timeout)
    if not (state_dir / "state.json").exists():
        raise RuntimeError("DoomV could not export the snapshot's state: " +
                           ((p.stdout or "") + (p.stderr or "")).strip()[-400:])
    elf, state, ram_mb = build_restore_elf(state_dir, out, args.timeout)
    # The snapshot's own hart: its -march, or for a Linux boot DoomV's
    # default there; --march, given, wins.
    march = next((a[7:] for a in machine if a.startswith("-march=")), None)
    if args.march_given:
        march = args.march
    elif march is None:
        march = LINUX_MARCH if any(a.startswith("-kernel=") for a in machine) else args.march
    march = "_".join(dict.fromkeys(t for t in march.lower().split("_") if t))
    return Program(snap.name, elf, machine, [f"-restore={snap}"], int(state["pc"], 16),
                   first_step=int(state["step"]), ram_mb=max(ram_mb, 256), vlen=int(state["vlen"]), march=march)


def build_restore_elf(state_dir: pathlib.Path, out: pathlib.Path, timeout: int):
    """An ELF of the RAM in an -export-state folder and the program that puts
    the rest of it back: (the ELF, the state, the RAM in MB)."""
    state = json.loads((state_dir / "state.json").read_text())
    ram = (state_dir / "ram.bin").read_bytes()
    base = int(state["ram_base"], 16)
    segs, nonzero = ram_segments(ram, base)

    # The restore program goes in the highest run of zero pages that holds it,
    # below the top of RAM.
    need = 16 + (len(state["v"][0]) // 2 * 32 + 4095) // 4096 if state.get("vector") else 16
    run, at = 0, None
    for i in range(len(nonzero) - 1, -1, -1):
        run = run + 1 if not nonzero[i] else 0
        if run >= need:
            at = base + i * 4096
            break
    if at is None:
        raise RuntimeError("no room in the snapshot's RAM for the restore program")
    src = out / "restore.S"
    src.write_text(restore_assembly(state, at))
    elf = out / "restore.elf"
    code, text = in_wsl(["riscv64-unknown-elf-gcc", "-nostdlib", "-static", "-march=rv64gcv_zicsr_zifencei",
                         "-mabi=lp64d", f"-Wl,-Ttext={at:#x}", "-Wl,--no-relax", "-o", wsl(out / "stub.elf"),
                         wsl(src)], timeout)
    if code != 0:
        raise RuntimeError("cannot assemble the restore program: " + text.strip()[-400:])
    code, text = in_wsl(["riscv64-unknown-elf-objcopy", "-O", "binary", wsl(out / "stub.elf"),
                         wsl(out / "stub.bin")], timeout)
    stub = (out / "stub.bin").read_bytes()
    if len(stub) > need * 4096:
        raise RuntimeError("the restore program does not fit where it was put")
    stub_syms = run_suite.elf_symbols(out / "stub.elf")
    write_elf(elf, at, segs + [(at, stub)], {k: stub_syms[k] for k in ("tohost", "fromhost")})
    return elf, state, (len(ram) + 2**20 - 1) // 2**20


# ---- one program ---------------------------------------------------------------------

def elf_program(elf: pathlib.Path, args) -> Program:
    syms = run_suite.elf_symbols(elf)
    return Program(elf.name, elf, elf_machine(elf, args.march, args.vlen, syms), [], elf_entry(elf),
                   syms.get("tohost"), vlen=args.vlen, march=args.march)


def corun(item, args) -> dict:
    out = WORK / (item.name if isinstance(item, pathlib.Path) else pathlib.Path(item).name)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    prog = snapshot_program(pathlib.Path(item), out, args) if args.snapshot else elf_program(item, args)
    entry = prog.entry
    run_suite.use_vlen(prog.vlen)
    config = run_suite.WSL_CFG
    jobs = {
        "sail": lambda: run_sail(prog, out, config, args.limit, args.timeout),
        "doomv": lambda: run_doomv(prog, out, args.limit, args.timeout),
        "spike": lambda: run_spike(prog, out, prog.march, args.limit, args.timeout),
        "whisper": lambda: run_whisper(prog, out, prog.march, args.limit, args.timeout),
        "qemu": lambda: run_qemu(prog, out, prog.march, args.limit, args.timeout),
    }
    jobs = {k: v for k, v in jobs.items() if k == "sail" or k in args.sims}
    with concurrent.futures.ThreadPoolExecutor(len(jobs)) as pool:
        futures = {k: pool.submit(f) for k, f in jobs.items()}
        runs = {}
        for k, f in futures.items():
            try:
                runs[k] = f.result()
            except Exception as e:   # one simulator's failure is a result, not the end of the run
                runs[k] = Run(k, error=f"{type(e).__name__}: {e}")
    sail = runs["sail"]
    report = {"elf": str(item), "entry": f"0x{entry:x}", "vlen": prog.vlen, "march": prog.march, "sims": {},
              "from_snapshot": bool(prog.start), "first_step": prog.first_step}
    sail_steps = len(sail.steps) - (sync(sail.steps, entry) or 0) if sail.steps else 0
    report["sail"] = {"steps": sail_steps, "error": sail.error}
    a0 = sync(sail.steps, entry) or 0
    for name in args.sims:
        r = runs[name]
        entry_rep = {"dropped": r.dropped, "log": str(r.log) if r.log else None}
        if r.error and not r.steps:
            entry_rep.update(status="error", error=r.error)
        elif sail.error and not sail.steps:
            entry_rep.update(status="no reference")
        else:
            matched, d = compare(sail, r, entry, prog.tohost, args.limit)
            if d is None:
                entry_rep.update(status="match", steps=matched, capped=r.capped)
            else:
                entry_rep.update(status="diverged", steps=matched, at_step=d.step, pc=f"0x{d.pc:x}", what=d.what,
                                 sail=d.sail, theirs=d.other)
                i = a0 + d.step
                if 0 <= i < len(sail.steps):
                    entry_rep["instruction"] = sail.steps[i].text
                    entry_rep["context"] = [f"0x{s.pc:x}  {s.text}" for s in sail.steps[max(a0, i - 4):i + 1]]
        report["sims"][name] = entry_rep
    if "doomv" in args.sims and not args.no_lockstep:
        report["lockstep"] = lockstep_and_snapshot(prog, out, args.limit, sail, args.timeout)
    (out / "report.json").write_text(json.dumps(report, indent=2))
    return report


def print_report(rep: dict):
    where = f"from step {rep['first_step']}, pc {rep['entry']}" if rep.get("from_snapshot") else f"entry {rep['entry']}"
    print(f"\n== {pathlib.Path(rep['elf']).name}   ({where}, VLEN {rep['vlen']})")
    s = rep["sail"]
    print(f"   sail      {'error: ' + s['error'] if s['error'] and not s['steps'] else str(s['steps']) + ' steps (the reference)'}")
    for name, r in rep["sims"].items():
        drop = f"   [not given: {', '.join(r['dropped'])}]" if r.get("dropped") else ""
        if r["status"] == "match":
            capped = " (its log stopped there, at the size limit)" if r.get("capped") else ""
            print(f"   {name:9} matches Sail, {r['steps']} steps{capped}{drop}")
        elif r["status"] == "diverged":
            print(f"   {name:9} DIVERGES at step {r['at_step']}, pc {r['pc']} ({r.get('instruction', '')}): "
                  f"{r['what']}  sail {r['sail']}  {name} {r['theirs']}{drop}")
        else:
            print(f"   {name:9} {r['status']}: {r.get('error', '')}{drop}")
    ls = rep.get("lockstep")
    if ls:
        if ls["status"] == "match":
            print(f"   lock-step DoomV vs Sail (strict): {ls.get('summary', 'match')}")
        elif ls["status"] == "mismatch":
            print("   lock-step DoomV vs Sail (strict): MISMATCH")
            print("      " + ls["detail"].splitlines()[0])
            if len(ls["detail"].splitlines()) > 1:
                print("      " + ls["detail"].splitlines()[1].strip())
            if ls.get("snapshot", "").startswith(("not", "the first")):
                print(f"      snapshot: {ls['snapshot']}")
            elif ls.get("snapshot"):
                ok = ls.get("verified")
                tag = "verified: restoring it runs the failing instruction next" if ok else \
                      f"NOT verified: restored, the next pc is {ls.get('next_pc')} not {ls.get('bad_pc')}" \
                      if ok is False else "unverified"
                print(f"      snapshot before the failing instruction (step {ls['snapshot_step']}): "
                      f"{ls['snapshot']} -- {tag}")
                print(f"      restore: {ls['restore']}")
        else:
            print(f"   lock-step DoomV vs Sail: {ls['status']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elfs", nargs="*", help="ELF files, or test names with --suite, or snapshot folders with --snapshot")
    ap.add_argument("--snapshot", action="store_true",
                    help="the arguments are DoomV snapshot folders: start every simulator from each one")
    ap.add_argument("--suite", help="take the ELFs from this directory under tests/suites")
    ap.add_argument("--limit", type=int, help="instructions each runs at most (default 2M; 20000 from a snapshot)")
    ap.add_argument("--count", type=int, help="with --suite and no names: at most this many tests")
    ap.add_argument("--march", help="the ISA every simulator is given (default: what the suites use with "
                                     "Sail; from a snapshot, the snapshot's own)")
    ap.add_argument("--vlen", type=int, default=128, help="vector length for all of them (default 128)")
    ap.add_argument("--sims", default=",".join(SIMS), help="which to run beside Sail (default all: "
                                                           + ",".join(SIMS) + ")")
    ap.add_argument("--no-lockstep", action="store_true", help="skip DoomV's strict lock-step and snapshot")
    ap.add_argument("--timeout", type=int, default=600, help="seconds each simulator may take (default 600)")
    ap.add_argument("--jobs", type=int, default=2, help="programs at a time, each running all simulators")
    argv = sys.argv[1:]
    if "--" in argv:
        i = argv.index("--")
        argv, passthrough = argv[:i], argv[i + 1:]
    else:
        passthrough = []
    args = ap.parse_args(argv)
    args.passthrough = passthrough
    args.march_given = args.march is not None
    if args.march is None:
        args.march = run_suite.SUITE_MARCH
    if args.limit is None:
        args.limit = 20_000 if args.snapshot else 2_000_000
    args.sims = [s for s in args.sims.split(",") if s]
    # Each extension once: the suites' string names zfh twice, which Whisper crashes on.
    args.march = "_".join(dict.fromkeys(t for t in args.march.lower().split("_") if t))
    if set(args.sims) - set(SIMS):
        ap.error("unknown simulator: " + ", ".join(sorted(set(args.sims) - set(SIMS))))
    if args.vlen < 128 or args.vlen & (args.vlen - 1):
        ap.error("--vlen must be a power of two, 128 or more")
    m = re.match(r"riscv-vector-tests-v(\d+)x", args.suite or "")
    if m and args.vlen == 128:
        args.vlen = int(m.group(1))    # the suite is built for that VLEN

    if args.snapshot:
        elfs = [pathlib.Path(e).resolve() for e in args.elfs]
        for e in elfs:
            if not (e / "state.bin").is_file():
                ap.error(f"no snapshot in {e}")
    elif args.suite:
        root = SUITES / args.suite
        elfs = [root / n for n in args.elfs] if args.elfs else \
            sorted(p for p in root.iterdir() if p.is_file() and not p.suffix and is_elf64(p))
        if args.count:
            elfs = elfs[:args.count]
    else:
        elfs = [pathlib.Path(e).resolve() for e in args.elfs]
    if not elfs:
        ap.error("no programs: give ELF files, or --suite")
    for e in elfs:
        if not args.snapshot and not e.is_file():
            ap.error(f"no such file: {e}")
    WORK.mkdir(parents=True, exist_ok=True)

    reports = []
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        for rep in pool.map(lambda e: corun(e, args), elfs):
            print_report(rep)
            reports.append(rep)

    if len(reports) > 1:
        print("\n== summary (against Sail)")
        for name in args.sims:
            counts = {}
            for rep in reports:
                st = rep["sims"][name]["status"]
                counts[st] = counts.get(st, 0) + 1
            print(f"   {name:9} " + "  ".join(f"{k} {v}" for k, v in sorted(counts.items())))
        ls = [r["lockstep"]["status"] for r in reports if "lockstep" in r]
        if ls:
            print(f"   {'lock-step':9} " + "  ".join(f"{k} {ls.count(k)}" for k in sorted(set(ls))))
        (WORK / "summary.json").write_text(json.dumps(reports, indent=2))
    print(f"\nTraces and reports: {WORK}")
    bad = any(r["sims"].get("doomv", {}).get("status") == "diverged"
              or r.get("lockstep", {}).get("status") == "mismatch" for r in reports)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
