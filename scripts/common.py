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


def split_passthrough(argv):
    """Split argv at `--`: what follows goes to riscv_doom.exe unchanged."""
    if "--" in argv:
        i = argv.index("--")
        return argv[:i], argv[i + 1:]
    return argv, []


def add_boot_options(parser, linux=True):
    """The machine options every boot script takes, defined once so they read
    the same. `linux` adds what only a Linux guest has: several harts, and (in
    add_advanced_options) storage drives and the shared folder."""
    g = parser.add_argument_group("machine")
    g.add_argument("--ram", metavar="SIZE",
                   help="guest memory, e.g. 4G (default 1G)" +
                        ("" if linux else "; DOOM is capped near 2G"))
    if linux:
        g.add_argument("--harts", type=int, default=1, metavar="N",
                       help="CPUs, 1 to 4095 (default 1); the device tree is made to match")
        g.add_argument("--net", action="store_true",
                       help="a network card; the guest reaches the internet through the host")
        g.add_argument("--gpu", nargs="?", const="all", choices=("all", "2d", "virgl"), metavar="2d",
                       help="a virtio-gpu, with OpenGL and Vulkan on this computer's GPU; the guest picks what "
                            "each program uses. --gpu 2d: the display alone")
        g.add_argument("--no-sound", dest="sound", action="store_false",
                       help="no sound card (by default the guest plays and records through this computer's)")
    g.add_argument("--headless", action="store_true", help="no window; output goes to this console")
    g.add_argument("--no-build", action="store_true", help="skip the build; use what is already built")


def add_advanced_options(parser, linux=True):
    """The rarer emulator options, as a group of their own at the end of --help."""
    a = parser.add_argument_group("advanced", "Anything after `--` is passed to riscv_doom.exe as it is.")
    a.add_argument("--snapshot", metavar="DIR", help="save the machine to DIR at --snapshot-at, and carry on")
    a.add_argument("--snapshot-at", type=int, metavar="STEP", help="the instruction to snapshot at")
    a.add_argument("--restore", metavar="DIR", help="start from a snapshot (same options as when it was made)")
    a.add_argument("--stop-at", type=int, metavar="STEP", help="stop after STEP instructions; state in crash.log")
    a.add_argument("--record", metavar="FILE", help="log every input with the instruction it arrived at")
    a.add_argument("--replay", metavar="FILE", help="replay a --record log exactly")
    a.add_argument("--march", metavar="ISA", help="override the extensions, e.g. rv64imafdc_zicsr")
    if linux:
        a.add_argument("--drives", metavar="DIR", help="storage drives folder (default drives/; '' for none)")
        a.add_argument("--shared", metavar="DIR", help="shared folder (default shared/; '' for none)")
        a.add_argument("--clock", default="host", metavar="WHEN",
                       help="where the guest's clock starts: host (the default, this computer's time), "
                            "fixed (the same date every run), or seconds since 1970")


def boot_args(args, passthrough=()):
    """The emulator flags for add_boot_options' options, in a fixed order."""
    if (args.snapshot is None) != (args.snapshot_at is None):
        raise RuntimeError("--snapshot and --snapshot-at go together")
    harts = getattr(args, "harts", 1)
    if not 1 <= harts <= 4095:
        raise RuntimeError("--harts must be 1 to 4095")
    out = []
    if args.ram:
        out.append(f"-ram={args.ram}")
    if harts > 1:
        out.append(f"-harts={harts}")
    if getattr(args, "net", False):
        out.append("-net")
    if getattr(args, "sound", False):
        out.append("-snd")
    gpu = getattr(args, "gpu", None)
    if gpu == "all":
        # Built, not fetched: the first time it is wanted, about two minutes.
        if not (ROOT / "build/venus/bin/libvirglrenderer-1.dll").exists():
            run([sys.executable, str(ROOT / "scripts/get_venus.py")])
        out.append("-gpu")
    elif gpu == "virgl":
        # Loaded at run time from build/virgl; fetched the first time it is wanted.
        if not (ROOT / "build/virgl/bin/libvirglrenderer-1.dll").exists():
            run([sys.executable, str(ROOT / "scripts/get_virgl.py")])
        out.append("-gpu=virgl")
    elif gpu:
        out.append("-gpu=2d")
    clock = getattr(args, "clock", "fixed")
    if clock != "fixed":
        if clock != "host" and not clock.isdigit():
            raise RuntimeError("--clock takes host, fixed, or a number of seconds since 1970")
        out.append(f"-rtc={clock}")
    if args.headless:
        out.append("-ng")
    if args.march:
        out.append(f"-march={args.march}")
    if args.snapshot:
        out += [f"-snapshotat={args.snapshot_at}", f"-snapshot={args.snapshot}"]
    if args.restore:
        out.append(f"-restore={args.restore}")
    if args.stop_at:
        out.append(f"-stopat={args.stop_at}")
    if args.record:
        out.append(f"-record={args.record}")
    if args.replay:
        out.append(f"-replay={args.replay}")
    for name in ("drives", "shared"):
        value = getattr(args, name, None)
        if value is not None:
            out.append(f"-{name}={value}")
    return out + list(passthrough)


def hart_dtb(dtb: Path, harts: int) -> Path:
    """The device tree for `harts` harts: dtb itself for one, else made from its source.

    tools/linux/dts/smp.py repeats cpu0 and widens the CLINT and IMSIC nodes;
    the result is kept beside the source and made again when the source is newer.
    """
    if harts <= 1:
        return dtb
    source = dtb.with_suffix(".dts")
    out = dtb.with_name(f"{dtb.stem}-h{harts}.dtb")
    require_files(source)
    if not out.is_file() or out.stat().st_mtime < source.stat().st_mtime:
        run([sys.executable, str(ROOT / "tools/linux/dts/smp.py"), str(source), str(harts)])
    if harts > 64:
        print(f"Note: {harts} harts boot slowly. Until OpenSBI is done, hart 0 gets one step in "
              f"{harts} while its setup grows with the cube of the count: 64 harts take about "
              "40 minutes, 128 about two and a half hours.", flush=True)
    return out


def ensure_host_tools():
    """Install native prerequisites once, then verify the commands are usable."""
    # A compiler is required, but not a particular one: the Makefile prefers
    # Clang and falls back to GCC, so only demand GCC when there is no Clang
    # for it to have found. install_dependencies.ps1 installs GCC either way.
    required = ["python", "make"]
    if not (list(ROOT.glob("build/toolchains/llvm-mingw-*/bin/clang++.exe"))
            or shutil.which("clang++")):
        required += ["gcc", "g++"]
    required = tuple(required)
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
