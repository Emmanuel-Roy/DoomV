// OPIVV/OPIVX/OPIVI single-width integer arithmetic: add/sub/rsub/min/max/
// and/or/xor, add-with-carry/sub-with-borrow, merge/move, comparisons
// (mask-producing), saturating add/sub, shifts, saturating scaling
// multiply/shift, and narrowing shift/clip. Gather/slide/compress and
// widening-reduce share this funct3 space too but are routed to
// ext_v_perm.cpp/ext_v_reduce.cpp by exec_V before this file ever sees them.
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>

using namespace vcommon;

namespace {

uint64_t sat_add(uint64_t a_raw, uint64_t b_raw, int sew, bool is_signed, Registers &regs)
{
	__int128 a = is_signed ? (__int128)sext_elem(a_raw, sew) : (__int128)a_raw;
	__int128 b = is_signed ? (__int128)sext_elem(b_raw, sew) : (__int128)b_raw;
	__int128 r = a + b;
	__int128 lo = is_signed ? -((__int128)1 << (sew - 1)) : (__int128)0;
	__int128 hi = is_signed ? (((__int128)1 << (sew - 1)) - 1)
	                        : (sew == 64 ? (__int128)UINT64_MAX : (((__int128)1 << sew) - 1));
	if (r < lo) { r = lo; regs.or_vxsat(1); }
	else if (r > hi) { r = hi; regs.or_vxsat(1); }
	return (uint64_t)r & elem_mask(sew);
}

uint64_t sat_sub(uint64_t a_raw, uint64_t b_raw, int sew, bool is_signed, Registers &regs)
{
	__int128 a = is_signed ? (__int128)sext_elem(a_raw, sew) : (__int128)a_raw;
	__int128 b = is_signed ? (__int128)sext_elem(b_raw, sew) : (__int128)b_raw;
	__int128 r = a - b;
	__int128 lo = is_signed ? -((__int128)1 << (sew - 1)) : (__int128)0;
	__int128 hi = is_signed ? (((__int128)1 << (sew - 1)) - 1)
	                        : (sew == 64 ? (__int128)UINT64_MAX : (((__int128)1 << sew) - 1));
	if (r < lo) { r = lo; regs.or_vxsat(1); }
	else if (r > hi) { r = hi; regs.or_vxsat(1); }
	return (uint64_t)r & elem_mask(sew);
}

int log2_width(int bits)
{
	switch (bits) {
	case 8: return 3;
	case 16: return 4;
	case 32: return 5;
	default: return 6;
	}
}

} // namespace

namespace vcommon {

void exec_v_int(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	uint8_t funct6 = op_v_funct6(instr.funct7);
	bool vm = op_v_vm(instr.funct7);
	bool is_vv = (instr.funct3 == 0b000);
	bool is_vx = (instr.funct3 == 0b100);
	uint64_t smask = elem_mask(sew);

	// Second ("scalar") operand: vs1[i] for .vv, the x-register truncated
	// to SEW bits for .vx, or the 5-bit immediate for .vi -- sign-extended
	// (simm5) by default since that's what almost every OPIVI op wants;
	// op2u below gives the small set that want the raw unsigned bits
	// instead (shift amounts, vrgather.vi's index, vslide*.vi's amount).
	auto op2 = [&](uint64_t i) -> uint64_t {
		if (is_vv) return read_velem(regs, instr.rs1, sew, i);
		if (is_vx) return regs.read_x(instr.rs1) & smask;
		return (uint64_t)simm5(instr.rs1) & smask;
	};
	auto op2s = [&](uint64_t i) -> int64_t { return sext_elem(op2(i), sew); };

	int shbits = log2_width(sew);
	uint64_t shmask = (1ull << shbits) - 1;
	auto shamt = [&](uint64_t i) -> uint64_t {
		if (is_vv) return read_velem(regs, instr.rs1, sew, i) & shmask;
		if (is_vx) return regs.read_x(instr.rs1) & shmask;
		return (uint64_t)instr.rs1 & shmask; // zimm5, unsigned
	};

	switch (funct6) {
	case 0x00: // vadd
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, (read_velem(regs, instr.rs2, sew, i) + op2(i)) & smask);
		});
		break;
	case 0x02: // vsub
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, (read_velem(regs, instr.rs2, sew, i) - op2(i)) & smask);
		});
		break;
	case 0x03: // vrsub (vx/vi only)
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, (op2(i) - read_velem(regs, instr.rs2, sew, i)) & smask);
		});
		break;
	case 0x04: // vminu
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i), b = op2(i);
			write_velem(regs, instr.rd, sew, i, (a < b) ? a : b);
		});
		break;
	case 0x05: // vmin
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			int64_t a = sext_elem(read_velem(regs, instr.rs2, sew, i), sew), b = op2s(i);
			write_velem(regs, instr.rd, sew, i, (uint64_t)((a < b) ? a : b) & smask);
		});
		break;
	case 0x06: // vmaxu
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i), b = op2(i);
			write_velem(regs, instr.rd, sew, i, (a > b) ? a : b);
		});
		break;
	case 0x07: // vmax
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			int64_t a = sext_elem(read_velem(regs, instr.rs2, sew, i), sew), b = op2s(i);
			write_velem(regs, instr.rd, sew, i, (uint64_t)((a > b) ? a : b) & smask);
		});
		break;
	case 0x09: // vand
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, read_velem(regs, instr.rs2, sew, i) & op2(i));
		});
		break;
	case 0x0a: // vor
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, read_velem(regs, instr.rs2, sew, i) | op2(i));
		});
		break;
	case 0x0b: // vxor
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, read_velem(regs, instr.rs2, sew, i) ^ op2(i));
		});
		break;

	case 0x10: // vadc.v{v,x,i}m -- vm is architecturally fixed 0 for this encoding; unconditional over
		// vl (v0 supplies a per-element carry *input*, it doesn't gate which elements execute).
		for_each(regs, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i);
			uint64_t carry = mask_bit(regs, i) ? 1 : 0;
			write_velem(regs, instr.rd, sew, i, (a + op2(i) + carry) & smask);
		});
		break;
	case 0x11: // vmadc.v{v,x,i}m (vm=0: carry-in from v0) / vmadc.v{v,x,i} (vm=1: carry-in 0) -- mask-producing carry-out
		for_each(regs, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i), b = op2(i);
			uint64_t carry_in = vm ? 0 : (mask_bit(regs, i) ? 1 : 0);
			unsigned __int128 sum = (unsigned __int128)a + b + carry_in;
			set_mask_bit(regs, i, (bool)((sum >> sew) & 1));
		});
		break;
	case 0x12: // vsbc.v{v,x}m -- vm fixed 0, borrow-in from v0 (no .vi form)
		for_each(regs, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i);
			uint64_t borrow = mask_bit(regs, i) ? 1 : 0;
			write_velem(regs, instr.rd, sew, i, (a - op2(i) - borrow) & smask);
		});
		break;
	case 0x13: // vmsbc.v{v,x}m / vmsbc.v{v,x} -- mask-producing borrow-out
		for_each(regs, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i), b = op2(i);
			uint64_t borrow_in = vm ? 0 : (mask_bit(regs, i) ? 1 : 0);
			bool borrow_out = (a < b) || (a == b && borrow_in != 0);
			set_mask_bit(regs, i, borrow_out);
		});
		break;

	case 0x17: // vmerge.v{v,x,i}m (vm=0: v0 selects per-element) / vmv.v.{v,x,i} (vm=1: vs2 unused) -- unconditional over vl
		if (!vm) {
			for_each(regs, vl, [&](uint64_t i) {
				uint64_t r = mask_bit(regs, i) ? op2(i) : read_velem(regs, instr.rs2, sew, i);
				write_velem(regs, instr.rd, sew, i, r);
			});
		} else {
			for_each(regs, vl, [&](uint64_t i) { write_velem(regs, instr.rd, sew, i, op2(i)); });
		}
		break;

	case 0x18: // vmseq
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, read_velem(regs, instr.rs2, sew, i) == op2(i));
		});
		break;
	case 0x19: // vmsne
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, read_velem(regs, instr.rs2, sew, i) != op2(i));
		});
		break;
	case 0x1a: // vmsltu
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, read_velem(regs, instr.rs2, sew, i) < op2(i));
		});
		break;
	case 0x1b: // vmslt
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, sext_elem(read_velem(regs, instr.rs2, sew, i), sew) < op2s(i));
		});
		break;
	case 0x1c: // vmsleu
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, read_velem(regs, instr.rs2, sew, i) <= op2(i));
		});
		break;
	case 0x1d: // vmsle
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, sext_elem(read_velem(regs, instr.rs2, sew, i), sew) <= op2s(i));
		});
		break;
	case 0x1e: // vmsgtu (vx/vi only)
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, read_velem(regs, instr.rs2, sew, i) > op2(i));
		});
		break;
	case 0x1f: // vmsgt (vx/vi only)
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			set_mask_bit(regs, i, sext_elem(read_velem(regs, instr.rs2, sew, i), sew) > op2s(i));
		});
		break;

	case 0x20: // vsaddu
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, sat_add(read_velem(regs, instr.rs2, sew, i), op2(i), sew, false, regs));
		});
		break;
	case 0x21: // vsadd
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, sat_add(read_velem(regs, instr.rs2, sew, i), op2(i), sew, true, regs));
		});
		break;
	case 0x22: // vssubu
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, sat_sub(read_velem(regs, instr.rs2, sew, i), op2(i), sew, false, regs));
		});
		break;
	case 0x23: // vssub
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, sat_sub(read_velem(regs, instr.rs2, sew, i), op2(i), sew, true, regs));
		});
		break;

	case 0x25: // vsll
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, (read_velem(regs, instr.rs2, sew, i) << shamt(i)) & smask);
		});
		break;
	case 0x27: { // vsmul (vv/vx only) -- saturating fixed-point multiply, result scaled down by SEW-1 bits with vxrm rounding
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			int64_t a = sext_elem(read_velem(regs, instr.rs2, sew, i), sew);
			int64_t b = op2s(i);
			__int128 product = (__int128)a * (__int128)b;
			__int128 rounded = vxrm_round(product, sew - 1, regs.get_vxrm());
			__int128 lo = -((__int128)1 << (sew - 1)), hi = ((__int128)1 << (sew - 1)) - 1;
			if (rounded < lo) { rounded = lo; regs.or_vxsat(1); }
			else if (rounded > hi) { rounded = hi; regs.or_vxsat(1); }
			write_velem(regs, instr.rd, sew, i, (uint64_t)rounded & smask);
		});
		break;
	}
	case 0x28: // vsrl
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i, read_velem(regs, instr.rs2, sew, i) >> shamt(i));
		});
		break;
	case 0x29: // vsra
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			int64_t a = sext_elem(read_velem(regs, instr.rs2, sew, i), sew);
			write_velem(regs, instr.rd, sew, i, (uint64_t)(a >> shamt(i)) & smask);
		});
		break;
	case 0x2a: // vssrl -- scaling shift right logical, vxrm-rounded, same width (not narrowing)
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i);
			__int128 r = vxrm_round((__int128)a, (int)shamt(i), regs.get_vxrm());
			write_velem(regs, instr.rd, sew, i, (uint64_t)r & smask);
		});
		break;
	case 0x2b: // vssra -- same, arithmetic
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			int64_t a = sext_elem(read_velem(regs, instr.rs2, sew, i), sew);
			__int128 r = vxrm_round((__int128)a, (int)shamt(i), regs.get_vxrm());
			write_velem(regs, instr.rd, sew, i, (uint64_t)r & smask);
		});
		break;

	case 0x2c: case 0x2d: { // vnsrl.w{v,x,i} / vnsra.w{v,x,i} -- narrowing shift: vs2 wide (2*sew), vd/shift-amount narrow (sew)
		bool arith = (funct6 == 0x2d);
		int wide = sew * 2;
		if (wide > 64) break; // SEW=64 destination has no legal (<=64-bit) wide source -- reserved encoding, no-op
		int wshbits = log2_width(wide);
		uint64_t wshmask = (1ull << wshbits) - 1;
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t wide_raw = read_velem(regs, instr.rs2, wide, i);
			uint64_t sh = is_vv ? (read_velem(regs, instr.rs1, sew, i) & wshmask)
			            : is_vx ? (regs.read_x(instr.rs1) & wshmask)
			            : ((uint64_t)instr.rs1 & wshmask);
			uint64_t r = arith ? (uint64_t)(sext_elem(wide_raw, wide) >> sh) : (wide_raw >> sh);
			write_velem(regs, instr.rd, sew, i, r & smask);
		});
		break;
	}
	case 0x2e: case 0x2f: { // vnclipu.w{v,x,i} / vnclip.w{v,x,i} -- narrowing saturating clip, vxrm-rounded
		bool is_signed = (funct6 == 0x2f);
		int wide = sew * 2;
		if (wide > 64) break;
		int wshbits = log2_width(wide);
		uint64_t wshmask = (1ull << wshbits) - 1;
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t wide_raw = read_velem(regs, instr.rs2, wide, i);
			uint64_t sh = is_vv ? (read_velem(regs, instr.rs1, sew, i) & wshmask)
			            : is_vx ? (regs.read_x(instr.rs1) & wshmask)
			            : ((uint64_t)instr.rs1 & wshmask);
			__int128 wide_val = is_signed ? (__int128)sext_elem(wide_raw, wide) : (__int128)wide_raw;
			__int128 rounded = vxrm_round(wide_val, (int)sh, regs.get_vxrm());
			__int128 lo = is_signed ? -((__int128)1 << (sew - 1)) : (__int128)0;
			__int128 hi = is_signed ? (((__int128)1 << (sew - 1)) - 1)
			                        : (sew == 64 ? (__int128)UINT64_MAX : (((__int128)1 << sew) - 1));
			if (rounded < lo) { rounded = lo; regs.or_vxsat(1); }
			else if (rounded > hi) { rounded = hi; regs.or_vxsat(1); }
			write_velem(regs, instr.rd, sew, i, (uint64_t)rounded & smask);
		});
		break;
	}

	default:
		break; // reserved encoding -- no-op, matching the rest of the decoder's permissive-illegal handling
	}
}

} // namespace vcommon
