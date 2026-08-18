#include "registers.hpp"

Registers::Registers() : pc(0), history_ptr(0)
{
	for (int i = 0; i < 32; i++) x[i] = 0;
	for (int i = 0; i < 4096; i++) csr[i] = 0;
	for (int i = 0; i < HISTORY_SIZE; i++) history[i] = {0, 0, "???"};
}

uint32_t Registers::read_x(int i) const
{
	return x[i];
}

void Registers::write_x(int i, uint32_t value)
{
	if (i != 0) x[i] = value;
}

uint32_t Registers::get_pc() const
{
	return pc;
}

void Registers::set_pc(uint32_t value)
{
	pc = value;
}

uint32_t Registers::read_csr(uint16_t addr) const
{
	return csr[addr];
}

void Registers::write_csr(uint16_t addr, uint32_t value)
{
	csr[addr] = value;
}

void Registers::record_history(uint32_t pc_val, uint32_t instr, const char *mnemonic)
{
	history[history_ptr] = {pc_val, instr, mnemonic};
	history_ptr = (history_ptr + 1) % HISTORY_SIZE;
}

const HistoryEntry &Registers::history_at(int index) const
{
	return history[index];
}

int Registers::history_pos() const
{
	return history_ptr;
}
