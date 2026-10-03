#!/usr/bin/env bash
# Network setup for an Ubuntu image that predates it: DHCP on the network
# card, through systemd-networkd, DoomV's DNS server, and apt sources that
# include universe.
#
#   wsl -d Ubuntu -u root -- bash tools/linux/ubuntu/mknetwork.sh /mnt/z/.../ubuntu.img
#
# Images made by mkrootfs.sh since have this already. It only matters when
# the image is booted with a network card (boot.py ubuntu --net, or -net),
# and it changes nothing for a boot without one: there is no interface for
# networkd to configure.
set -euo pipefail

IMG="${1:?usage: mknetwork.sh <ubuntu.img>}"
[ -f "$IMG" ] || { echo "no image at $IMG" >&2; exit 1; }

W="$(mktemp -d)"
LOOP=""
cleanup() {
	if mountpoint -q "$W"; then umount "$W" || echo "warning: image still mounted at $W" >&2; fi
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rmdir "$W" 2>/dev/null || true
}
trap cleanup EXIT

LOOP="$(losetup -fP --show "$IMG")"
mount "${LOOP}p1" "$W"

# DHCP on any Ethernet card. User-mode networking (src/net/usernet.cpp) hands
# out 10.0.2.15, with 10.0.2.2 as the router and 10.0.2.3 as DNS.
mkdir -p "$W/etc/systemd/network"
cat > "$W/etc/systemd/network/20-doomv.network" <<'EOF'
[Match]
Name=e*

[Network]
DHCP=ipv4
EOF

# Enabled the way systemctl enable would, without running anything riscv64.
mkdir -p "$W/etc/systemd/system/multi-user.target.wants" "$W/etc/systemd/system/sockets.target.wants"
ln -sf /usr/lib/systemd/system/systemd-networkd.service "$W/etc/systemd/system/multi-user.target.wants/systemd-networkd.service"
[ -e "$W/usr/lib/systemd/system/systemd-networkd.socket" ] &&
	ln -sf /usr/lib/systemd/system/systemd-networkd.socket "$W/etc/systemd/system/sockets.target.wants/systemd-networkd.socket"

# No systemd-resolved in this image, so the resolver is named directly. The
# file there before was written by WSL when stage 1 ran, and names WSL's.
rm -f "$W/etc/resolv.conf"
echo "nameserver 10.0.2.3" > "$W/etc/resolv.conf"

# What apt fetches from: universe as well as main, and the updates and
# security suites. debootstrap wrote main alone, and the browsers
# (boot.py ubuntu --setup-browser) are in universe.
cat > "$W/etc/apt/sources.list" <<'EOF'
deb http://ports.ubuntu.com/ubuntu-ports noble main universe
deb http://ports.ubuntu.com/ubuntu-ports noble-updates main universe
deb http://ports.ubuntu.com/ubuntu-ports noble-security main universe
EOF

echo "network set up in $IMG: DHCP on e*, DNS 10.0.2.3, apt from main and universe"
