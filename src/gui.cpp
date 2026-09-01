#include "gui.hpp"
#include "riscv_decoder.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

static const char *x_name(uint8_t r)
{
	static const char *names[32] = {
		"zero", "ra", "sp",  "gp",  "tp",  "t0",  "t1",  "t2",
		"s0",   "s1", "a0",  "a1",  "a2",  "a3",  "a4",  "a5",
		"a6",   "a7", "s2",  "s3",  "s4",  "s5",  "s6",  "s7",
		"s8",   "s9", "s10", "s11", "t3",  "t4",  "t5",  "t6",
	};
	return names[r & 0x1F];
}

static const char *f_name(uint8_t r)
{
	static const char *names[32] = {
		"ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6", "ft7",
		"fs0", "fs1", "fa0",  "fa1",  "fa2", "fa3", "fa4", "fa5",
		"fa6", "fa7", "fs2",  "fs3",  "fs4", "fs5", "fs6", "fs7",
		"fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
	};
	return names[r & 0x1F];
}

static const char *csr_name(uint16_t addr)
{
	switch (addr) {
	case 0x001: return "fflags";
	case 0x002: return "frm";
	case 0x003: return "fcsr";
	// V's dedicated-field CSRs -- see registers.hpp's accessors.
	case 0x008: return "vstart";
	case 0x009: return "vxsat";
	case 0x00A: return "vxrm";
	case 0x00F: return "vcsr";
	case 0xC20: return "vl";
	case 0xC21: return "vtype";
	case 0xC22: return "vlenb";
	// S-mode CSRs (Stage 1+).
	case 0x100: return "sstatus";
	case 0x104: return "sie";
	case 0x105: return "stvec";
	case 0x140: return "sscratch";
	case 0x141: return "sepc";
	case 0x142: return "scause";
	case 0x143: return "stval";
	case 0x144: return "sip";
	case 0x14D: return "stimecmp"; // Sstc (Stage 2)
	case 0x150: return "siselect"; // AIA indirect (Stage 2)
	case 0x151: return "sireg";
	case 0x15C: return "stopei";
	case 0x180: return "satp";
	// M-mode CSRs.
	case 0x300: return "mstatus";
	case 0x301: return "misa";
	case 0x302: return "medeleg";
	case 0x303: return "mideleg";
	case 0x304: return "mie";
	case 0x305: return "mtvec";
	case 0x30A: return "menvcfg";
	case 0x340: return "mscratch";
	case 0x341: return "mepc";
	case 0x342: return "mcause";
	case 0x343: return "mtval";
	case 0x344: return "mip";
	case 0x350: return "miselect"; // AIA indirect (Stage 2)
	case 0x351: return "mireg";
	case 0x35C: return "mtopei";
	case 0xC01: return "time"; // unprivileged read-only mtime alias (Stage 4)
	case 0xF11: return "mvendorid";
	case 0xF12: return "marchid";
	case 0xF13: return "mimpid";
	case 0xF14: return "mhartid";
	default: return nullptr;
	}
}

// Renders "rd, rs1, rs2"-style operand text for a decoded instruction, the
// way a real RISC-V disassembler (objdump, etc.) would show it. Dispatches
// on the same opcode/funct7/funct3 fields exec_32I/exec_32M/exec_FD
// already use to decide behavior, not on the mnemonic string -- stays
// correct for every compressed alias automatically, since a C.ADDI's
// decoded fields already look exactly like a real ADDI's would.
static void format_operands(char *buf, size_t buf_size, uint64_t pc, const DecodedInstruction &d)
{
	char imm_buf[32];
	auto imm_str = [&](int64_t v) { sprintf(imm_buf, "%lld", (long long)v); return imm_buf; };

	switch (d.opcode) {
	case 0b0110111: // LUI
	case 0b0010111: // AUIPC
		snprintf(buf, buf_size, "%s, 0x%llx", x_name(d.rd), (unsigned long long)(((uint64_t)d.imm >> 12) & 0xFFFFF));
		return;
	case 0b1101111: // JAL -- shown as the absolute target address, like objdump does
		snprintf(buf, buf_size, "%s, 0x%llx", x_name(d.rd), (unsigned long long)(pc + (uint64_t)d.imm));
		return;
	case 0b1100111: // JALR
		snprintf(buf, buf_size, "%s, %s(%s)", x_name(d.rd), imm_str(d.imm), x_name(d.rs1));
		return;
	case 0b1100011: // Branch -- target shown absolute, same reasoning as JAL
		snprintf(buf, buf_size, "%s, %s, 0x%llx", x_name(d.rs1), x_name(d.rs2), (unsigned long long)(pc + (uint64_t)d.imm));
		return;
	case 0b0000011: // Load (int)
		snprintf(buf, buf_size, "%s, %s(%s)", x_name(d.rd), imm_str(d.imm), x_name(d.rs1));
		return;
	case 0b0100011: // Store (int)
		snprintf(buf, buf_size, "%s, %s(%s)", x_name(d.rs2), imm_str(d.imm), x_name(d.rs1));
		return;
	case 0b0010011: // OP-IMM
	case 0b0011011: // OP-IMM-32
		snprintf(buf, buf_size, "%s, %s, %s", x_name(d.rd), x_name(d.rs1), imm_str(d.imm));
		return;
	case 0b0110011: // OP
	case 0b0111011: // OP-32
		snprintf(buf, buf_size, "%s, %s, %s", x_name(d.rd), x_name(d.rs1), x_name(d.rs2));
		return;
	case 0b0001111: // FENCE / FENCE.I -- no operands
		buf[0] = '\0';
		return;
	case 0b1110011: { // SYSTEM: ECALL/EBREAK/MRET (no operands) or a CSR op
		if (d.funct3 == 0) { buf[0] = '\0'; return; }
		uint16_t csr = (uint16_t)d.imm;
		const char *name = csr_name(csr);
		char csr_buf[16];
		if (!name) { sprintf(csr_buf, "0x%x", csr); name = csr_buf; }
		if (d.funct3 & 0x4) snprintf(buf, buf_size, "%s, %s, %u", x_name(d.rd), name, d.rs1); // CSRR*I -- rs1 field holds a 5-bit immediate, not a register
		else snprintf(buf, buf_size, "%s, %s, %s", x_name(d.rd), name, x_name(d.rs1));
		return;
	}
	case 0b0101111: { // AMO
		uint8_t amo_op = d.funct7 >> 2;
		if (amo_op == 0b00010) snprintf(buf, buf_size, "%s, (%s)", x_name(d.rd), x_name(d.rs1)); // LR
		else snprintf(buf, buf_size, "%s, %s, (%s)", x_name(d.rd), x_name(d.rs2), x_name(d.rs1)); // SC/AMO*
		return;
	}
	case 0b0000111: // LOAD-FP
		snprintf(buf, buf_size, "%s, %s(%s)", f_name(d.rd), imm_str(d.imm), x_name(d.rs1));
		return;
	case 0b0100111: // STORE-FP
		snprintf(buf, buf_size, "%s, %s(%s)", f_name(d.rs2), imm_str(d.imm), x_name(d.rs1));
		return;
	case 0b1000011: case 0b1000111: case 0b1001011: case 0b1001111: // FMADD/FMSUB/FNMSUB/FNMADD
		snprintf(buf, buf_size, "%s, %s, %s, %s", f_name(d.rd), f_name(d.rs1), f_name(d.rs2), f_name(d.rs3));
		return;
	case 0b1010011: // OP-FP -- funct7 re-selects the exact shape, same as exec_FD/decode()
		switch (d.funct7) {
		case 0b0000000: case 0b0000001: case 0b0000100: case 0b0000101: // FADD/FSUB
		case 0b0001000: case 0b0001001: case 0b0001100: case 0b0001101: // FMUL/FDIV
		case 0b0010000: case 0b0010001: case 0b0010100: case 0b0010101: // FSGNJ family, FMIN/FMAX
			snprintf(buf, buf_size, "%s, %s, %s", f_name(d.rd), f_name(d.rs1), f_name(d.rs2));
			return;
		case 0b0101100: case 0b0101101: // FSQRT
			snprintf(buf, buf_size, "%s, %s", f_name(d.rd), f_name(d.rs1));
			return;
		case 0b1010000: case 0b1010001: // FEQ/FLT/FLE -- rd integer
			snprintf(buf, buf_size, "%s, %s, %s", x_name(d.rd), f_name(d.rs1), f_name(d.rs2));
			return;
		case 0b1100000: case 0b1100001: // FCVT.(W|WU|L|LU).(S|D) -- rd integer, rs1 float
		case 0b1110000: case 0b1110001: // FMV.X.W/D, FCLASS
			snprintf(buf, buf_size, "%s, %s", x_name(d.rd), f_name(d.rs1));
			return;
		case 0b1101000: case 0b1101001: // FCVT.(S|D).(W|WU|L|LU) -- rd float, rs1 integer
		case 0b1111000: case 0b1111001: // FMV.W.X, FMV.D.X
			snprintf(buf, buf_size, "%s, %s", f_name(d.rd), x_name(d.rs1));
			return;
		case 0b0100000: case 0b0100001: // FCVT.S.D / FCVT.D.S -- both float
			snprintf(buf, buf_size, "%s, %s", f_name(d.rd), f_name(d.rs1));
			return;
		default:
			buf[0] = '\0';
			return;
		}
	default:
		buf[0] = '\0';
		return;
	}
}

Gui::Gui()
{
	init_font();
}

Gui::~Gui()
{
	if (texture) SDL_DestroyTexture(texture);
	if (renderer) SDL_DestroyRenderer(renderer);
	if (window) SDL_DestroyWindow(window);
	SDL_Quit();
}

bool Gui::init(int window_w, int window_h)
{
	// Must be set before SDL_Init on Windows -- without it, an app with no
	// DPI-awareness manifest gets bitmap-scaled by the OS on any display
	// with scaling above 100%: SDL_GetWindowSize (what resize_canvas_if_
	// needed() uses to size screen_buf/texture) then reports a *smaller*
	// logical size than the physical window actually is, so the rendered
	// content only fills part of it, leaving black bars around the real
	// edges -- looks like broken/shifted scaling, but it's really a
	// logical-vs-physical pixel mismatch, not a layout bug.
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;

	// Fixed size, not resizable -- window_w/window_h are meant as a set
	// 1920x1080, not just a starting default (see gui.hpp).
	window = SDL_CreateWindow("RISC-V Doom SoC",
	                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                          window_w, window_h, SDL_WINDOW_SHOWN);
	if (!window) return false;

	// VSYNC is back on purpose: rendering now runs on its own thread
	// (see DoomSystem::run), so capping it to the display refresh rate no
	// longer throttles instruction throughput -- the CPU thread keeps
	// burning through instructions uncapped regardless. Without VSYNC this
	// thread just spun as fast as possible re-presenting duplicate frames
	// between snapshot publishes, burning a full core fighting the CPU
	// thread for scheduler time, which showed up as choppiness.
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // nearest-neighbor -- sharp blocks, never blurry

	resize_canvas_if_needed();

	return (renderer && texture);
}

void Gui::resize_canvas_if_needed()
{
	int w, h;
	SDL_GetWindowSize(window, &w, &h);
	if (w == canvas_w && h == canvas_h) return;

	canvas_w = w;
	canvas_h = h;
	scale_x = (float)canvas_w / (float)DESIGN_W;
	scale_y = (float)canvas_h / (float)DESIGN_H;
	screen_buf.resize((size_t)canvas_w * (size_t)canvas_h);

	if (texture) SDL_DestroyTexture(texture);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
	                            SDL_TEXTUREACCESS_STREAMING, canvas_w, canvas_h);
}

void Gui::render(const Snapshot &snap)
{
	resize_canvas_if_needed();

	std::fill(screen_buf.begin(), screen_buf.end(), 0x876A96);

	uint32_t pal_pink  = 0xD580B8;
	uint32_t pal_white = 0xD7D0D0;
	uint32_t pal_red   = 0xB95167;
	uint32_t pal_dark  = 0x25080C;
	uint32_t pal_stats = 0xC3A9C4;

	// GAME SCREEN: design box is 280x175 at (10,7) -- same box, same 1.6
	// aspect ratio, just rescaled 2/3 alongside DESIGN_W/H's own 960x540
	// -> 640x360 shrink (see gui.hpp) so it renders at the same actual
	// screen size as before. GAME_BOX_Y=7 matches the CSRS/REGISTER FILE
	// headers' own y (see their draw_shadow_text calls below) so the
	// display's top edge lines up with them instead of starting lower.
	// GAME_BOX_* are the design-unit source of truth -- the CSRS panel
	// below anchors off the box's own right edge, and TRACE LOG's own y
	// tracks its bottom edge, so neither needs a second edit if this box
	// ever moves again.
	const int GAME_BOX_X = 10, GAME_BOX_Y = 7, GAME_BOX_W = 280, GAME_BOX_H = 175;
	int box_x = (int)(GAME_BOX_X * scale_x);
	int box_y = (int)(GAME_BOX_Y * scale_y);
	int box_w = (int)(GAME_BOX_W * scale_x);
	int box_h = (int)(GAME_BOX_H * scale_y);

	// Bilinear, not nearest-neighbor: at native 320x200 scaled ~3-4x, hard
	// pixel blocks looked wrong for the game view (dashboard text stays
	// sharp block-fills on purpose, this is just the rendered scene).
	// Fixed-point (8-bit fraction) so the per-pixel blend is pure integer
	// math, no floats in the hot loop.
	struct Sample { int i0, i1; uint32_t frac; };
	static std::vector<Sample> sx_lut, sy_lut;
	static int last_box_w = -1, last_box_h = -1;
	if (box_w != last_box_w || box_h != last_box_h) {
		sx_lut.resize(box_w > 0 ? box_w : 0);
		sy_lut.resize(box_h > 0 ? box_h : 0);
		for (int x = 0; x < box_w; x++) {
			float src = ((float)x + 0.5f) * Memory::FB_W / box_w - 0.5f;
			int i0 = (int)std::floor(src);
			float frac = src - (float)i0;
			if (i0 < 0) { i0 = 0; frac = 0.0f; }
			int i1 = (i0 + 1 < Memory::FB_W) ? i0 + 1 : i0;
			sx_lut[x] = { i0, i1, (uint32_t)(frac * 256.0f) };
		}
		for (int y = 0; y < box_h; y++) {
			float src = ((float)y + 0.5f) * Memory::FB_H / box_h - 0.5f;
			int i0 = (int)std::floor(src);
			float frac = src - (float)i0;
			if (i0 < 0) { i0 = 0; frac = 0.0f; }
			int i1 = (i0 + 1 < Memory::FB_H) ? i0 + 1 : i0;
			sy_lut[y] = { i0, i1, (uint32_t)(frac * 256.0f) };
		}
		last_box_w = box_w;
		last_box_h = box_h;
	}

	const uint32_t *fb32 = snap.framebuffer.data();
	for (int y = 0; y < box_h; y++) {
		int ty = box_y + y;
		if (ty < 0 || ty >= canvas_h) continue;

		const Sample &ys = sy_lut[y];
		const uint32_t *row0 = fb32 + ys.i0 * Memory::FB_W;
		const uint32_t *row1 = fb32 + ys.i1 * Memory::FB_W;
		uint32_t *dst_row = &screen_buf[(size_t)ty * canvas_w];

		for (int x = 0; x < box_w; x++) {
			int tx = box_x + x;
			if (tx < 0 || tx >= canvas_w) continue;

			const Sample &xs = sx_lut[x];
			uint32_t p00 = row0[xs.i0], p10 = row0[xs.i1];
			uint32_t p01 = row1[xs.i0], p11 = row1[xs.i1];

			uint32_t out = 0;
			for (int shift = 16; shift >= 0; shift -= 8) {
				uint32_t c00 = (p00 >> shift) & 0xFF, c10 = (p10 >> shift) & 0xFF;
				uint32_t c01 = (p01 >> shift) & 0xFF, c11 = (p11 >> shift) & 0xFF;
				uint32_t top = c00 * (256 - xs.frac) + c10 * xs.frac;
				uint32_t bot = c01 * (256 - xs.frac) + c11 * xs.frac;
				uint32_t chan = (top * (256 - ys.frac) + bot * ys.frac) >> 16;
				out |= chan << shift;
			}
			dst_row[tx] = out;
		}
	}

	char buf[96];

	auto draw_shadow_text = [&](int x, int y, const char *s, uint32_t col, float scale_x_ = 1.0f, float scale_y_ = -1.0f) {
		draw_string(x + 1, y + 1, s, pal_dark, scale_x_, scale_y_);
		draw_string(x, y, s, col, scale_x_, scale_y_);
	};

	// Header centering -- matches draw_string's own advance formula
	// exactly (8*scale, rounded, min 1) so a centered title lines up
	// with the actual glyphs pixel-for-pixel, not just approximately.
	auto glyph_adv = [&](float scale_x_) {
		int adv = (int)(8 * scale_x_ + 0.5f); return adv < 1 ? 1 : adv;
	};
	auto text_w = [&](const char *s, float scale_x_) {
		return (int)strlen(s) * glyph_adv(scale_x_);
	};
	auto draw_centered_title = [&](int col_x, int col_w, int y, const char *s, uint32_t col, float scale_x_ = 1.0f) {
		draw_shadow_text(col_x + (col_w - text_w(s, scale_x_)) / 2, y, s, col, scale_x_);
	};

	// Shared register-entry sizing -- "roughly the size of one of the
	// normal registers" applies to the CSRs panel too, so both use the
	// same scale/row-height constants. REG_SCALE_X (0.6 -> 0.7 -> 0.85 ->
	// 0.567) widens the actual hex digits themselves, not just the row
	// height -- the 0.85 -> 0.567 drop tracks DESIGN_W/H's own 2/3
	// shrink (960x540 -> 640x360, see gui.hpp), so glyphs land at the
	// same actual screen size as before despite the smaller design space.
	const float REG_SCALE_X = 0.567f;
	const float REG_SCALE_Y = 0.867f;
	// 32 rows * 10 + the header offset is the tallest this can go without
	// the last row clipping past DESIGN_H=360 -- picked to stretch the
	// CSRs/register rows down until there's not much space left at the
	// bottom, not just an arbitrary round number.
	const int REG_ROW_H = 10;
	const int REG_COL_W = 106; // label (4 chars) + 16 hex digits at REG_SCALE_X, plus a small gap
	const int REG_VALUE_OFFSET = 19; // label -> hex value gap, X/V columns and CSRS' name column both use it
	const int REG_HEX_W = 16 * glyph_adv(REG_SCALE_X); // 16 hex digits at REG_SCALE_X's own glyph advance

	// REGISTER FILE's left edge is a fixed design-space anchor, not
	// derived from the CSRs panel's own width -- so pulling the CSRs
	// panel closer (below) doesn't also drag the register file sideways
	// with it. Picked so the V column's rightmost hex digit lands just
	// shy of DESIGN_W=640, not clipped by it -- see gui.hpp's comment on
	// why that's a hard bound.
	const int hud_x = 429;

	// CSRS -- a separate, updating list of every CSR address a CSRR*/
	// CSRR*I instruction has actually touched (Registers::csr_history,
	// most-recently-used first -- see registers.hpp), each shown with its
	// *live* current value (RiscvCore::read_csr_effective, not a raw
	// csr[] read -- several of the most interesting ones, like sstatus/
	// mip/misa/time, are computed rather than stored, see that function's
	// comment). Only entries actually seen so far are drawn, not a fixed
	// 20 blank rows -- an "updating list", not a static table. Longest
	// name (e.g. "mvendorid"/"siselect") is 9 chars, so the value column
	// sits at +46 instead of the register file's +19. csr_x anchors off
	// the game box's own right edge (moved "more to the left", as close
	// to the box as still leaves a visible gap) instead of working
	// backward from hud_x -- the corridor between the box and the
	// register file is only wide enough for one anchor choice at a time,
	// and pushing CSRs toward the box is what actually moves them left
	// (working from hud_x just trades box-gap for register-gap, it
	// can't shift the block itself).
	const int CSR_VALUE_OFFSET = 46;
	// box->CSRS and CSRS->REGISTER FILE gaps are now equal (6/7, as close
	// as the corridor's odd total slack allows) -- was 2/11, which read as
	// CSRS sitting flush against the display but oddly distant from the
	// register file. "Aligned" spacing means matching, not just small.
	const int CSR_BOX_GAP = 6;
	const int CSR_COL_W = CSR_VALUE_OFFSET + REG_HEX_W; // name column + 16 hex digits -- the title centers over this
	int csr_x = GAME_BOX_X + GAME_BOX_W + CSR_BOX_GAP; // box's own design-unit position, not the scaled box_x/box_w below

	draw_centered_title(csr_x, CSR_COL_W, 7, "--- CSRS ---", pal_pink);
	int csr_start_y = 7 + 13;
	for (int i = 0; i < snap.csr_count; i++) {
		int cy = csr_start_y + (i * REG_ROW_H);
		const Snapshot::CsrEntry &c = snap.csrs[i];
		const char *name = csr_name(c.addr);
		char name_buf[16];
		if (!name) { sprintf(name_buf, "0x%03x", c.addr); name = name_buf; }
		sprintf(buf, "%s:", name);
		draw_shadow_text(csr_x, cy, buf, pal_red, REG_SCALE_X, REG_SCALE_Y);
		sprintf(buf, "%016llX", (unsigned long long)c.value);
		draw_shadow_text(csr_x + CSR_VALUE_OFFSET, cy, buf, pal_white, REG_SCALE_X, REG_SCALE_Y);
	}

	// REGISTER FILE
	int current_y = 7;

	// Two columns (X, V). Each glyph is drawn taller-not-wider
	// (REG_SCALE_Y > REG_SCALE_X, see draw_char/draw_string's independent
	// x/y scale) and rows sit further apart (REG_ROW_H grown well past
	// the taller glyph height, so there's real gap between rows, not
	// just bigger text touching). DESIGN_H has the extra room this needs
	// set aside (see gui.hpp). V is currently all zeros -- storage only,
	// see registers.hpp -- but the layout doesn't assume that.
	const int REG_TOTAL_W = REG_COL_W + REG_VALUE_OFFSET + REG_HEX_W; // X column + V column -- the title centers over both together
	draw_centered_title(hud_x, REG_TOTAL_W, current_y, "--- REGISTER FILE ---", pal_pink);
	int reg_start_y = current_y + 13;
	int x_col = hud_x;
	int v_col = hud_x + REG_COL_W;
	for (int i = 0; i < 32; i++) {
		int cy = reg_start_y + (i * REG_ROW_H);

		sprintf(buf, "X%02d:", i);
		draw_shadow_text(x_col, cy, buf, pal_red, REG_SCALE_X, REG_SCALE_Y);
		sprintf(buf, "%016llX", (unsigned long long)snap.x[i]);
		draw_shadow_text(x_col + REG_VALUE_OFFSET, cy, buf, pal_white, REG_SCALE_X, REG_SCALE_Y);

		sprintf(buf, "V%02d:", i);
		draw_shadow_text(v_col, cy, buf, pal_red, REG_SCALE_X, REG_SCALE_Y);
		sprintf(buf, "%016llX", (unsigned long long)snap.v_lo[i]);
		draw_shadow_text(v_col + REG_VALUE_OFFSET, cy, buf, pal_white, REG_SCALE_X, REG_SCALE_Y);
	}

	// TRACE LOG -- lives below the game view box instead of the right
	// panel, which the register file now needs in full. trace_y tracks
	// GAME_BOX_Y/H's own bottom edge instead of a bare literal, so the
	// two can't silently drift out of sync again.
	//
	// TRACE_SCALE shrinks this text below the default 1.0 for two
	// reasons: (1) a disassembled line ("0000000080021B94: BNE A4, A0,
	// 0X80021B98", ~40 chars) at full size runs well past GAME_BOX_W --
	// 0.75 keeps even the longest lines within a few chars of the box's
	// own width, so the trace log and the display "try to be near the
	// same width" instead of overhanging it. (2) TRACE_ROW_H is derived
	// FROM TRACE_SCALE's actual glyph height plus a fixed gap, instead
	// of being a separate hand-picked number that can silently drift out
	// of sync with it again (that mismatch is exactly what caused lines
	// to overlap/garble after DESIGN_W/H's 960x540 -> 640x360 shrink
	// raised the global render scale from 2.0 to 3.0, enlarging every
	// glyph, while the row step here stayed a bare unrelated literal) --
	// this construction makes that class of bug structurally impossible,
	// not just fixed for the current scale.
	const float TRACE_SCALE = 0.75f;
	const int TRACE_LINE_GAP = 4; // extra breathing room between rows, on top of glyph height
	const int TRACE_ROW_H = (int)(8 * TRACE_SCALE) + TRACE_LINE_GAP;
	int trace_x = GAME_BOX_X;
	int trace_y = GAME_BOX_Y + GAME_BOX_H + 2;
	draw_centered_title(GAME_BOX_X, GAME_BOX_W, trace_y, "--- TRACE LOG ---", pal_pink, TRACE_SCALE);
	trace_y += TRACE_ROW_H;

	char op_buf[64];

	// Most recently recorded history entry == the instruction that just executed.
	format_operands(op_buf, sizeof(op_buf), snap.active.pc, snap.active.decoded);
	sprintf(buf, "ACTIVE: %08X %s %s", snap.active.instr, snap.active.decoded.mnemonic, op_buf);
	draw_shadow_text(trace_x, trace_y, buf, pal_pink, TRACE_SCALE);
	trace_y += TRACE_ROW_H;

	sprintf(buf, "CURR PC: %016llX", (unsigned long long)snap.pc);
	draw_shadow_text(trace_x, trace_y, buf, pal_white, TRACE_SCALE);
	trace_y += TRACE_ROW_H;

	for (int i = 0; i < 13; i++) {
		const HistoryEntry &h = snap.trace[i];
		format_operands(op_buf, sizeof(op_buf), h.pc, h.decoded);
		sprintf(buf, "%016llX: %s %s", (unsigned long long)h.pc, h.decoded.mnemonic, op_buf);
		draw_shadow_text(trace_x, trace_y, buf, pal_stats, TRACE_SCALE);
		trace_y += TRACE_ROW_H;
	}

	SDL_UpdateTexture(texture, nullptr, screen_buf.data(), canvas_w * 4);
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

std::vector<RawKeyEvent> Gui::poll_input()
{
	std::vector<RawKeyEvent> events;

	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) exit(0);
		if (e.type == SDL_KEYDOWN) events.push_back({(uint32_t)e.key.keysym.sym, true});
		if (e.type == SDL_KEYUP) events.push_back({(uint32_t)e.key.keysym.sym, false});
	}

	return events;
}

void Gui::draw_char(int x, int y, char c, uint32_t col, float scale_x_, float scale_y_)
{
	if ((uint8_t)c >= 128) return;
	if (scale_y_ < 0.0f) scale_y_ = scale_x_;

	// The glyph's origin (x,y) stays in the normal design-unit grid --
	// only the 8x8 bitmap's own pixels shrink -- so a smaller-scale
	// string still lines up with normal-scale text around it.
	float px_scale = scale_x * scale_x_;
	float py_scale = scale_y * scale_y_;
	int bw = (int)(px_scale + 0.5f); if (bw < 1) bw = 1;
	int bh = (int)(py_scale + 0.5f); if (bh < 1) bh = 1;
	int origin_x = (int)(x * scale_x);
	int origin_y = (int)(y * scale_y);

	for (int r = 0; r < 8; r++) {
		uint8_t b = font8x8[(uint8_t)c][r];
		for (int cl = 0; cl < 8; cl++) {
			if (!(b & (0x80 >> cl))) continue;

			int px = origin_x + (int)(cl * px_scale);
			int py = origin_y + (int)(r * py_scale);
			for (int by = 0; by < bh; by++) {
				int ty = py + by;
				if (ty < 0 || ty >= canvas_h) continue;
				uint32_t *row = &screen_buf[(size_t)ty * canvas_w];
				for (int bx = 0; bx < bw; bx++) {
					int tx = px + bx;
					if (tx < 0 || tx >= canvas_w) continue;
					row[tx] = col;
				}
			}
		}
	}
}

void Gui::draw_string(int x, int y, const char *s, uint32_t c, float scale_x_, float scale_y_)
{
	// Advance (character pitch) tracks only scale_x_ -- a taller-but-not-
	// wider string (scale_y_ > scale_x_) still lays its characters out at
	// their normal horizontal spacing, it just draws each one taller.
	int advance = (int)(8 * scale_x_ + 0.5f); if (advance < 1) advance = 1;
	while (*s) { draw_char(x, y, *s++, c, scale_x_, scale_y_); x += advance; }
}

void Gui::init_font()
{
	std::memset(font8x8, 0, sizeof(font8x8));
	auto set_char = [this](char c, std::initializer_list<uint8_t> rows) {
		int i = 0;
		for (uint8_t row : rows) {
			if (i < 8) font8x8[(uint8_t)c][i++] = row;
		}
	};

	set_char('0', {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00});
	set_char('1', {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00});
	set_char('2', {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00});
	set_char('3', {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00});
	set_char('4', {0x0C, 0x1C, 0x2C, 0x4C, 0x7E, 0x0C, 0x0C, 0x00});
	set_char('5', {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00});
	set_char('6', {0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00});
	set_char('7', {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00});
	set_char('8', {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00});
	set_char('9', {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00});
	set_char('A', {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00});
	set_char('B', {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00});
	set_char('C', {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00});
	set_char('D', {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00});
	set_char('E', {0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x7E, 0x00});
	set_char('F', {0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x60, 0x00});

	set_char('G', {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00});
	set_char('H', {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00});
	set_char('I', {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00});
	set_char('L', {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00});
	set_char('M', {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00});
	set_char('N', {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00});
	set_char('O', {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00});
	set_char('P', {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00});
	set_char('R', {0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00});
	set_char('S', {0x3E, 0x60, 0x60, 0x3C, 0x06, 0x06, 0x7C, 0x00});
	set_char('T', {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00});
	set_char('U', {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00});
	set_char('V', {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00});
	set_char('Y', {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00});
	set_char('J', {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00});
	set_char('K', {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00});
	set_char('Q', {0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x0E, 0x00});
	set_char('W', {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00});
	set_char('X', {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00});
	set_char('Z', {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00});
	set_char('?', {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00});

	set_char('n', {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00});
	set_char('s', {0x00, 0x00, 0x3C, 0x60, 0x3C, 0x06, 0x3C, 0x00});
	set_char('i', {0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00});
	set_char('.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00});
	set_char('/', {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00});
	set_char(':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00});
	set_char('-', {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00});
	set_char(' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

	// Disassembly operand text (register names like "a1"/"sp"/"t0"/"fa0",
	// CSR names like "mstatus") needs the full lowercase alphabet, not
	// just the handful (n/s/i) existing labels used -- and needs ',' '('
	// ')', which nothing before this needed either.
	set_char(',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30});
	set_char('(', {0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0x18, 0x0C});
	set_char(')', {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C, 0x18, 0x30});

	// Same shape as the uppercase glyph for any lowercase letter that
	// doesn't already have its own distinct bitmap above -- good enough
	// for a debug HUD font, and far less error-prone than hand-authoring
	// two dozen more 8x8 bitmaps from scratch.
	for (char c = 'a'; c <= 'z'; c++) {
		bool already_defined = false;
		for (int i = 0; i < 8; i++) {
			if (font8x8[(uint8_t)c][i] != 0) { already_defined = true; break; }
		}
		if (!already_defined) {
			char upper = (char)(c - 'a' + 'A');
			for (int i = 0; i < 8; i++) font8x8[(uint8_t)c][i] = font8x8[(uint8_t)upper][i];
		}
	}
}
