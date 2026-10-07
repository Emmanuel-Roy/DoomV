/*
 * DoomV as a library: lock-step a core against DoomV in the same process.
 *
 * For a core's simulation to check itself one retired instruction at a time
 * -- its C++ compiled natively (Vitis HLS C simulation, "software
 * emulation"), or a testbench reading what its RTL retired (Vitis HLS
 * co-simulation in XSim, "hardware emulation", or Verilator) -- with no trace
 * file in between. The comparison is DoomV's -lockstep, record for record
 * (src/lockstep.cpp): privilege, pc and instruction; every register and CSR
 * write; every store, by physical address; traps with their cause, epc, tval
 * and the CSRs trap entry writes.
 *
 * A plain C interface, so any compiler links it: the DLL is built with
 * DoomV's own (make lockstep-lib, build/lockstep-lib/), statically -- it
 * needs nothing beside it -- and called from Vitis's MinGW g++, from C, or
 * from SystemVerilog through DPI-C (doomv_ls_open_line, doomv_ls_step_text:
 * a chandle, strings and ints).
 *
 *     doomv_ls *ls = doomv_ls_open_line("-ng -lockstep-strict -cycle-clock=100 "
 *                                       "-march=... vtest.elf ...");
 *     for each record the core retires:
 *         if (doomv_ls_step(ls, &rec) != DOOMV_LS_MATCH) {
 *             puts(doomv_ls_message(ls));
 *             break;
 *         }
 *     doomv_ls_close(ls);
 *
 * The machine is the one riscv_doom's command line describes, from reset or
 * -restore=<snapshot>; one per process. The records are the core's, and its
 * order is followed: with several harts, each record names its hart, and that
 * hart steps.
 *
 * The clock. With -cycle-clock=<n> each record carries the core's cycle
 * count, and DoomV's mtime and mcycle are what that count makes them: before
 * the record's step, every hart's mcycle advances by the cycles since the
 * last record (where mcountinhibit and the Smcntrpmf filters let it), and
 * mtime by one tick per <n> cycles. Counter and time reads then match by
 * construction, and a timer interrupt is due in DoomV at the cycle it is due
 * in the core, so -lockstep-strict holds the core to DoomV's interrupts too.
 * The count is the one the record's instruction sees, starting from 0 where
 * the run starts (reset, or the restored state), the remainder towards the
 * next mtime tick starting at 0 too. A WFI completes or traps by the state at
 * its own stamp: no time passes inside it as DoomV sees it, and the core's
 * wait is the gap between its stamp and the one before. Without
 * -cycle-clock, the clock is Sail's: one mtime tick every two steps.
 *
 * -lockstep-strict compares everything; without it, what a core's own clock
 * and devices decide (counter and time reads, interrupt-pending state, loads
 * from anything that is not RAM) is taken from its record, and interrupts are
 * taken where it took them. -lockstep-take=<base>:<size> takes loads from a
 * range from the record even when strict.
 */
#ifndef DOOMV_LOCKSTEP_H
#define DOOMV_LOCKSTEP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(DOOMV_LS_BUILD)
#    define DOOMV_LS_API __declspec(dllexport)
#  else
#    define DOOMV_LS_API __declspec(dllimport)
#  endif
#else
#  define DOOMV_LS_API
#endif

/* This header's interface version: doomv_ls_version() returns it. */
#define DOOMV_LS_VERSION 1

typedef struct doomv_ls doomv_ls;

/* What a step returns. */
enum {
	DOOMV_LS_MATCH = 0,     /* the record matched */
	DOOMV_LS_MISMATCH = 1,  /* it did not: doomv_ls_message says where and why */
	DOOMV_LS_ERROR = 2,     /* it could not be stepped: a hart the machine lacks */
	DOOMV_LS_STOPPED = 3    /* the machine has stopped: the guest wrote tohost, or it halted */
};

/* A record's kind. */
enum {
	DOOMV_LS_COMMIT = 0,     /* an instruction that completed */
	DOOMV_LS_EXCEPTION = 1,  /* one that trapped, or a fetch that faulted (no HAS_INSN) */
	DOOMV_LS_INTERRUPT = 2   /* an interrupt taken before the next instruction */
};

/* A record's flags. */
enum {
	DOOMV_LS_HAS_INSN = 1,   /* pc, insn and insn_len are the instruction's */
	DOOMV_LS_HAS_CYCLE = 2,  /* cycle is the core's cycle count */
	DOOMV_LS_HAS_EPC = 4,    /* a trap: epc is set */
	DOOMV_LS_HAS_TVAL = 8,   /* a trap: tval is set */
	DOOMV_LS_VIRT = 16       /* the step began with V=1 */
};

/* A register or CSR the step wrote, with the value it left. With several
 * writes to one vector register or CSR, the last counts. */
typedef struct doomv_ls_write {
	char kind;              /* 'x', 'f', 'v', or 'c' for a CSR */
	uint16_t index;         /* the register's number, or the CSR's address */
	uint64_t value;         /* x, f (64 bits, NaN-boxed) and CSRs */
	const uint8_t *bytes;   /* v: the register's VLEN/8 bytes, byte 0 first */
} doomv_ls_write;

/* A store, at its physical address. */
typedef struct doomv_ls_store {
	uint64_t paddr;
	uint32_t size;          /* bytes */
	uint64_t value;         /* up to 8 bytes: the value stored */
	const uint8_t *bytes;   /* more (a cbo.zero block): the bytes, the lowest address first */
} doomv_ls_store;

/* One retired step of one hart. */
typedef struct doomv_ls_record {
	uint32_t hart;
	uint32_t kind;          /* DOOMV_LS_COMMIT, _EXCEPTION or _INTERRUPT */
	uint32_t flags;         /* DOOMV_LS_HAS_INSN and the rest */
	uint32_t priv;          /* privilege the step began in: 0 U, 1 S, 3 M */
	uint64_t cycle;         /* with -cycle-clock: the cycle count the step sees */
	uint64_t pc;            /* HAS_INSN: the instruction's address */
	uint32_t insn;          /* HAS_INSN: its encoding (16 bits for a compressed one) */
	uint32_t insn_len;      /* HAS_INSN: 2 or 4 */
	uint64_t cause;         /* a trap: its cause, without the interrupt bit */
	uint64_t epc, tval;     /* a trap: HAS_EPC, HAS_TVAL */
	const doomv_ls_write *writes;       /* what the instruction wrote */
	uint32_t n_writes;
	const doomv_ls_write *trap_writes;  /* the CSRs trap entry wrote */
	uint32_t n_trap_writes;
	const doomv_ls_store *stores;
	uint32_t n_stores;
} doomv_ls_record;

/* The interface version the library was built with: DOOMV_LS_VERSION. */
DOOMV_LS_API int doomv_ls_version(void);

/* A machine, set up by riscv_doom's arguments (without the program name),
 * plus -lockstep-strict, -cycle-clock=<n>, -lockstep-take=<base>:<size>.
 * NULL if they set up nothing: doomv_ls_open_error says why. */
DOOMV_LS_API doomv_ls *doomv_ls_open(int argc, const char *const *argv);
/* The same, from one string of arguments separated by spaces, an argument
 * with spaces in double quotes. */
DOOMV_LS_API doomv_ls *doomv_ls_open_line(const char *arguments);
DOOMV_LS_API const char *doomv_ls_open_error(void);

/* One record: DOOMV_LS_MATCH, or what went wrong (doomv_ls_message). */
DOOMV_LS_API int doomv_ls_step(doomv_ls *ls, const doomv_ls_record *record);
/* Records as Sail's trace writes them (sail_riscv_sim --trace-instr
 * --trace-gpr --trace-fpr --trace-vreg --trace-csr --trace-mem
 * --trace-exception --trace-interrupt), one or more whole ones: "hart <i>"
 * before the records of another hart, "cycle <n>" before a record its stamp.
 * Each is stepped, in order, until one does not match. */
DOOMV_LS_API int doomv_ls_step_text(doomv_ls *ls, const char *records);

/* What the last step that did not match said: both records, the field that
 * differs. The machine's state is in crash.log. */
DOOMV_LS_API const char *doomv_ls_message(const doomv_ls *ls);
/* Records matched so far, and values taken from them (0 when strict). */
DOOMV_LS_API uint64_t doomv_ls_matched(const doomv_ls *ls);
DOOMV_LS_API uint64_t doomv_ls_taken(const doomv_ls *ls);
/* DoomV's step count, all harts together; a hart's pc; the guest's tohost
 * value once it has written one (0 before). */
DOOMV_LS_API uint64_t doomv_ls_steps(const doomv_ls *ls);
DOOMV_LS_API uint64_t doomv_ls_pc(const doomv_ls *ls, uint32_t hart);
DOOMV_LS_API uint64_t doomv_ls_tohost(const doomv_ls *ls);

/* The whole machine as a DoomV snapshot (riscv_doom -restore=<dir>), or its
 * architectural state for another simulator (-export-state). 0 on success. */
DOOMV_LS_API int doomv_ls_snapshot(doomv_ls *ls, const char *dir);
DOOMV_LS_API int doomv_ls_export_state(doomv_ls *ls, const char *dir);

DOOMV_LS_API void doomv_ls_close(doomv_ls *ls);

#ifdef __cplusplus
}
#endif

#endif /* DOOMV_LOCKSTEP_H */
