#include "registers.hpp"
#include <cstring>

// vtype.vill=1 (bit 63) is the spec-mandated reset state: no vset{i}vl{i}
// has run yet, so the vector unit's configuration is not yet legal to use.
Registers::Registers()
	: pc(0), frm(0), fflags(0),
	  vtype(1ull << 63), vl(0), vstart(0), vxrm(0), vxsat(0),
	  history_ptr(0)
{
	for (int i = 0; i < 32; i++) { x[i] = 0; f[i] = 0.0; std::memset(v[i], 0, VLEN_BYTES); }
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

const uint8_t *Registers::read_v(int i) const
{
	return v[i];
}

uint8_t *Registers::write_v(int i)
{
	return v[i];
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

uint64_t Registers::get_vtype() const
{
	return vtype;
}

void Registers::set_vtype(uint64_t value)
{
	vtype = value;
}

uint64_t Registers::get_vl() const
{
	return vl;
}

void Registers::set_vl(uint64_t value)
{
	vl = value;
}

uint64_t Registers::get_vstart() const
{
	return vstart;
}

void Registers::set_vstart(uint64_t value)
{
	vstart = value;
}

uint8_t Registers::get_vxrm() const
{
	return vxrm;
}

void Registers::set_vxrm(uint8_t mode)
{
	vxrm = mode & 0x3;
}

uint8_t Registers::get_vxsat() const
{
	return vxsat;
}

void Registers::set_vxsat(uint8_t flag)
{
	vxsat = flag & 0x1;
}

void Registers::or_vxsat(uint8_t flag)
{
	vxsat |= (flag & 0x1);
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
