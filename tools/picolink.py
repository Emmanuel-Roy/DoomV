#!/usr/bin/env python3
"""The PC side of Pico2Doom: talk to a Pico 2 over its two USB serial ports.

  console   print the Pico's console (perf stats once a second); --csv FILE
            also logs them as a table (see docs/PERFORMANCE.md)
  bench     reboot into the benchmark (DOOM's -timedemo demo1) and print the
            result -- the classic DOOM benchmark, on the Pico
  record    reboot into link mode and record the run for DoomV lock-step;
            --video N also streams every Nth frame and shows it in a window
  flash     put the Pico into BOOTSEL over USB and copy a UF2 onto it

The Pico shows up as two serial ports (USB 2E8A:4D44): interface 0 is the
console (plain text), interface 2 is the link (binary, see port/port_link.h).

Examples:
  python tools/picolink.py console --csv perf.csv
  python tools/picolink.py bench
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
TAG_TICKS, TAG_TICKS_REP, TAG_KEY, TAG_NOKEY, TAG_FRAME, TAG_HELLO, TAG_VIDEO, TAG_PALETTE, TAG_TEXT, TAG_ARGS = range(1, 11)


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
    """A tkinter window showing the Pico's 320x200 frames, 2x."""

    def __init__(self):
        import tkinter as tk
        self.tk = tk
        self.root = tk.Tk()
        self.root.title("Pico 2 (USB video)")
        self.label = tk.Label(self.root)
        self.label.pack()
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


def cmd_record(args):
    print("picolink: rebooting the Pico into link mode...")
    console, link = reboot_into(b"R")
    out = open(args.output, "wb")
    viewer = Viewer() if args.video else None
    stop = threading.Event()
    stats = {"frames": 0, "bytes": 0}

    def reader():
        with open_port(link) as s:
            if args.video:
                s.write(b"V" + bytes([args.video]))
            pending = b""
            while not stop.is_set():
                data = s.read(65536)
                if not data:
                    continue
                pending += data
                # Write everything (DoomV skips video records itself); parse
                # only to show video and count frames.
                out.write(data)
                out.flush()
                stats["bytes"] += len(data)
                pending = parse(pending)

    def parse(buf):
        i = 0
        sizes = {TAG_TICKS: 4, TAG_TICKS_REP: 1, TAG_KEY: 2, TAG_NOKEY: 0, TAG_FRAME: 20,
                 TAG_HELLO: 16, TAG_PALETTE: 768, TAG_VIDEO: 4 + 64000}
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
            elif tag == TAG_PALETTE and viewer:
                viewer.palette = body
            elif tag == TAG_VIDEO and viewer:
                viewer.frame(body[4:])
            i += 1 + n
        return buf[i:]

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    print(f"picolink: recording to {args.output} -- replay it with DoomV:\n"
          f"  riscv_doom build/doomv/doom1.wad build/doomv/pico2doom-doomv.elf "
          f"-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -picolink={args.output}\n"
          f"(DoomV can follow the file live, while this runs.) Ctrl+C to stop.")
    try:
        if viewer:
            viewer.root.mainloop()
        else:
            while True:
                time.sleep(1)
                print(f"\r  {stats['frames']} frames, {stats['bytes'] / 1024:.0f} KB", end="", flush=True)
    except KeyboardInterrupt:
        pass
    stop.set()
    t.join(1)
    out.close()
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
    p = sub.add_parser("record"); p.add_argument("output"); p.add_argument("--video", type=int, default=0)
    p.set_defaults(fn=cmd_record)
    p = sub.add_parser("flash"); p.add_argument("uf2", nargs="?", default="build/firmware/pico2doom.uf2")
    p.set_defaults(fn=cmd_flash)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
