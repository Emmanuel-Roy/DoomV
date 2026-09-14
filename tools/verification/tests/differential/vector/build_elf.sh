#!/usr/bin/env bash
# Builds one differential test into an ELF. Nothing else -- no reference is
# run, no signature is dumped.
#
# Usage (run inside WSL):  build_elf.sh <test_basename>
#
# This exists because the build used to live inside spike_sig.sh, which meant
# a Sail run had to invoke spike first just to get an ELF. Sail is the golden
# reference for RVA23S64; it must not depend on the model it outranks being
# installed. The per-test MARCH lives here now, and spike_sig.sh sources this
# file for the ISA string it still needs.
set -euo pipefail

TEST="${1:?usage: build_elf.sh <test_basename>}"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

TOOLS="${DOOMV_TOOLS:-$HOME/.local/share/doomv}"
if [ -x "$TOOLS/xpack/bin/riscv-none-elf-gcc" ]; then
    CC="$TOOLS/xpack/bin/riscv-none-elf-gcc"
else
    CC=riscv64-linux-gnu-gcc
fi

# Each test needs its own extension set. VLEN is pinned to 128 on the
# reference side because DoomV is fixed there (Registers::VLEN_BITS), and
# nothing is comparable unless the two agree.
case "$TEST" in
  vtest_v)  MARCH=rv64gcv_zvbb
            ISA=rv64gcv_zvbb_zvl128b_zicsr_zifencei ;;
  vtest_zvfh) MARCH=rv64gcv_zvfhmin
            ISA=rv64gcv_zvfhmin_zvl128b_zicsr_zifencei ;;
  vtest_zvbb) MARCH=rv64gcv_zvbb
            ISA=rv64gcv_zvbb_zvl128b_zicsr_zifencei ;;
  vtest_restart) MARCH=rv64gcv
            ISA=rv64gcv_zvl128b_zicsr_zifencei ;;
  vtest_trap) MARCH=rv64gcv
            ISA=rv64gcv_zvl128b_zicsr_zifencei ;;
  vtest_stateen) MARCH=rv64gch_smstateen_sscofpmf
            ISA=rv64gch_smstateen_sscofpmf_zicsr_zifencei ;;
  vtest_hdeleg) MARCH=rv64gch
            ISA=rv64gch_zicsr_zifencei ;;
  vtest_hgatp) MARCH=rv64gch
            ISA=rv64gch_zicsr_zifencei ;;
  vtest_hlv) MARCH=rv64gch
            ISA=rv64gch_zicsr_zifencei ;;
  vtest_h) MARCH=rv64gch
            ISA=rv64gch_zicsr_zifencei ;;
  vtest_pm) MARCH=rv64gc
            ISA=rv64gc_ssnpm_smnpm_zicsr_zifencei ;;
  vtest_sv) MARCH=rv64gc_svinval
            ISA=rv64gc_svinval_svnapot_svpbmt_zicsr_zifencei ;;
  vtest_mmu) MARCH=rv64gcv
            ISA=rv64gcv_zvl128b_zicsr_zifencei ;;
  vtest_zfh) MARCH=rv64gc_zfhmin
            ISA=rv64gc_zfhmin_zicsr_zifencei ;;
  vtest_zfa) MARCH=rv64gc_zfa
            ISA=rv64gc_zfa_zicsr_zifencei ;;
  vtest_fd) MARCH=rv64gc
            ISA=rv64gc_zicsr_zifencei ;;
  vtest_hints) MARCH=rv64gc_zihintpause_zihintntl_zimop_zcmop_zicbom_zicbop
            ISA=rv64gc_zihintpause_zihintntl_zimop_zcmop_zicbom_zicbop_zicsr_zifencei ;;
  vtest_csr) MARCH=rv64gc_zicboz_zawrs
            ISA=rv64gc_zicboz_zawrs_zicntr_zihpm_zicsr_zifencei ;;
  vtest_zb) MARCH=rv64gc_zba_zbb_zbs_zicond_zcb
            ISA=rv64gc_zba_zbb_zbs_zicond_zcb_zicsr_zifencei ;;
  *) echo "unknown test: $TEST" >&2; exit 1 ;;
esac
export MARCH ISA

# Sourced (BUILD_ELF_NO_RUN set) when the caller only wants MARCH/ISA.
if [ -z "${BUILD_ELF_NO_RUN:-}" ]; then
	# Never let a failed compile reuse yesterday's ELF.
	rm -f "$TEST.elf"
	"$CC" -march="$MARCH" -mabi=lp64d -static -mcmodel=medany -nostdlib -nostartfiles \
	    -T vtest_v.lds -o "$TEST.elf" "$TEST.S"
	[ -f "$TEST.elf" ] || { echo "ERROR: $TEST.elf was not produced" >&2; exit 1; }
fi
