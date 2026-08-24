#pragma once
#include "riscv_decoder.hpp" // for DecodedInstruction, embedded in HistoryEntry below
#include <cstdint>

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

	// V (vector) register file -- storage only for now; no V instruction
	// exists yet to read/write it (classify() never produces Extension::V),
	// this is purely groundwork for when it does. Raw bytes, not a fixed
	// element type: a real vector register gets reinterpreted at different
	// element widths (SEW) from one instruction to the next, so an untyped
	// byte array is the correct representation, not e.g. an array of
	// uint32_t. VLEN=128 is a common, modest choice for a first
	// implementation -- easy to widen later, nothing outside Registers
	// assumes a specific value.
	static constexpr int VLEN_BITS = 128;
	static constexpr int VLEN_BYTES = VLEN_BITS / 8;
	const uint8_t *read_v(int i) const;

	uint64_t get_pc() const;
	void set_pc(uint64_t value);

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

	void record_history(uint64_t pc, uint32_t instr, const DecodedInstruction &decoded);
	const HistoryEntry &history_at(int index) const;
	int history_pos() const;

private:
	uint64_t x[32];
	double f[32];
	uint8_t v[32][VLEN_BYTES];
	uint64_t pc;
	uint64_t csr[4096];
	uint8_t frm;
	uint8_t fflags;

	HistoryEntry history[HISTORY_SIZE];
	int history_ptr;
};
