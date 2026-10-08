#pragma once
#include <string>
#include <cstdint>

// Runtime-configurable, not compile-time -- every extension (and the base
// XLEN width) can be switched on/off per run via -march= on the command
// line (see parse_march below and main.cpp), no rebuild required. This
// used to be a namespace of `constexpr bool`s; the decoder/core still read
// these the same way at every call site (just `Extensions.I` instead of
// `Extensions::I` now), so making a wrong runtime choice is exactly as
// safe as it was at compile time -- classify()/decode() already treat any
// disabled extension's instructions as illegal, gated fresh on every call.
struct ExtensionConfig {
	bool I = true;
	bool M = true;
	bool A = true;
	bool C = true;
	bool ZICSR = true;
	bool ZIFENCEI = true;
	// D requires F per spec (a hart can't have double without single) --
	// parse_march enforces this; hand-setting the fields yourself doesn't.
	bool F = true;
	bool D = true;
	bool V = false; // off by default like the rest -- opt in with -march=...v
	// Zba/Zbb/Zbs default ON, unlike V. Bare-metal Doom never emits them,
	// but every modern riscv64 Linux userspace assumes them (the RVA23
	// profile mandates all three), so defaulting them off would just mean
	// every distro binary halts on its first sh*add.
	bool ZBA = true;
	bool ZBB = true;
	bool ZBS = true;
	bool ZICOND = true; // czero.eqz/czero.nez -- RVA23 again, same reason as Zb*


	// The RVA23 hint and reserved-encoding extensions. All six retire
	// without architectural effect on this machine, and several were
	// already retiring correctly by accident -- PAUSE is a FENCE, the NTL
	// hints are C.ADD into x0, and the prefetches are ORI into x0. What
	// enabling them buys is that the machine names them honestly, and that
	// Zimop/Zcmop write the zero the spec requires rather than leaving rd
	// untouched, which is the one place a "does nothing" extension can
	// actually be wrong.
	//
	// Default on for the same reason as Zb*: RVA23 mandates them, so a
	// distro userspace may emit them freely and defaulting them off would
	// only manufacture illegal instructions.
	bool ZIHINTPAUSE = true;
	bool ZIHINTNTL = true;
	bool ZIMOP = true;
	bool ZCMOP = true;
	bool ZICBOM = true;
	bool ZICBOP = true;
	// Zicboz has real behaviour, unlike the group above: cbo.zero is a
	// store. Zawrs retires immediately, which the spec explicitly permits
	// and which is the only implementation that terminates on one hart.
	bool ZICBOZ = true;
	bool ZAWRS = true;
	// Zfa is real FP arithmetic, not a hint: fli materialises constants,
	// fminm/fmaxm differ from FMIN/FMAX on NaN, and fleq/fltq differ from
	// FLE/FLT only in which NaNs raise invalid.
	bool ZFA = true;
	// Zfhmin, not Zfh: RVA23U64 mandates the minimal half-precision set
	// (load/store, bit moves, conversions) and makes full half-precision
	// arithmetic an expansion option. Software is expected to widen to
	// single, compute, and narrow back.
	bool ZFHMIN = true;
	// Svinval's three instructions, and the two Sv* page-table features
	// (Svnapot's N bit, Svpbmt's memory-type field), which add no
	// instructions and are handled in mmu.cpp.
	bool SVINVAL = true;
	bool SVNAPOT = true;
	bool SVPBMT = true;
	// Pointer masking. One flag covers Ssnpm, Smnpm and Sspm: they are the
	// same mechanism named for which envcfg holds the PMM field, and no
	// guest would sensibly have one without the others.
	// Smpmp: physical memory protection. Optional in RVA23S64, but the
	// architectural tests assume it and, more to the point, without it
	// S and U mode reach all of memory unchecked.
	bool SMPMP = true;
	bool SSNPM = true;
	// H: the hypervisor extension. Off by default -- unlike the others
	// here it changes how existing CSR numbers behave (the VS-mode
	// redirection), so a guest that never asked for virtualisation should
	// not have to pay for it.
	bool H = false;
	// Both are RVA23 mandatory, and both are CSR-and-interrupt extensions
	// with no instructions of their own -- which is exactly why they were
	// missed until DoomV was audited against the profile's own machine-
	// readable extension list rather than against a hand-written plan.
	bool SSCOFPMF = true;
	bool SSSTATEEN = true;

	// Zkr: the entropy source. One CSR, `seed`, and no instructions -- but
	// it is not a plain register. Reading it *consumes* entropy, so a
	// read-only access to it is illegal rather than harmless, and which
	// modes may reach it at all is controlled from mseccfg. Off by default
	// like H: a machine that does not claim an entropy source should not
	// answer as though it had one.
	// The crypto bitmanip trio. Off by default like the other optional
	// extensions: they add instructions in encodings that are otherwise
	// illegal, so claiming them changes what an unknown encoding does.
	// Full half-precision arithmetic. Zfhmin is mandatory and separate;
	// this is the expansion option on top of it, so it implies Zfhmin.
	bool ZFH = false;
	bool ZBC = false;
	bool ZBKB = false;
	bool ZBKX = false;

	bool ZKR = false;

	// Zicfilp: landing pads. Every indirect jump arms an expectation that
	// the next instruction is an `lpad`, and anything else is a
	// software-check exception. Off by default -- turning it on changes
	// what ordinary indirect jumps do.
	bool ZICFILP = false;

	// Zicfiss: the shadow stack. Adds `ssp`, four instructions that hide
	// inside Zimop/Zcmop encodings when the extension is off, and a page
	// permission -- W without R, otherwise reserved -- that means "shadow
	// stack". Off by default for the same reason.
	bool ZICFISS = false;

	// Base ISA width, not an optional extension. Registers/Memory always
	// store values in 64-bit containers regardless of this flag: RV32
	// mode just means every integer op computes at 32-bit width and
	// sign-extends its result into that container (the same mechanism
	// RV64's *W-suffixed instructions use), rather than every register
	// genuinely being a narrower type. That's what keeps Registers/
	// Memory/Snapshot/Gui untouched by this flag -- only the decoder
	// (RV64-only opcodes, RV32-vs-RV64 compressed-encoding meanings) and
	// RiscvCore's compute width need to branch on it.
	bool XLEN64 = true;
};

// Single global instance, mutated once at startup by parse_march() (or left
// at its rv64imafdc_zicsr_zifencei-equivalent defaults above) before
// DoomSystem constructs anything that reads it. Not touched again after
// that -- reads happen constantly (every decode), writes happen at most
// once per run.
inline ExtensionConfig Extensions;

// Smaia/Ssaia: the AIA CSRs (miselect, mireg, mtopei, mtopi and the S and VS
// copies, mvien, mvip, the hvi* registers). Not a misa letter, so a flag of
// the machine's beside ExtensionConfig rather than in it -- whose layout
// snapshots record. On unless a -march leaves "smaia"/"ssaia" out; Sail's
// RVA23S64 has no AIA, and a hart without it traps on all of them, as Sail's
// does.
inline bool ExtAia = true;

// The extensions -march switches that are not misa letters and that misa
// cannot turn off: kept beside ExtensionConfig rather than in it, as ExtAia
// is, because snapshots record ExtensionConfig's layout byte for byte.
// Snapshots from version 10 on record these too (savestate.cpp); an older
// one had them as the DoomV of its day did: the vector unit's and the page
// tables' on, Zacas and Zabha absent.
struct ExtensionSwitches {
	// The A extension's additions, both off unless a -march names them (Sail's
	// configuration has both). Zacas: amocas.w, .d, and .q on an even register
	// pair. Zabha: the AMOs at byte and halfword width, and with Zacas,
	// amocas.b and .h. See ext_a.cpp.
	bool ZACAS = false;
	bool ZABHA = false;

	// The vector unit's own extensions, beside V. Each is a -march switch, so
	// a core that leaves one out can be held to a DoomV that does too; on
	// unless a -march leaves them out, as Sail's configuration has them all.
	// They act only with V on. The floating-point ones are about element
	// widths, so they are checked as an instruction runs (exec_V); the rest
	// by their encodings, as they decode (riscv_decoder.cpp).
	bool ZVFH = true;       // half-precision vector arithmetic (implies Zvfhmin)
	bool ZVFHMIN = true;    // the two f16 <-> f32 conversions
	bool ZVFBFMIN = true;   // the bf16 conversions
	bool ZVFBFWMA = true;   // vfwmaccbf16 (implies Zvfbfmin)
	bool ZVBB = true;       // vector bit manipulation (implies Zvkb)
	bool ZVKB = true;       // its crypto subset: vandn, vbrev8, vrev8, vrol, vror
	bool ZVBC = true;       // vclmul, vclmulh
	bool ZVKG = true;       // vghsh, vgmul
	bool ZVKNED = true;     // AES
	bool ZVKNHA = true;     // SHA-256
	bool ZVKNHB = true;     // SHA-256 and SHA-512 (implies Zvknha)
	bool ZVKSED = true;     // SM4
	bool ZVKSH = true;      // SM3

	// Virtual memory beyond Sv39: the deeper page tables (satp and hgatp
	// accept their modes only when on; Sv57 implies Sv48), and Svadu --
	// envcfg.ADUE, the walker setting A and D itself, is writable only with
	// it. On unless a -march leaves them out, as with the vector ones.
	bool SV48 = true;
	bool SV57 = true;
	bool SVADU = true;
};
inline ExtensionSwitches ExtSwitch;

// The machine's architectural parameters that are not extensions: what
// Sail's configuration sets in its "memory" section, each a command-line
// switch (machine.cpp) so that a core built otherwise can be held to a DoomV
// built the same, and checked against Sail configured so
// (tools/verification/ext_switches.py). The defaults are Sail's. Snapshots
// record them from version 10 on.
struct MachineConfig {
	unsigned pmp_count = 16;       // -pmp=N: 0, 16 or 64 pmpaddr CSRs
	unsigned pmp_usable = 16;      // -pmp=N:U: entries from U on read zero and ignore writes
	unsigned pmp_grain = 0;        // -pmp-grain=G: regions of at least 2^(G+2) bytes
	unsigned asidlen = 16;         // -asidlen=: satp's and vsatp's ASID bits
	unsigned vmidlen = 14;         // -vmidlen=: hgatp's VMID bits
	unsigned physaddr_bits = 56;   // -physaddr-bits=: what satp, hgatp and pmpaddr keep
	unsigned cbo_block = 64;       // -cbo-block=: the bytes a cbo.* instruction covers
	bool misaligned_trap = false;  // -misaligned=trap: a misaligned load or store traps
};
inline MachineConfig Machine;
// What the hart supports, as -march chose it. Extensions is what is enabled
// right now: misa is writable, and clearing a letter turns its extension off
// until it is set again, so Extensions follows misa and this does not.
inline ExtensionConfig SupportedExtensions;
// Bumped whenever Extensions changes at run time, so anything that cached a
// decision made under the old set -- the decode cache -- knows to redo it.
inline uint32_t ExtensionsEpoch = 0;

// Parses a GCC/toolchain-style march string ("rv64imafdc_zicsr",
// "rv32ima", ...): resets every extension to off, sets XLEN64 from the
// rv32/rv64 prefix, turns on one flag per recognized base letter (i/m/a/f/
// d/c, plus g as shorthand for imafd), and checks for "zicsr"/"zifencei"
// tokens after an underscore. Unrecognized letters/tokens are silently
// ignored (this isn't trying to be a strict validator, just a convenience
// toggle).
void parse_march(const std::string &march);

// What a Linux boot gets when no -march says otherwise: RVA23S64, with AIA
// and every switch on (machine.cpp; see the comment there).
inline constexpr const char *DEFAULT_LINUX_MARCH =
	"rv64imafdcv_zicsr_zifencei_zba_zbb_zbs_zicond"
	"_zicbom_zicbop_zicboz_zicntr_zihintpause_zihintntl"
	"_zimop_zcmop_zawrs_zfa_zfh_svinval_svnapot_svpbmt"
	"_sscofpmf_ssstateen_ssnpm_smnpm_smaia_ssaia"
	"_zvfh_zvfhmin_zvfbfmin_zvfbfwma_zvbb_zvkb_zvbc_zvkg_zvkned_zvknha_zvknhb_zvksed_zvksh_sv48_sv57_svadu_zacas_zabha";

// `march` with its ExtensionSwitches names replaced by those `s` has on: the
// -march that rebuilds a machine with exactly these switches.
std::string march_with_switches(const std::string &march, const ExtensionSwitches &s);
