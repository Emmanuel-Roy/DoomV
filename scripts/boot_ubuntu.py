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
"""
import argparse
from pathlib import Path
import subprocess
import sys
import time

from common import ROOT, BUILD, build_emulator, checkout_lock, entrypoint, environment, require_files, run, wsl_script

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
# which is not quick at ~6.6 MIPS, and anything typed during it is echoed by
# the tty before login has finished.
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


def ubuntu_command(image: Path, headless=False, extra=()):
    images = BUILD / "linux"
    paths = [images / "fw_jump.elf", images / "Image", images / "ubuntu.dtb"]
    require_files(ROOT / "riscv_doom.exe", *paths, image)
    command = [str(ROOT / "riscv_doom.exe")]
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


def login_test(image: Path, timeout: float):
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

    command = ubuntu_command(image, headless=True, extra=(
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
    parser.add_argument("--timeout", type=float, default=1800,
                        help="--login timeout in seconds (a systemd boot here is minutes)")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    image = args.image.resolve()
    # Checked before building: a kernel build takes minutes and there is no
    # point spending them to then discover there is nothing to boot.
    require_image(image)
    with checkout_lock():
        if not args.no_build:
            build_emulator()
            wsl_script("build_linux.sh")
        if args.login:
            login_test(image, args.timeout)
        else:
            if not args.headless:
                print("Ubuntu opens in the emulator window. Log in as root / doomv at the")
                print("framebuffer console; Ctrl+Alt+F makes it full-window, Ctrl+Alt+G grabs")
                print("the mouse. Close the window to exit.")
            print("A systemd boot takes minutes at this speed -- see the README's note on MIPS.")
            run(ubuntu_command(image, headless=args.headless))
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
