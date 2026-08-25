#!/usr/bin/env python3
# Runs one riscv-arch-test source file through both spike (reference) and
# DoomV (device under test), extracts each side's signature-region dump,
# and diffs them. Spike side uses spike's own official arch_test_target/
# port (spike's native `+signature=` mechanism -- fast and exact, unlike
# the --log-commits-parsing approach this file used earlier, which turned
# out both catastrophically slow *and* unreliable to capture -- see git
# history/session notes). DoomV side uses tools/vtest/arch-test-src/
# doomv_env/ (this project's own port of the same test-format contract).
import subprocess, sys, os

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC_ROOT = os.path.join(ROOT, "arch-test-src")
TC_BIN = r"C:\Users\royem\SWE\toolchains\xpack-riscv-none-elf-gcc-15.2.0-1\bin"
GCC = os.path.join(TC_BIN, "riscv-none-elf-gcc-15.2.0.exe")
NM = os.path.join(TC_BIN, "riscv-none-elf-nm.exe")
SPIKE = r"C:\Users\royem\SWE\toolchains\spike\build\spike.exe"
SPIKE_ENV = r"C:\Users\royem\SWE\toolchains\spike\arch_test_target\spike"
DOOMV = r"C:\Users\royem\SWE\GitHub\DoomV\riscv_doom.exe"
DOOMV_DIR = r"C:\Users\royem\SWE\GitHub\DoomV"
DUMMY_WAD = os.path.join(DOOMV_DIR, "tools", "doombuild", "DOOM1.WAD")
MSYS_BASH = r"C:\msys64\usr\bin\bash.exe"

def run(cmd, **kw):
	return subprocess.run(cmd, capture_output=True, text=True, timeout=kw.get("timeout", 60))

def compile_test(src, env_dir, link_ld, out_elf, flen, freg, fregwidth):
	cmd = [GCC, "-march=rv64imafdc_zicsr_zifencei", "-mabi=lp64d", "-static", "-mcmodel=medany",
	       "-fvisibility=hidden", "-nostdlib", "-nostartfiles",
	       "-I", os.path.join(SRC_ROOT, "riscv-test-suite", "env"),
	       "-I", env_dir,
	       "-DXLEN=64", "-DTEST_CASE_1=True",
	       f"-DFLEN={flen}", f"-DFLREG={freg}", f"-DFREGWIDTH={fregwidth}",
	       "-T", link_ld, "-o", out_elf, src]
	r = run(cmd, timeout=30)
	return r.returncode == 0, r.stderr

def get_syms(elf):
	r = run([NM, elf])
	syms = {}
	for line in r.stdout.splitlines():
		parts = line.split()
		if len(parts) == 3:
			addr, _, name = parts
			syms[name] = int(addr, 16)
	return syms

def run_spike_signature(elf):
	# --instructions=<huge>, empirically: without *some* --instructions
	# value present, spike's default run loop is dramatically slower (a
	# 15s+ hang on tests that complete in ~1s once any --instructions cap,
	# however large, is given) -- a real, reproducible spike/environment
	# quirk found during this verification pass, not a tuning nicety.
	# +signature is spike's own fast, purpose-built arch-test dump (see
	# arch_test_target/spike/device/*/Makefile.include upstream) --
	# unlike --log-commits it doesn't print per-instruction, so it isn't
	# subject to the same slowdown either way.
	sig_path = elf + ".sig"
	if os.path.exists(sig_path): os.remove(sig_path)
	elf_posix = elf.replace("\\", "/")
	spike_posix = SPIKE.replace("\\", "/")
	sig_posix = sig_path.replace("\\", "/")
	bash_cmd = (f'timeout 20 "{spike_posix}" --isa=rv64imafdc_zicsr_zifencei --instructions=100000000 '
	            f'+signature="{sig_posix}" +signature-granularity=4 "{elf_posix}"')
	run([MSYS_BASH, "-lc", bash_cmd], timeout=25)
	if not os.path.exists(sig_path):
		return None
	with open(sig_path) as f:
		words = [int(line.strip(), 16) for line in f if line.strip()]
	os.remove(sig_path)
	return words

def run_doomv_signature(elf, begin, end, halt_addr):
	# DoomV hardcodes the output filename to "signature.log" relative to
	# its cwd (see src/main.cpp) -- not parameterizable from the CLI. To
	# let concurrent invocations (e.g. a manual check alongside a batch)
	# coexist without silently clobbering each other's result, serialize
	# just this run+read critical section with a cross-process lockfile
	# rather than changing DoomV's cwd/output-path assumptions.
	lock_path = os.path.join(DOOMV_DIR, "signature.log.lock")
	import time
	while True:
		try:
			fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
			os.close(fd)
			break
		except FileExistsError:
			# A holder that got force-killed (taskkill, TaskStop, an
			# environment-triggered background-task kill) never reaches
			# the `finally` that removes this file, so a lock older than
			# any single test could plausibly take is abandoned, not
			# actually held -- break it rather than spinning forever.
			try:
				if time.time() - os.path.getmtime(lock_path) > 30:
					os.remove(lock_path)
			except FileNotFoundError:
				pass
			time.sleep(0.05)
	try:
		sig_path = os.path.join(DOOMV_DIR, "signature.log")
		if os.path.exists(sig_path): os.remove(sig_path)
		cmd = [DOOMV, DUMMY_WAD, elf, "-march=rv64imafdc_zicsr_zifencei",
		       f"-break=0x{halt_addr:x}", f"-sig={begin:x}:{end:x}"]
		# DoomV is a GUI app whose run() loop never returns on its own --
		# the CPU thread hits the breakpoint and writes signature.log
		# almost immediately, but the process itself must always be
		# killed externally afterward. subprocess.run(timeout=...) with
		# capture_output=True was found to hang indefinitely on some
		# runs (Python's pipe-drain after .kill() waiting on a handle
		# that Windows/the emulated process never actually released) --
		# so instead: no pipes to drain (stdout/stderr -> DEVNULL), poll
		# for the signature file directly, and force-kill via taskkill
		# /T (kills the whole process tree, not just the main thread's
		# process object) once it's done or a deadline is hit.
		devnull = subprocess.DEVNULL
		proc = subprocess.Popen(cmd, cwd=DOOMV_DIR, stdout=devnull, stderr=devnull)
		import time
		deadline = time.time() + 10
		last_size = -1
		stable_since = None
		while time.time() < deadline:
			if os.path.exists(sig_path):
				size = os.path.getsize(sig_path)
				if size == last_size and size > 0:
					if stable_since is None:
						stable_since = time.time()
					elif time.time() - stable_since > 0.2:
						break
				else:
					stable_since = None
				last_size = size
			time.sleep(0.05)
		subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
		               capture_output=True)
		try:
			proc.wait(timeout=5)
		except subprocess.TimeoutExpired:
			pass
		if not os.path.exists(sig_path):
			return None
		with open(sig_path) as f:
			return [int(line.strip(), 16) for line in f if line.strip()]
	finally:
		os.remove(lock_path)

def run_one(src_path, category):
	name = os.path.basename(src_path)
	flen, freg, fregwidth = (64, "fld", 8) if category == "D" else (32, "flw", 4)
	# Unique per-process (not just per-test) so concurrent invocations --
	# e.g. a manual one-off check running alongside a batch -- can't
	# clobber each other's scratch ELF/signature files.
	tag = f"{os.getpid()}_{name.replace('.', '_')}"
	spike_elf = os.path.join(ROOT, f"tmp_{tag}_spike.elf")
	doomv_elf = os.path.join(ROOT, f"tmp_{tag}_doomv.elf")
	spike_link = os.path.join(SPIKE_ENV, "link.ld")
	doomv_env = os.path.join(SRC_ROOT, "doomv_env")
	doomv_link = os.path.join(doomv_env, "link.ld")

	ok, err = compile_test(src_path, SPIKE_ENV, spike_link, spike_elf, flen, freg, fregwidth)
	if not ok:
		return "COMPILE_FAIL_SPIKE", err[-500:]
	ok, err = compile_test(src_path, doomv_env, doomv_link, doomv_elf, flen, freg, fregwidth)
	if not ok:
		return "COMPILE_FAIL_DOOMV", err[-500:]

	doomv_syms = get_syms(doomv_elf)
	db, de = doomv_syms.get("begin_signature"), doomv_syms.get("end_signature")
	halt = doomv_syms.get("rvtest_halt_doomv")
	if db is None or de is None or halt is None:
		return "NO_SIGNATURE_SYMS", ""

	spike_words = run_spike_signature(spike_elf)
	doomv_words = run_doomv_signature(doomv_elf, db, de, halt)

	if spike_words is None:
		return "SPIKE_NO_OUTPUT", ""
	if doomv_words is None:
		return "DOOMV_NO_OUTPUT", ""
	if len(spike_words) != len(doomv_words):
		return "LENGTH_MISMATCH", f"spike={len(spike_words)} doomv={len(doomv_words)}"

	diffs = [(i, s, d) for i, (s, d) in enumerate(zip(spike_words, doomv_words)) if s != d]
	if diffs:
		detail = "; ".join(f"word{i}: spike={s:08x} doomv={d:08x}" for i, s, d in diffs[:5])
		return "MISMATCH", f"{len(diffs)}/{len(spike_words)} words differ: {detail}"
	return "PASS", f"{len(spike_words)} words match"

if __name__ == "__main__":
	src = sys.argv[1]
	category = sys.argv[2] if len(sys.argv) > 2 else "F"
	status, detail = run_one(src, category)
	print(f"{status}\t{os.path.basename(src)}\t{detail}")
