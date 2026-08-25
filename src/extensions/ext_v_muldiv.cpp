// OPMVV/OPMVX "real arithmetic" funct6 slots: averaging add/sub, multiply/
// divide/remainder, single-width multiply-add/accumulate, and widening
// add/sub/multiply/multiply-accumulate. The rest of OPMVV/OPMVX's encoding
// space (reductions, mask-producing/unary families, permutation) is routed
// to ext_v_reduce.cpp/ext_v_mask.cpp/ext_v_perm.cpp by exec_V instead.
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>

using namespace vcommon;

namespace vcommon {

void exec_v_muldiv(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	uint8_t funct6 = op_v_funct6(instr.funct7);
	bool vm = op_v_vm(instr.funct7);
	bool is_vv = (instr.funct3 == 0b010);
	uint64_t smask = elem_mask(sew);

	// vs1[i] for .vv, the x-register truncated to SEW bits for .vx.
	auto op2 = [&](uint64_t i) -> uint64_t {
		return is_vv ? read_velem(regs, instr.rs1, sew, i) : (regs.read_x(instr.rs1) & smask);
	};
	auto op2s = [&](uint64_t i) -> int64_t { return sext_elem(op2(i), sew); };

	// Averaging add/sub: (a op b), rounded right by 1 bit per vxrm.
	if (funct6 >= 0x08 && funct6 <= 0x0b) {
		bool is_add = (funct6 == 0x08 || funct6 == 0x09);
		bool is_signed = (funct6 == 0x09 || funct6 == 0x0b);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			__int128 a = is_signed ? (__int128)sext_elem(read_velem(regs, instr.rs2, sew, i), sew)
			                       : (__int128)read_velem(regs, instr.rs2, sew, i);
			__int128 b = is_signed ? (__int128)op2s(i) : (__int128)op2(i);
			__int128 sum = is_add ? (a + b) : (a - b);
			__int128 r = vxrm_round(sum, 1, regs.get_vxrm());
			write_velem(regs, instr.rd, sew, i, (uint64_t)r & smask);
		});
		return;
	}

	// Divide/remainder/multiply-high (0x20-0x27), single-width
	// multiply-add/accumulate (0x29/0x2b/0x2d/0x2f, odd funct6 only).
	if (funct6 >= 0x20 && funct6 <= 0x27) {
		switch (funct6) {
		case 0x20: // vdivu -- divide-by-zero yields all-1s, matching the base M extension's DIVU
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				uint64_t a = read_velem(regs, instr.rs2, sew, i), b = op2(i);
				write_velem(regs, instr.rd, sew, i, (b == 0) ? smask : (a / b));
			});
			break;
		case 0x21: // vdiv -- overflow (MIN/-1) yields MIN, matching DIV
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t a = sext_elem(read_velem(regs, instr.rs2, sew, i), sew), b = op2s(i);
				int64_t r;
				if (b == 0) r = -1;
				else if (a == (int64_t)(-((int64_t)1 << (sew - 1))) && b == -1) r = a;
				else r = a / b;
				write_velem(regs, instr.rd, sew, i, (uint64_t)r & smask);
			});
			break;
		case 0x22: // vremu
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				uint64_t a = read_velem(regs, instr.rs2, sew, i), b = op2(i);
				write_velem(regs, instr.rd, sew, i, (b == 0) ? a : (a % b));
			});
			break;
		case 0x23: // vrem
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t a = sext_elem(read_velem(regs, instr.rs2, sew, i), sew), b = op2s(i);
				int64_t r;
				if (b == 0) r = a;
				else if (a == (int64_t)(-((int64_t)1 << (sew - 1))) && b == -1) r = 0;
				else r = a % b;
				write_velem(regs, instr.rd, sew, i, (uint64_t)r & smask);
			});
			break;
		case 0x24: // vmulhu
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				unsigned __int128 p = (unsigned __int128)read_velem(regs, instr.rs2, sew, i) * op2(i);
				write_velem(regs, instr.rd, sew, i, (uint64_t)(p >> sew) & smask);
			});
			break;
		case 0x25: // vmul
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, sew, i, (read_velem(regs, instr.rs2, sew, i) * op2(i)) & smask);
			});
			break;
		case 0x26: // vmulhsu (vs2 signed x op2 unsigned)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				__int128 p = (__int128)sext_elem(read_velem(regs, instr.rs2, sew, i), sew) * (__int128)op2(i);
				write_velem(regs, instr.rd, sew, i, (uint64_t)((unsigned __int128)p >> sew) & smask);
			});
			break;
		case 0x27: // vmulh
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				__int128 p = (__int128)sext_elem(read_velem(regs, instr.rs2, sew, i), sew) * (__int128)op2s(i);
				write_velem(regs, instr.rd, sew, i, (uint64_t)((unsigned __int128)p >> sew) & smask);
			});
			break;
		}
		return;
	}

	if (funct6 == 0x29 || funct6 == 0x2b || funct6 == 0x2d || funct6 == 0x2f) {
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t old_vd = read_velem(regs, instr.rd, sew, i);
			uint64_t vs2 = read_velem(regs, instr.rs2, sew, i);
			uint64_t mul1 = op2(i);
			uint64_t r;
			switch (funct6) {
			case 0x29: r = (mul1 * old_vd + vs2) & smask; break;      // vmadd: vd = vs1*vd + vs2
			case 0x2b: r = ((uint64_t)0 - mul1 * old_vd + vs2) & smask; break; // vnmsub: vd = -(vs1*vd) + vs2
			case 0x2d: r = (mul1 * vs2 + old_vd) & smask; break;      // vmacc: vd = vs1*vs2 + vd
			default:   r = ((uint64_t)0 - mul1 * vs2 + old_vd) & smask; break; // vnmsac: vd = -(vs1*vs2) + vd
			}
			write_velem(regs, instr.rd, sew, i, r);
		});
		return;
	}

	// Widening add/sub/mul/multiply-accumulate (0x30-0x3f): destination and
	// (for non-".w" forms) both sources conceptually double-width. Only
	// meaningful when the narrow SEW is <=32 -- SEW=64 has no legal <=64-
	// bit-representable widened destination, so it's a no-op here (same
	// reserved-encoding stance as the narrowing ops in ext_v_int.cpp).
	int wide = sew * 2;
	if (funct6 >= 0x30 && wide <= 64) {
		bool op2_is_wide = (funct6 == 0x34 || funct6 == 0x35 || funct6 == 0x36 || funct6 == 0x37);
		uint64_t wmask = elem_mask(wide);

		// vs2 (or op2 for ".w" forms is really vs1 that's narrow -- see
		// below): read at `wide` when op2_is_wide names vs2, else `sew`.
		auto read_vs2 = [&](uint64_t i) -> uint64_t {
			return op2_is_wide ? read_velem(regs, instr.rs2, wide, i) : read_velem(regs, instr.rs2, sew, i);
		};
		auto vs2_signed = [&](uint64_t i) -> int64_t {
			return op2_is_wide ? sext_elem(read_vs2(i), wide) : sext_elem(read_vs2(i), sew);
		};
		auto narrow2 = [&](uint64_t i) -> uint64_t { // vs1 (.vv) / rs1 (.vx), always narrow (sew) for every widening op
			return is_vv ? read_velem(regs, instr.rs1, sew, i) : (regs.read_x(instr.rs1) & smask);
		};
		auto narrow2_signed = [&](uint64_t i) -> int64_t { return sext_elem(narrow2(i), sew); };

		switch (funct6) {
		case 0x30: case 0x34: // vwaddu(.w)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, wide, i, (read_vs2(i) + narrow2(i)) & wmask);
			});
			break;
		case 0x31: case 0x35: // vwadd(.w)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t r = vs2_signed(i) + narrow2_signed(i);
				write_velem(regs, instr.rd, wide, i, (uint64_t)r & wmask);
			});
			break;
		case 0x32: case 0x36: // vwsubu(.w)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, wide, i, (read_vs2(i) - narrow2(i)) & wmask);
			});
			break;
		case 0x33: case 0x37: // vwsub(.w)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t r = vs2_signed(i) - narrow2_signed(i);
				write_velem(regs, instr.rd, wide, i, (uint64_t)r & wmask);
			});
			break;
		case 0x38: // vwmulu
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				uint64_t p = read_vs2(i) * narrow2(i);
				write_velem(regs, instr.rd, wide, i, p & wmask);
			});
			break;
		case 0x3a: // vwmulsu (vs2 signed x narrow-operand unsigned)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t r = vs2_signed(i) * (int64_t)narrow2(i);
				write_velem(regs, instr.rd, wide, i, (uint64_t)r & wmask);
			});
			break;
		case 0x3b: // vwmul
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t r = vs2_signed(i) * narrow2_signed(i);
				write_velem(regs, instr.rd, wide, i, (uint64_t)r & wmask);
			});
			break;
		case 0x3c: // vwmaccu: vd(wide) += unsigned(narrow2) * unsigned(vs2)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				uint64_t old_vd = read_velem(regs, instr.rd, wide, i);
				write_velem(regs, instr.rd, wide, i, (old_vd + narrow2(i) * read_vs2(i)) & wmask);
			});
			break;
		case 0x3d: // vwmacc: vd(wide) += signed(narrow2) * signed(vs2)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t old_vd = sext_elem(read_velem(regs, instr.rd, wide, i), wide);
				int64_t r = old_vd + narrow2_signed(i) * vs2_signed(i);
				write_velem(regs, instr.rd, wide, i, (uint64_t)r & wmask);
			});
			break;
		case 0x3e: // vwmaccus (.vx only): vd += unsigned(rs1) * signed(vs2)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t old_vd = sext_elem(read_velem(regs, instr.rd, wide, i), wide);
				int64_t r = old_vd + (int64_t)narrow2(i) * vs2_signed(i);
				write_velem(regs, instr.rd, wide, i, (uint64_t)r & wmask);
			});
			break;
		case 0x3f: // vwmaccsu: vd += signed(narrow2) * unsigned(vs2)
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				int64_t old_vd = sext_elem(read_velem(regs, instr.rd, wide, i), wide);
				int64_t r = old_vd + narrow2_signed(i) * (int64_t)read_vs2(i);
				write_velem(regs, instr.rd, wide, i, (uint64_t)r & wmask);
			});
			break;
		default:
			break;
		}
	}
}

} // namespace vcommon
