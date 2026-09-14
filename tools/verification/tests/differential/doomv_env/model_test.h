#ifndef _COMPLIANCE_MODEL_H
#define _COMPLIANCE_MODEL_H

// DoomV port of the riscv-arch-test model header. DoomV has no HTIF, so
// unlike the spike_simple env this doesn't write to a tohost mailbox to
// signal completion -- instead RVMODEL_HALT just spins at a labeled
// address, and the test harness runs DoomV with -break=<that address> so
// it halts (and dumps full register/memory state) right there, matching
// the same breakpoint mechanism used for the hand-written V tests earlier
// in this verification pass.
#define RVMODEL_DATA_SECTION \
        .align 8; .global begin_regstate; begin_regstate: \
        .word 128; \
        .align 8; .global end_regstate; end_regstate: \
        .word 4;

#ifndef RVMODEL_PMP_GRAIN
  #define RVMODEL_PMP_GRAIN   0
#endif

#ifndef RVMODEL_NUM_PMPS
  #define RVMODEL_NUM_PMPS    16
#endif

#define RVMODEL_HALT \
        .align 4;                          \
        .global rvtest_halt_doomv;         \
rvtest_halt_doomv:                         \
        j rvtest_halt_doomv

#define RVMODEL_BOOT

// Structurally mirrors spike's own arch_test_target/spike/model_test.h
// exactly (RVMODEL_DATA_SECTION goes *after* end_signature, not before) --
// the two need matching layouts relative to begin/end_signature, or the
// signature region each side dumps stops being a like-for-like comparison
// even before any actual test content is considered.
#define RVMODEL_DATA_BEGIN                                              \
  .align 4;\
  .global begin_signature; begin_signature:

#define RVMODEL_DATA_END                                                \
  .align 4;\
  .global end_signature; end_signature: \
  RVMODEL_DATA_SECTION

#define RVMODEL_IO_INIT
#define RVMODEL_IO_WRITE_STR(_R, _STR)
#define RVMODEL_IO_CHECK()
#define RVMODEL_IO_ASSERT_GPR_EQ(_S, _R, _I)
#define RVMODEL_IO_ASSERT_SFPR_EQ(_F, _R, _I)
#define RVMODEL_IO_ASSERT_DFPR_EQ(_D, _R, _I)

#define RVMODEL_SET_MSW_INT
#define RVMODEL_CLEAR_MSW_INT
#define RVMODEL_CLEAR_MTIMER_INT
#define RVMODEL_CLEAR_MEXT_INT

#endif // _COMPLIANCE_MODEL_H
