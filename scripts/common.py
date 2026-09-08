"""Shared paths and processes for the Windows + WSL toolchain."""
from __future__ import annotations

import contextlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
CONFIG = BUILD / "tools.json"


def environment():
    env = os.environ.copy()
    if CONFIG.exists():
        paths = json.loads(CONFIG.read_text(encoding="utf-8-sig"))["path"]
        env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    return env


def bash():
    """Use Windows Bash, never Windows' legacy WSL bash.exe launcher."""
    candidates = [os.environ.get("DOOMV_BASH", "")]
    if CONFIG.exists():
        candidates.append(json.loads(CONFIG.read_text(encoding="utf-8-sig"))["bash"])
    candidates += [r"C:\Program Files\Git\bin\bash.exe", shutil.which("bash") or ""]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and "system32" not in candidate.lower():
            return candidate
    raise RuntimeError("Windows Bash is missing. Run scripts/install_dependencies.ps1.")


def run(command, *, cwd=ROOT):
    print("+ " + subprocess.list2cmdline([str(c) for c in command]), flush=True)
    subprocess.run([str(c) for c in command], cwd=cwd, env=environment(), check=True)


def wsl_path(path):
    path = Path(path).resolve()
    if os.name != "nt":
        return str(path)
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]


def wsl_script(name, *args):
    run(["wsl.exe", "-d", os.environ.get("DISTRO", "Ubuntu"), "-u", "root", "--",
         "bash", wsl_path(ROOT / "scripts" / name), wsl_path(ROOT), *args])


def require_files(*paths):
    missing = [str(p) for p in paths if not Path(p).is_file() or Path(p).stat().st_size == 0]
    if missing:
        raise RuntimeError("Missing build inputs:\n  " + "\n  ".join(missing))


@contextlib.contextmanager
def checkout_lock():
    """Never stop another run to acquire the checkout."""
    lock = ROOT / ".scripts.lock"
    try:
        lock.mkdir()
    except FileExistsError:
        raise RuntimeError(f"Another script owns {lock}. Wait for it to finish. "
                           "After a crashed run, remove this empty directory manually.") from None
    try:
        if (ROOT / ".signature.lock").exists():
            raise RuntimeError("A legacy verification run is using this checkout. Wait or use another checkout.")
        yield
    finally:
        lock.rmdir()


def build_emulator():
    ensure_host_tools()
    require_files(ROOT / "tools/verification/simulators/spike/src/softfloat/f64_add.c")
    # A locked output or compiler/linker error stops verification immediately.
    run([bash(), str(ROOT / "scripts/build_host.sh")])
    require_files(ROOT / "riscv_doom.exe")


def ensure_host_tools():
    """Install native prerequisites once, then verify the commands are usable."""
    required = ("python", "make", "gcc", "g++")
    path = environment().get("PATH")
    missing = [name for name in required if shutil.which(name, path=path) is None]
    if missing:
        installer = ROOT / "scripts/install_dependencies.ps1"
        print(f"Missing native tools ({', '.join(missing)}); installing prerequisites...", flush=True)
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(installer)],
                       cwd=ROOT, check=True)
    path = environment().get("PATH")
    missing = [name for name in required if shutil.which(name, path=path) is None]
    if missing:
        raise RuntimeError("Native prerequisites still missing: " + ", ".join(missing))


def entrypoint(main):
    try:
        return main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted.", file=sys.stderr)
        return 130
