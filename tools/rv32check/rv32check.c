// RV32 instruction spot-check for DoomV (see tools/rv32check/check.py).
//
// Runs integer, M, Zba, Zbb, Zbs and Zbkb instructions -- the ones the
// Pico's -march lets GCC emit -- on edge-case operands and prints each
// result. check.py compares them with RV32 semantics computed in Python.
// Built for DoomV's guest memory map (doomv/start.S and doomv/doomv.ld).
#include <stdint.h>

#define MMIO_DEBUG 0x10000008u

static void out(const char *s) { while (*s) *(volatile uint8_t *)MMIO_DEBUG = (uint8_t)*s++; }
static void hex(uint32_t v)
{
	char b[9];
	for (int i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15];
	b[8] = 0;
	out(b);
}

#define R(op) static uint32_t f_##op(uint32_t a, uint32_t b) { uint32_t r; __asm__ volatile(#op " %0,%1,%2" : "=r"(r) : "r"(a), "r"(b)); return r; }
#define U(name, op) static uint32_t f_##name(uint32_t a, uint32_t b) { uint32_t r; (void)b; __asm__ volatile(#op " %0,%1" : "=r"(r) : "r"(a)); return r; }
#define I(name, op, imm) static uint32_t f_##name(uint32_t a, uint32_t b) { uint32_t r; (void)b; __asm__ volatile(#op " %0,%1," #imm : "=r"(r) : "r"(a)); return r; }

R(add) R(sub) R(sll) R(srl) R(sra) R(slt) R(sltu) R(xor) R(or) R(and)
R(mul) R(mulh) R(mulhu) R(mulhsu) R(div) R(divu) R(rem) R(remu)
R(sh1add) R(sh2add) R(sh3add)
R(andn) R(orn) R(xnor) R(max) R(maxu) R(min) R(minu) R(rol) R(ror)
U(clz, clz) U(ctz, ctz) U(cpop, cpop) U(sext_b, sext.b) U(sext_h, sext.h) U(zext_h, zext.h) U(orc_b, orc.b) U(rev8, rev8)
R(bclr) R(bext) R(binv) R(bset)
R(pack) R(packh) U(brev8, brev8)
I(srli1, srli, 1) I(srli31, srli, 31) I(srai1, srai, 1) I(srai31, srai, 31) I(slli1, slli, 1) I(slli31, slli, 31)
I(rori1, rori, 1) I(rori31, rori, 31) I(bseti31, bseti, 31) I(bclri31, bclri, 31) I(bexti31, bexti, 31) I(binvi31, binvi, 31)
I(addi_m1, addi, -1) I(sltiu_m1, sltiu, -1) I(xori_m1, xori, -1)

typedef uint32_t (*fn)(uint32_t, uint32_t);
static const struct { const char *name; fn f; } ops[] = {
#define E(n) { #n, f_##n },
	E(add) E(sub) E(sll) E(srl) E(sra) E(slt) E(sltu) E(xor) E(or) E(and)
	E(mul) E(mulh) E(mulhu) E(mulhsu) E(div) E(divu) E(rem) E(remu)
	E(sh1add) E(sh2add) E(sh3add)
	E(andn) E(orn) E(xnor) E(max) E(maxu) E(min) E(minu) E(rol) E(ror)
	E(clz) E(ctz) E(cpop) E(orc_b) E(rev8) E(sext_b) E(sext_h) E(zext_h)
	E(bclr) E(bext) E(binv) E(bset) E(pack) E(packh) E(brev8)
	E(srli1) E(srli31) E(srai1) E(srai31) E(slli1) E(slli31)
	E(rori1) E(rori31) E(bseti31) E(bclri31) E(bexti31) E(binvi31)
	E(addi_m1) E(sltiu_m1) E(xori_m1)
};

static const uint32_t vals[] = {
	0x00000000, 0x00000001, 0x00000002, 0x0000001f, 0x00000020, 0x7fffffff,
	0x80000000, 0x80000001, 0xffffffff, 0xfffffffe, 0x12345678, 0x87654321,
	0x0000ffff, 0xffff0000, 0x00010000, 0xc0000000,
};

int main(void)
{
	const int nv = sizeof vals / sizeof vals[0];
	for (unsigned o = 0; o < sizeof ops / sizeof ops[0]; o++)
		for (int i = 0; i < nv; i++)
			for (int j = 0; j < nv; j++) {
				out(ops[o].name); out(" "); hex(vals[i]); out(" "); hex(vals[j]); out(" ");
				hex(ops[o].f(vals[i], vals[j])); out("\n");
			}
	out("DONE\n");
	for (;;) {}
}
