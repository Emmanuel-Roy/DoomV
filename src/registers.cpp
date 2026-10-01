#include <cstdlib>
#include "registers.hpp"
#include "event_gen.hpp"
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
	bump_event_gen();
}



void Registers::write_csr(uint16_t addr, uint64_t value)
{
	// An mstatus write that changes none of the bits translation, PMP or the
	// counter enables depend on -- the interrupt enables, FS/VS/XS and the
	// like -- leaves every cache keyed on state_gen valid. EventGen still
	// moves, since the interrupt enables are exactly what it is for. SUM and
	// MXR are in the data key instead (see Registers::data_key), and MPP and
	// MPV change a translation only while MPRV borrows them, so they count
	// only then. DOOMV_COARSE=1 restores the old behaviour, for comparison.
	static const bool coarse = std::getenv("DOOMV_COARSE") != nullptr;
	if (addr == 0x300 && !coarse) {
		const uint64_t KEYED = (1ull << 17) /*MPRV*/ | (((csr[addr] | value) & (1ull << 17)) ? (3ull << 11) | (1ull << 39) : 0) /*MPP MPV only matter under MPRV*/
		                         | (1ull << 20) /*TVM*/ | (1ull << 6) /*UBE*/
		                         | (1ull << 36) /*SBE*/ | (1ull << 37) /*MBE*/ | (1ull << 38) /*GVA*/;
		const uint64_t diff = csr[addr] ^ value;
		csr[addr] = value;
		if (diff & KEYED) state_gen++;
		// Only a change can change an interrupt decision. Every FP instruction
		// that writes FP state marks FS dirty with an mstatus write, almost
		// always of the value already there; bumping for it sent the step
		// after every such instruction back through the interrupt check --
		// 475M times in 3G steps of the XFCE session starting.
		if (diff) bump_event_gen();
		if (csr_log) csr_log->push_back(addr);
		return;
	}
	// Every other CSR bumps state_gen when its value changes -- a CSR read
	// comes through here with the old value, and a read changes nothing --
	// except the ones below, which no state_gen-keyed decision reads at all.
	// Linux writes the first row on every trap, and DoomV writes `time` on
	// every rdtime. Leaving a CSR off this list costs speed; putting one on it
	// that translation, PMP or the counter enables read breaks the caches.
	//
	// EventGen, which says the interrupt check must be made again, follows the
	// same rule for the same reason: it moves only when a CSR's value changes,
	// and not for the CSRs on the list -- except stimecmp, a compare value the
	// interrupt deadline reads (DoomSystem::interrupt_may_be_due).
	const bool same = csr[addr] == value;
	csr[addr] = value;
	bool interrupts = !same;
	switch (addr) {
	case 0x140: case 0x141: case 0x142: case 0x143:   // sscratch sepc scause stval
	case 0x340: case 0x341: case 0x342: case 0x343:   // mscratch mepc mcause mtval
	case 0x001: case 0x002: case 0x003:               // fflags frm fcsr
	case 0x008: case 0x009: case 0x00A:               // vstart vxsat vxrm
	case 0xC00: case 0xC01: case 0xC02:               // cycle time instret
		interrupts = false;
		break;
	case 0x14D:                                       // stimecmp
		break;
	default:
		if (!same) state_gen++;
	}
	if (interrupts) bump_event_gen();
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
