#include "registers.hpp"

Registers::Registers() : pc(0), frm(0), fflags(0), history_ptr(0)
{
	for (int i = 0; i < 32; i++) { x[i] = 0; f[i] = 0.0; }
	for (int i = 0; i < 4096; i++) csr[i] = 0;
	for (int i = 0; i < HISTORY_SIZE; i++) history[i] = {0, 0, DecodedInstruction{}};
}

uint64_t Registers::read_x(int i) const
{
	return x[i];
}

void Registers::write_x(int i, uint64_t value)
{
	if (i != 0) x[i] = value;
}

double Registers::read_f(int i) const
{
	return f[i];
}

void Registers::write_f(int i, double value)
{
	f[i] = value;
}

uint64_t Registers::get_pc() const
{
	return pc;
}

void Registers::set_pc(uint64_t value)
{
	pc = value;
}

uint64_t Registers::read_csr(uint16_t addr) const
{
	return csr[addr];
}

void Registers::write_csr(uint16_t addr, uint64_t value)
{
	csr[addr] = value;
}

uint8_t Registers::get_frm() const
{
	return frm;
}

void Registers::set_frm(uint8_t mode)
{
	frm = mode & 0x7;
}

uint8_t Registers::get_fflags() const
{
	return fflags;
}

void Registers::set_fflags(uint8_t flags)
{
	fflags = flags & 0x1F;
}

void Registers::or_fflags(uint8_t bits)
{
	fflags |= (bits & 0x1F);
}

void Registers::record_history(uint64_t pc_val, uint32_t instr, const DecodedInstruction &decoded)
{
	history[history_ptr] = {pc_val, instr, decoded};
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
