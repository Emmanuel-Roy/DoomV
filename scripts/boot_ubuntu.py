#!/usr/bin/env python3
"""Boot Ubuntu 24.04 from ubuntu.img.

  python scripts/boot.py ubuntu                       # window; logs in as root
  python scripts/boot.py ubuntu --desktop xfce        # boot into a desktop
  python scripts/boot.py ubuntu --desktop-snapshot    # the booted XFCE desktop, in seconds
  python scripts/boot.py ubuntu --net                 # with a network (once: --setup-network)
  python scripts/boot.py ubuntu --setup-browser       # once: a browser, and the network for it
  python scripts/boot.py ubuntu --setup-sound         # once: aplay, arecord, speaker-test
  python scripts/boot.py ubuntu --login               # test: log in, run a command, exit

The image is an input: it takes hours to build, so this script never builds
it. See tools/linux/ubuntu/README.md.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import time

from common import (ROOT, BUILD, add_advanced_options, add_boot_options, boot_args, build_emulator, checkout_lock, entrypoint,
                    environment, hart_dtb, require_files, run, split_passthrough, wsl_path, wsl_script)

DESKTOPS = ("openbox", "xfce", "x")
DESKTOP_SNAPSHOT = BUILD / "desktop" / "xfce-100G"

# Printed by the serial getty once userspace is up. Unlike systemd's unit
# names, which are colourised, a login prompt is contiguous and unstyled.
LOGIN_PROMPT = "doomv login:"

# What --login types once that prompt appears. The commands send their output
# to /dev/hvc0, the serial console this script reads; the shell itself is on
# tty1. Waits count instructions (10,000 per "millisecond"), not host time, so
# they are the same guest work on every host.
LOGIN_SCRIPT = """\
sleep 20000
type root
key 28 1
key 28 0
sleep 30000
type doomv
key 28 1
key 28 0
sleep 75000
type uname -srm > /dev/hvc0
key 28 1
key 28 0
sleep 30000
type echo DOOMV-LOGIN-OK > /dev/hvc0
key 28 1
key 28 0
sleep 45000
"""

# Only a shell can print this, and only a successful login gives a shell.
LOGIN_MARKER = "DOOMV-LOGIN-OK"

# What a window boot types at the login prompt: user and password, then the
# shell is yours.
AUTOLOGIN_SCRIPT = """\
sleep 20000
type root
key 28 1
key 28 0
sleep 30000
type doomv
key 28 1
key 28 0
"""


def autologin_args():
    """Log in as root through the emulated keyboard once the prompt is up.

    Typed from the host rather than configured in the image, which is not
    something a boot script should edit.
    """
    script = BUILD / "logs/ubuntu-autologin.script"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(AUTOLOGIN_SCRIPT, newline="\n")
    return [f"-expect={LOGIN_PROMPT}", f"-input={script}"]


def ubuntu_command(image: Path, emulator_args=(), extra=(), dtb="ubuntu.dtb", harts=1):
    """The emulator command line for a disk boot of `image`.

    No -initrd: with an initramfs the kernel runs that and never mounts the disk.
    """
    images = BUILD / "linux"
    paths = [images / "fw_jump.elf", images / "Image", hart_dtb(images / dtb, harts)]
    require_files(ROOT / "riscv_doom.exe", *paths, image)
    return ([str(ROOT / "riscv_doom.exe"), *emulator_args]
            + [f"-{name}={path}" for name, path in zip(("opensbi", "kernel", "dtb"), paths)]
            + [f"-disk={image}", *extra])


def headless(emulator_args):
    return list(emulator_args) if "-ng" in emulator_args else ["-ng", *emulator_args]


def require_image(image: Path):
    if image.is_file() and image.stat().st_size > 0:
        return
    raise RuntimeError(
        f"No Ubuntu image at {image}.\n"
        "  Build one (stage 1 unpacks on the host, stage 2 runs on DoomV):\n"
        "    git submodule update --init tools/linux/ubuntu/src\n"
        "    wsl -d Ubuntu -u root -- bash tools/linux/ubuntu/mkrootfs.sh\n"
        "    tools/linux/ubuntu/boot-stage2.sh\n"
        "  Stage 2 takes hours. See tools/linux/ubuntu/README.md.")


def stop(proc, wait=5):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def login_test(image: Path, timeout: float, emulator_args=(), harts=1):
    """Boot headless, log in through the emulated keyboard, run a command."""
    log = BUILD / "logs/ubuntu-login.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    script = BUILD / "logs/ubuntu-login.script"
    script.write_text(LOGIN_SCRIPT, newline="\n")
    dump = BUILD / "logs/ubuntu-login.ppm"
    command = ubuntu_command(image, headless(emulator_args), harts=harts, extra=(
        f"-expect={LOGIN_PROMPT}", f"-input={script}", f"-fbdump={dump}"))
    with log.open("wb") as output:
        proc = subprocess.Popen(command, cwd=ROOT, env=environment(),
                                stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + timeout
            reached_prompt = False
            while time.monotonic() < deadline:
                text = log.read_text(errors="replace")
                if "Kernel panic" in text:
                    raise RuntimeError(f"Ubuntu panicked; see {log}")
                if not reached_prompt and LOGIN_PROMPT in text:
                    reached_prompt = True
                    print(f"reached {LOGIN_PROMPT!r}; logging in...", flush=True)
                if LOGIN_MARKER in text:
                    release = next((line for line in text.splitlines()
                                    if line.startswith("Linux ") and "riscv64" in line), None)
                    print("PASS: logged in through the emulated keyboard and ran a command."
                          + (f" Guest reports: {release.strip()}" if release else ""))
                    print(f"      Log: {log}")
                    print(f"      Framebuffer dump: {dump}")
                    return
                if "input script finished" in text and LOGIN_MARKER not in text:
                    raise RuntimeError("The script typed everything and no shell answered -- the "
                                       f"login did not succeed. Check the password in the image; see {log}")
                if proc.poll() is not None:
                    raise RuntimeError(f"Emulator exited before the login completed; see {log}")
                time.sleep(1.0)
            what = "log in" if reached_prompt else f"reach {LOGIN_PROMPT!r}"
            raise RuntimeError(f"Ubuntu did not {what} within {timeout:g}s; see {log}")
        finally:
            stop(proc)


def emulator_running():
    """Whether any DoomV is running. Two emulators on one disk image corrupt it."""
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq riscv_doom.exe", "/NH"],
                             capture_output=True, text=True, check=False).stdout
    except OSError:
        return False
    return "riscv_doom.exe" in out.lower()


def install_desktops(image: Path, timeout_hours: float, emulator_args=()):
    """Install Openbox, XFCE and bare X into the image, with DoomV running apt.

    The image is backed up first. mkdesktop.sh, on the host, puts the packages
    in the image as a local repository; then DoomV boots the image with the
    install script as init, and the guest's own apt and dpkg install them.
    """
    if emulator_running():
        raise RuntimeError("A DoomV is already running. Close it first: two emulators "
                           "writing ubuntu.img at once would corrupt it.")
    backup = image.with_name(image.stem + ".pre-desktop.img")
    if backup.exists():
        print(f"backup already exists, keeping it: {backup}")
    else:
        print(f"backing up the image to {backup} ...", flush=True)
        shutil.copyfile(image, backup)

    run(["wsl.exe", "-d", "Ubuntu", "-u", "root", "--", "bash",
         wsl_path(ROOT / "tools/linux/ubuntu/mkdesktop.sh"), wsl_path(image)])

    log = BUILD / "logs/ubuntu-desktop-install.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    command = ubuntu_command(image, headless(emulator_args), dtb="ubuntu-install.dtb")
    print(f"DoomV is installing the desktops. This takes hours; the log is {log}", flush=True)
    started = time.monotonic()
    with log.open("wb") as output:
        proc = subprocess.Popen(command, cwd=ROOT, env=environment(),
                                stdout=output, stderr=subprocess.STDOUT)
        try:
            position, set_up, last_report, result = 0, 0, 0.0, None
            deadline = started + timeout_hours * 3600
            while time.monotonic() < deadline:
                # Only what is new: the log runs to megabytes over hours.
                with log.open("rb") as f:
                    f.seek(position)
                    chunk = f.read().decode("utf-8", errors="replace")
                    position = f.tell()
                set_up += chunk.count("Setting up ")
                if "Kernel panic" in chunk:
                    raise RuntimeError(f"the guest panicked; see {log}")
                if "DOOMV-DESKTOP-FAILED" in chunk:
                    result = False
                if "DOOMV-DESKTOP-OK" in chunk:
                    result = True
                if result is not None:
                    break
                now = time.monotonic()
                if now - last_report > 300:
                    print(f"  {(now - started) / 3600:4.1f} h: {set_up} packages set up so far", flush=True)
                    last_report = now
                if proc.poll() is not None:
                    raise RuntimeError(f"the emulator exited before the install finished; see {log}")
                time.sleep(2)
            if result is None:
                raise RuntimeError(f"the install did not finish within {timeout_hours:g} h; see {log}")
            # The marker comes after the guest's sync: let it power off.
            for _ in range(120):
                if proc.poll() is not None:
                    break
                time.sleep(1)
            if not result:
                raise RuntimeError("apt in the guest failed; the failing package is in "
                                   f"{log}. The image still has the local repository, so "
                                   "--install-desktops can be run again.")
            hours = (time.monotonic() - started) / 3600
            print(f"PASS: desktops installed in {hours:.1f} h ({set_up} packages set up).")
            print("Boot one with: python scripts/boot.py ubuntu --desktop openbox|xfce|x")
        finally:
            stop(proc, 10)


# What --setup-browser and --setup-sound install. The browser set: a clock
# that keeps itself right over the network (systemd-timesyncd), the CA
# certificates, curl, w3m, and NetSurf, a browser light enough to use on an
# emulated CPU. The sound set: ALSA's aplay, arecord, speaker-test and mixer.
BROWSER_PACKAGES = "systemd-timesyncd ca-certificates curl w3m netsurf-gtk"
SOUND_PACKAGES = "alsa-utils"


def install_script(packages: str) -> str:
    """What the install boot types once logged in: wait for DHCP and DNS,
    install, report on the serial console, power off. One line, because the
    input script types it into the shell as it is."""
    return f"""\
sleep 20000
type root
key 28 1
key 28 0
sleep 30000
type doomv
key 28 1
key 28 0
sleep 75000
type (for i in $(seq 90); do getent hosts ports.ubuntu.com >/dev/null && break; sleep 2; done; export DEBIAN_FRONTEND=noninteractive; apt-get update -q && apt-get install -y -q {packages}) > /dev/hvc0 2>&1 && echo DOOMV-INSTALL-OK > /dev/hvc0 || echo DOOMV-INSTALL-FAILED > /dev/hvc0; sync; poweroff
key 28 1
key 28 0
"""


def install_packages(image: Path, packages: str, timeout: float, emulator_args=()):
    """Install Ubuntu packages into the image, with DoomV running apt.

    On the host, the network setup (DHCP, DNS, apt sources with universe);
    then a boot with the network card and the host's clock -- apt and HTTPS
    both check dates -- that logs in, installs, and powers off.
    """
    if emulator_running():
        raise RuntimeError("A DoomV is already running; close it before changing the image.")
    run(["wsl.exe", "-d", "Ubuntu", "-u", "root", "--", "bash",
         wsl_path(ROOT / "tools/linux/ubuntu/mknetwork.sh"), wsl_path(image)])
    log = BUILD / "logs/ubuntu-install.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    script = BUILD / "logs/ubuntu-install.script"
    script.write_text(install_script(packages), newline="\n")
    args = headless(emulator_args)
    if "-net" not in args:
        args.append("-net")
    if not any(a.startswith("-rtc=") for a in args):
        args.append("-rtc=host")
    command = ubuntu_command(image, args, extra=(f"-expect={LOGIN_PROMPT}", f"-input={script}"))
    print(f"DoomV is installing {packages}. About 10 minutes; the log is {log}", flush=True)
    started = time.monotonic()
    with log.open("wb") as output:
        proc = subprocess.Popen(command, cwd=ROOT, env=environment(),
                                stdout=output, stderr=subprocess.STDOUT)
        try:
            result = None
            while time.monotonic() - started < timeout and result is None:
                text = log.read_text(errors="replace")
                if "Kernel panic" in text:
                    raise RuntimeError(f"the guest panicked; see {log}")
                if "DOOMV-INSTALL-FAILED" in text:
                    result = False
                elif "DOOMV-INSTALL-OK" in text:
                    result = True
                elif proc.poll() is not None:
                    raise RuntimeError(f"the emulator exited before the install finished; see {log}")
                else:
                    time.sleep(2)
            if result is None:
                raise RuntimeError(f"the install did not finish within {timeout:g}s; see {log}")
            # The marker comes before the guest's sync and poweroff.
            for _ in range(120):
                if proc.poll() is not None:
                    break
                time.sleep(1)
            if not result:
                raise RuntimeError(f"apt in the guest failed; see {log}")
            print(f"PASS: installed in {(time.monotonic() - started) / 60:.0f} minutes.")
        finally:
            stop(proc, 10)


def desktop_snapshot_command(image: Path, snapshot: Path, args, passthrough):
    """The booted XFCE desktop, restored: the exact machine the snapshot was made on.

    performance/make_desktop_snapshot.py makes it with default RAM, one hart,
    the XFCE device tree, no storage drives and no shared folder, and a
    restore checks all of that, so none of it can be changed here.
    """
    if not (snapshot / "state.bin").is_file():
        raise RuntimeError(f"No desktop snapshot at {snapshot}. Make it once (about 25 minutes):\n"
                           "    python performance/make_desktop_snapshot.py")
    changed = [name for name in ("ram", "march", "restore", "drives", "shared")
               if getattr(args, name) not in (None, "")] + (["harts"] if args.harts != 1 else []) \
              + (["net"] if args.net else []) + (["gpu"] if args.gpu else [])
    if changed:
        raise RuntimeError("--desktop-snapshot restores the machine the snapshot was made on; "
                           "it cannot take --" + ", --".join(changed))
    args.drives, args.shared = "", ""
    args.sound = False   # the snapshot predates the sound card
    return ubuntu_command(image, boot_args(args, passthrough), dtb="ubuntu-xfce.dtb",
                          extra=[f"-restore={snapshot}"])


def main():
    argv, passthrough = split_passthrough(sys.argv[1:])
    parser = argparse.ArgumentParser(prog="boot.py ubuntu", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    u = parser.add_argument_group("ubuntu")
    u.add_argument("--image", type=Path, default=ROOT / "ubuntu.img", help="the disk image (default ubuntu.img)")
    u.add_argument("--desktop", choices=DESKTOPS, help="boot into a desktop: xfce, openbox, or bare x")
    u.add_argument("--desktop-snapshot", nargs="?", const=DESKTOP_SNAPSHOT, type=Path, metavar="DIR",
                   help="restore the booted XFCE desktop (default build/desktop/xfce-100G)")
    u.add_argument("--no-autologin", action="store_true", help="stop at the login prompt (root / doomv)")
    u.add_argument("--install-desktops", action="store_true",
                   help="install all three desktops into the image, once (hours)")
    u.add_argument("--setup-network", action="store_true",
                   help="set the image up for --net (DHCP, DNS), once; images made since need not")
    u.add_argument("--setup-browser", action="store_true",
                   help="install a browser (NetSurf, and w3m for the console), once; includes --setup-network")
    u.add_argument("--setup-sound", action="store_true",
                   help="install ALSA's tools (aplay, arecord, speaker-test), once; includes --setup-network")
    add_boot_options(parser)
    t = parser.add_argument_group("test")
    t.add_argument("--login", action="store_true", help="headless: log in, run a command, exit")
    t.add_argument("--timeout", type=float, default=1800, metavar="SECONDS",
                   help="--login, --setup-browser and --setup-sound timeout (default 1800)")
    t.add_argument("--install-timeout", type=float, default=16, metavar="HOURS", help="--install-desktops timeout (default 16)")
    add_advanced_options(parser)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if sum(bool(x) for x in (args.login, args.desktop, args.desktop_snapshot, args.install_desktops)) > 1:
        parser.error("--login, --desktop, --desktop-snapshot and --install-desktops are separate runs; pick one")
    image = args.image.resolve()
    require_image(image)   # before a build that would be wasted without one
    with checkout_lock():
        if not args.no_build:
            build_emulator()
            wsl_script("build_linux.sh")
        if args.install_desktops:
            install_desktops(image, args.install_timeout, boot_args(args, passthrough))
            return 0
        if args.setup_browser or args.setup_sound:
            packages = " ".join(p for p, on in ((BROWSER_PACKAGES, args.setup_browser),
                                                 (SOUND_PACKAGES, args.setup_sound)) if on)
            install_packages(image, packages, args.timeout, boot_args(args, passthrough))
            if args.setup_browser:
                print("Browse with: python scripts/boot.py ubuntu --net --desktop openbox, then in its "
                      "terminal: netsurf https://en.wikipedia.org &   (or w3m <url>, in any shell)")
            if args.setup_sound:
                print("Test with: speaker-test -c 2 -t wav -l 1   (play)   "
                      "arecord -d 5 a.wav && aplay a.wav   (record, then play back)")
            return 0
        if args.setup_network:
            if emulator_running():
                raise RuntimeError("A DoomV is already running; close it before changing the image.")
            run(["wsl.exe", "-d", "Ubuntu", "-u", "root", "--", "bash",
                 wsl_path(ROOT / "tools/linux/ubuntu/mknetwork.sh"), wsl_path(image)])
            return 0
        if args.login:
            login_test(image, args.timeout, boot_args(args, passthrough), args.harts)
            return 0
        if emulator_running() and not args.headless:
            raise RuntimeError("A DoomV is already running; two on one disk image corrupt it. Close it first.")
        if args.desktop_snapshot:
            command = desktop_snapshot_command(image, args.desktop_snapshot.resolve(), args, passthrough)
            print("Restoring the XFCE desktop: about 15 s to copy the disk, then it is yours.")
            run(command)
        elif args.desktop:
            print(f"Booting into {args.desktop}: X appears after ~15 min, the desktop is usable "
                  "after ~25-30 min. Ctrl+Alt+G grabs the mouse.")
            run(ubuntu_command(image, boot_args(args, passthrough), dtb=f"ubuntu-{args.desktop}.dtb",
                               harts=args.harts))
        else:
            extra = []
            if not args.headless:
                print("Ubuntu opens in the window; systemd takes a few minutes. Ctrl+Alt+F for full")
                print("window, Ctrl+Alt+G to grab the mouse." + (
                    " Log in as root / doomv." if args.no_autologin else " It logs in as root by itself."))
                if not args.no_autologin:
                    extra = autologin_args()
            run(ubuntu_command(image, boot_args(args, passthrough), extra=extra, harts=args.harts))
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
