#include "imsic.hpp"
#include <cstring>

namespace {
// AIA spec Table 2 (§2.3): miselect/siselect values 0x70-0xFF select this
// interrupt file's registers through the mireg/sireg indirect window.
constexpr uint64_t SEL_EIDELIVERY  = 0x70;
constexpr uint64_t SEL_EITHRESHOLD = 0x72;
constexpr uint64_t SEL_EIP0        = 0x80;
constexpr uint64_t SEL_EIP63       = 0xBF;
constexpr uint64_t SEL_EIE0        = 0xC0;
constexpr uint64_t SEL_EIE63       = 0xFF;
}

Imsic::Imsic() : eidelivery(false), eithreshold(0)
{
	std::memset(eip, 0, sizeof(eip));
	std::memset(eie, 0, sizeof(eie));
}

uint64_t Imsic::read_indirect(uint64_t iselect) const
{
	if (iselect == SEL_EIDELIVERY) return eidelivery ? 1 : 0;
	if (iselect == SEL_EITHRESHOLD) return eithreshold;
	if (iselect >= SEL_EIP0 && iselect <= SEL_EIP63) return eip[iselect - SEL_EIP0];
	if (iselect >= SEL_EIE0 && iselect <= SEL_EIE63) return eie[iselect - SEL_EIE0];
	return 0;
}

void Imsic::write_indirect(uint64_t iselect, uint64_t value)
{
	if (iselect == SEL_EIDELIVERY) eidelivery = (value & 1) != 0;
	else if (iselect == SEL_EITHRESHOLD) eithreshold = (uint32_t)value;
	else if (iselect >= SEL_EIP0 && iselect <= SEL_EIP63) eip[iselect - SEL_EIP0] = (uint32_t)value;
	else if (iselect >= SEL_EIE0 && iselect <= SEL_EIE63) eie[iselect - SEL_EIE0] = (uint32_t)value;
}

bool Imsic::heard(uint32_t id) const
{
	if (id == 0 || id >= (uint32_t)NUM_WORDS * 32) return false;
	uint32_t word = id / 32, bit = id % 32;
	if (!((eip[word] >> bit) & 1) || !((eie[word] >> bit) & 1)) return false;
	return eithreshold == 0 || id < eithreshold;
}

uint32_t Imsic::topei_value() const
{
	if (!eidelivery) return 0;
	for (int w = 0; w < NUM_WORDS; w++) {
		if (!(eip[w] & eie[w])) continue; // whole-word fast skip
		for (int b = 0; b < 32; b++) {
			uint32_t id = (uint32_t)(w * 32 + b);
			if (heard(id)) return (id << 16) | id;
		}
	}
	return 0;
}

void Imsic::claim()
{
	uint32_t top = topei_value();
	if (top == 0) return;
	uint32_t id = top & 0xFFFF;
	eip[id / 32] &= ~(1u << (id % 32));
}

void Imsic::set_pending(uint32_t id)
{
	if (id == 0 || id >= (uint32_t)NUM_WORDS * 32) return; // not an implemented identity -- ignored, per spec
	eip[id / 32] |= (1u << (id % 32));
}
