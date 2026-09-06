import os, sys, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_arch_test import run_one

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC_ROOT = os.path.join(ROOT, "arch-test-src", "riscv-test-suite")

# Names already recorded in run_all_results.log -- skip those, run the rest.
done = set()
with open(os.path.join(ROOT, "run_all_results.log")) as f:
	for line in f:
		parts = line.split("\t")
		if len(parts) >= 2:
			done.add(parts[1].strip())

dirs = [
	("rv32i_m/F/src", "F"),
	("rv32i_m/D/src", "D"),
	("rv64i_m/F/src", "F"),
	("rv64i_m/D/src", "D"),
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

print("\n==== REMAINING BATCH DONE ====")
