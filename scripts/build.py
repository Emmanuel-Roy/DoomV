#!/usr/bin/env python3
"""Build DoomV, its DOOM guest, Linux images, or everything.

Examples:
  python scripts/build.py doom
  python scripts/build.py linux
  python scripts/build.py all
  python scripts/build.py all --no-pgo     # skip training a PGO profile

Built with Clang, the emulator is profile-guided: the first build that has a
guest to train on also trains a profile (performance/pgo.py, a few minutes),
and every `make` after it uses that profile. See the Makefile's PROFILE. A
later build retrains it when the sources have changed since it was trained,
since a stale profile gives up a good part of what PGO is worth.
"""
import argparse
import subprocess
import sys

from common import build_emulator, checkout_lock, entrypoint, wsl_path, wsl_script, ROOT, require_files

sys.path.insert(0, str(ROOT / "performance"))
import pgo  # noqa: E402  (where the profile lives, and which compiler make picks)


def keep_profile_fresh():
    """Train the PGO profile if there is none, or retrain it if it is stale.

    After the guests, because training runs them. Stale means the sources
    differ from the ones it was trained on (pgo.py --stale), and matters: a
    profile three commits old kept about half of what a fresh one is worth.
    GCC is left out: its profile is not one `make` reuses, so training on
    every build would only cost minutes for a binary the next `make` replaces.
    """
    cxx, _ = pgo.default_compiler()
    if not pgo.is_clang(cxx):
        return
    if not pgo.PROFILE.exists():
        print("=== PGO profile (first build) ===", flush=True)
    else:
        note = pgo.stale_note()
        if not note:
            return
        print(f"=== {note} ===", flush=True)
    subprocess.run([sys.executable, str(ROOT / "performance" / "pgo.py")], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("doom", "linux", "all"), nargs="?", default="all")
    parser.add_argument("--game", choices=("free", "doom", "doom2", "finaldoom"), default="free")
    parser.add_argument("--no-pgo", action="store_true",
                        help="do not train or retrain the PGO profile, even if there is none or it is stale")
    args = parser.parse_args()
    with checkout_lock():
        if args.target in ("doom", "linux", "all"):
            build_emulator()
        if args.target in ("doom", "all"):
            wad_names = {"free": "DOOM1.WAD", "doom": "DOOM.WAD", "doom2": "DOOM2.WAD", "finaldoom": "TNT.WAD"}
            wad = ROOT / "tools/doom/doombuild" / wad_names[args.game]
            require_files(wad)
            wsl_script("build_doom.sh", args.game, wsl_path(wad))
        if args.target in ("linux", "all"):
            wsl_script("build_linux.sh")
        if not args.no_pgo:
            keep_profile_fresh()
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
