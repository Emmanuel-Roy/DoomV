#pragma once
// Shared plumbing for the V extension's ext_v_*.cpp files: vtype decoding,
// LMUL-grouped element addressing, and the handful of bitfield extractions
// (vm/funct6/mop/nf) that OP-V and vector loads/stores both derive from the
// same funct7 byte every other extension already stores on
// DecodedInstruction -- no new struct fields needed for any of it.
#include "registers.hpp"
#include <cstdint>
#include <cstring>

class Memory;
class RiscvCore;

namespace vcommon {

struct VType {
	bool vill;
	bool vma;
	bool vta;
	int sew;      // 8, 16, 32, or 64
	int lmul_num; // LMUL numerator: 1/2/4/8 whole, or 1 for fractional
	int lmul_den; // LMUL denominator: 1 for whole, 2/4/8 for fractional (1/2, 1/4, 1/8)
};

inline VType decode_vtype(uint64_t raw)
{
	VType vt{};
	vt.vill = (raw >> 63) & 1;
	vt.vma = (raw >> 7) & 1;
	vt.vta = (raw >> 6) & 1;
	uint8_t vsew = (raw >> 3) & 0x7;
	uint8_t vlmul = raw & 0x7;
	switch (vsew) {
	case 0b000: vt.sew = 8;  break;
	case 0b001: vt.sew = 16; break;
	case 0b010: vt.sew = 32; break;
	case 0b011: vt.sew = 64; break;
	default: vt.vill = true; vt.sew = 8; break; // vsew 100-111 reserved
	}
	switch (vlmul) {
	case 0b000: vt.lmul_num = 1; vt.lmul_den = 1; break;
	case 0b001: vt.lmul_num = 2; vt.lmul_den = 1; break;
	case 0b010: vt.lmul_num = 4; vt.lmul_den = 1; break;
	case 0b011: vt.lmul_num = 8; vt.lmul_den = 1; break;
	case 0b101: vt.lmul_num = 1; vt.lmul_den = 8; break;
	case 0b110: vt.lmul_num = 1; vt.lmul_den = 4; break;
	case 0b111: vt.lmul_num = 1; vt.lmul_den = 2; break;
	default: vt.vill = true; vt.lmul_num = 1; vt.lmul_den = 1; break; // 100 reserved
	}
	return vt;
}

inline uint64_t encode_vtype(bool vma, bool vta, uint8_t vsew, uint8_t vlmul)
{
	return ((uint64_t)vma << 7) | ((uint64_t)vta << 6) | ((uint64_t)vsew << 3) | vlmul;
}

// VLMAX: how many SEW-wide elements fit across one full LMUL-grouped
// register at this vtype -- (VLEN * LMUL) / SEW, done as (VLEN/SEW)*LMUL
// via the num/den pair to stay exact for fractional LMUL.
inline uint64_t vlmax(const VType &vt)
{
	if (vt.vill) return 0;
	return ((uint64_t)Registers::VLEN_BITS * (uint64_t)vt.lmul_num) / ((uint64_t)vt.sew * (uint64_t)vt.lmul_den);
}

// How many physical (16-byte) registers a group of `lmul_num`/`lmul_den`
// whole-or-fractional registers actually spans -- 1 for any fractional
// LMUL (it still occupies one physical register, just partially), lmul_num
// for whole LMUL.
inline int group_span(int lmul_num, int lmul_den)
{
	return (lmul_den == 1) ? lmul_num : 1;
}

// Element `idx` (0-based, within a possibly-multi-register LMUL group
// starting at physical register `base`) at width `eew` bits -- SEW-wide
// elements are packed contiguously across the group's registers in
// register-file order, so byte offset idx*(eew/8) addresses register
// base+offset/VLEN_BYTES at offset%VLEN_BYTES within it.
inline uint64_t read_velem(const Registers &regs, int base, int eew, uint64_t idx)
{
	uint64_t byte_off = idx * (uint64_t)(eew / 8);
	int reg = base + (int)(byte_off / (uint64_t)Registers::VLEN_BYTES);
	int off = (int)(byte_off % (uint64_t)Registers::VLEN_BYTES);
	uint64_t v = 0;
	std::memcpy(&v, regs.read_v(reg) + off, (size_t)(eew / 8));
	return v;
}

inline void write_velem(Registers &regs, int base, int eew, uint64_t idx, uint64_t value)
{
	uint64_t byte_off = idx * (uint64_t)(eew / 8);
	int reg = base + (int)(byte_off / (uint64_t)Registers::VLEN_BYTES);
	int off = (int)(byte_off % (uint64_t)Registers::VLEN_BYTES);
	std::memcpy(regs.write_v(reg) + off, &value, (size_t)(eew / 8));
}

inline int64_t sext_elem(uint64_t v, int eew)
{
	switch (eew) {
	case 8:  return (int64_t)(int8_t)v;
	case 16: return (int64_t)(int16_t)v;
	case 32: return (int64_t)(int32_t)v;
	default: return (int64_t)v;
	}
}

inline uint64_t elem_mask(int eew)
{
	return (eew >= 64) ? ~0ull : ((1ull << eew) - 1);
}

// v0.t predication: one bit per element, bit-packed LSB-first across v0's
// bytes regardless of the active SEW (masks are always bit vectors).
inline bool mask_bit(const Registers &regs, uint64_t idx)
{
	return (regs.read_v(0)[idx / 8] >> (idx % 8)) & 1;
}

// Writes bit `idx` of the mask register `vd`. The destination is a real
// operand, NOT always v0: every mask-producing instruction (the integer
// and FP compares, vmadc/vmsbc, the vm*.mm logic ops, vmsbf/vmsof/vmsif)
// names its own vd, and v0 is merely the register that *consumers* of a
// mask read implicitly. This used to hardcode v0, so a compare into any
// other register left its real destination untouched and silently
// clobbered the active mask instead -- `vmsne.vv v9, v1, v2` produced an
// all-zero v9, which then made vmor.mm and vfirst.m wrong downstream.
// Caught by the spike differential test in tools/vtest/vector/.
inline void set_mask_bit(Registers &regs, int vd, uint64_t idx, bool bit)
{
	uint8_t *v = regs.write_v(vd);
	uint8_t m = (uint8_t)(1u << (idx % 8));
	if (bit) v[idx / 8] |= m;
	else v[idx / 8] &= (uint8_t)~m;
}

// Whether element `idx` is active under this instruction's masking: vm=1
// (the field, confusingly -- vm=1 means "mask bit ignored, unmasked")
// always executes; vm=0 means "masked", gated on v0.t.
inline bool elem_active(const Registers &regs, bool vm, uint64_t idx)
{
	return vm || mask_bit(regs, idx);
}

// funct7 (bits[31:25], already extracted by every decode_* the same way)
// packs different sub-fields depending on which V encoding it's part of.
// OP-V arithmetic (opcode 1010111): funct6[31:26] | vm[25].
inline uint8_t op_v_funct6(uint8_t funct7) { return funct7 >> 1; }
inline bool op_v_vm(uint8_t funct7) { return funct7 & 1; }

// Vector loads/stores (opcode 0000111/0100111, once classify() has already
// routed them to V instead of F/D): nf[31:29] | mew[28] | mop[27:26] | vm[25].
inline uint8_t ldst_nf(uint8_t funct7) { return funct7 >> 4; }
inline bool ldst_mew(uint8_t funct7) { return (funct7 >> 3) & 1; }
// mop is instruction bits[27:26], i.e. funct7 bits[2:1] -- funct7 starts at
// instruction bit 25, so this shifts by 1, not 2. Shifting by 2 read
// {mew, mop[1]} instead, which silently remapped two addressing modes:
// strided (10) became indexed (01), so the stride *register number* was
// used as an index vector, and indexed-unordered (01) became unit-stride
// (00), ignoring the index vector entirely. Unit-stride and
// indexed-ordered happened to survive the mangling, which is why this went
// unnoticed. Caught by tools/vtest/vector's spike diff.
inline uint8_t ldst_mop(uint8_t funct7) { return (funct7 >> 1) & 0x3; }
inline bool ldst_vm(uint8_t funct7) { return funct7 & 1; }

// OPIVI's 5-bit immediate lives at the vs1 field position (bits[19:15]),
// which decode already extracts into instr.rs1 like any register number --
// reinterpreted here as signed (most arithmetic ops) or left raw/unsigned
// (index-like ops: vrgather.vi, vslideup/down.vi amounts) by the caller.
inline int64_t simm5(uint8_t raw5) { return (int64_t)(int8_t)(raw5 << 3) >> 3; }

// Iterates the active element indices in [vstart, vl) -- vm=true (the
// field's confusing polarity: 1 means "mask ignored") always active, vm=
// false gates on v0.t. Elements outside this range (before vstart, at/past
// vl) are left completely untouched -- "undisturbed" is always a spec-legal
// choice regardless of the vta/vma agnostic-policy bits, so this project
// doesn't bother implementing the "may scribble 1s" agnostic alternative,
// the same kind of simplification F/D made for RMM rounding. vstart resets
// to 0 on normal completion, per spec.
template <typename Fn>
void for_each_active(Registers &regs, bool vm, uint64_t vl, Fn &&fn)
{
	uint64_t vstart = regs.get_vstart();
	for (uint64_t i = vstart; i < vl; i++) {
		if (elem_active(regs, vm, i)) fn(i);
	}
	regs.set_vstart(0);
}

// Unconditional counterpart to for_each_active -- used by vadc/vsbc/vmadc/
// vmsbc/vmerge/vmv.v.* and mask-logical ops, none of which are gated by the
// instruction's own vm bit the normal way (vadc/vsbc/vmadc/vmsbc use v0 as
// a per-element carry *input*, not an execution gate; vmerge uses v0 as a
// per-element select; vmv.v.*/mask-logical ops don't consult v0 at all).
template <typename Fn>
void for_each(Registers &regs, uint64_t vl, Fn &&fn)
{
	uint64_t vstart = regs.get_vstart();
	for (uint64_t i = vstart; i < vl; i++) fn(i);
	regs.set_vstart(0);
}

// vxrm-controlled rounding for fixed-point ops (vaadd/vasub averaging,
// vssrl/vssra scaling shift, vnclip{u} narrowing clip, vsmul): `v` is the
// full-precision value before the low `shift` bits are discarded. Takes/
// returns __int128 uniformly (rather than separate signed/unsigned
// variants) -- any of this project's <=64-bit signed or unsigned values fit
// as a plain non-negative __int128 with no sign ambiguity, so the same
// arithmetic-shift-based implementation is exact for both. 00=round-to-
// nearest-up (add the round bit, i.e. round half up), 01=round-to-nearest-
// even, 10=round-down (truncate), 11=round-to-odd.
inline __int128 vxrm_round(__int128 v, int shift, uint8_t vxrm)
{
	if (shift == 0) return v;
	unsigned __int128 uv = (unsigned __int128)v;
	uint64_t round_bit = (uint64_t)((uv >> (shift - 1)) & 1);
	uint64_t rest_bits = (shift > 1) ? (uint64_t)(uv & (((unsigned __int128)1 << (shift - 1)) - 1)) : 0;
	__int128 result = v >> shift; // arithmetic shift -- GCC/Clang guarantee sign-extending >> for signed types
	switch (vxrm) {
	case 0b00: result += round_bit; break;
	case 0b01: if (round_bit && (rest_bits != 0 || ((uint64_t)result & 1))) result += 1; break;
	case 0b10: break;
	case 0b11: if (round_bit || rest_bits) result |= 1; break;
	}
	return result;
}

// Category entry points, one per ext_v_*.cpp file, called from exec_V's
// dispatcher in ext_v.cpp. Each independently re-derives whatever it needs
// from instr.funct6 (via op_v_funct6 above)/instr.rs1/rs2/rd, matching the
// project-wide "self-sufficient re-extraction, no shared prelude" decode
// convention -- exec_V's job is purely routing, not field extraction.
void exec_v_config(const DecodedInstruction &instr, Registers &regs);
// Returns false if a page fault happened partway through (pc has already
// been redirected into the trap handler by then -- the caller must not
// then overwrite it by advancing pc normally).
bool exec_v_ldst(const DecodedInstruction &instr, Registers &regs, Memory &mem, RiscvCore &core);
void exec_v_int(const DecodedInstruction &instr, Registers &regs);
void exec_v_muldiv(const DecodedInstruction &instr, Registers &regs);
void exec_v_mask(const DecodedInstruction &instr, Registers &regs);
void exec_v_perm(const DecodedInstruction &instr, Registers &regs);
void exec_v_reduce(const DecodedInstruction &instr, Registers &regs);
void exec_v_fp(const DecodedInstruction &instr, Registers &regs);

} // namespace vcommon
