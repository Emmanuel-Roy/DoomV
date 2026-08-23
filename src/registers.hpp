#pragma once
#include <cstdint>

struct HistoryEntry {
	uint64_t pc;
	uint32_t instr; // still max 32 bits wide -- RV64 has no wider instruction encoding
	const char *mnemonic;
};

class Registers {
public:
	static constexpr int HISTORY_SIZE = 4096;

	Registers();

	uint64_t read_x(int i) const;
	void write_x(int i, uint64_t value);

	uint64_t get_pc() const;
	void set_pc(uint64_t value);

	uint64_t read_csr(uint16_t addr) const;
	void write_csr(uint16_t addr, uint64_t value);

	void record_history(uint64_t pc, uint32_t instr, const char *mnemonic);
	const HistoryEntry &history_at(int index) const;
	int history_pos() const;

private:
	uint64_t x[32];
	uint64_t pc;
	uint64_t csr[4096];

	HistoryEntry history[HISTORY_SIZE];
	int history_ptr;
};
