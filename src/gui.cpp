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
	case 0x106: return "scounteren"; // Zicntr/Zihpm gating (phase 2)
	case 0x10A: return "senvcfg";    // Ssnpm's PMM lives here (phase 7)
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
	case 0x306: return "mcounteren";
	case 0x30A: return "menvcfg";
	case 0x340: return "mscratch";
	case 0x341: return "mepc";
	case 0x342: return "mcause";
	case 0x343: return "mtval";
	case 0x344: return "mip";
	case 0x350: return "miselect"; // AIA indirect (Stage 2)
	case 0x351: return "mireg";
	case 0x35C: return "mtopei";
	case 0xC00: return "cycle";   // Zicntr: mcycle, mtime and minstret
	case 0xC01: return "time";    // read unprivileged -- see
	case 0xC02: return "instret"; // ext_zicntr.cpp
	case 0xDB0: return "stopi";   // Ssaia top-interrupt
	case 0xFB0: return "mtopi";   // Smaia top-interrupt
	case 0xF11: return "mvendorid";
	case 0xF12: return "marchid";
	case 0xF13: return "mimpid";
	case 0xF14: return "mhartid";
	// Hypervisor (H). Two groups that are easy to confuse in a dashboard,
	// which is exactly why they are named rather than left as raw
	// addresses: the h* registers belong to the hypervisor running in
	// HS-mode, while the vs* registers are the guest's *shadow* copies of
	// the S-mode registers. When the hart is in VS-mode a guest's write to
	// "stvec" lands in vstvec, so seeing both here side by side is what
	// makes a two-stage trap legible at all.
	case 0x600: return "hstatus";
	case 0x602: return "hedeleg";
	case 0x603: return "hideleg";
	case 0x604: return "hie";
	case 0x605: return "htimedelta";
	case 0x606: return "hcounteren";
	case 0x607: return "hgeie";
	case 0x643: return "htval";
	case 0x644: return "hip";
	case 0x645: return "hvip";
	case 0x64A: return "htinst";
	case 0x60A: return "henvcfg";
	case 0x680: return "hgatp";
	case 0xE12: return "hgeip";
	// The VS-mode shadows of the S-mode registers.
	case 0x200: return "vsstatus";
	case 0x204: return "vsie";
	case 0x205: return "vstvec";
	case 0x240: return "vsscratch";
	case 0x241: return "vsepc";
	case 0x242: return "vscause";
	case 0x243: return "vstval";
	case 0x244: return "vsip";
	case 0x280: return "vsatp";
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

namespace {

// One kind of dashboard text: the 8x8 font scaled by sx across and sy down,
// in layout units, with `track` units of extra letter spacing.
struct TextStyle {
	float sx, sy;
	int track;
};

// Where everything around the display goes. Two of these exist and they
// draw the same information: the design-unit layout that has always been
// here, and the compact one every guest uses at 1920x1080 and up.
struct DashLayout {
	int box_x, box_y;            // the display's origin, if the layout places it
	int shadow;                  // drop-shadow offset
	TextStyle title, reg, trace, trace_title;
	int title_y, list_y, row_h;  // CSRS / REGISTER FILE headers and rows
	int csr_x, csr_value_offset;
	int reg_x, reg_col_w, reg_value_offset;
	int trace_x, trace_y, trace_row_h;
	int trace_title_x, trace_title_w, trace_title_h;
	int trace_max_chars;         // 0: lines may run past trace_title_w
	int banner_line_h;
	int banner_y;
	// 0: right-align the banner to the register file. Otherwise centre it
	// over [banner_center_x, banner_center_x + banner_center_w).
	int banner_center_x, banner_center_w;
};

// The original layout, in design units (see gui.hpp's DESIGN_W/H), built
// around the display box it is given.
DashLayout doom_layout(int box_x, int box_y, int box_w, int box_h)
{
	DashLayout L{};
	L.shadow = 1;
	L.title = {1.0f, 1.0f, 0};
	// Taller than wide, and sized to track the 960x540 -> 640x360 design
	// shrink so glyphs land at the same screen size as before it.
	L.reg = {0.567f, 0.867f, 0};
	// A disassembled line is ~40 chars; at 1.0 it overhangs the display
	// box, at 0.75 it stays within a few characters of the box's width.
	L.trace = {0.75f, 0.75f, 0};

	L.title_y = 7;
	L.list_y = 7 + 13;
	// 32 rows at 10 plus the header offset is the tallest this goes
	// without the last row clipping past DESIGN_H.
	L.row_h = 10;

	// CSRS anchors off the box's right edge. The corridor between the box
	// and the register file is only wide enough for a 6/7 split of its
	// slack, which is why the gap is what it is. The value column sits at
	// 46 -- nine glyphs, the longest name when it was written -- and is a
	// floor rather than a fixed position: the hypervisor's ten-character
	// names push only their own row's value across.
	L.csr_x = box_x + box_w + 6;
	L.csr_value_offset = 46;

	// Picked so the V column's last hex digit lands just shy of DESIGN_W.
	L.reg_x = 429;
	L.reg_col_w = 106;
	L.reg_value_offset = 19;

	L.trace_x = box_x;
	L.trace_y = box_y + box_h + 2;
	// Derived from the glyph height rather than a separate literal: a bare
	// number here is what let rows overlap when the render scale changed.
	L.trace_row_h = (int)(8 * L.trace.sy) + 4;
	L.trace_title_x = box_x;
	L.trace_title_w = box_w;
	// The title is the trace text's own size and one row tall, and the
	// longest lines are allowed to overhang the box by a few characters.
	L.trace_title = L.trace;
	L.trace_title_h = L.trace_row_h;
	L.trace_max_chars = 0;

	L.banner_line_h = 9;
	L.banner_y = L.list_y + 32 * L.row_h;   // in the corner under x31/v31
	L.banner_center_x = 0;
	L.banner_center_w = 0;
	return L;
}

// The compact layout, for any guest in a window of 1920x1080 or more, in
// *pixels*.
//
// The design-unit layout spends 13.5 pixels across on each register
// character and leaves the display a box that shrinks a Linux console to
// fit. This one draws the same panels with narrower text -- 10 pixels a
// character -- and packs them into one column, which buys a display area the
// size of the Linux console: that guest is shown at exactly 1:1, with no
// resampling at all, and DOOM's 320x200 is scaled up to fill the same area.
// The column does not move between guests.
//
// Pixels rather than design units because the point is pixel-exact glyphs,
// and that only means something at a known size: this layout needs a
// 1920x1080 canvas, and render() falls back to the design-unit one below it.
constexpr int COMPACT_MIN_W = 1920;
constexpr int COMPACT_MIN_H = 1080;

DashLayout compact_layout(int canvas_w, int canvas_h)
{
	DashLayout L{};
	L.shadow = 1;
	L.title = {2.0f, 3.0f, 2};   // 16x24, 18 pitch
	// 10x16 on a 10-pixel pitch. As wide as the column's 728 pixels allow:
	// the two panels are 70 characters across between them, and the height
	// is already spoken for by 32 register rows and the trace log, so the
	// only room to grow is sideways. The 1.25x width puts the font's
	// two-column strokes at 2 or 3 pixels, and its blank first column
	// leaves a gap between letters.
	L.reg = {1.25f, 2.0f, 0};
	// 12x16 on a 13-pixel pitch, so a typical disassembled line fills the
	// trace log's width (below). A 1.5x width alternates 1- and 2-pixel
	// columns, but two adjacent columns always make 3 pixels, so the
	// font's two-column strokes all come out the same.
	L.trace = {1.5f, 2.0f, 1};

	const int console_w = Memory::LFB_W;
	const int console_h = Memory::LFB_H;
	const int pitch = (int)(8 * L.reg.sx + 0.5f) + L.reg.track;
	const int hex_w = 16 * pitch;

	// The register file: "X00:" plus a gap, 16 hex digits, and two
	// characters between the X and V columns.
	L.reg_value_offset = 5 * pitch;
	L.reg_col_w = L.reg_value_offset + hex_w + 2 * pitch;
	const int reg_w = L.reg_col_w + L.reg_value_offset + hex_w;
	// CSRS: the value column fits a nine-character name and its colon. It
	// is a floor, so a longer name pushes only its own row's value across.
	L.csr_value_offset = 10 * pitch;
	const int csr_w = L.csr_value_offset + hex_w;

	// The console is centred vertically, and the same margin is used on
	// the left and right, so the window has one margin all the way round.
	L.box_y = (canvas_h - console_h) / 2;
	const int margin = L.box_y;

	// Left to right: margin, console, gap, CSRS, gap, register file,
	// margin. Whatever is left after the fixed margins is split evenly
	// between the two inner gaps, with an odd pixel going to the second.
	const int slack_x = canvas_w - 2 * margin - console_w - csr_w - reg_w;
	const int gap_x = slack_x / 2;
	L.box_x = margin;
	L.csr_x = L.box_x + console_w + gap_x;
	L.reg_x = L.csr_x + csr_w + (slack_x - gap_x);
	const int column_right = L.reg_x + reg_w;   // canvas_w - margin

	// Top to bottom: the console centred, with equal margins above and
	// below, and the column beside it spanning exactly the same top and
	// bottom. Its three blocks -- the panels, the trace log, the paused
	// banner -- are separated by two equal gaps. The banner's space is kept
	// even while running, so nothing moves when the machine pauses.
	const int top = L.box_y;
	const int bottom = L.box_y + console_h;

	const int glyph_h = 16;
	// 16-pixel text on a 20-pixel pitch; the font leaves its bottom row
	// blank and this text is capitals and hex, so the visible gap is six.
	L.row_h = 20;
	L.trace_row_h = 18;
	L.trace_title_h = 24 + 8;
	L.banner_line_h = 20;
	const int panels_h = 24 + 14 + 31 * L.row_h + glyph_h;
	const int trace_h = L.trace_title_h + 14 * L.trace_row_h + glyph_h;   // title + 15 rows
	const int banner_h = L.banner_line_h + glyph_h;
	const int gap_y = (bottom - top - panels_h - trace_h - banner_h) / 2;

	L.title_y = top;
	L.list_y = top + 24 + 14;

	// The trace log runs from the CSR panel's left edge to the register
	// file's right one, under a title the size of the CSRS and REGISTER
	// FILE headers. At 13 pixels a character that is 54 characters at
	// 1920 wide; a line longer than fits is cut at the edge rather than
	// running into the margin.
	L.trace_y = top + panels_h + gap_y;
	L.trace_x = L.csr_x;
	L.trace_title_x = L.csr_x;
	L.trace_title_w = column_right - L.csr_x;
	L.trace_title = L.title;
	const int trace_pitch = (int)(8 * L.trace.sx + 0.5f) + L.trace.track;
	L.trace_max_chars = L.trace_title_w / trace_pitch;

	// The paused banner closes the column, bottom-aligned with the console
	// and centred like the trace log above it.
	L.banner_y = bottom - banner_h;
	L.banner_center_x = L.csr_x;
	L.banner_center_w = L.trace_title_w;
	return L;
}

} // namespace

namespace {
// The fallback layout's display box, in design units: 280x175 at (10,7), the
// same 1.6 aspect ratio the box has always had. Its y matches the CSRS and
// REGISTER FILE headers so the display's top edge lines up with them. The
// CSRS panel anchors off the box's right edge and the trace log off its
// bottom, so neither needs a second edit if this box ever moves.
constexpr int GAME_BOX_X = 10, GAME_BOX_Y = 7, GAME_BOX_W = 280, GAME_BOX_H = 175;
constexpr uint32_t DASH_BG = 0x876A96;
}

void Gui::set_guest_display(int w, int h)
{
	guest_w = w;
	guest_h = h;
	frame.assign((size_t)w * (size_t)h, 0);
	frame_shown.assign((size_t)w * (size_t)h, 0);
}

Gui::Rect Gui::display_rect(bool full, bool *compact_out) const
{
	// With room for it -- a 1920x1080 canvas -- every guest gets the
	// compact layout: its display on the left in the console-sized area, and
	// the CSRS, register file and trace log in one column beside it. See
	// compact_layout. Linux's console fills that area at exactly 1:1; DOOM's
	// 320x200 is scaled up into it, keeping its shape.
	//
	// Below 1920x1080 the design-unit layout is the fallback: the display
	// goes in its game box, which a Linux console only fits well under 1:1.
	// Either way a source of a different shape from its box is letterboxed
	// rather than stretched, and Ctrl+Alt+F hands a Linux framebuffer the
	// whole window.
	const bool fullscreen = guest_is_linux() && full;
	const bool compact = !fullscreen && canvas_w >= COMPACT_MIN_W && canvas_h >= COMPACT_MIN_H;
	if (compact_out) *compact_out = compact;

	Rect r{ (int)(GAME_BOX_X * scale_x), (int)(GAME_BOX_Y * scale_y),
	        (int)(GAME_BOX_W * scale_x), (int)(GAME_BOX_H * scale_y) };
	if (fullscreen) r = { 0, 0, canvas_w, canvas_h };
	if (compact) {
		// The display area is the console's size whatever the guest is, so
		// the column beside it does not move between guests.
		const DashLayout L = compact_layout(canvas_w, canvas_h);
		r = { L.box_x, L.box_y, Memory::LFB_W, Memory::LFB_H };
	}
	if ((guest_is_linux() || compact) && guest_w > 0 && guest_h > 0) {
		// Fit, preserving aspect: shrink the long axis and re-centre in
		// whichever dimension gave way.
		const long long by_w = (long long)r.w * guest_h;
		const long long by_h = (long long)r.h * guest_w;
		if (by_w > by_h) {
			const int fit_w = (int)(by_h / guest_h);
			r.x += (r.w - fit_w) / 2;
			r.w = fit_w;
		} else if (by_h > by_w) {
			const int fit_h = (int)(by_w / guest_w);
			r.y += (r.h - fit_h) / 2;
			r.h = fit_h;
		}
	}
	return r;
}

void Gui::submit_frame(std::vector<uint32_t> &pixels)
{
	std::lock_guard<std::mutex> lock(frame_mutex);
	if (pixels.size() != frame.size()) return;
	frame.swap(pixels);
	frame_seq++;
}

void Gui::blit_display(const Rect &r)
{
	const int box_x = r.x, box_y = r.y, box_w = r.w, box_h = r.h;

	// Bilinear, not nearest-neighbor: at native 320x200 scaled ~3-4x, hard
	// pixel blocks looked wrong for the game view (dashboard text stays
	// sharp block-fills on purpose, this is just the rendered scene).
	// Fixed-point (8-bit fraction) so the per-pixel blend is pure integer
	// math, no floats in the hot loop.
	//
	// Interpolation is only right when scaling up. Text being scaled down
	// wants the nearest source pixel, because blending neighbours is
	// precisely what destroys a one-pixel stem -- so the LUT is built with
	// a zero fraction and both taps on the same pixel there, which turns
	// the same blend loop below into a plain copy without a second code
	// path.
	struct Sample { int i0, i1; uint32_t frac; };
	static std::vector<Sample> sx_lut, sy_lut;
	static int last_box_w = -1, last_box_h = -1, last_src_w = -1, last_src_h = -1;
	// Nearest whenever the source is at least as big as the box it is
	// going into. Interpolation is only right when scaling up, and at the
	// compact layout's exact 1:1 this makes the copy exact by construction
	// rather than by a blend weight happening to come out as zero.
	const bool nearest = (guest_w >= box_w || guest_h >= box_h);
	if (box_w != last_box_w || box_h != last_box_h
	    || guest_w != last_src_w || guest_h != last_src_h) {
		sx_lut.resize(box_w > 0 ? box_w : 0);
		sy_lut.resize(box_h > 0 ? box_h : 0);
		for (int x = 0; x < box_w; x++) {
			float src = ((float)x + 0.5f) * guest_w / box_w - 0.5f;
			int i0 = (int)std::floor(src);
			float frac = src - (float)i0;
			if (i0 < 0) { i0 = 0; frac = 0.0f; }
			if (i0 >= guest_w) i0 = guest_w - 1;
			int i1 = (i0 + 1 < guest_w) ? i0 + 1 : i0;
			if (nearest) { if (frac >= 0.5f) i0 = i1; i1 = i0; frac = 0.0f; }
			sx_lut[x] = { i0, i1, (uint32_t)(frac * 256.0f) };
		}
		for (int y = 0; y < box_h; y++) {
			float src = ((float)y + 0.5f) * guest_h / box_h - 0.5f;
			int i0 = (int)std::floor(src);
			float frac = src - (float)i0;
			if (i0 < 0) { i0 = 0; frac = 0.0f; }
			if (i0 >= guest_h) i0 = guest_h - 1;
			int i1 = (i0 + 1 < guest_h) ? i0 + 1 : i0;
			if (nearest) { if (frac >= 0.5f) i0 = i1; i1 = i0; frac = 0.0f; }
			sy_lut[y] = { i0, i1, (uint32_t)(frac * 256.0f) };
		}
		last_box_w = box_w;
		last_box_h = box_h;
		last_src_w = guest_w;
		last_src_h = guest_h;
	}

	const uint32_t *fb32 = frame_shown.data();
	for (int y = 0; y < box_h; y++) {
		int ty = box_y + y;
		if (ty < 0 || ty >= canvas_h) continue;

		const Sample &ys = sy_lut[y];
		const uint32_t *row0 = fb32 + (size_t)ys.i0 * guest_w;
		const uint32_t *row1 = fb32 + (size_t)ys.i1 * guest_w;
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
}

void Gui::render_dashboard(const Snapshot &snap)
{
	uint32_t pal_pink  = 0xD580B8;
	uint32_t pal_white = 0xD7D0D0;
	uint32_t pal_red   = 0xB95167;
	uint32_t pal_dark  = 0x25080C;
	uint32_t pal_stats = 0xC3A9C4;

	// Always the layout that shares the window. In the full-window mode the
	// compositor does not show this layer, but it is kept current so that
	// leaving that mode shows the machine's state now, not from before.
	bool compact = false;
	display_rect(false, &compact);
	const DashLayout L = compact ? compact_layout(canvas_w, canvas_h)
	                             : doom_layout(GAME_BOX_X, GAME_BOX_Y, GAME_BOX_W, GAME_BOX_H);

	dash_work.assign((size_t)canvas_w * (size_t)canvas_h, DASH_BG);

	char buf[96];

	text_ux = compact ? 1.0f : scale_x;
	text_uy = compact ? 1.0f : scale_y;

	auto draw_shadow_text = [&](int x, int y, const char *s, uint32_t col, const TextStyle &st) {
		draw_string(x + L.shadow, y + L.shadow, s, pal_dark, st.sx, st.sy, st.track);
		draw_string(x, y, s, col, st.sx, st.sy, st.track);
	};
	// Matches draw_string's own advance exactly, so a centred title lines
	// up with the glyphs pixel for pixel rather than approximately.
	auto glyph_adv = [&](const TextStyle &st) {
		int adv = (int)(8 * st.sx + 0.5f); if (adv < 1) adv = 1;
		return adv + st.track;
	};
	auto text_w = [&](const char *s, const TextStyle &st) {
		return (int)strlen(s) * glyph_adv(st);
	};
	auto draw_centered_title = [&](int col_x, int col_w, int y, const char *s, uint32_t col, const TextStyle &st) {
		draw_shadow_text(col_x + (col_w - text_w(s, st)) / 2, y, s, col, st);
	};

	const int REG_HEX_W = 16 * glyph_adv(L.reg);

	// CSRS -- the CSRs the guest uses most across its last 1024 CSR
	// instructions, most frequent first (Registers::top_csrs), each with
	// its *live* value, since several of the interesting ones (sstatus,
	// mip, time) are computed rather than stored.
	const int CSR_COL_W = L.csr_value_offset + REG_HEX_W;
	draw_centered_title(L.csr_x, CSR_COL_W, L.title_y, "--- CSRS ---", pal_pink, L.title);
	for (int i = 0; i < snap.csr_count; i++) {
		int cy = L.list_y + (i * L.row_h);
		const Snapshot::CsrEntry &c = snap.csrs[i];
		const char *name = csr_name(c.addr);
		char name_buf[16];
		if (!name) { sprintf(name_buf, "0x%03x", c.addr); name = name_buf; }
		sprintf(buf, "%s:", name);
		// A floor, not a position: a name longer than the column pushes
		// only its own value across rather than drawing over it.
		int value_x = L.csr_value_offset;
		int label_w = text_w(buf, L.reg) + glyph_adv(L.reg); // one glyph of gap
		if (label_w > value_x) value_x = label_w;
		draw_shadow_text(L.csr_x, cy, buf, pal_red, L.reg);
		sprintf(buf, "%016llX", (unsigned long long)c.value);
		draw_shadow_text(L.csr_x + value_x, cy, buf, pal_white, L.reg);
	}

	// REGISTER FILE -- two columns, X and V.
	const int REG_TOTAL_W = L.reg_col_w + L.reg_value_offset + REG_HEX_W;
	draw_centered_title(L.reg_x, REG_TOTAL_W, L.title_y, "--- REGISTER FILE ---", pal_pink, L.title);
	int x_col = L.reg_x;
	int v_col = L.reg_x + L.reg_col_w;
	for (int i = 0; i < 32; i++) {
		int cy = L.list_y + (i * L.row_h);

		sprintf(buf, "X%02d:", i);
		draw_shadow_text(x_col, cy, buf, pal_red, L.reg);
		sprintf(buf, "%016llX", (unsigned long long)snap.x[i]);
		draw_shadow_text(x_col + L.reg_value_offset, cy, buf, pal_white, L.reg);

		sprintf(buf, "V%02d:", i);
		draw_shadow_text(v_col, cy, buf, pal_red, L.reg);
		sprintf(buf, "%016llX", (unsigned long long)snap.v_lo[i]);
		draw_shadow_text(v_col + L.reg_value_offset, cy, buf, pal_white, L.reg);
	}

	// TRACE LOG -- under the display, since the right side is the
	// register file's.
	int trace_y = L.trace_y;
	draw_centered_title(L.trace_title_x, L.trace_title_w, trace_y, "--- TRACE LOG ---", pal_pink, L.trace_title);
	trace_y += L.trace_title_h;
	// Cut a line at the layout's width, if it has one.
	auto fit_trace = [&](char *line) {
		if (L.trace_max_chars > 0 && (int)strlen(line) > L.trace_max_chars)
			line[L.trace_max_chars] = '\0';
	};

	char op_buf[64];

	// Most recently recorded history entry == the instruction that just executed.
	format_operands(op_buf, sizeof(op_buf), snap.active.pc, snap.active.decoded);
	sprintf(buf, "ACTIVE: %08X %s %s", snap.active.instr, snap.active.decoded.mnemonic, op_buf);
	fit_trace(buf);
	draw_shadow_text(L.trace_x, trace_y, buf, pal_pink, L.trace);
	trace_y += L.trace_row_h;

	sprintf(buf, "CURR PC: %016llX", (unsigned long long)snap.pc);
	draw_shadow_text(L.trace_x, trace_y, buf, pal_white, L.trace);
	trace_y += L.trace_row_h;

	for (int i = 0; i < 13; i++) {
		const HistoryEntry &h = snap.trace[i];
		format_operands(op_buf, sizeof(op_buf), h.pc, h.decoded);
		sprintf(buf, "%016llX: %s %s", (unsigned long long)h.pc, h.decoded.mnemonic, op_buf);
		fit_trace(buf);
		draw_shadow_text(L.trace_x, trace_y, buf, pal_stats, L.trace);
		trace_y += L.trace_row_h;
	}

	// Paused indicator, drawn last so nothing overdraws it -- without it a
	// frozen machine looks exactly like one stuck in a tight loop. Two
	// lines in the corner under the last register row, right-aligned to
	// the register block, rather than across the display.
	if (snap.halted) {
		const char *msg_top = "PAUSED -- F9 TO RESUME";
		const char *msg_bot = "(DELIVERS THE TRAP)";
		const int by = L.banner_y;
		if (L.banner_center_w > 0) {
			draw_centered_title(L.banner_center_x, L.banner_center_w, by, msg_top, pal_red, L.reg);
			draw_centered_title(L.banner_center_x, L.banner_center_w, by + L.banner_line_h, msg_bot, pal_red, L.reg);
		} else {
			const int right_edge = L.reg_x + REG_TOTAL_W;
			draw_shadow_text(right_edge - text_w(msg_top, L.reg), by, msg_top, pal_red, L.reg);
			draw_shadow_text(right_edge - text_w(msg_bot, L.reg), by + L.banner_line_h, msg_bot, pal_red, L.reg);
		}
	}

	std::lock_guard<std::mutex> lock(dash_mutex);
	dash_ready.swap(dash_work);
	dash_seq++;
}

void Gui::present()
{
	// Composite only when something changed: a new guest frame, a new
	// dashboard, or the full-window mode toggling. Otherwise the texture
	// already holds the right picture and presenting it again is free.
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(frame_mutex);
		if (frame_seq != frame_seen) {
			frame_shown.swap(frame);
			frame_seen = frame_seq;
			changed = true;
		}
	}
	{
		std::lock_guard<std::mutex> lock(dash_mutex);
		if (dash_seq != dash_seen) {
			dash_shown.swap(dash_ready);
			dash_seen = dash_seq;
			changed = true;
		}
	}
	if (fb_full != composed_full) {
		composed_full = fb_full;
		changed = true;
	}

	if (changed) {
		// In the full-window mode there is nowhere to put the dashboard, and
		// half-drawing it over the guest's console would be worse than not
		// drawing it.
		const bool fullscreen = guest_is_linux() && fb_full;
		if (!fullscreen && dash_shown.size() == screen_buf.size())
			std::copy(dash_shown.begin(), dash_shown.end(), screen_buf.begin());
		else
			std::fill(screen_buf.begin(), screen_buf.end(), DASH_BG);
		blit_display(display_rect(fb_full));
		SDL_UpdateTexture(texture, nullptr, screen_buf.data(), canvas_w * (int)sizeof(uint32_t));
	}

	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	dump_canvas();
	SDL_RenderPresent(renderer);
}

void Gui::dump_canvas()
{
	if (canvas_dump_path.empty()) return;
	// Every 60th frame rather than every frame: this is a debugging aid
	// watched from outside the process, and what it needs is a file that
	// is never very stale, not one rewritten at the frame rate.
	if (canvas_dump_frames++ % 60 != 0) return;
	if (screen_buf.empty() || canvas_w <= 0 || canvas_h <= 0) return;

	FILE *f = std::fopen(canvas_dump_path.c_str(), "wb");
	if (!f) return;
	std::fprintf(f, "P6\n%d %d 255\n", canvas_w, canvas_h);
	// screen_buf is SDL_PIXELFORMAT_RGB888, which is 32-bit xRGB in host
	// order despite the name -- the byte the name leaves out is the unused
	// one. PPM wants three bytes per pixel, so drop it.
	std::vector<uint8_t> row((size_t)canvas_w * 3);
	for (int y = 0; y < canvas_h; y++) {
		const uint32_t *src = &screen_buf[(size_t)y * canvas_w];
		for (int x = 0; x < canvas_w; x++) {
			row[x * 3 + 0] = (uint8_t)(src[x] >> 16);
			row[x * 3 + 1] = (uint8_t)(src[x] >> 8);
			row[x * 3 + 2] = (uint8_t)(src[x]);
		}
		std::fwrite(row.data(), 1, row.size(), f);
	}
	std::fclose(f);
}

void Gui::set_mouse_captured(bool on)
{
	captured = on;
	apply_mouse_rect();
	if (absolute_pointer) {
		// The guest's cursor follows the host pointer exactly, so there is
		// nothing to warp and no deltas to ask for: confine the pointer to
		// the display and hide the host's copy of it. Start it inside, or
		// the first motion would be clamped from wherever it was.
		SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
		if (on) {
			const Rect r = display_rect(fb_full);
			SDL_WarpMouseInWindow(window, r.x + r.w / 2, r.y + r.h / 2);
		}
	} else {
		// SDL_SetRelativeMouseMode hides the cursor, warps it back after
		// every motion event, and reports deltas -- the only way to give
		// DOOM a pointer that can keep turning in one direction.
		if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) != 0 && on) {
			captured = false;
			apply_mouse_rect();
		}
	}
}

void Gui::apply_mouse_rect()
{
	// The display area, not the window. The rest of the window is the
	// dashboard, and a pointer that can wander over it while captured is a
	// pointer that stops following the guest.
	if (!window) return;
	if (captured) {
		const Rect r = display_rect(fb_full);
		const SDL_Rect sr{ r.x, r.y, r.w, r.h };
		SDL_SetWindowMouseRect(window, &sr);
	} else {
		SDL_SetWindowMouseRect(window, nullptr);
	}
}

void Gui::toggle_fb_fullscreen()
{
	fb_full = !fb_full;
	// The display just changed size, so a captured pointer's fence has to
	// move with it.
	apply_mouse_rect();
}

bool Gui::map_pointer(int wx, int wy, int &gx, int &gy) const
{
	// SDL reports the pointer in window coordinates, which are canvas
	// pixels: the process is per-monitor DPI aware (see init), so there is
	// no logical-to-physical scale between them.
	const Rect r = display_rect(fb_full);
	if (r.w <= 0 || r.h <= 0 || guest_w <= 0 || guest_h <= 0) return false;
	const bool inside = wx >= r.x && wx < r.x + r.w && wy >= r.y && wy < r.y + r.h;
	long long x = (long long)(wx - r.x) * guest_w / r.w;
	long long y = (long long)(wy - r.y) * guest_h / r.h;
	gx = (int)(x < 0 ? 0 : x >= guest_w ? guest_w - 1 : x);
	gy = (int)(y < 0 ? 0 : y >= guest_h ? guest_h - 1 : y);
	return inside;
}

std::vector<RawInputEvent> Gui::poll_input()
{
	std::vector<RawInputEvent> events;

	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) exit(0);

		switch (e.type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			RawInputEvent ev;
			ev.kind = RawInputEvent::Kind::Key;
			ev.sdl_keysym = (uint32_t)e.key.keysym.sym;
			ev.sdl_scancode = (uint32_t)e.key.keysym.scancode;
			ev.mods = (uint16_t)e.key.keysym.mod;
			ev.pressed = (e.type == SDL_KEYDOWN);
			// Repeats are passed along rather than filtered. A guest with
			// its own input layer does its own repeat from the held state,
			// so forwarding the host's would double it; a serial console
			// has no held state and needs the host's, or holding a key
			// types one character. The consumer knows which it is, so
			// both get the event and one of them ignores it -- see the
			// repeat handling in DoomSystem::run.
			ev.repeat = (e.key.repeat != 0);
			events.push_back(ev);
			break;
		}
		case SDL_TEXTINPUT: {
			// One event per composed character. SDL hands over UTF-8 with
			// the host's keyboard layout, modifiers and any dead-key
			// composition already resolved, which is the whole reason to
			// use it rather than deriving characters from keysyms.
			RawInputEvent ev;
			ev.kind = RawInputEvent::Kind::Text;
			std::snprintf(ev.text, sizeof(ev.text), "%s", e.text.text);
			events.push_back(ev);
			break;
		}
		// The mouse belongs to the guest only over its display. Everywhere
		// else in the window is the dashboard, so motion there is not
		// reported at all, and neither is a click. The one exception is a
		// release whose press was reported: dropping that would leave the
		// guest holding a button down after the pointer wandered off.
		case SDL_MOUSEMOTION: {
			RawInputEvent ev;
			ev.kind = RawInputEvent::Kind::MouseMotion;
			ev.dx = e.motion.xrel;
			ev.dy = e.motion.yrel;
			if (captured && !absolute_pointer) {
				// Relative mode: the pointer is hidden and pinned, so its
				// position means nothing and only the deltas count.
				if (ev.dx == 0 && ev.dy == 0) break;
			} else if (!map_pointer(e.motion.x, e.motion.y, ev.x, ev.y)) {
				break;
			}
			events.push_back(ev);
			break;
		}
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			RawInputEvent ev;
			ev.kind = RawInputEvent::Kind::MouseButton;
			ev.button = e.button.button;
			ev.pressed = (e.type == SDL_MOUSEBUTTONDOWN);
			const uint32_t bit = 1u << (ev.button & 31);
			const bool inside = map_pointer(e.button.x, e.button.y, ev.x, ev.y)
			                    || (captured && !absolute_pointer);
			if (ev.pressed) {
				if (!inside) break;
				buttons_down |= bit;
			} else {
				if (!(buttons_down & bit)) break;
				buttons_down &= ~bit;
			}
			events.push_back(ev);
			break;
		}
		case SDL_MOUSEWHEEL: {
			RawInputEvent ev;
			ev.kind = RawInputEvent::Kind::MouseWheel;
			ev.dx = e.wheel.x;
			ev.dy = e.wheel.y;
			const bool inside = map_pointer(e.wheel.mouseX, e.wheel.mouseY, ev.x, ev.y)
			                    || (captured && !absolute_pointer);
			if (!inside) break;
			events.push_back(ev);
			break;
		}
		default:
			break;
		}
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
	float px_scale = text_ux * scale_x_;
	float py_scale = text_uy * scale_y_;
	int origin_x = (int)(x * text_ux);
	int origin_y = (int)(y * text_uy);

	// Each source pixel covers the span from where it starts to where the
	// *next* one starts. That sounds like a long way of saying "draw a
	// block of size px_scale", and it is not: a fixed rounded size and a
	// truncated position disagree whenever the scale is not an integer,
	// and the disagreement leaves undrawn columns and rows scattered
	// through the glyph.
	//
	// Concretely, at px_scale 2.25 a rounded size of 2 puts source column
	// 3 at pixels 6-7 and column 4 at pixel 9 -- pixel 8 is never written.
	// Repeated down the glyph that reads as a transparent line straight
	// through every character, and since the same arithmetic runs on the
	// other axis, the two cross. It only showed up at some window sizes,
	// because it needs a fractional scale to appear at all: the trace text
	// draws at 0.75, so a 1920-wide window (scale_x 3.0) lands exactly on
	// it while a 1280-wide one (2.0, giving 1.5) happens not to.
	//
	// Deriving the extent from the next start makes the blocks tile with
	// no gaps and no overlap at any scale, which is what nearest-neighbour
	// scaling should have been doing in the first place.
	for (int r = 0; r < 8; r++) {
		uint8_t b = font8x8[(uint8_t)c][r];
		int py0 = origin_y + (int)(r * py_scale);
		int py1 = origin_y + (int)((r + 1) * py_scale);
		if (py1 <= py0) py1 = py0 + 1; // never collapse a row away entirely

		for (int cl = 0; cl < 8; cl++) {
			if (!(b & (0x80 >> cl))) continue;

			int px0 = origin_x + (int)(cl * px_scale);
			int px1 = origin_x + (int)((cl + 1) * px_scale);
			if (px1 <= px0) px1 = px0 + 1;

			for (int ty = py0; ty < py1; ty++) {
				if (ty < 0 || ty >= canvas_h) continue;
				uint32_t *row = &dash_work[(size_t)ty * canvas_w];
				for (int tx = px0; tx < px1; tx++) {
					if (tx < 0 || tx >= canvas_w) continue;
					row[tx] = col;
				}
			}
		}
	}
}

void Gui::draw_string(int x, int y, const char *s, uint32_t c, float scale_x_, float scale_y_, int track)
{
	// Advance (character pitch) tracks only scale_x_ -- a taller-but-not-
	// wider string (scale_y_ > scale_x_) still lays its characters out at
	// their normal horizontal spacing, it just draws each one taller.
	int advance = (int)(8 * scale_x_ + 0.5f); if (advance < 1) advance = 1;
	advance += track;
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
