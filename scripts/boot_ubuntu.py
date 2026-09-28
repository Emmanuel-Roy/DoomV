#!/usr/bin/env python3
"""Boot the Ubuntu 24.04 image with systemd as PID 1.

Unlike `boot.py doom` and `boot.py linux`, this one cannot build its guest.
The other two produce their userland in minutes from a cross-compiler; this
image is built in two stages, the second of which is DoomV running Ubuntu's
own dpkg over about four hours. So the image is an input here, and if it is
missing this says how to make one rather than starting a build nobody asked
for. See tools/linux/ubuntu/README.md.

  python scripts/boot.py ubuntu                 # build the kernel, then boot
  python scripts/boot.py ubuntu --no-build      # boot what is already there
  python scripts/boot.py ubuntu --headless      # no window; log to the console
  python scripts/boot.py ubuntu --login         # prove the login works, headless

  python scripts/boot.py ubuntu --install-desktops   # once: DoomV installs them (hours)
  python scripts/boot.py ubuntu --desktop openbox    # Xorg + Openbox + xterm
  python scripts/boot.py ubuntu --desktop xfce       # the XFCE desktop
  python scripts/boot.py ubuntu --desktop x          # bare X, xterm windows only
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import time

from common import ROOT, BUILD, add_ram_option, build_emulator, checkout_lock, entrypoint, environment, ram_args, require_files, run, wsl_path, wsl_script

DESKTOPS = ("openbox", "xfce", "x")

# Printed by the serial getty once userspace is up. The gate for --login has
# to be a string that survives systemd's own formatting: unit names are
# colourised, so "Started getty@tty1.service" is really
# "Started \x1b[0;1;39mgetty@tty1.service" and a needle spanning that space
# never matches. A login prompt is contiguous and unstyled.
LOGIN_PROMPT = "doomv login:"

# What --login types once that prompt appears.
#
# The commands redirect to /dev/hvc0, which is the serial console, because
# getty is on tty1 and anything it prints goes to the framebuffer where this
# script cannot read it. Sending the result back down the serial line is what
# makes the check an assertion rather than a guess -- see LOGIN_MARKER.
#
# Waits are generous because they have to be: authenticating means crypt(),
# which is not quick at emulated speed, and anything typed during it is echoed by
# the tty before login has finished. A script's `sleep` counts instructions
# (10,000 per "millisecond"), not host time, so these waits are the same
# amount of guest work on every run however fast the host is.
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

# Only a shell can produce this, and only a successful login produces a
# shell. Watching for "the script finished typing" instead would pass just as
# happily on a rejected password, which is the one outcome worth catching.
LOGIN_MARKER = "DOOMV-LOGIN-OK"


# What a window boot types at the login prompt: the first half of
# LOGIN_SCRIPT, with the same instruction-counted waits, and nothing after the
# password -- the shell is the person's from there.
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
    """Emulator arguments that log in as root once the login prompt is up.

    Done from the host rather than by configuring getty's --autologin in the
    image: the image is built over hours and is not something a boot script
    should edit, and a headless boot or a --desktop session -- which starts
    as root on its own -- is left exactly as it was. The typing is the
    emulated keyboard's, on tty1, and waits for the serial getty's prompt.
    """
    script = BUILD / "logs/ubuntu-autologin.script"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(AUTOLOGIN_SCRIPT, newline="\n")
    return (f"-expect={LOGIN_PROMPT}", f"-input={script}")


def ubuntu_command(image: Path, headless=False, extra=(), dtb="ubuntu.dtb", ram=None):
    images = BUILD / "linux"
    paths = [images / "fw_jump.elf", images / "Image", images / dtb]
    require_files(ROOT / "riscv_doom.exe", *paths, image)
    command = [str(ROOT / "riscv_doom.exe")] + ram_args(ram)
    if headless:
        command.append("-ng")
    command += [f"-{name}={path}" for name, path in zip(("opensbi", "kernel", "dtb"), paths)]
    # No -initrd, deliberately: with an initramfs present the kernel runs
    # that and never mounts the disk, which looks like a successful boot of
    # nothing. The device tree drops the properties for the same reason.
    command.append(f"-disk={image}")
    return command + list(extra)


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


def login_test(image: Path, timeout: float, ram=None):
    """Boot headless, log in through the emulated keyboard, run a command.

    This is the end-to-end check that the input devices reach a real
    distribution -- not just that the machine boots. It drives the virtio
    keyboard, so it needs no window and no person.
    """
    log = BUILD / "logs/ubuntu-login.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    script = BUILD / "logs/ubuntu-login.script"
    script.write_text(LOGIN_SCRIPT, newline="\n")
    dump = BUILD / "logs/ubuntu-login.ppm"

    command = ubuntu_command(image, headless=True, ram=ram, extra=(
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
                # The shell is on tty1 and prints to the framebuffer, so the
                # typed commands redirect to /dev/hvc0 to put their output
                # where this can see it.
                if LOGIN_MARKER in text:
                    release = next((line for line in text.splitlines()
                                    if line.startswith("Linux ") and "riscv64" in line), None)
                    print(f"PASS: logged in through the emulated keyboard and ran a command."
                          + (f" Guest reports: {release.strip()}" if release else ""))
                    print(f"      Log: {log}")
                    print(f"      Framebuffer dump: {dump}")
                    return
                if "input script finished" in text and LOGIN_MARKER not in text:
                    raise RuntimeError(
                        "The script typed everything and no shell answered -- the login did "
                        f"not succeed. Check the password in the image; see {log}")
                if proc.poll() is not None:
                    raise RuntimeError(f"Emulator exited before the login completed; see {log}")
                time.sleep(1.0)
            what = "log in" if reached_prompt else f"reach {LOGIN_PROMPT!r}"
            raise RuntimeError(f"Ubuntu did not {what} within {timeout:g}s; see {log}")
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def emulator_running():
    """Whether any DoomV is running. Two emulators on one disk image corrupt it."""
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq riscv_doom.exe", "/NH"],
                             capture_output=True, text=True, check=False).stdout
    except OSError:
        return False
    return "riscv_doom.exe" in out.lower()


def install_desktops(image: Path, timeout_hours: float, ram=None):
    """Put Openbox, XFCE and bare X into the image, with DoomV doing the install.

    Three steps. The image is copied aside first, because this changes it and
    takes hours. Then mkdesktop.sh, on the host, downloads the packages into
    the image as a local apt repository and writes the X config, sessions and
    the install script -- running nothing riscv64. Then DoomV boots the image
    with that script as init, and the guest's own apt and dpkg install
    everything, exactly as stage 2 configured the base system.
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
    command = ubuntu_command(image, headless=True, dtb="ubuntu-install.dtb", ram=ram)
    print(f"DoomV is installing the desktops. This takes hours; the log is {log}", flush=True)
    started = time.monotonic()
    with log.open("wb") as output:
        proc = subprocess.Popen(command, cwd=ROOT, env=environment(),
                                stdout=output, stderr=subprocess.STDOUT)
        try:
            position, set_up, last_report, result = 0, 0, 0.0, None
            deadline = started + timeout_hours * 3600
            while time.monotonic() < deadline:
                # Read only what is new: the log runs to megabytes over hours.
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
            # The marker is printed after the guest's sync. Give it a couple
            # of minutes to power itself off rather than being killed.
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
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", type=Path, default=ROOT / "ubuntu.img",
                        help="the configured Ubuntu disk image (default: ubuntu.img)")
    parser.add_argument("--no-build", action="store_true",
                        help="use the emulator and images already built")
    parser.add_argument("--headless", action="store_true",
                        help="no window; the kernel log goes to this console")
    parser.add_argument("--login", action="store_true",
                        help="headless: log in through the emulated keyboard and exit")
    parser.add_argument("--no-autologin", action="store_true",
                        help="in the window, stop at the login prompt instead of logging in as root")
    parser.add_argument("--timeout", type=float, default=1800,
                        help="--login timeout in seconds (a systemd boot here is minutes)")
    parser.add_argument("--desktop", choices=DESKTOPS,
                        help="boot into an X desktop: openbox, xfce, or bare x")
    parser.add_argument("--install-desktops", action="store_true",
                        help="install all three desktops into the image (DoomV does it; hours)")
    parser.add_argument("--install-timeout", type=float, default=16,
                        help="--install-desktops timeout in hours")
    add_ram_option(parser)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    image = args.image.resolve()
    if sum(bool(x) for x in (args.login, args.desktop, args.install_desktops)) > 1:
        parser.error("--login, --desktop and --install-desktops are separate runs; pick one")
    # Checked before building: a kernel build takes minutes and there is no
    # point spending them to then discover there is nothing to boot.
    require_image(image)
    with checkout_lock():
        if not args.no_build:
            build_emulator()
            wsl_script("build_linux.sh")
        if args.install_desktops:
            install_desktops(image, args.install_timeout, args.ram)
        elif args.login:
            login_test(image, args.timeout, args.ram)
        elif args.desktop:
            if emulator_running():
                raise RuntimeError("A DoomV is already running on an image; close it first.")
            print(f"Ubuntu boots into the {args.desktop} desktop in the emulator window.")
            print("systemd, then X, then the session all start at emulated speed: X draws")
            print("the screen after ~15 min and the desktop is usable after ~25-30 min.")
            print("Ctrl+Alt+G grabs the mouse.")
            run(ubuntu_command(image, headless=args.headless, dtb=f"ubuntu-{args.desktop}.dtb", ram=args.ram))
        else:
            extra = ()
            if not args.headless:
                print("Ubuntu opens in the emulator window. Ctrl+Alt+F makes it full-window,")
                print("Ctrl+Alt+G grabs the mouse. Close the window to exit.")
                if args.no_autologin:
                    print("Log in as root / doomv at the framebuffer console.")
                else:
                    print("It logs in as root by itself once the login prompt appears.")
                    extra = autologin_args()
            print("A systemd boot takes minutes at this speed -- see the README's note on MIPS.")
            run(ubuntu_command(image, headless=args.headless, extra=extra, ram=args.ram))
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
