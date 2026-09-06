#include "aplic.hpp"
#include "imsic.hpp"
#include <cstring>

namespace {
// AIA spec §4.5's memory-map table for an interrupt domain's control
// region, confirmed exactly against the ratified spec text.
constexpr uint64_t OFF_DOMAINCFG = 0x0000;
constexpr uint64_t OFF_SOURCECFG = 0x0004; // sourcecfg[1] -- sourcecfg[i] = OFF_SOURCECFG + (i-1)*4
constexpr uint64_t OFF_SETIPNUM  = 0x1CDC;
constexpr uint64_t OFF_TARGET    = 0x3004; // target[1] -- target[i] = OFF_TARGET + (i-1)*4

constexpr uint32_t DOMAINCFG_IE = 1u << 8;
constexpr uint32_t DOMAINCFG_DM = 1u << 2;
constexpr uint32_t DOMAINCFG_WRITABLE = DOMAINCFG_IE | DOMAINCFG_DM | 1u /* BE */;

// MSI-mode target[i]: bits 31:18 Hart Index, bits 17:12 Guest Index
// (always 0 here -- no H-extension), bits 10:0 EIID. Bit 11 reserved.
constexpr uint32_t TARGET_WRITABLE = 0xFFFFF7FFu;
constexpr uint32_t TARGET_EIID_MASK = 0x7FFu;
}

Aplic::Aplic(Imsic &s_file) : s_file(s_file), domaincfg(0)
{
	std::memset(sourcecfg, 0, sizeof(sourcecfg));
	std::memset(target, 0, sizeof(target));
}

uint32_t Aplic::read32(uint64_t offset) const
{
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
	if (offset == OFF_SETIPNUM) {
		uint32_t n = val;
		if (n == 0 || n >= NUM_SOURCES) return; // not an implemented source
		if (sourcecfg[n] == 0) return;          // source inactive (SM == 0)
		if (!(domaincfg & DOMAINCFG_IE)) return; // domain-wide delivery disabled
		// Forward as an MSI: write the configured EIID to the target
		// IMSIC file's pending set, same effect a real bus write to
		// seteipnum_le would have (Hart Index/Guest Index ignored --
		// single hart, no H-extension).
		s_file.set_pending(target[n] & TARGET_EIID_MASK);
	}
}
