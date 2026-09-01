#pragma once
#include "riscv_decoder.hpp" // for DecodedInstruction, embedded in HistoryEntry below
#include <cstdint>

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
	static constexpr int HISTORY_SIZE = 4096;

	Registers();

	uint64_t read_x(int i) const;
	void write_x(int i, uint64_t value);

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

	uint64_t get_pc() const;
	void set_pc(uint64_t value);

	// Current privilege level. Defaults to M -- every prior version of
	// this project ran exclusively in M-mode, so M is the only backward-
	// compatible reset state. Nothing outside trap-entry/xRET (ext_zicsr.cpp)
	// and the MMU should ever need to write this.
	PrivMode get_priv() const;
	void set_priv(PrivMode mode);

	uint64_t read_csr(uint16_t addr) const;
	void write_csr(uint16_t addr, uint64_t value);

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

	void record_history(uint64_t pc, uint32_t instr, const DecodedInstruction &decoded);
	const HistoryEntry &history_at(int index) const;
	int history_pos() const;

	// Distinct CSR *addresses* touched by CSR instructions (exec_32ZICSR
	// calls this once per CSRR*/CSRR*I), most-recently-used first -- for
	// the dashboard's CSRs panel, which wants "what's actually in use"
	// rather than every address in the 4096-entry space (nothing touches
	// almost all of it). Separate from history[] above, which tracks
	// executed instructions broadly, not CSR addresses specifically.
	static constexpr int CSR_HISTORY_SIZE = 20;
	void record_csr_access(uint16_t addr);
	uint16_t csr_history_at(int index) const; // 0 = most recently accessed
	int csr_history_count() const;

private:
	uint64_t x[32];
	double f[32];
	uint8_t v[32][VLEN_BYTES];
	uint64_t pc;
	PrivMode priv;
	uint64_t csr[4096];
	uint8_t frm;
	uint8_t fflags;

	uint64_t vtype;
	uint64_t vl;
	uint64_t vstart;
	uint8_t vxrm;
	uint8_t vxsat;

	HistoryEntry history[HISTORY_SIZE];
	int history_ptr;

	uint16_t csr_history[CSR_HISTORY_SIZE];
	int csr_history_len; // valid entries so far -- grows to CSR_HISTORY_SIZE, then stays
};
