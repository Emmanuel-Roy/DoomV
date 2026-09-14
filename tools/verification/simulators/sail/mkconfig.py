#!/usr/bin/env python3
"""Generate an RVA23S64 configuration for the Sail RISC-V model.

Why generate rather than use riscv-arch-test's config/sail/sail-RVA23S64:
that file cannot be loaded by any tagged Sail release. Its required
`platform` properties postdate it (simple_interrupt_generator 2026-04-22,
reservation 2026-04-24, wfi_available_to_user_mode 2026-06-03), and pinning
Sail to before those dates makes it demand *different* properties the config
also lacks. It appears to track an untagged revision.

The extension list in sail-RVA23S64.yaml is the authoritative content; the
json is one serialisation of it for one model build. So this reads the yaml
and enables the named extensions in whatever the installed model's own
default config offers, which is robust across Sail versions.

It also prints an audit of DoomV's extension set against the same list --
which is what found Sscofpmf and Ssstateen missing after the hypervisor work
was believed to have completed the mandatory set.

Usage:  python3 mkconfig.py [--audit-only]
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
YAML = os.path.join(REPO, "tools", "verification", "tests", "arch-test",
                    "config", "sail", "sail-RVA23S64", "sail-RVA23S64.yaml")
SIM = os.path.join(HERE, "src", "build", "c_emulator", "sail_riscv_sim")
OUT = os.path.join(HERE, "rva23s64.json")
SIM = os.environ.get("SAIL", SIM)
YAML = os.environ.get("SAIL_PROFILE", YAML)

# Extensions the profile names that are not separate toggles in Sail's
# config, because the model expresses them another way.
#
#   Zve*/Zvl*        implied by V plus the configured VLEN/ELEN
#   Za64rs/Za128rs   reservation-set size guarantees, not features
#   I/C/Sm           part of the base configuration
#   Ssu64xl          UXLEN=64, set in base rather than as an extension
NOT_A_TOGGLE = {
    "Zve64d", "Zve64f", "Zve64x", "Zve32f", "Zve32x",
    "Zvl128b", "Zvl64b", "Zvl32b",
    "Za128rs", "Za64rs", "Sm", "I", "C", "Ssu64xl",
}

# Sail's spelling differs from the profile's in a couple of places.
ALIAS = {
    "Ssstateen": "Stateen",  # Sail names the whole family after the base
    "Supm": "Ssnpm",         # user pointer masking is the same toggle
}

# What DoomV implements, and what it satisfies structurally rather than by
# implementing anything. Kept here so the audit lives next to the list it
# audits against.
DOOMV_IMPLEMENTS = set("""
I M A F D C B V U S Sm Zic64b Zicbom Zicbop Zicboz Zicntr Zicond Zicsr
Zifencei Zihintntl Zihintpause Zihpm Zimop Zaamo Zalrsc Za128rs Za64rs Zawrs
Zfa Zfhmin Zca Zcd Zcb Zcmop Zba Zbb Zbs Zve64d Zve64f Zve64x Zve32f Zve32x
Zvl128b Zvl64b Zvl32b Zvbb Zvkb Zvfhmin Sstc Ssnpm Supm Sv39 Svade Svbare
Svinval Svnapot Svpbmt Sha H Sscofpmf Ssstateen
""".split())

DOOMV_GUARANTEES = set("""
Ziccamoa Ziccif Zicclsm Ziccrse Zkt Zvkt Ssccptr Sstvala Sstvecd Ssu64xl
Sscounterenw Shcounterenw Shvstvala Shtvala Shvstvecd Shvsatpa Shgatpa
""".split())


def profile_extensions():
    return re.findall(r"- \{ name: ([A-Za-z0-9_]+), version", open(YAML).read())


def audit(exts):
    impl = [e for e in exts if e in DOOMV_IMPLEMENTS]
    guar = [e for e in exts if e in DOOMV_GUARANTEES and e not in DOOMV_IMPLEMENTS]
    missing = [e for e in exts if e not in DOOMV_IMPLEMENTS and e not in DOOMV_GUARANTEES]
    print("RVA23S64 mandatory extensions: %d" % len(exts))
    print("  implemented by DoomV:        %d" % len(impl))
    print("  satisfied as guarantees:     %d" % len(guar))
    print("  NOT IMPLEMENTED:             %d%s"
          % (len(missing), ("  -> " + ", ".join(missing)) if missing else ""))
    return missing


def main():
    exts = profile_extensions()
    missing = audit(exts)
    if "--audit-only" in sys.argv:
        return 1 if missing else 0

    if not os.path.exists(SIM):
        print("\nsail_riscv_sim not built; run build.sh first", file=sys.stderr)
        return 1

    raw = subprocess.run([SIM, "--print-default-config"], capture_output=True,
                         text=True).stdout
    # The model emits JSON with // comments, which it accepts on the way back
    # in but json.loads does not.
    cfg = json.loads(re.sub(r"^\s*//.*$", "", raw, flags=re.M))

    enabled, unknown = [], []
    for name in sorted(set(exts)):
        if name in NOT_A_TOGGLE:
            continue
        key = ALIAS.get(name, name)
        if key in cfg["extensions"]:
            cfg["extensions"][key]["supported"] = True
            enabled.append(key)
        else:
            unknown.append(name)

    # VLEN must match the device under test. DoomV is fixed at 128 bits
    # (Registers::VLEN_BITS) and Sail defaults to 256, which makes every
    # vector test disagree for a reason that has nothing to do with
    # correctness -- the two machines are simply different widths. ELEN is
    # 64 on both, so only vlen_exp needs setting.
    #
    # This is a property of the device under test rather than of RVA23: the
    # profile mandates Zvl128b, a *minimum* of 128, and both 128 and 256
    # satisfy it.
    cfg["extensions"]["V"]["vlen_exp"] = 7   # 2**7 = 128 bits

    # PMP is deliberately left at the model's default rather than disabled.
    #
    # Sail implements it faithfully: with entries present but none
    # configured, the spec denies S and U mode access outright. That is what
    # revealed the differential tests had been depending on spike's
    # permissive default -- every suite that drops privilege took a fetch
    # access fault before its first instruction there. The tests now install
    # a permit-all entry themselves, as real software does, so no
    # accommodation is needed here and the two references run the same
    # configuration.

    json.dump(cfg, open(OUT, "w"), indent=2)
    print("\nenabled %d extensions; wrote %s" % (len(enabled), OUT))
    if unknown:
        print("named by the profile but absent from this Sail build: %s"
              % ", ".join(unknown))
    return 0


if __name__ == "__main__":
    sys.exit(main())
