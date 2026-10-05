# DoomV scripts

Run everything from the repository root. Builds go to `build/`.

## First time

```powershell
powershell -ExecutionPolicy Bypass -File scripts/install_dependencies.ps1   # host tools: make, compiler, SDL2
wsl --install -d Ubuntu                                                      # once; reboot if asked
powershell -ExecutionPolicy Bypass -File scripts/toolchain.ps1               # RISC-V tools in WSL
python scripts/get_clang.py                                                  # optional: a faster build
python scripts/get_venus.py                                                  # optional: OpenGL and Vulkan for --gpu (builds)
```

## Boot a guest

```powershell
python scripts/boot.py doom          # DOOM
python scripts/boot.py linux         # Linux with a BusyBox shell
python scripts/boot.py ubuntu        # Ubuntu 24.04, logs in as root
```

Each one builds what it needs first; `--no-build` skips that. Close the window
to exit. `python scripts/boot.py <guest> --help` lists everything a guest takes.

### Options every guest takes

| Option | |
|---|---|
| `--ram 4G` | Guest memory (default 1G). DOOM is capped near 2G. |
| `--harts 4` | CPUs, 1 to 4095 (Linux and Ubuntu). The device tree is made to match. |
| `--net` | A network card (Linux and Ubuntu); the guest reaches the internet through the host. |
| `--gpu` | A virtio-gpu (Linux and Ubuntu) in place of the simple framebuffer, with OpenGL and Vulkan on this computer's GPU; the guest picks per program (built the first time). |
| `--sound` | A sound card (Linux and Ubuntu), playing and recording through this computer's default devices. |
| `--all` | Every device: `--net --gpu --sound`. |
| `--headless` | No window; output goes to the console. |
| `--no-build` | Use what is already built. |

More than 64 harts boot slowly -- see [Several harts](../README.md#harts).

### Ubuntu

Ubuntu needs `ubuntu.img` in the repository root. It takes hours to build, so
the script never builds it; [tools/linux/ubuntu](../tools/linux/ubuntu/README.md)
has the steps.

```powershell
python scripts/boot.py ubuntu --no-autologin       # stop at the login prompt (root / doomv)
python scripts/boot.py ubuntu --desktop xfce       # boot into a desktop: xfce, openbox or x
python scripts/boot.py ubuntu --desktop-snapshot   # the booted XFCE desktop, in seconds
python scripts/boot.py ubuntu --install-desktops   # once, before --desktop (hours)
python scripts/boot.py ubuntu --setup-network      # once, before --net, for an image made before networking
python scripts/boot.py ubuntu --setup-browser      # once: a browser (NetSurf) and w3m; includes --setup-network
python scripts/boot.py ubuntu --setup-sound        # once: aplay, arecord and speaker-test; includes --setup-network
python scripts/boot.py ubuntu --setup-gpu          # once: the desktops on --gpu, current Mesa, glxgears/vkcube
```

`--desktop` boots from scratch, about 25 minutes to a usable desktop.
`--desktop-snapshot` restores one that was booted already; make it once with
`python performance/make_desktop_snapshot.py` (about 25 minutes).

### Tests

```powershell
python scripts/boot.py linux --smoke     # BusyBox runs, then exit
python scripts/boot.py ubuntu --login    # log in through the emulated keyboard, then exit
python scripts/verify.py                 # every regression suite (or name some)
python scripts/ci.py                     # the gate: build, lock-step against Sail, every suite
python scripts/install_hooks.py          # run the gate before every git push
```

### Advanced

| Option | |
|---|---|
| `--snapshot DIR --snapshot-at STEP` | Save the whole machine at an instruction, and carry on. |
| `--restore DIR` | Start from a snapshot. Use the same options it was made with. |
| `--stop-at STEP` | Stop after exactly STEP instructions and write the state to `crash.log`. |
| `--record FILE` / `--replay FILE` | Log every input, then replay it exactly. |
| `--march ISA` | Change the extensions, e.g. `rv64imafdc_zicsr`. |
| `--drives DIR` / `--shared DIR` | Storage drives and shared folder (default `drives/` and `shared/`; `''` for none). |
| `--clock host\|fixed\|SECONDS` | Where the guest's clock starts (Linux and Ubuntu). Default `host`, this computer's time; `fixed` is the same date every run. |
| `-- <options>` | Anything after `--` goes to `riscv_doom.exe` unchanged. |

```powershell
python scripts/boot.py ubuntu --snapshot snap/booted --snapshot-at 3000000000
python scripts/boot.py ubuntu --restore snap/booted
python scripts/boot.py linux --headless --stop-at 300000000 -- -trace=trace.log
```

The emulator's own options are listed in the
[README](../README.md#command-line-options).

## Build without booting

```powershell
python scripts/build.py doom | linux | all     # all is the default
python performance/pgo.py                      # retrain the speed profile after changing the hot path
```

The first build also trains a profile-guided build (a few minutes); `--no-pgo`
skips it.

## Network, disks and the shared folder

With `--net`, BusyBox Linux gets its address from `udhcpc -i eth0` and Ubuntu
by itself at boot. The guest is `10.0.2.15`; the host is `10.0.2.2`. See
[Networking](../README.md#networking).

```powershell
python scripts/mkdrive.py data 1G    # every *.img in drives/ is attached to Linux and Ubuntu
```

`shared/` is served live to the guest. Inside it:
`mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared`.

## Good to know

- **Same inputs, same run.** A guest does the same thing every time it is given
  the same inputs. Those include the disk -- a boot writes to it, so boot a
  copy to repeat a run exactly -- what is in `drives/` and `shared/`, what
  you type, and the date the clock starts at (`--record` and `--replay` make
  those repeatable; `--clock fixed` the date as well).
- **Memory is free until used.** `--ram 16G` starts as fast as 1G; only what
  the guest touches is allocated.
- **One emulator per disk image.** Two DoomVs on one `ubuntu.img` corrupt it;
  the scripts refuse to start a second.
- **One script at a time.** Scripts take a lock on the checkout
  (`.scripts.lock`). After a crash, delete that empty folder.
