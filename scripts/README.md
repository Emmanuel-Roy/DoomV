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

# Boot either guest. Linux --smoke exits after BusyBox proves userspace runs.
python scripts/boot.py doom
python scripts/boot.py linux
python scripts/boot.py linux --smoke

# Run every regression suite, or name the suites to run.
python scripts/verify.py
python scripts/verify.py --quick
python scripts/verify.py differential archtest
```

`toolchain.ps1` deliberately does not install WSL itself. Install Ubuntu once
with `wsl --install -d Ubuntu`, reboot if Windows requests it, and run the
toolchain script afterwards. `verify.py` uses suite exit status and requires a
complete nonempty signature; a missing suite or truncated output is a failure.
