# DoomV scripts

Run these from the repository root in PowerShell. Build artifacts live under
`build/`, and the scripts never stop an emulator started by another process.

```powershell
# Native host prerequisites (MSYS2, Make, GCC, SDL2, Python)
powershell -ExecutionPolicy Bypass -File scripts/install_dependencies.ps1

# Ubuntu WSL packages, the RISC-V newlib compiler, and optional Sail/ACT tools
powershell -ExecutionPolicy Bypass -File scripts/toolchain.ps1

# Build only DoomV; build Linux; or build both
python scripts/build.py doom
python scripts/build.py linux
python scripts/build.py all

# Boot a guest. Linux --smoke exits after BusyBox proves userspace runs;
# Ubuntu --login logs in through the emulated keyboard and exits.
python scripts/boot.py doom
python scripts/boot.py linux
python scripts/boot.py linux --smoke
python scripts/boot.py ubuntu                 # window; logs in as root once getty asks
python scripts/boot.py ubuntu --no-autologin  # window; stops at the login prompt
python scripts/boot.py ubuntu --login

# Desktops: install all three once (DoomV runs the install, for hours), then
# pick one per boot.
python scripts/boot.py ubuntu --install-desktops
python scripts/boot.py ubuntu --desktop openbox    # or xfce, or x

# More memory, for any of the three. Becomes the emulator's -ram=, and the
# device tree's memory node is rewritten to match, so the guest is told what
# it actually got. Bytes or a K/M/G/T suffix, any value.
python scripts/boot.py linux --ram 4G
python scripts/boot.py ubuntu --ram 8G
python scripts/boot.py doom --ram 2G               # DOOM is capped near 2G, see below

# What each guest takes is not the same set; ask it.
python scripts/boot.py ubuntu --help

# Make a storage drive; every *.img in drives/ is attached to Linux at boot.
python scripts/mkdrive.py data 1G
# shared/ needs no setup: it is served to Linux live. In the guest:
#   mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared

# Run every regression suite, or name the suites to run.
python scripts/verify.py
python scripts/verify.py --quick
python scripts/verify.py differential archtest
```

`boot.py ubuntu` is the one boot script that cannot build its guest. The
other two produce a userland in minutes from a cross-compiler; the Ubuntu
image is built in two stages, the second of which is DoomV running Ubuntu's
own `dpkg` for about four hours. So the image is an input, and the script
says how to make one rather than starting that on your behalf --
`tools/linux/ubuntu/README.md` has the two commands.

`--ram` is capped near 2G for DOOM and unlimited for the other two. DOOM's WAD
sits directly above RAM and the guest reads its address from a 32-bit MMIO
register, so RAM has to end below 4GB; nothing but DOOM reads the WAD. More
RAM costs nothing to give: the allocation is lazy, so pages the guest never
touches are never faulted in, and a 16G guest starts in 0.39s against 0.03s
for 1G. What costs is the guest *using* it -- past the host's own RAM the host
swaps and nothing else matters. See `performance/README.md`.

## Determinism

A guest is deterministic given the same inputs, and the inputs are more than
the command line. Two runs of one configuration produce byte-identical machine
state, which is what `performance/bench.py` checks by hashing `crash.log` and
what the lock-step harness depends on.

What counts as an input, and therefore what has to be equal for that to hold:

* **The disk, including what the last run wrote to it.** A root disk is opened
  read-write and a systemd boot writes to it -- the journal, the random seed --
  so booting the same image twice does *not* repeat: measured here, the same
  `ubuntu.img` booted twice gave `367365fc8236` and then `28a5cd93af8e`. It is
  not that the emulator is nondeterministic; it is that the second boot started
  from a different disk. Copy the image first and the hashes match exactly,
  which is what `bench.py`'s ubuntu workload does.
* **`drives/` and `shared/`**, which are attached from the working directory by
  default. Their contents are guest-visible, so a file appearing in `shared/`
  changes the machine. `-drives=` and `-shared=` detach them; the benchmark
  passes both.
* **Input.** Typing into the window is live and is not repeatable by nature.
  `-record` captures a session and `-replay` reproduces it exactly, committing
  each event on an instruction count rather than a wall-clock time.

Nothing else varies: no host clock, no thread interleaving and no address-space
layout reaches the guest. Host timing changes how fast a run goes, never what
it computes.

`toolchain.ps1` deliberately does not install WSL itself. Install Ubuntu once
with `wsl --install -d Ubuntu`, reboot if Windows requests it, and run the
toolchain script afterwards. `verify.py` uses suite exit status and requires a
complete nonempty signature; a missing suite or truncated output is a failure.
