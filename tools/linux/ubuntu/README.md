# Ubuntu root filesystem

`src/` is a git submodule of
[Ubuntu's debootstrap source](https://git.launchpad.net/ubuntu/+source/debootstrap),
pinned to the `applied/ubuntu/noble` branch — Ubuntu's packaging rather than
Debian's, because the suite scripts differ and `scripts/noble` only exists in
Ubuntu's tree.

`mkrootfs.sh` builds `ubuntu.img`, a GPT-partitioned ext4 disk image holding
an Ubuntu 24.04 riscv64 root filesystem with systemd as PID 1. Both are
gitignored build output, same as every other `tools/*` artifact.

## Why this is separate from `../rootfs/`

`../rootfs/` builds a BusyBox initramfs: one static binary, exec'd directly
as PID 1 by `rdinit=/bin/sh`. It proves the kernel boots. It proves almost
nothing about the machine underneath, because BusyBox's shell asks the kernel
for very little.

Ubuntu asks for a great deal. systemd needs cgroups, epoll, signalfd,
timerfd, inotify, mount namespaces, a working monotonic clock and a real
`/proc` and `/sys`; the package tooling needs a filesystem it can actually
write; and all of it runs as several hundred processes rather than one. A
machine that boots BusyBox is not thereby known to boot a distribution, and
the gap between the two is where the interesting emulator bugs live.

Keeping it in its own directory with its own submodule follows the pattern
the rest of `tools/` already uses: one third-party source pinned per
component, with DoomV's own build script beside it rather than inside it.

## Building

Runs inside WSL as root — it needs loop devices, `mkfs.ext4`, `sgdisk` and
binfmt, none of which exist on the MSYS2 side:

```
git submodule update --init tools/linux/ubuntu/src
wsl -d Ubuntu -u root -- apt-get install -y qemu-user-static binfmt-support gdisk
wsl -d Ubuntu -u root -- bash /mnt/z/Code/Dev/DoomV/tools/linux/ubuntu/mkrootfs.sh
```

It downloads from `ports.ubuntu.com` — riscv64 is a *ports* architecture and
is not on `archive.ubuntu.com` — so it needs network and takes a while.

One Windows-specific trap, handled by the script so it cannot be hit:
`core.autocrlf=true` is set on this machine, and the parent
`.gitattributes` deliberately leaves the vendored trees under `tools/*/src`
alone -- they keep whatever upstream uses. Upstream debootstrap ships no
`.gitattributes`, so a plain clone here arrives with CRLF endings, and
debootstrap is POSIX shell:

```
debootstrap: line 2: set: -: invalid option
debootstrap: line 69: syntax error near unexpected token (a stray CR)
```

`mkrootfs.sh` works from a CR-stripped copy in a temp directory rather than
requiring every checkout to be configured correctly. It is a few hundred KB
of text, so the copy costs nothing and cannot be forgotten.

Cross-architecture bootstrapping is two stages for a reason worth knowing:
stage 1 (`--foreign`) only unpacks, executing nothing from the target
architecture, which is what makes it possible on an x86 host at all. Stage 2
runs the maintainer scripts, which are riscv64 binaries, so it needs
`qemu-user-static` and binfmt. Without them stage 1 still appears to succeed
and stage 2 fails on the first maintainer script — a confusing place to
discover a missing dependency, which is why `mkrootfs.sh` checks up front.

## Booting

```
sed 's|rdinit=/bin/sh|root=/dev/vda1 rootwait rw|' tools/linux/dts/doomv.dts > /tmp/ubuntu.dts
dtc -I dts -O dtb -o ubuntu.dtb /tmp/ubuntu.dts
riscv_doom.exe -opensbi=tools/linux/opensbi/fw_jump.elf \
               -kernel=tools/linux/linux/Image \
               -dtb=ubuntu.dtb -disk=ubuntu.img
```

Log in as `root` / `doomv`.

Two things not to do:

* **No `-initrd`.** With an initramfs present the kernel runs that instead
  and never mounts the disk, which looks like success and proves nothing.
* **No `init=` override.** `init=/bin/sh` gets you a shell much faster and
  skips systemd entirely, which is the whole point of this image.

## Known constraints

**Memory.** DoomV currently has 1 GB (`Memory::RAM_SIZE` in
`src/memory.hpp`, mirrored by the `memory@80000000` node in
`tools/linux/dts/doomv.dts` — the two have to agree). systemd in 1 GB is
workable but tight, and `apt` is not. If the boot dies in the OOM killer,
that pair is the thing to raise, and both must be changed together.

**Speed.** DoomV runs around 13 MIPS, so a systemd boot that takes two
seconds on hardware takes minutes here. That is expected, not a fault, and
it is why `-ng` exists for the test suites — but for this image you want the
window, since the point is to log in and look around.

**No network.** There is no NIC in this machine, so `apt` cannot reach a
mirror from inside the guest; everything the image needs has to be in it
before it boots. `mkrootfs.sh`'s `--include=` list is where to add packages.
`systemd-networkd-wait-online` is disabled by the script for the same
reason — left enabled it blocks the boot for two minutes waiting for a link
that will never come up.

## Status

Not yet booted. The block path it depends on is verified: `../rootfs/mkdisk.sh`
builds a GPT-partitioned image and DoomV boots BusyBox from `/dev/vda1`
through it, so virtio-blk reports the right capacity, the primary GPT parses,
and the partition mounts. What is untested is everything systemd does after
that.
