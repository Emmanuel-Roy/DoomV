#include "pmp.hpp"
#include "registers.hpp"

namespace pmp {
namespace {

// Entry index for a pmpaddr CSR, and the (csr, byte) pair for an entry's
// configuration byte. On RV64 pmpcfg CSRs are even-numbered and hold eight
// bytes each.
inline unsigned addr_index(uint16_t csr) { return (unsigned)(csr - CSR_PMPADDR0); }

inline uint8_t cfg_byte(Registers &regs, unsigned entry)
{
	uint16_t csr = (uint16_t)(CSR_PMPCFG0 + (entry / 8) * 2);
	uint64_t word = regs.read_csr(csr);
	return (uint8_t)(word >> ((entry % 8) * 8));
}

inline uint64_t addr_of(Registers &regs, unsigned entry)
{
	return regs.read_csr((uint16_t)(CSR_PMPADDR0 + entry));
}

inline bool locked(Registers &regs, unsigned entry)
{
	return entry < ENTRIES && (cfg_byte(regs, entry) & CFG_L) != 0;
}

// pmpaddr holds the address shifted right by two: bit 0 of the register is
// bit 2 of the physical address. Everything below works in *byte* addresses
// and converts at the edges, because mixing the two conventions is the
// classic way to get a region off by a factor of four.
//
// Returns the region [lo, hi) for an entry, or false if the entry is off.
bool region_of(Registers &regs, unsigned entry, uint64_t &lo, uint64_t &hi)
{
	uint8_t cfg = cfg_byte(regs, entry);
	uint8_t a = (uint8_t)((cfg & CFG_A_MASK) >> CFG_A_SHIFT);
	uint64_t addr = addr_of(regs, entry);

	switch (a) {
	case A_OFF:
		return false;
	case A_TOR:
		// The bottom comes from the previous entry, or zero for entry 0.
		lo = (entry == 0) ? 0 : (addr_of(regs, entry - 1) << 2);
		hi = addr << 2;
		return hi > lo;   // an empty or inverted range matches nothing
	case A_NA4:
		lo = addr << 2;
		hi = lo + 4;
		return true;
	case A_NAPOT:
	default: {
		// NAPOT encodes size in the trailing ones: the lowest zero bit is
		// the boundary. All-ones means the whole address space.
		uint64_t a1 = ~addr;
		if (a1 == 0) { lo = 0; hi = ~0ull; return true; }
		unsigned zero = 0;
		while (((addr >> zero) & 1) != 0) zero++;
		// Region covers 2^(zero+3) bytes: zero trailing ones in the
		// register, plus the two bits the register does not hold.
		uint64_t size = 1ull << (zero + 3);
		lo = (addr << 2) & ~(size - 1);
		hi = lo + size;
		return true;
	}
	}
}

} // namespace

uint64_t read_cfg(Registers &regs, uint16_t csr)
{
	// Odd pmpcfg numbers are RV32-only; on RV64 they are not implemented.
	// Reporting zero rather than trapping keeps probing software happy.
	if ((csr - CSR_PMPCFG0) % 2 != 0) return 0;
	return regs.read_csr(csr);
}

uint64_t read_addr(Registers &regs, uint16_t csr)
{
	if (addr_index(csr) >= ENTRIES) return 0;   // unimplemented: hardwired zero
	return regs.read_csr(csr);
}

void write_cfg(Registers &regs, uint16_t csr, uint64_t value)
{
	if ((csr - CSR_PMPCFG0) % 2 != 0) return;
	unsigned base = (unsigned)((csr - CSR_PMPCFG0) / 2) * 8;

	uint64_t old = regs.read_csr(csr);
	uint64_t out = 0;
	for (unsigned i = 0; i < 8; i++) {
		unsigned entry = base + i;
		uint8_t oldb = (uint8_t)(old >> (i * 8));
		uint8_t newb = (uint8_t)(value >> (i * 8));

		if (entry >= ENTRIES) { newb = 0; }
		else if (oldb & CFG_L) {
			// Locked entries ignore writes entirely, including from
			// M-mode. This is the rule that makes PMP a guarantee rather
			// than a suggestion.
			newb = oldb;
		} else {
			// A=NA4 is not implementable when the grain is larger than
			// four bytes; DoomV's grain is 4, so NA4 stays legal and all
			// four encodings are kept.
			//
			// W without R is reserved. The architecture leaves the
			// behaviour of that combination undefined rather than
			// requiring a trap, and the reference clears it, so the bit
			// pattern is normalised here rather than stored and puzzled
			// over later.
			if ((newb & CFG_W) && !(newb & CFG_R)) newb &= (uint8_t)~CFG_W;
			newb &= (uint8_t)(CFG_L | CFG_A_MASK | CFG_X | CFG_W | CFG_R);
		}
		out |= (uint64_t)newb << (i * 8);
	}
	regs.write_csr(csr, out);
}

void write_addr(Registers &regs, uint16_t csr, uint64_t value)
{
	unsigned entry = addr_index(csr);
	if (entry >= ENTRIES) return;

	// The entry's own lock freezes its address...
	if (locked(regs, entry)) return;
	// ...and so does the next entry's lock, when that entry is TOR: this
	// address is then the *bottom* of that locked region, so letting it
	// move would resize a region that is supposed to be immutable.
	if (entry + 1 < ENTRIES && locked(regs, entry + 1)) {
		uint8_t next = cfg_byte(regs, entry + 1);
		if (((next & CFG_A_MASK) >> CFG_A_SHIFT) == A_TOR) return;
	}

	// Only the bits covering the implemented physical address width are
	// writable. DoomV's physical addresses are 56 bits (Sv39's limit), so
	// pmpaddr holds 54.
	regs.write_csr(csr, value & ((1ull << 54) - 1));
}

bool check(Registers &regs, uint64_t paddr, unsigned size, int access, uint8_t priv)
{
	// The whole access has to fall in one entry. An access straddling two
	// regions is denied even when both would permit it individually --
	// the architecture allows either that or splitting, and denying is
	// what the references do.
	uint64_t first = paddr;
	uint64_t last  = paddr + (size ? size - 1 : 0);

	for (unsigned i = 0; i < ENTRIES; i++) {
		uint64_t lo, hi;
		if (!region_of(regs, i, lo, hi)) continue;
		bool hit_first = (first >= lo && first < hi);
		bool hit_last  = (last  >= lo && last  < hi);
		if (!hit_first && !hit_last) continue;
		if (hit_first != hit_last) return false;   // straddles this region's edge

		uint8_t cfg = cfg_byte(regs, i);
		// M-mode ignores permissions on unlocked entries -- the lock bit
		// is precisely what makes an entry bind M-mode too.
		if (priv == (uint8_t)3 && !(cfg & CFG_L)) return true;

		switch (access) {
		case ACC_FETCH: return (cfg & CFG_X) != 0;
		case ACC_LOAD:  return (cfg & CFG_R) != 0;
		default:        return (cfg & CFG_W) != 0;
		}
	}

	// No entry matched. M-mode may go anywhere; everything else is denied.
	// That default is the one people get backwards: with no PMP entries
	// configured at all, S and U mode can reach *nothing*, which is why a
	// bare-metal test that drops privilege has to program an entry first.
	return priv == (uint8_t)3;
}

} // namespace pmp
