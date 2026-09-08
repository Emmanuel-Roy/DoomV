#!/bin/sh
# PID 1 for the regression-only initramfs. A marker proves shell execution.
set -eu
/bin/busybox test "$(/bin/busybox uname -m)" = riscv64
/bin/busybox test -r /bin/busybox
value=$((20 + 22))
test "$value" = 42
echo DOOMV_USERSPACE_OK
# PID 1 must remain alive after success; the harness stops its own emulator.
exec /bin/sh
