#!/usr/bin/env bash
# Stage 3 of the Ubuntu image: three X11 desktops, installed by DoomV.
#
#   wsl -d Ubuntu -u root -- bash tools/linux/ubuntu/mkdesktop.sh /mnt/z/.../ubuntu.img
#   wsl -d Ubuntu -u root -- bash tools/linux/ubuntu/mkdesktop.sh --sessions-only /mnt/z/.../ubuntu.img
#
# --sessions-only rewrites just the X configuration, the session scripts and
# the doomv-desktop service in an image that already has the desktops, and
# stages no packages. It is how a fix to a session reaches an installed image
# without another few hours of dpkg.
#
# Normally run for you by `python scripts/boot.py ubuntu --install-desktops`,
# which also backs the image up first and then boots DoomV to do the install.
#
# The split is the same one stage 1 and stage 2 use, for the same reason.
# This script runs on the host and executes nothing riscv64: it downloads the
# packages and puts them inside the image as a local apt repository. The
# install itself -- apt resolving the order, dpkg unpacking, every maintainer
# script -- happens in the guest, run by DoomV, when the image is booted with
# init=/doomv-desktop-install.
#
# One package set covers all three desktops -- Openbox, XFCE, and bare X with
# xterm windows -- because the smaller two are almost entirely inside the
# largest. Which one runs is chosen per boot, by doomv.desktop= on the kernel
# command line; see the doomv-desktop service this writes.
set -euo pipefail

SESSIONS_ONLY=""
if [ "${1:-}" = "--sessions-only" ]; then
	SESSIONS_ONLY=1
	shift
fi
IMG="${1:?usage: mkdesktop.sh [--sessions-only] <ubuntu.img>}"
[ -f "$IMG" ] || { echo "error: $IMG not found" >&2; exit 1; }

# Everything the three sessions need, and nothing recommended beyond it: at
# DoomV's speed each extra package is minutes of dpkg. dbus-x11 is XFCE's
# session bus; xauth and x11-xserver-utils are what xinit and the session
# scripts call.
PKGS="xserver-xorg-core xserver-xorg-video-fbdev xserver-xorg-input-libinput \
xinit xauth x11-xserver-utils openbox xterm xfce4 xfce4-terminal dbus-x11 fonts-dejavu-core"

for tool in apt-get dpkg-scanpackages losetup mountpoint; do
	command -v "$tool" >/dev/null || { echo "error: $tool is missing on the host" >&2; exit 2; }
done

W="$(mktemp -d /tmp/doomv-desktop.XXXXXX)"
LOOP=""
cleanup() {
	set +e
	if mountpoint -q "$W/img"; then
		umount "$W/img" || { echo "warning: image still mounted at $W/img; leaving it" >&2; return; }
	fi
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$W"
}
trap cleanup EXIT

mkdir -p "$W/img"

echo "== mounting $IMG"
LOOP="$(losetup -fP --show "$IMG")"
sleep 1
mount "${LOOP}p1" "$W/img"
R="$W/img"
[ -f "$R/var/lib/dpkg/status" ] || { echo "error: no dpkg database in $IMG -- is stage 2 done?" >&2; exit 1; }
if [ -n "$SESSIONS_ONLY" ] && [ ! -x "$R/usr/lib/xorg/Xorg" ]; then
	echo "error: $IMG has no X server installed; run without --sessions-only first" >&2
	exit 1
fi

if [ -z "$SESSIONS_ONLY" ]; then
A="$W/apt"
mkdir -p "$A"/etc/apt/{sources.list.d,preferences.d,apt.conf.d} \
	"$A"/var/lib/apt/lists/partial "$A"/var/cache/apt/archives/partial \
	"$A"/var/lib/dpkg "$A"/keyrings

# Resolved against the image's own package database, so apt downloads what
# the guest lacks and not what it already has.
cp "$R/var/lib/dpkg/status" "$A/var/lib/dpkg/status"
cp "$R/usr/share/keyrings/ubuntu-archive-keyring.gpg" "$A/keyrings/"
echo "deb [arch=riscv64 signed-by=$A/keyrings/ubuntu-archive-keyring.gpg] http://ports.ubuntu.com/ubuntu-ports noble main universe" \
	> "$A/etc/apt/sources.list"
APT=(apt-get -o "Dir=$A" -o "Dir::State::status=$A/var/lib/dpkg/status"
	-o APT::Architecture=riscv64 -o "APT::Architectures::=riscv64" -o Debug::NoLocking=1)

echo "== fetching riscv64 package lists from ports.ubuntu.com"
"${APT[@]}" update -qq
echo "== downloading the desktop packages"
# shellcheck disable=SC2086
"${APT[@]}" install --download-only --no-install-recommends -y $PKGS >/dev/null
DEBS=("$A"/var/cache/apt/archives/*.deb)
echo "   ${#DEBS[@]} packages"

echo "== writing the local repository into the image"
REPO="$R/var/cache/doomv-desktop"
rm -rf "$REPO"
mkdir -p "$REPO"
cp "${DEBS[@]}" "$REPO/"
(cd "$REPO" && dpkg-scanpackages --multiversion . /dev/null > Packages 2>/dev/null)
# Kept out of sources.list.d on purpose: a normal apt run in the guest has
# no network and should not trip over this, and the install script names it
# explicitly.
echo "deb [trusted=yes] file:/var/cache/doomv-desktop ./" > "$R/etc/apt/doomv-desktop.list"
echo "$PKGS" > "$R/etc/apt/doomv-desktop.packages"
fi

echo "== writing the X configuration and the sessions"
mkdir -p "$R/etc/X11/xorg.conf.d" "$R/usr/local/lib/doomv" "$R/usr/local/sbin"
cat > "$R/etc/X11/xorg.conf.d/10-doomv.conf" <<'XORG'
# DoomV: X on the simple-framebuffer and the two virtio-input devices.
#
# Input devices are named here rather than discovered, because this image
# has no udev to discover them with, and AutoAddDevices is off so X does not
# go looking. The kernel numbers event devices in probe order, which follows
# the device tree: the keyboard is event0 and the mouse event1.
Section "ServerFlags"
	Option "AutoAddDevices" "false"
	Option "AutoAddGPU" "false"
EndSection

Section "Device"
	Identifier "DoomV framebuffer"
	Driver "fbdev"
	Option "fbdev" "/dev/fb0"
EndSection

Section "Screen"
	Identifier "DoomV screen"
	Device "DoomV framebuffer"
	DefaultDepth 24
EndSection

Section "InputDevice"
	Identifier "DoomV keyboard"
	Driver "libinput"
	Option "Device" "/dev/input/event0"
EndSection

Section "InputDevice"
	Identifier "DoomV mouse"
	Driver "libinput"
	Option "Device" "/dev/input/event1"
EndSection

Section "ServerLayout"
	Identifier "DoomV"
	Screen "DoomV screen"
	InputDevice "DoomV keyboard" "CoreKeyboard"
	InputDevice "DoomV mouse" "CorePointer"
EndSection
XORG

# xterm's default font is X's core bitmap "fixed", from xfonts-base, which is
# not installed: with it missing xterm prints "cannot load font" and exits,
# which leaves Openbox with no terminal and ends the bare X session outright.
# The one font that is installed is DejaVu, a TrueType font xterm reaches
# through Xft. Setting it as an X resource rather than on each command line
# covers every xterm, including the ones Openbox's menu opens.
cat > "$R/usr/local/lib/doomv/Xresources" <<'XRES'
XTerm*faceName: DejaVu Sans Mono
XTerm*faceSize: 11
XTerm*background: #1d1f21
XTerm*foreground: #c5c8c6
XRES

# Shared by the two xterm-based sessions. Progress goes to the serial console
# as DOOMV-SESSION lines, so a headless run can tell a terminal that is slow
# to appear from one that never started and one that exited: "xterm is up" is
# printed by the shell inside the terminal, so it only appears once xterm has
# really got as far as running a program.
cat > "$R/usr/local/lib/doomv/session-lib" <<'SESSION'
say() { echo "DOOMV-SESSION: $*" > /dev/hvc0 2>/dev/null; }
# -nocpp: xrdb runs the C preprocessor over resource files by default, and
# this image has no cpp. The file needs no preprocessing.
load_resources() {
	if xrdb -nocpp -merge /usr/local/lib/doomv/Xresources; then say "xrdb loaded the xterm font"
	else say "xrdb failed rc=$?"; fi
}
terminal() {
	geometry="$1"
	( xterm -geometry "$geometry" -e sh -c 'echo "DOOMV-SESSION: xterm is up" > /dev/hvc0; exec bash -l'
	  say "xterm $geometry exited rc=$?" )
}
SESSION

cat > "$R/usr/local/lib/doomv/session-openbox" <<'SESSION'
#!/bin/sh
# Openbox: right-click the desktop for its menu.
. /usr/local/lib/doomv/session-lib
say "openbox session begins"
load_resources
xsetroot -solid '#2e3440' 2>/dev/null
terminal 100x30+40+40 &
exec openbox
SESSION

cat > "$R/usr/local/lib/doomv/session-xfce" <<'SESSION'
#!/bin/sh
# XFCE needs a session bus, which nothing else in this image starts.
exec dbus-launch --exit-with-session startxfce4
SESSION

cat > "$R/usr/local/lib/doomv/session-x" <<'SESSION'
#!/bin/sh
# Bare X: no window manager, so windows cannot be moved. Closing the second
# xterm ends the session.
. /usr/local/lib/doomv/session-lib
say "bare x session begins"
load_resources
xsetroot -solid '#303030' 2>/dev/null
terminal 80x24+640+60 &
terminal 90x30+30+30
say "bare x session ends"
SESSION

cat > "$R/usr/local/sbin/doomv-desktop" <<'LAUNCH'
#!/bin/sh
# Start the X session named by doomv.desktop= on the kernel command line.
kind=$(sed -n 's/.*doomv\.desktop=\([a-z]*\).*/\1/p' /proc/cmdline)
case "$kind" in
	openbox|xfce|x) session=/usr/local/lib/doomv/session-$kind ;;
	*) echo "doomv-desktop: no doomv.desktop= on the kernel command line"; exit 0 ;;
esac
export HOME=/root
cd /root
# On the serial console too, so a headless check can see the session start.
echo "DOOMV-DESKTOP-SESSION: $kind" > /dev/hvc0 2>/dev/null || true
# The server binary directly: /usr/bin/X is a wrapper this minimal image
# does not install.
#
# The server's and the session's own output go to the serial console. Under
# systemd they would otherwise land only in the journal, and a guest that is
# stopped rather than shut down never flushes that to disk -- so when a
# session fails, this is the one place the reason survives.
if [ -w /dev/hvc0 ]; then
	exec xinit "$session" -- /usr/lib/xorg/Xorg :0 vt7 -nolisten tcp > /dev/hvc0 2>&1
fi
exec xinit "$session" -- /usr/lib/xorg/Xorg :0 vt7 -nolisten tcp
LAUNCH
chmod 755 "$R/usr/local/lib/doomv/session-"* "$R/usr/local/sbin/doomv-desktop"

cat > "$R/etc/systemd/system/doomv-desktop.service" <<'UNIT'
[Unit]
Description=DoomV desktop session (doomv.desktop= on the kernel command line picks which)
# Only when a desktop was asked for; a plain boot stays at the text console.
ConditionKernelCommandLine=|doomv.desktop=openbox
ConditionKernelCommandLine=|doomv.desktop=xfce
ConditionKernelCommandLine=|doomv.desktop=x
After=systemd-user-sessions.service getty@tty1.service

[Service]
ExecStart=/usr/local/sbin/doomv-desktop
Restart=no

[Install]
WantedBy=multi-user.target
UNIT
# Enabled by hand, since systemctl cannot run against an image from here.
mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/doomv-desktop.service \
	"$R/etc/systemd/system/multi-user.target.wants/doomv-desktop.service"

if [ -n "$SESSIONS_ONLY" ]; then
	sync
	echo "== done: X configuration, sessions and service rewritten; no packages staged"
	exit 0
fi

echo "== writing the install script DoomV will run as init"
cat > "$R/doomv-desktop-install" <<'INSTALL'
#!/bin/sh
# Installs the desktops, run by DoomV as PID 1. Every binary from here on is
# riscv64. Like stage 2, nothing is mounted yet and there is no service
# manager, so this sets up what apt and dpkg need and powers the machine off
# at the end rather than returning.
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null || true
# apt runs dpkg on a pseudo-terminal so it can keep a log of the install;
# without devpts it prints "E: Can not write log (Is /dev/pts mounted?)",
# carries on, and the log is lost.
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null || true
mount -t tmpfs tmpfs /run
mount -t tmpfs tmpfs /tmp
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8
export DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true

echo "=== DOOMV-DESKTOP-BEGIN ==="
# No services may start mid-install: there is no systemd to start them, and
# a maintainer script that tries should be told so and carry on.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d

APT="apt-get -o Dir::Etc::sourcelist=/etc/apt/doomv-desktop.list -o Dir::Etc::sourceparts=-"
$APT update
rc=$?
if [ "$rc" = 0 ]; then
	# shellcheck disable=SC2046
	$APT install -y --no-install-recommends \
		-o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
		$(cat /etc/apt/doomv-desktop.packages)
	rc=$?
fi
echo "=== DOOMV-DESKTOP-APT rc=$rc ==="

rm -f /usr/sbin/policy-rc.d
if [ "$rc" = 0 ]; then
	# The local repository has done its job; its 120-odd MB go back to the
	# guest. On failure it stays, so the install can simply be run again.
	rm -rf /var/cache/doomv-desktop /etc/apt/doomv-desktop.list
	rm -f /var/lib/apt/lists/_var_cache_doomv-desktop_*
	rm -f /doomv-desktop-install
fi

# sync before the marker: the host ends the run when it sees one.
sync
if [ "$rc" = 0 ]; then
	echo "=== DOOMV-DESKTOP-OK ==="
else
	echo "=== DOOMV-DESKTOP-FAILED ==="
fi
umount /tmp /run 2>/dev/null || true
# Remount the root filesystem read-only before powering off. sysrq's power
# off does not unmount anything, so without this the image is left with an
# unreplayed journal: harmless to the next boot, which replays it, but it
# makes a host-side e2fsck of the image report errors until then.
mount -o remount,ro / 2>/dev/null || true
echo o > /proc/sysrq-trigger 2>/dev/null || true
poweroff -f 2>/dev/null || true
echo "=== DOOMV-DESKTOP-POWEROFF-FAILED ==="
while true; do sleep 60; done
INSTALL
chmod 755 "$R/doomv-desktop-install"

sync
echo "== done: ${#DEBS[@]} packages staged; boot with init=/doomv-desktop-install to install"
