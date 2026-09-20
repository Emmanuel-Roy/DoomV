#pragma once
#include "event_gen.hpp"
#include "riscv_decoder.hpp" // for DecodedInstruction, embedded in HistoryEntry below
#include <cstdint>
#include <vector>

// Values match the spec's own privilege encoding (used directly in
// mstatus.MPP/sstatus.SPP, both 2-bit fields with this same numbering --
// there is no privilege level encoded as 2) so a trap/return can just
// store/compare this enum without a translation table.
enum class PrivMode : uint8_t {
	U = 0,
	S = 1,
	M = 3,
};

struct HistoryEntry {
	uint64_t pc;
	uint32_t instr; // raw fetched bytes, still max 32 bits wide -- RV64 has no wider instruction encoding
	// Full decode, not just the mnemonic string -- lets the dashboard/
	// trace log render actual operands ("ADDI a0, a0, 4") instead of a
	// bare "ADDI". DecodedInstruction defaults its own mnemonic to "???",
	// so a default-constructed HistoryEntry (the ring buffer's initial
	// fill) is already safe to display without a separate placeholder.
	DecodedInstruction decoded;
};

class Registers {
public:
	static constexpr int HISTORY_SIZE = 4096; // a power of two -- record_history masks with it
	static_assert((HISTORY_SIZE & (HISTORY_SIZE - 1)) == 0, "HISTORY_SIZE must be a power of two");

	Registers();

	// The accessors every instruction calls several times, defined here so
	// they inline: the build has no link-time optimization to do it across
	// files (see the Makefile), and as calls they were several per cent of
	// the run on their own.
	uint64_t read_x(int i) const { return x[i]; }
	void write_x(int i, uint64_t value) { if (i != 0) x[i] = value; }

	// FP registers, unlike x0, have no hardwired-zero special case -- all
	// 32 are ordinary read/write storage. Always 64 bits wide (NaN-boxed
	// when holding a single-precision value alongside D) -- see riscv_core.
	double read_f(int i) const;
	void write_f(int i, double value);

	// V (vector) register file. Raw bytes, not a fixed element type: a real
	// vector register gets reinterpreted at different element widths (SEW)
	// from one instruction to the next, so an untyped byte array is the
	// correct representation, not e.g. an array of uint32_t. VLEN=128 is a
	// common, modest choice -- nothing outside Registers assumes a specific
	// value. write_v() hands back a mutable pointer rather than an
	// element-level setter because the V extension code needs to memcpy
	// arbitrary EEW-sized/LMUL-grouped spans across register boundaries;
	// that addressing logic lives with the V instructions, not here.
	static constexpr int VLEN_BITS = 128;
	static constexpr int VLEN_BYTES = VLEN_BITS / 8;
	const uint8_t *read_v(int i) const;
	uint8_t *write_v(int i);

	uint64_t get_pc() const { return pc; }
	void set_pc(uint64_t value) { pc = value; }

	// Current privilege level. Defaults to M -- every prior version of
	// this project ran exclusively in M-mode, so M is the only backward-
	// compatible reset state. Nothing outside trap-entry/xRET (ext_zicsr.cpp)
	// and the MMU should ever need to write this.
	PrivMode get_priv() const { return priv; }
	void set_priv(PrivMode mode);

	// Bumped by every CSR write and every change of privilege or V. Caches of
	// decisions that depend only on those -- whether an interrupt could be
	// taken, which counters count, whether a code page may be fetched from --
	// compare it with the value they were made under.
	uint64_t state_gen = 0;

	// Virtualisation mode (the H extension's V bit). Orthogonal to the
	// privilege level rather than another value of it: the hart is in one
	// of M, HS, VS, HU or VU, which is (priv, virt) rather than a single
	// five-valued enum. Keeping them separate is what lets every existing
	// priv comparison keep working unchanged -- VS-mode code is still
	// PrivMode::S, and only the places that genuinely care about
	// virtualisation ask for get_virt().
	//
	// M-mode is never virtual, so set_virt(true) is only ever meaningful
	// alongside S or U.
	bool get_virt() const { return virt; }
	void set_virt(bool v) { virt = v; state_gen++; bump_event_gen(); }

	uint64_t read_csr(uint16_t addr) const { return csr[addr]; }
	void write_csr(uint16_t addr, uint64_t value);
	// While set, every write_csr appends its address (lockstep.cpp).
	std::vector<uint16_t> *csr_log = nullptr;
	// A counter's own increment: not a write, so not logged.
	void bump_csr(uint16_t addr) { csr[addr]++; }
	// Whether minstret counts the instruction now executing. Decided before
	// it runs, from mcountinhibit.IR and minstretcfg, and cleared by a write
	// to minstret, which then holds the value written: Sail's
	// minstret_increment.
	bool minstret_increment = false;

	// fflags/frm are the two fields fcsr (CSR 0x003) packs together --
	// dedicated storage instead of the generic csr[] array because fflags
	// has accumulate (OR) semantics from FP ops as well as overwrite
	// semantics from CSR writes, which the generic array can't express.
	uint8_t get_frm() const;
	void set_frm(uint8_t mode);
	uint8_t get_fflags() const;
	void set_fflags(uint8_t flags);
	void or_fflags(uint8_t bits);

	// V's own CSR-ish state. vtype/vl are nominally CSRs (0xC21/0xC20) but
	// are read-only except through vset{i}vl{i} -- exec_32ZICSR still lets
	// a plain CSRRS/etc *read* them (matching real hardware), it just never
	// routes a write there. vstart(0x008)/vxrm(0x00A)/vxsat(0x009) are
	// ordinary read-write CSRs, dedicated fields for the same reason
	// fflags/frm are: vxsat in particular accumulates (OR) from fixed-point
	// ops. vtype is stored as its raw 64-bit encoding (vill at bit 63, vma/
	// vta/vsew/vlmul packed in the low byte per spec) -- decoding those
	// fields into SEW/LMUL is V-instruction-execution logic, not state.
	uint64_t get_vtype() const;
	void set_vtype(uint64_t value);
	uint64_t get_vl() const;
	void set_vl(uint64_t value);
	uint64_t get_vstart() const;
	void set_vstart(uint64_t value);
	uint8_t get_vxrm() const;
	void set_vxrm(uint8_t mode);
	uint8_t get_vxsat() const;
	void set_vxsat(uint8_t flag);
	void or_vxsat(uint8_t flag);

	// The last HISTORY_SIZE instructions, as pc and encoding only. It is
	// written before every instruction, so it holds no more than crash.log
	// prints; the dashboard decodes the few entries it shows.
	struct HistoryRecord { uint64_t pc; uint32_t instr; };
	void record_history(uint64_t pc, uint32_t instr)
	{
		history[history_ptr] = { pc, instr };
		// & rather than %: HISTORY_SIZE is a power of two, but history_ptr is
		// signed, so the compiler has to emit the sign-correcting sequence for
		// the modulo -- it cannot know the value never goes negative. The mask
		// is the same answer for every value this actually takes.
		history_ptr = (history_ptr + 1) & (HISTORY_SIZE - 1);
	}
	const HistoryRecord &history_at(int index) const { return history[index]; }
	int history_pos() const { return history_ptr; }

	// Which CSRs the guest is busy with, for the dashboard's CSRs panel.
	//
	// A sliding window, like the instruction trace: the last CSR_WINDOW
	// CSR accesses (exec_32ZICSR records one per CSRR*/CSRR*I), with a
	// count per address kept in step as accesses enter and leave it. The
	// panel shows the CSR_TOP most frequent. That is a different question
	// from "most recently touched", which is what this used to answer:
	// recency lists every CSR a trap path brushes past, while frequency
	// over a window shows what the machine is actually doing right now,
	// and lets a CSR the guest stopped using fall off by itself.
	static constexpr int CSR_WINDOW = 1024;
	static constexpr int CSR_TOP = 10;
	void record_csr_access(uint16_t addr);
	// Fills `out` with up to `max` addresses, most frequent in the window
	// first, and returns how many. Ties go to the lower address, so rows
	// with equal counts do not swap places from one frame to the next.
	int top_csrs(uint16_t out[], int max) const;

	// The guest physical address of a G-stage fault, set by the MMU and
	// consumed by the trap path. It cannot be written straight to a CSR at
	// the point of the fault, because which CSR it belongs in is not known
	// until delegation has been resolved: htval when the trap is taken to
	// HS-mode, mtval2 when it is taken to M. Writing htval unconditionally
	// left mtval2 at zero for every undelegated guest page fault, and a
	// firmware handler that forwards such a trap by copying mtval2 into
	// htval then overwrote the one correct value with that zero.
	uint64_t pending_gpa = 0;

	// The pseudoinstruction htinst reports for a fault taken on an
	// *implicit* access -- one the hardware made walking the guest's page
	// tables, not one the program asked for. The architecture defines two:
	// 0x3000 for the read of a PTE, 0x3020 for the write that updates its
	// A/D bits. A hypervisor uses this to tell "the guest's load faulted"
	// from "the walk for the guest's load faulted", which are different
	// problems with different fixes. Zero means "not provided", which is
	// legal for explicit accesses and wrong for these two.
	uint64_t pending_htinst = 0;

	// Zicfilp's expected-landing-pad bit. Live hart state rather than a
	// CSR: it is armed by an indirect jump and disarmed by the `lpad` that
	// follows, and it only becomes visible in a register when a trap saves
	// it into the trapping mode's xPELP. See ext_zicfilp.hpp.
	bool elp = false;

private:
	uint64_t x[32];
	double f[32];
	uint8_t v[32][VLEN_BYTES];
	uint64_t pc;
	PrivMode priv;
	bool virt = false; // the H extension's V bit -- see get_virt()
	uint64_t csr[4096];

	uint8_t frm;
	uint8_t fflags;

	uint64_t vtype;
	uint64_t vl;
	uint64_t vstart;
	uint8_t vxrm;
	uint8_t vxsat;

	HistoryRecord history[HISTORY_SIZE];
	int history_ptr;

	uint16_t csr_window[CSR_WINDOW];  // ring of recent accesses
	int csr_window_pos;               // where the next one goes
	int csr_window_len;               // grows to CSR_WINDOW, then stays
	uint16_t csr_counts[4096];        // occurrences of each address in the ring
};
