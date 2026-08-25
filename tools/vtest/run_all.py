import os, sys, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_arch_test import run_one

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC_ROOT = os.path.join(ROOT, "arch-test-src", "riscv-test-suite")

dirs = [
	("rv32i_m/F/src", "F"),
	("rv32i_m/D/src", "D"),
	("rv64i_m/F/src", "F"),
	("rv64i_m/D/src", "D"),
]

results = {}
total = 0
for rel, cat in dirs:
	files = sorted(glob.glob(os.path.join(SRC_ROOT, rel, "*.S")))
	for f in files:
		total += 1
		name = os.path.basename(f)
		try:
			status, detail = run_one(f, cat)
		except Exception as e:
			status, detail = "EXCEPTION", str(e)[:300]
		results.setdefault(status, []).append((name, detail))
		print(f"[{total}] {status}\t{name}\t{detail}", flush=True)

print("\n==== SUMMARY ====")
for status, items in sorted(results.items()):
	print(f"{status}: {len(items)}")
