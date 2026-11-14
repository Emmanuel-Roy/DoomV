"""Demo 4: a histogram of the instructions DOOM executes.

Replays a recording of the real Pico in DoomV with -histogram, which counts
how many times each instruction address retires (DoomV takes its exact,
one-step-at-a-time path for this; the replay is still checked frame by frame
against the Pico). Each address is then looked up in the guest ELF -- the
same machine code as the Pico's firmware -- to give:

  * the instructions executed most, by mnemonic (compressed forms apart)
  * instruction classes: loads, stores, ALU, branches, jumps, mul/div,
    bit-manipulation (Zba/Zbb/Zbs/Zbkb), CSR/system
  * the functions that run most, and DOOM engine vs port vs C runtime

Writes build/demo/histogram/report.html (charts + tables) and CSV files.

  python demo/histogram.py                         # the newest recording, all of it
  python demo/histogram.py --frames 700            # the first 700 frames
  python demo/histogram.py build/demo/run.p2dl --record 30
"""
import argparse
import collections
import csv
import html
import os
import re
import shutil
import subprocess
import sys

from _common import GUEST, OUT, banner, frames, latest_recording, record, rel, replay

LOADS = {"lb", "lh", "lw", "lbu", "lhu", "lwsp"}
STORES = {"sb", "sh", "sw", "swsp"}
BRANCHES = {"beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez"}
JUMPS = {"jal", "jalr", "j", "jr"}
MULDIV = {"mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"}
BITMANIP = {"sh1add", "sh2add", "sh3add", "andn", "orn", "xnor", "clz", "ctz", "cpop", "max", "maxu",
            "min", "minu", "sext.b", "sext.h", "zext.h", "rol", "ror", "rori", "rev8", "orc.b", "bset",
            "bseti", "bclr", "bclri", "binv", "binvi", "bext", "bexti", "pack", "packh", "brev8", "zip", "unzip"}
CLASSES = ["load", "store", "ALU", "branch", "jump", "mul/div", "bit-manip", "CSR/system"]


def iclass(m):
    base = m[2:] if m.startswith("c.") else m
    if base in LOADS: return "load"
    if base in STORES: return "store"
    if base in BRANCHES: return "branch"
    if base in JUMPS: return "jump"
    if base in MULDIV: return "mul/div"
    if base in BITMANIP: return "bit-manip"
    if base.startswith("csr") or base in ("ecall", "ebreak", "fence", "fence.i", "wfi", "mret"): return "CSR/system"
    return "ALU"


def tool(name):
    t = shutil.which("riscv-none-elf-" + name)
    if not t:
        sys.exit(f"demo: riscv-none-elf-{name} not found on PATH (it comes with the RISC-V toolchain)")
    return t


def disassemble(elf):
    """{address: (mnemonic, function)} for every instruction in the ELF."""
    out = subprocess.run([tool("objdump"), "-d", "-M", "no-aliases", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    insns, fn = {}, "?"
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <(.+)>:$", line)
        if m:
            fn = m.group(2)
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)", line)
        if m:
            insns[int(m.group(1), 16)] = (m.group(2), fn)
    return insns


def origins(elf):
    """{function: 'DOOM engine' | 'port' | 'C runtime'} from the symbols' source files."""
    out = subprocess.run([tool("nm"), "-l", "--defined-only", elf], capture_output=True, text=True).stdout
    where = {}
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 3 or parts[1].lower() not in "tw":
            continue
        src = parts[3].replace("\\", "/") if len(parts) > 3 else ""
        where[parts[2]] = ("DOOM engine" if "doomgeneric" in src
                           else "port" if re.search(r"pico2doom/(port|doomv|firmware)/", src)
                           else "C runtime")
    return where


def bars_svg(rows, title, unit_total, width=760):
    """One horizontal bar chart (a single series), as inline SVG with hover titles."""
    bar, gap, label_w, value_w = 18, 6, 170, 70
    h = len(rows) * (bar + gap) + 8
    top = max(v for _, v in rows) or 1
    plot_w = width - label_w - value_w
    out = [f'<svg viewBox="0 0 {width} {h}" width="100%" role="img" aria-label="{html.escape(title)}">']
    for i, (name, v) in enumerate(rows):
        y = 4 + i * (bar + gap)
        w = max(2.0, plot_w * v / top)
        pct = 100 * v / unit_total
        tip = f"{name}: {v:,} ({pct:.2f}%)"
        out.append(f'<g class="row"><title>{html.escape(tip)}</title>'
                   f'<rect class="hit" x="0" y="{y - gap / 2}" width="{width}" height="{bar + gap}"/>'
                   f'<text class="lbl" x="{label_w - 8}" y="{y + bar * 0.72}" text-anchor="end">{html.escape(name)}</text>'
                   f'<rect class="bar" x="{label_w}" y="{y}" width="{w:.1f}" height="{bar}" rx="4"/>'
                   f'<text class="val" x="{label_w + w + 6:.1f}" y="{y + bar * 0.72}">{pct:.1f}%</text></g>')
    out.append("</svg>")
    return "\n".join(out)


def table(headers, rows):
    th = "".join(f"<th>{html.escape(h)}</th>" for h in headers)
    body = "".join("<tr>" + "".join(f"<td>{html.escape(str(c))}</td>" for c in r) + "</tr>" for r in rows)
    return f"<details><summary>Table</summary><table><tr>{th}</tr>{body}</table></details>"


def write_report(path, meta, mn, cls, fns, org, compressed, total):
    css = """
:root{--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--rule:#e4e3df;--bar:#2a78d6;--hover:#f0efec}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--rule:#383835;--bar:#3987e5;--hover:#262624}}
:root[data-theme="dark"]{--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--rule:#383835;--bar:#3987e5;--hover:#262624}
body{background:var(--surface);color:var(--ink);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;margin:0 auto;max-width:820px;padding:24px 16px}
h1{font-size:24px;margin:0 0 4px}h2{font-size:17px;margin:32px 0 8px}p.sub{color:var(--ink2);margin:0 0 16px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin:16px 0}
.tile{border:1px solid var(--rule);border-radius:8px;padding:10px 12px}.tile b{display:block;font-size:22px}.tile span{color:var(--ink2);font-size:13px}
svg text{font-size:12px;fill:var(--ink2)} svg .lbl{fill:var(--ink);font-family:ui-monospace,Consolas,monospace}
svg .bar{fill:var(--bar)} svg .hit{fill:transparent} svg .row:hover .hit{fill:var(--hover)}
details{margin:6px 0 0;color:var(--ink2)} table{border-collapse:collapse;margin-top:6px;font-size:13px}
td,th{border-bottom:1px solid var(--rule);padding:3px 10px;text-align:left} td:not(:first-child){text-align:right;font-variant-numeric:tabular-nums}
"""
    pct = lambda v: f"{100 * v / total:.2f}%"
    top_mn = mn.most_common(25)
    top_fn = fns.most_common(20)
    parts = [f"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
             f"<title>DOOM Instruction Histogram</title><style>{css}</style></head><body>",
             "<h1>What DOOM executes on RISC-V</h1>",
             f"<p class='sub'>{html.escape(meta)}</p><div class='tiles'>",
             f"<div class='tile'><b>{total / 1e9:.2f} B</b><span>instructions retired</span></div>",
             f"<div class='tile'><b>{len(mn)}</b><span>distinct instructions used</span></div>",
             f"<div class='tile'><b>{100 * compressed / total:.0f}%</b><span>compressed (16-bit, C extension)</span></div>",
             f"<div class='tile'><b>{100 * org.get('DOOM engine', 0) / total:.0f}%</b><span>in DOOM's own code</span></div></div>",
             "<h2>Most executed instructions</h2>", bars_svg(top_mn, "most executed instructions", total),
             table(["instruction", "retired", "share"], [(m, f"{v:,}", pct(v)) for m, v in mn.most_common()]),
             "<h2>Instruction classes</h2>",
             bars_svg([(c, cls[c]) for c in sorted(CLASSES, key=lambda c: -cls[c]) if cls[c]], "instruction classes", total),
             table(["class", "retired", "share"], [(c, f"{cls[c]:,}", pct(cls[c])) for c in CLASSES]),
             "<h2>Where the time goes: functions</h2>", bars_svg(top_fn, "hottest functions", total),
             table(["function", "retired", "share"], [(f, f"{v:,}", pct(v)) for f, v in fns.most_common(60)]),
             "<h2>DOOM engine, port and C runtime</h2>",
             bars_svg(org.most_common(), "code origin", total),
             table(["origin", "retired", "share"], [(o, f"{v:,}", pct(v)) for o, v in org.most_common()]),
             "</body></html>"]
    open(path, "w", encoding="utf-8").write("\n".join(parts))


def text_bars(rows, total, n=15, width=40):
    top = rows[0][1] if rows else 1
    for name, v in rows[:n]:
        print(f"  {name:<22} {'#' * max(1, round(width * v / top)):<{width}} {100 * v / total:5.1f}%")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("recording", nargs="?", help="a .p2dl (default: the newest in build/demo)")
    ap.add_argument("--frames", type=int, help="only the first N frames (rounded down to a multiple of 35)")
    ap.add_argument("--record", type=float, metavar="SECONDS", help="record a new run first")
    a = ap.parse_args()

    src = a.recording
    if a.record or not src:
        newest = latest_recording()
        if a.record or not newest:
            src = src or os.path.join(OUT, "run.p2dl")
            record(src, a.record or 30, (1, 1))
        else:
            src = newest
    fr = frames(open(src, "rb").read())
    # DoomV writes the counts every 35 frames: stop one frame past such a point.
    n = min(a.frames or len(fr), len(fr) - 1) // 35 * 35
    if n < 35:
        sys.exit(f"demo: {rel(src)} is too short ({len(fr)} frames; need 36)")
    work = os.path.join(OUT, "histogram")
    os.makedirs(work, exist_ok=True)
    counts_path = os.path.join(work, "pc_counts.txt")
    if os.path.exists(counts_path):
        os.remove(counts_path)
    banner(f"Instruction histogram: {rel(src)}, frames 1..{n}\n"
           "  DoomV counts every retired instruction by address (exact path; still checked against the Pico)")
    matched, _, lines, diverged = replay(src, work, extra=["-histogram=" + os.path.abspath(counts_path)],
                                         max_frames=n + 1)
    if diverged or not os.path.exists(counts_path):
        sys.exit("demo: the replay did not complete:\n" + "\n".join(lines))

    counts, meta_frames = {}, None
    for line in open(counts_path):
        if line.startswith("# picolink frames"):
            meta_frames = int(line.split()[-1])
        elif line[0] != "#":
            pc, c = line.split()
            counts[int(pc, 16) & 0xFFFFFFFF] = int(c)
    elf = os.path.join(GUEST, "pico2doom-doomv.elf")
    insns, where = disassemble(elf), origins(elf)
    mn, cls, fns, org = (collections.Counter() for _ in range(4))
    compressed = unknown = 0
    for pc, c in counts.items():
        m, fn = insns.get(pc, ("?", "?"))
        if m == "?":
            unknown += c
            continue
        mn[m] += c
        cls[iclass(m)] += c
        fns[fn] += c
        org[where.get(fn, "C runtime")] += c
        if m.startswith("c."):
            compressed += c
    total = sum(mn.values())

    with open(os.path.join(work, "instructions.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["instruction", "class", "retired", "share"])
        for m, v in mn.most_common():
            w.writerow([m, iclass(m), v, f"{v / total:.6f}"])
    with open(os.path.join(work, "functions.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["function", "origin", "retired", "share"])
        for fn, v in fns.most_common():
            w.writerow([fn, where.get(fn, "C runtime"), v, f"{v / total:.6f}"])
    meta = (f"Retired instructions of the Pico2Doom guest in DoomV, replaying {os.path.basename(src)} "
            f"from boot to frame {meta_frames} -- every one also checked against the real Pico 2. "
            f"Mnemonics as objdump writes them without aliases; c.* are 16-bit compressed forms. "
            f"The port's share includes work only the replay needs: backend_present copies each frame "
            f"to DoomV's screen and port_hash_bytes fingerprints it for the comparison -- both outside "
            f"the instruction counts compared with the Pico.")
    report = os.path.join(work, "report.html")
    write_report(report, meta, mn, cls, fns, org, compressed, total)

    banner(f"Instruction histogram: {total:,} instructions retired, boot to frame {meta_frames}"
           f" ({len(mn)} distinct; {100 * compressed / total:.0f}% compressed)")
    print("\n  Most executed instructions")
    text_bars(mn.most_common(), total)
    print("\n  Classes")
    text_bars(sorted(((c, cls[c]) for c in CLASSES), key=lambda r: -r[1]), total, n=8)
    print("\n  Hottest functions")
    text_bars(fns.most_common(), total, n=10)
    print("\n  Origin")
    text_bars(org.most_common(), total, n=3)
    if unknown:
        print(f"\n  ({unknown:,} at addresses outside the ELF's code)")
    print(f"\n  report: {rel(report)}\n  tables: {rel(os.path.join(work, 'instructions.csv'))}, "
          f"{rel(os.path.join(work, 'functions.csv'))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
