#!/usr/bin/env bash
# Cross-compile llama.cpp with its Vulkan backend for the guest, and put the
# binary in shared/llama-vulkan.
#
# Run from the repository root, in PowerShell or Git Bash:  bash tools/llama/build_vulkan.sh
#
# The guest runs it on Mesa's Venus driver, which hands the Vulkan calls to
# virtio-gpu and so to this computer's GPU (DoomV -gpu, tools/venus/).
#
# Unlike tools/llama/build.sh this binary is dynamic: the Vulkan loader finds
# the driver by dlopen()ing the ICD named in /usr/share/vulkan/icd.d, and a
# static binary cannot dlopen. Dynamic means it must be linked against the
# guest's own glibc (Ubuntu 24.04's 2.39), not the cross-compiler's newer one,
# so it builds against an Ubuntu 24.04 riscv64 sysroot. libstdc++ and libgcc
# are linked statically, because the cross-compiler's are newer than the
# guest's too.
#
# The packages are downloaded on the Windows side (fetch_vulkan_debs.py),
# because WSL often has no network of its own here (a VPN); WSL only unpacks
# them. glslc, which compiles the shaders on the build machine, comes the
# same way.
set -euo pipefail
. "$(dirname "$0")/in_git_bash.sh"
cd "$(dirname "$0")/../.."
WSLREPO="/mnt$(pwd)"
DISTRO="${DISTRO:-Ubuntu}"
REF="${LLAMA_REF:-}"   # a llama.cpp commit; default, whatever /root/llama.cpp has (a fresh clone: master)
DEBS=build/llama/debs

# glslc is installed into the WSL distro, so it is that release's package.
CODENAME="$(MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -- lsb_release -cs | tr -d '\r')"
python -I tools/llama/fetch_vulkan_debs.py "$DEBS" "${CODENAME:-resolute}"

MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -u root -- \
    bash "$WSLREPO/tools/llama/build_vulkan_wsl.sh" "$WSLREPO" "$DEBS" "$REF"

ls -la shared/llama-vulkan
echo
echo "Now: bash tools/llama/run_vulkan.sh shared/<model>.gguf  (see tools/llama/README.md)"
