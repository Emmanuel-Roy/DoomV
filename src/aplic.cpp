#include "aplic.hpp"
#include "imsic.hpp"
#include "event_gen.hpp"
#include <cstring>

namespace {
// AIA spec §4.5's memory-map table for an interrupt domain's control
// region, confirmed exactly against the ratified spec text.
constexpr uint64_t OFF_DOMAINCFG = 0x0000;
constexpr uint64_t OFF_SOURCECFG = 0x0004; // sourcecfg[1] -- sourcecfg[i] = OFF_SOURCECFG + (i-1)*4
constexpr uint64_t OFF_SETIP     = 0x1C00; // setip[0]: sources 0-31
constexpr uint64_t OFF_SETIPNUM  = 0x1CDC;
constexpr uint64_t OFF_INCLRIP   = 0x1D00; // in_clrip[0]
constexpr uint64_t OFF_CLRIPNUM  = 0x1DDC;
constexpr uint64_t OFF_SETIE     = 0x1E00; // setie[0]
constexpr uint64_t OFF_SETIENUM  = 0x1EDC;
constexpr uint64_t OFF_CLRIE     = 0x1F00; // clrie[0]
constexpr uint64_t OFF_CLRIENUM  = 0x1FDC;
constexpr uint64_t OFF_SETIPNUM_LE = 0x2000;
// Each hart's IDC, at IDC_BASE + 32 * hart.
constexpr uint64_t IDC_IDELIVERY  = 0x00;
constexpr uint64_t IDC_IFORCE     = 0x04;
constexpr uint64_t IDC_ITHRESHOLD = 0x08;
constexpr uint64_t IDC_TOPI       = 0x18;
constexpr uint64_t IDC_CLAIMI     = 0x1C;
constexpr uint64_t OFF_TARGET    = 0x3004; // target[1] -- target[i] = OFF_TARGET + (i-1)*4

constexpr uint32_t DOMAINCFG_IE = 1u << 8;
constexpr uint32_t DOMAINCFG_DM = 1u << 2;
constexpr uint32_t DOMAINCFG_WRITABLE = DOMAINCFG_IE | DOMAINCFG_DM | 1u /* BE */;

// MSI-mode target[i]: bits 31:18 Hart Index, bits 17:12 Guest Index
// (always 0 here -- no H-extension), bits 10:0 EIID. Bit 11 reserved.
constexpr uint32_t TARGET_WRITABLE = 0xFFFFF7FFu;
constexpr uint32_t TARGET_EIID_MASK = 0x7FFu;
}

Aplic::Aplic(std::vector<Imsic> &s_files) : s_files(s_files), domaincfg(0)
{
	std::memset(sourcecfg, 0, sizeof(sourcecfg));
	std::memset(target, 0, sizeof(target));
}

uint32_t Aplic::topi(unsigned hart) const
{
	if (hart >= idc.size()) return 0;
	uint32_t best = 0, best_prio = 0;
	for (uint32_t n = 1; n < NUM_SOURCES; n++) {
		if (!(ip & ie & (1u << n)) || sourcecfg[n] == 0 || (target[n] >> 18) != hart) continue;
		const uint32_t prio = (target[n] & 0xFF) ? (target[n] & 0xFF) : 1;
		const uint32_t th = idc[hart].ithreshold & 0xFF;
		if (th && prio >= th) continue;
		if (!best || prio < best_prio) { best = n; best_prio = prio; }   // lower number, higher priority; ties to the lower id
	}
	return best ? (best << 16 | best_prio) : 0;
}

bool Aplic::direct_line(unsigned hart) const
{
	if (!direct() || !(domaincfg & DOMAINCFG_IE) || hart >= idc.size() || !(idc[hart].idelivery & 1)) return false;
	return topi(hart) != 0 || (idc[hart].iforce & 1);
}

uint32_t Aplic::read32(uint64_t offset) const
{
	if (offset >= IDC_BASE) {
		const uint64_t h = (offset - IDC_BASE) / 32, r = (offset - IDC_BASE) % 32;
		if (h >= s_files.size()) return 0;
		if (idc.size() < s_files.size()) const_cast<Aplic *>(this)->idc.resize(s_files.size());
		switch (r) {
		case IDC_IDELIVERY:  return idc[h].idelivery;
		case IDC_IFORCE:     return idc[h].iforce;
		case IDC_ITHRESHOLD: return idc[h].ithreshold;
		case IDC_TOPI:       return topi((unsigned)h);
		case IDC_CLAIMI:
			// Reading claimi claims: the source's pending bit clears, and a
			// forced interrupt with nothing behind it reads as 0 and clears.
			{
				Aplic &self = *const_cast<Aplic *>(this);
				const uint32_t t = topi((unsigned)h);
				if (t) self.ip &= ~(1u << (t >> 16));
				else self.idc[h].iforce = 0;
				bump_event_gen();
				return t;
			}
		default: return 0;
		}
	}
	if (offset == OFF_SETIP) return ip;
	if (offset == OFF_SETIE) return ie;
	if (offset == OFF_DOMAINCFG) return 0x80000000u | (domaincfg & DOMAINCFG_WRITABLE);

	if (offset >= OFF_SOURCECFG && offset < OFF_SOURCECFG + (NUM_SOURCES - 1) * 4) {
		int i = 1 + (int)((offset - OFF_SOURCECFG) / 4);
		return sourcecfg[i];
	}
	if (offset >= OFF_TARGET && offset < OFF_TARGET + (NUM_SOURCES - 1) * 4) {
		int i = 1 + (int)((offset - OFF_TARGET) / 4);
		return target[i];
	}
	return 0; // setipnum and everything else read as zero -- these are trigger registers, not storage
}

void Aplic::write32(uint64_t offset, uint32_t val)
{
	if (offset == OFF_DOMAINCFG) {
		domaincfg = val & DOMAINCFG_WRITABLE;
		return;
	}
	if (offset >= OFF_SOURCECFG && offset < OFF_SOURCECFG + (NUM_SOURCES - 1) * 4) {
		int i = 1 + (int)((offset - OFF_SOURCECFG) / 4);
		sourcecfg[i] = val & 0x7; // SM field only -- no child-domain delegation support
		return;
	}
	if (offset >= OFF_TARGET && offset < OFF_TARGET + (NUM_SOURCES - 1) * 4) {
		int i = 1 + (int)((offset - OFF_TARGET) / 4);
		target[i] = val & TARGET_WRITABLE;
		return;
	}
	if (offset == OFF_SETIPNUM || offset == OFF_SETIPNUM_LE) {
		assert_source(val);
		return;
	}
	if (offset >= IDC_BASE) {
		const uint64_t h = (offset - IDC_BASE) / 32, r = (offset - IDC_BASE) % 32;
		if (h >= s_files.size()) return;
		if (idc.size() < s_files.size()) idc.resize(s_files.size());
		if (r == IDC_IDELIVERY) idc[h].idelivery = val & 1;
		else if (r == IDC_IFORCE) idc[h].iforce = val & 1;
		else if (r == IDC_ITHRESHOLD) idc[h].ithreshold = val & 0xFF;
		bump_event_gen();
		return;
	}
	const uint32_t bit = (val > 0 && val < NUM_SOURCES) ? 1u << val : 0;
	if (offset == OFF_SETIP)      ip |= val & ~1u;
	else if (offset == OFF_INCLRIP) ip &= ~val;
	else if (offset == OFF_CLRIPNUM) ip &= ~bit;
	else if (offset == OFF_SETIE)  ie |= val & ~1u;
	else if (offset == OFF_SETIENUM) ie |= bit;
	else if (offset == OFF_CLRIE)  ie &= ~val;
	else if (offset == OFF_CLRIENUM) ie &= ~bit;
	else return;
	bump_event_gen();
}

void Aplic::assert_source(uint32_t n)
{
	if (n == 0 || n >= NUM_SOURCES) return; // not an implemented source
	if (sourcecfg[n] == 0) return;          // source inactive (SM == 0)
	if (direct()) {
		// Pending until claimed. What reaches the hart is decided by
		// direct_line, from the enables and the IDC.
		ip |= 1u << n;
		bump_event_gen();
		return;
	}
	if (!(domaincfg & DOMAINCFG_IE)) return; // domain-wide delivery disabled
	// Forward as an MSI: write the configured EIID to the target hart's
	// IMSIC file's pending set, same effect a real bus write to its
	// seteipnum_le would have. A Hart Index with no hart behind it goes
	// nowhere, as a write to an address with no IMSIC would; the Guest
	// Index is ignored (the domain delivers to S-level files only).
	const uint32_t hart = target[n] >> 18;
	if (hart < s_files.size()) s_files[hart].set_pending(target[n] & TARGET_EIID_MASK);
}
