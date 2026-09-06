import os, sys, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_arch_test import run_one

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC_ROOT = os.path.join(ROOT, "arch-test-src", "riscv-test-suite")

LOG = os.path.join(ROOT, "run_ima_c_results.log")

done = set()
if os.path.exists(LOG):
	with open(LOG) as f:
		for line in f:
			parts = line.split("\t")
			if len(parts) >= 2:
				done.add(parts[1].strip())

# DoomV implements RV64 only (no RV32 mode), so only rv64i_m/* test
# directories apply. rv32i_m/* tests were compiled/run with the same
# rv64 march/isa flags as everything else, which doesn't match what
# those tests assume -- spike halts prematurely on them, producing
# signature mismatches that reflect the wrong methodology, not a real
# DoomV or spike bug.
dirs = [
	("rv64i_m/I/src", "I"),
	("rv64i_m/M/src", "M"),
	("rv64i_m/A/src", "A"),
	("rv64i_m/C/src", "C"),
	("rv64i_m/Zifencei/src", "I"),
]

total = len(done)
for rel, cat in dirs:
	files = sorted(glob.glob(os.path.join(SRC_ROOT, rel, "*.S")))
	for f in files:
		name = os.path.basename(f)
		if name in done:
			continue
		total += 1
		try:
			status, detail = run_one(f, cat)
		except Exception as e:
			status, detail = "EXCEPTION", str(e)[:300]
		print(f"[{total}] {status}\t{name}\t{detail}", flush=True)

print("\n==== IMA+C BATCH DONE ====")
