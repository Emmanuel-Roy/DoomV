#!/usr/bin/env python3
"""Build DoomV, its DOOM guest, Linux images, or everything.

Examples:
  python scripts/build.py doom
  python scripts/build.py linux
  python scripts/build.py all
  python scripts/build.py all --no-pgo     # skip training a PGO profile

Built with Clang, the emulator is profile-guided: the first build that has a
guest to train on also trains a profile (performance/pgo.py, a few minutes),
and every `make` after it uses that profile. See the Makefile's PROFILE.
"""
import argparse
import subprocess
import sys

from common import build_emulator, checkout_lock, entrypoint, wsl_path, wsl_script, ROOT, require_files

sys.path.insert(0, str(ROOT / "performance"))
import pgo  # noqa: E402  (where the profile lives, and which compiler make picks)


def train_profile_once():
    """Train the PGO profile if there is none yet and make builds with Clang.

    After the guests, because training runs them. GCC is left out: its
    profile is not one `make` reuses, so training on every build would only
    cost minutes for a binary the next `make` replaces.
    """
    cxx, _ = pgo.default_compiler()
    if pgo.PROFILE.exists() or not pgo.is_clang(cxx):
        return
    print("=== PGO profile (first build only) ===", flush=True)
    subprocess.run([sys.executable, str(ROOT / "performance" / "pgo.py")], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("doom", "linux", "all"), nargs="?", default="all")
    parser.add_argument("--game", choices=("free", "doom", "doom2", "finaldoom"), default="free")
    parser.add_argument("--no-pgo", action="store_true",
                        help="do not train a PGO profile, even if there is none yet")
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
            train_profile_once()
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
