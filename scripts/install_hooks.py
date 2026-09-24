#!/usr/bin/env python3
"""Point git at this repo's hooks, in .githooks/.

Git does not version-control .git/hooks, so a hook in a fresh clone does
nothing until this is run. Rather than copying files into .git/hooks, where
they then drift from the versioned copy, this sets core.hooksPath -- so the
hook that runs is always the one in the tree.

  python scripts/install_hooks.py            # enable
  python scripts/install_hooks.py --disable  # back to git's default

What it enables: pre-push, which refuses a push until scripts/ci.py passes.
`git push --no-verify` skips it when you need it to.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOKS = "'.githooks'"


def git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disable", action="store_true", help="unset core.hooksPath")
    args = ap.parse_args()

    if args.disable:
        git("config", "--unset", "core.hooksPath")
        print("hooks disabled: git is back to .git/hooks")
        return 0

    hook = ROOT / ".githooks" / "pre-push"
    if not hook.exists():
        print(f"missing {hook}", file=sys.stderr)
        return 1
    # The executable bit matters wherever git runs hooks through a shell, and
    # a fresh clone on Windows does not necessarily carry it.
    result = git("update-index", "--chmod=+x", ".githooks/pre-push")
    if result.returncode != 0 and "not in the cache" not in result.stderr:
        pass  # not fatal: the file may simply not be staged yet

    if git("config", "core.hooksPath", ".githooks").returncode != 0:
        print("could not set core.hooksPath", file=sys.stderr)
        return 1
    print("hooks enabled: pre-push will run scripts/ci.py before every push")
    print("  skip once with: git push --no-verify")
    print("  disable with:   python scripts/install_hooks.py --disable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
