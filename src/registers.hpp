#pragma once
#include <cstdint>

struct HistoryEntry {
	uint32_t pc;
	uint32_t instr;
};

class Registers {
public:
	static constexpr int HISTORY_SIZE = 4096;

	Registers();

	uint32_t read_x(int i) const;
	void write_x(int i, uint32_t value);

	uint32_t get_pc() const;
	void set_pc(uint32_t value);

	uint32_t read_csr(uint16_t addr) const;
	void write_csr(uint16_t addr, uint32_t value);

	void record_history(uint32_t pc, uint32_t instr);
	const HistoryEntry &history_at(int index) const;
	int history_pos() const;

private:
	uint32_t x[32];
	uint32_t pc;
	uint32_t csr[4096];

	HistoryEntry history[HISTORY_SIZE];
	int history_ptr;
};
