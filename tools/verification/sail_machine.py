#!/usr/bin/env python3
"""Sail configured as a smaller machine than RVA23: fewer privilege modes,
fewer extensions -- the machines DoomV's -priv and -march can also be, so
the two can be lock-stepped for them.

    sail_machine.config(priv="m", march="rv64i_zicsr") -> a Sail configuration (dict)

`priv` is "m", "mu" or "msu"; `march` names what stays on. Everything the
specification makes depend on S -- supervisor-level extensions, page
tables, H -- goes with S; everything that needs a vector unit, an FPU or
compressed instructions goes with them.
"""
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = ROOT / "tools" / "verification" / "simulators" / "sail" / "rva23s64.json"

# Sail's extension names, by what they need.
NEEDS_S = {"S", "H", "Sha", "Shcounterenw", "Shgatpa", "Shtvala", "Shvsatpa", "Shvstvala", "Shvstvecd", "Ssccptr",
           "Sscofpmf", "Sscounterenw", "Ssnpm", "Ssqosid", "Sstc", "Sstvala", "Sstvecd", "Sv32", "Sv39", "Sv48",
           "Sv57", "Svade", "Svadu", "Svbare", "Svinval", "Svnapot", "Svpbmt", "Svrsw60t59b", "Svukte", "Svvptc",
           "Stateen"}
NEEDS_U = {"U", "Smnpm", "Ssnpm"}
VECTOR = {"V", "Zvabd", "Zvbb", "Zvbc", "Zvfbfmin", "Zvfbfwma", "Zvfh", "Zvfhmin", "Zvkb", "Zvkg", "Zvkned",
          "Zvknha", "Zvknhb", "Zvksed", "Zvksh", "Zvkt"}
FP = {"F", "D", "Zfa", "Zfbfmin", "Zfh", "Zfhmin", "Zcd", "Zcf"}
COMPRESSED = {"C", "Zca", "Zcb", "Zcd", "Zcf", "Zcmop"}
# -march token -> Sail names it keeps on.
TOKENS = {
    "m": {"M", "Zmmul"}, "a": {"A", "Zaamo", "Zalrsc"}, "f": {"F"}, "d": {"D"}, "c": {"C", "Zca", "Zcb"},
    "v": {"V"}, "h": {"H"}, "b": {"B", "Zba", "Zbb", "Zbs"}, "zicsr": {"Zicsr"}, "zifencei": {"Zifencei"},
}


def config(priv: str = "msu", march: str = "", base: pathlib.Path = BASE) -> dict:
    cfg = json.loads(base.read_text())
    ext = cfg["extensions"]
    if priv not in ("m", "mu", "msu"):
        raise ValueError("priv is m, mu or msu")
    off = set()
    if "s" not in priv:
        off |= NEEDS_S
    if "u" not in priv:
        off |= NEEDS_U
    if march:
        letters = march[4:].split("_")[0]
        keep = {"Zicsr"} if "zicsr" in march else set()
        for c in letters:
            keep |= TOKENS.get(c, set())
        if "g" in letters:
            keep |= TOKENS["m"] | TOKENS["a"] | TOKENS["f"] | TOKENS["d"] | {"Zicsr", "Zifencei"}
        for tok in march.split("_")[1:]:
            keep |= TOKENS.get(tok, {tok[:1].upper() + tok[1:]})
        # DoomV's C has the compressed FP loads and stores whenever D is on,
        # and Sail reports C in misa only with them (Zcd).
        if "c" in letters and ("d" in letters or "g" in letters):
            keep.add("Zcd")
        if "v" not in letters:
            off |= VECTOR
        if "f" not in letters and "g" not in letters:
            off |= FP
        if "c" not in letters:
            off |= COMPRESSED
        # Every other optional extension: only if named.
        for name in ext:
            if name not in keep and name not in ("S", "U") and len(name) > 1:
                off.add(name)
        for single in ("M", "A", "B", "H"):
            if single not in keep:
                off.add(single)
        # What DoomV always has once it has S: Bare and Sv39 translation,
        # A/D faults (Svade), the stimecmp timer and the S-mode trap
        # guarantees. Deeper page tables and the rest only if named.
        if "s" in priv:
            off -= {"S", "Svbare", "Sv39", "Svade", "Sstc", "Sstvala", "Sstvecd", "Ssccptr", "Sscounterenw"}
    for name in off:
        if name in ext:
            ext[name]["supported"] = False
    # V's own switch is its support level, not "supported".
    if "V" in off:
        ext["V"]["support_level"] = "Disabled"
    # Delegable exceptions: the hypervisor's (10, the VS ecall; 20-23, the
    # guest faults and virtual instruction) are reserved without H, as
    # Sail's validation has it.
    deleg = int(cfg["base"]["medeleg"]["delegatable_bits"]["value"].replace("_", ""), 16)
    if not ext.get("H", {}).get("supported", False):
        deleg &= ~((0xF << 20) | (1 << 10))
    cfg["base"]["medeleg"]["delegatable_bits"]["value"] = f"0x{deleg:016x}"
    # mstatus.FS and .VS are read-only zero without the unit and without S
    # (which can save a context for it).
    on = lambda n: ext.get(n, {}).get("supported", False)
    if not on("F") and not on("S"):
        cfg["base"]["mstatus"]["fs_legal_states"] = "ExtContext_Off"
    if not on("V") and not on("S"):
        cfg["base"]["mstatus"]["vs_legal_states"] = "ExtContext_Off"
    return cfg


if __name__ == "__main__":
    import sys
    print(json.dumps(config(*sys.argv[1:]), indent=2))
