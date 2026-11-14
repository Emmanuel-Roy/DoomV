# Sourced first by build.sh and build_vulkan.sh: make sure they run in Git Bash.
#
# They are Windows-side scripts. They turn Git Bash's /z/Code/... paths into
# WSL's /mnt/z/Code/..., run Windows Python (WSL often has no network of its
# own here), and call wsl.exe for the cross-compile. In PowerShell, though,
# `bash` is C:\Windows\System32\bash.exe -- WSL's bash -- where all three go
# wrong: the path came out as /mnt/mnt/z/... So under WSL, start the same
# script again in Git Bash, through WSL's Windows interop, and stop here.
#
# GIT_BASH overrides where Git Bash is (as a WSL path).
if [ -n "${WSL_DISTRO_NAME:-}" ]; then
	git_bash="${GIT_BASH:-/mnt/c/Program Files/Git/bin/bash.exe}"
	if [ ! -f "$git_bash" ]; then
		echo "$(basename "$0"): needs Git Bash, and it is not at $git_bash." >&2
		echo "Install Git for Windows, or set GIT_BASH to its bash.exe as a WSL path." >&2
		exit 1
	fi
	# Git Bash starts in the Windows form of this directory (interop converts
	# it), so the script's own name is enough to find it.
	cd "$(dirname "$0")"
	exec "$git_bash" "./$(basename "$0")" "$@"
fi
