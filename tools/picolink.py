#!/usr/bin/env python3
"""The PC side of Pico2Doom: talk to a Pico 2 over its two USB serial ports.

  console   print the Pico's console (perf stats once a second); --csv FILE
            also logs them as a table (see docs/PERFORMANCE.md)
  bench     reboot into the benchmark (DOOM's -timedemo demo1) and print the
            result -- the classic DOOM benchmark, on the Pico
  play      show the Pico's screen in a window (video over USB) and play
            with the PC keyboard -- keys go into the same input buffer as
            the buttons; every key DOOM reads, from either, is printed
  record    reboot into link mode and record the run for DoomV lock-step;
            --video N also opens the window (keyboard works there too)
  flash     put the Pico into BOOTSEL over USB and copy a UF2 onto it

The Pico shows up as two serial ports (USB 2E8A:4D44): interface 0 is the
console (plain text), interface 2 is the link (binary, see port/port_link.h).

Examples:
  python tools/picolink.py console --csv perf.csv
  python tools/picolink.py bench
  python tools/picolink.py play --video 2
  python tools/picolink.py record build/run.p2dl --video 4
  python tools/picolink.py flash build/firmware/pico2doom.uf2
"""
import argparse
import os
import shutil
import struct
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("picolink needs pyserial:  python -m pip install pyserial")

VID, PID = 0x2E8A, 0x4D44
TAG_TICKS, TAG_TICKS_REP, TAG_KEY, TAG_NOKEY, TAG_FRAME, TAG_HELLO, TAG_VIDEO, TAG_PALETTE, TAG_TEXT, TAG_ARGS, TAG_INPUT = range(1, 12)

# PC keyboard (tkinter key names) -> DOOM key codes (doomkeys.h).
DOOM_KEYS = {
    "Up": 0xAD, "Down": 0xAF, "Left": 0xAC, "Right": 0xAE,
    "Control_L": 0xA3, "Control_R": 0xA3,          # fire
    "space": 0xA2,                                # use
    "Shift_L": 0x80 + 0x36, "Shift_R": 0x80 + 0x36,  # run
    "Alt_L": 0x80 + 0x38, "Alt_R": 0x80 + 0x38,      # strafe
    "comma": 0xA0, "period": 0xA1,                # strafe left / right
    "Escape": 27, "Return": 13, "Tab": 9, "BackSpace": 0x7F, "Pause": 0xFF,
    "minus": 0x2D, "equal": 0x3D, "bracketright": ord("]"),
    **{f"F{i}": 0x80 + 0x3A + i for i in range(1, 11)}, "F11": 0x80 + 0x57,
}
KEY_NAMES = {0xAD: "up", 0xAF: "down", 0xAC: "left", 0xAE: "right", 0xA3: "fire", 0xA2: "use",
             0xA0: "strafe-left", 0xA1: "strafe-right", 27: "esc", 13: "enter", 9: "tab",
             0x80 + 0x36: "run", 0x80 + 0x38: "alt", ord("]"): "next-weapon"}


def doom_key(event):
    if event.keysym in DOOM_KEYS:
        return DOOM_KEYS[event.keysym]
    ch = event.char.lower() if event.char else ""
    if len(ch) == 1 and 32 < ord(ch) < 127:
        return ord(ch)                            # letters, digits (weapons 1-7), y/n
    return None


def describe_input(source, pressed, key):
    name = KEY_NAMES.get(key, chr(key) if 32 < key < 127 else f"0x{key:02x}")
    return f"{'Pico button' if source == 0 else 'PC key':11s} {name:12s} {'down' if pressed else 'up'}"


def find_ports(timeout=0.0):
    """(console, link) port names, waiting up to `timeout` seconds."""
    end = time.time() + timeout
    while True:
        console = link = None
        ours = [p for p in list_ports.comports() if p.vid == VID and p.pid == PID]
        # The link is USB interface 2; the console is the other one. (Windows
        # does not always report a location for interface 0.)
        for p in ours:
            loc = (p.location or "") + " " + (p.hwid or "")
            if "x.2" in loc or ":1.2" in loc or "MI_02" in loc:
                link = p.device
        for p in ours:
            if p.device != link:
                console = p.device
        if (console and link) or time.time() >= end:
            return console, link
        time.sleep(0.25)


def open_port(name):
    s = serial.Serial(name, 115200, timeout=0.2)
    s.dtr = True     # TinyUSB reports "connected" once DTR is set
    return s


def need_ports(timeout=5.0):
    console, link = find_ports(timeout)
    if not (console and link):
        sys.exit("picolink: no Pico2Doom board found (USB 2E8A:4D44). Is it plugged in and flashed?")
    return console, link


def reboot_into(cmd, wait=15.0):
    """Send a one-byte command on the link port, wait for the board to return."""
    _, link = need_ports()
    with open_port(link) as s:
        s.write(cmd)
        s.flush()
    time.sleep(1.5)                 # it drops off the bus...
    return need_ports(wait)         # ...and comes back


# ---- console -------------------------------------------------------------

def parse_stats(line):
    """'fps 8.2 | frame 121.7 ms (max 140) | tic 3.1 bsp ... | zone 234/271 KB (peak 236)' -> dict"""
    import re
    if not line.startswith("fps "):
        return None
    out = {}
    for key, val in re.findall(r"([a-z]+) ([0-9.]+)", line):
        out.setdefault(key, float(val))
    m = re.search(r"max (\d+)", line)
    if m:
        out["frame_max"] = float(m.group(1))
    m = re.search(r"zone (\d+)/(\d+) KB \(peak (\d+)\)", line)
    if m:
        out["zone_kb"], out["zone_size_kb"], out["zone_peak_kb"] = map(float, m.groups())
    return out


def cmd_console(args):
    console, _ = need_ports()
    columns = ["time_s", "fps", "frame", "frame_max", "tic", "bsp", "planes", "sprites", "hud", "lcd",
               "zone_kb", "zone_peak_kb"]
    csv = open(args.csv, "w") if args.csv else None
    if csv:
        csv.write(",".join(columns) + "\n")
    t0, rows = time.time(), []
    print(f"picolink: console on {console} (Ctrl+C to stop)")
    try:
        with open_port(console) as s:
            buf = b""
            while args.seconds <= 0 or time.time() - t0 < args.seconds:
                buf += s.read(4096)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("ascii", "replace").strip()
                    if not line:
                        continue
                    print(line)
                    st = parse_stats(line)
                    if st:
                        st["time_s"] = round(time.time() - t0, 2)
                        rows.append(st)
                        if csv:
                            csv.write(",".join(str(st.get(c, "")) for c in columns) + "\n")
                            csv.flush()
    except KeyboardInterrupt:
        pass
    if rows:
        fps = [r["fps"] for r in rows if "fps" in r]
        print(f"\npicolink: {len(rows)} seconds: fps avg {sum(fps) / len(fps):.1f}, "
              f"min {min(fps):.1f}, max {max(fps):.1f}")


# ---- benchmark ------------------------------------------------------------

def cmd_bench(args):
    print("picolink: rebooting the Pico into the benchmark (timedemo DEMO1, E1M5)...")
    console, _ = reboot_into(b"B")
    t0 = time.time()
    with open_port(console) as s:
        buf = b""
        while time.time() - t0 < args.timeout:
            buf += s.read(4096)
            text = buf.decode("ascii", "replace")
            if "timed " in text:
                time.sleep(0.5)
                buf += s.read(4096)
                for line in buf.decode("ascii", "replace").splitlines():
                    if line.startswith(("timed ", "fps ", "Pico2Doom")):
                        print(line.strip())
                print("(DOOM's own measure: gametics are 1/35 s of game time; "
                      "fps = gametics * 35 / realtics)")
                return
    sys.exit("picolink: no benchmark result within %d s" % args.timeout)


# ---- record + video --------------------------------------------------------

class Viewer:
    """A tkinter window showing the Pico's 320x200 frames, 2x. With `send`,
    keys pressed in it go to the Pico ('K' pressed key on the link port)."""

    def __init__(self, send=None):
        import tkinter as tk
        self.tk = tk
        self.root = tk.Tk()
        self.root.title("Pico 2 (USB video) -- keys go to the Pico" if send else "Pico 2 (USB video)")
        self.label = tk.Label(self.root, text="waiting for video...", width=80, height=25)
        self.label.pack()
        self.held = set()
        if send:
            def key(event, pressed):
                k = doom_key(event)
                if k is None:
                    return
                if pressed and k in self.held:
                    return                        # the OS's auto-repeat: DOOM wants one press
                (self.held.add if pressed else self.held.discard)(k)
                send(bytes([ord("K"), 1 if pressed else 0, k]))
            self.root.bind("<KeyPress>", lambda e: key(e, True))
            self.root.bind("<KeyRelease>", lambda e: key(e, False))
        self.palette = bytes(768)
        self.pending = None
        self.lock = threading.Lock()
        self.root.after(30, self.poll)

    def frame(self, pixels):
        pal = self.palette
        rgb = bytearray(320 * 200 * 3)
        for i, p in enumerate(pixels):
            rgb[3 * i:3 * i + 3] = pal[3 * p:3 * p + 3]
        with self.lock:
            self.pending = bytes(rgb)

    def poll(self):
        with self.lock:
            rgb, self.pending = self.pending, None
        if rgb:
            img = self.tk.PhotoImage(data=b"P6 320 200 255\n" + rgb, format="PPM").zoom(2, 2)
            self.label.configure(image=img)
            self.label.image = img
        self.root.after(30, self.poll)


def run_session(link, output=None, video=0, keyboard=False, show_inputs=True, seconds=0):
    """Read the link stream: save it to `output` (for DoomV), show video, print
    the key events the Pico reports, and send keys typed in the window."""
    out = open(output, "wb") if output else None
    port = open_port(link)
    write_lock = threading.Lock()

    def send(data):
        with write_lock:
            port.write(data)

    viewer = Viewer(send if keyboard else None) if (video or keyboard) else None
    stop = threading.Event()
    stats = {"frames": 0, "bytes": 0}
    sizes = {TAG_TICKS: 4, TAG_TICKS_REP: 1, TAG_KEY: 2, TAG_NOKEY: 0, TAG_FRAME: 20,
             TAG_HELLO: 16, TAG_PALETTE: 768, TAG_VIDEO: 4 + 64000, TAG_INPUT: 3}

    def parse(buf):
        i = 0
        while i < len(buf):
            tag = buf[i]
            if tag in (TAG_TEXT, TAG_ARGS):
                if i + 2 > len(buf) or i + 2 + buf[i + 1] > len(buf):
                    break
                if tag == TAG_TEXT:
                    print("Pico:", buf[i + 2:i + 2 + buf[i + 1]].decode("ascii", "replace"))
                i += 2 + buf[i + 1]
                continue
            n = sizes.get(tag)
            if n is None:
                print("picolink: unknown record tag 0x%02x -- out of sync?" % tag)
                return b""
            if i + 1 + n > len(buf):
                break
            body = buf[i + 1:i + 1 + n]
            if tag == TAG_FRAME:
                stats["frames"] += 1
            elif tag == TAG_INPUT and show_inputs:
                print("input:", describe_input(body[0], body[1], body[2]))
            elif tag == TAG_PALETTE and viewer:
                viewer.palette = body
            elif tag == TAG_VIDEO and viewer:
                viewer.frame(body[4:])
            i += 1 + n
        return buf[i:]

    def reader():
        if video:
            send(b"V" + bytes([video]))
        pending = b""
        while not stop.is_set():
            data = port.read(65536)
            if not data:
                continue
            if out:                      # all of it: DoomV skips what it does not need
                out.write(data)
                out.flush()
            stats["bytes"] += len(data)
            pending = parse(pending + data)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    t0 = time.time()
    try:
        if viewer:
            if seconds:
                viewer.root.after(int(seconds * 1000), viewer.root.destroy)
            viewer.root.mainloop()
        else:
            while not seconds or time.time() - t0 < seconds:
                time.sleep(1)
                if out:
                    print(chr(13) + "  %d frames, %.0f KB" % (stats["frames"], stats["bytes"] / 1024), end="", flush=True)
    except KeyboardInterrupt:
        pass
    stop.set()
    if video:
        send(b"V" + bytes([0]))          # stop the video stream
    t.join(1)
    port.close()
    if out:
        out.close()
    return stats


def cmd_play(args):
    _, link = need_ports()
    print(f"picolink: the Pico's screen, {args.video and f'every {args.video} frames' or 'no video'}; "
          f"click the window and play (arrows, Ctrl fire, Space use, Esc menu, Enter select, 1-7 weapons).")
    run_session(link, video=args.video, keyboard=True)


def cmd_record(args):
    print("picolink: rebooting the Pico into link mode...")
    console, link = reboot_into(b"R")
    print(f"picolink: recording to {args.output} -- replay it with DoomV:\n"
          f"  riscv_doom build/doomv/doom1.wad build/doomv/pico2doom-doomv.elf "
          f"-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -picolink={args.output}\n"
          f"(DoomV can follow the file live, while this runs.) Ctrl+C or close the window to stop.")
    stats = run_session(link, output=args.output, video=args.video, keyboard=bool(args.video),
                        seconds=args.seconds)
    print(f"\npicolink: {stats['frames']} frames recorded")


# ---- flash ---------------------------------------------------------------

def find_bootsel_drive(timeout):
    end = time.time() + timeout
    while time.time() < end:
        for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
            info = f"{letter}:\\INFO_UF2.TXT" if os.name == "nt" else None
            if info and os.path.exists(info) and "RP2350" in open(info).read():
                return f"{letter}:\\"
        for base in ("/media", "/run/media", "/Volumes"):
            for root, dirs, files in os.walk(base) if os.path.isdir(base) else []:
                if "INFO_UF2.TXT" in files and "RP2350" in open(os.path.join(root, "INFO_UF2.TXT")).read():
                    return root
        time.sleep(0.5)
    return None


def cmd_flash(args):
    drive = find_bootsel_drive(0.5)
    if not drive:
        console, link = find_ports(2.0)
        if link:
            print("picolink: asking the Pico to reboot into BOOTSEL...")
            with open_port(link) as s:
                s.write(b"U")
        else:
            print("picolink: no Pico2Doom board running; hold BOOTSEL while plugging it in.")
        drive = find_bootsel_drive(20)
    if not drive:
        sys.exit("picolink: no RP2350 BOOTSEL drive appeared")
    print(f"picolink: copying {args.uf2} to {drive} ...")
    t0 = time.time()
    shutil.copy(args.uf2, drive)
    print(f"picolink: done in {time.time() - t0:.0f} s; the Pico reboots into the new firmware")
    console, link = find_ports(15)
    print("picolink: Pico2Doom is up on %s (console) and %s (link)" % (console, link) if console else
          "picolink: the board has not come back yet")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("console"); p.add_argument("--csv"); p.add_argument("--seconds", type=float, default=0)
    p.set_defaults(fn=cmd_console)
    p = sub.add_parser("bench"); p.add_argument("--timeout", type=float, default=600); p.set_defaults(fn=cmd_bench)
    p = sub.add_parser("play"); p.add_argument("--video", type=int, default=2); p.set_defaults(fn=cmd_play)
    p = sub.add_parser("record"); p.add_argument("output"); p.add_argument("--video", type=int, default=0)
    p.add_argument("--seconds", type=float, default=0, help="stop after this long (default: Ctrl+C)")
    p.set_defaults(fn=cmd_record)
    p = sub.add_parser("flash"); p.add_argument("uf2", nargs="?", default="build/firmware/pico2doom.uf2")
    p.set_defaults(fn=cmd_flash)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
