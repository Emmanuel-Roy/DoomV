#include "registers.hpp"
#include <cstring>

// vtype.vill=1 (bit 63) is the spec-mandated reset state: no vset{i}vl{i}
// has run yet, so the vector unit's configuration is not yet legal to use.
Registers::Registers()
	: pc(0), priv(PrivMode::M), frm(0), fflags(0),
	  vtype(1ull << 63), vl(0), vstart(0), vxrm(0), vxsat(0),
	  history_ptr(0), csr_window_pos(0), csr_window_len(0)
{
	std::memset(csr_window, 0, sizeof(csr_window));
	std::memset(csr_counts, 0, sizeof(csr_counts));
	for (int i = 0; i < 32; i++) { x[i] = 0; f[i] = 0.0; std::memset(v[i], 0, VLEN_BYTES); }
	for (int i = 0; i < 4096; i++) csr[i] = 0;
	for (int i = 0; i < HISTORY_SIZE; i++) history[i] = {0, 0};
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







void Registers::set_priv(PrivMode mode)
{
	priv = mode;
	state_gen++;
}



void Registers::write_csr(uint16_t addr, uint64_t value)
{
	csr[addr] = value;
	state_gen++;
	if (csr_log) csr_log->push_back(addr);
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



void Registers::record_csr_access(uint16_t addr)
{
	// Constant time, since this is on the path of every CSR instruction:
	// the access leaving the window gives back its count, the new one
	// takes one. Nothing is sorted here; top_csrs does that when asked.
	addr &= 0xFFF;
	if (csr_window_len == CSR_WINDOW) csr_counts[csr_window[csr_window_pos]]--;
	else csr_window_len++;
	csr_window[csr_window_pos] = addr;
	csr_counts[addr]++;
	csr_window_pos = (csr_window_pos + 1) % CSR_WINDOW;
}

int Registers::top_csrs(uint16_t out[], int max) const
{
	// A scan of all 4096 counts with a small insertion-sorted top list.
	// It runs once per published snapshot, not per instruction, and at
	// max=10 that is a few tens of thousands of comparisons at most.
	int n = 0;
	for (int a = 0; a < 4096; a++) {
		const uint16_t c = csr_counts[a];
		if (c == 0) continue;
		if (n == max && c <= csr_counts[out[n - 1]]) continue;
		int i = (n < max) ? n++ : n - 1;
		// Strictly greater moves up, so an equal count stays behind the
		// lower address already placed -- scanning upward makes that the
		// stable tie-break.
		while (i > 0 && csr_counts[out[i - 1]] < c) { out[i] = out[i - 1]; i--; }
		out[i] = (uint16_t)a;
	}
	return n;
}
