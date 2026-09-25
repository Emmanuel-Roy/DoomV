#pragma once
#include "riscv_decoder.hpp"
#include "mmu.hpp"
#include "memory.hpp"   // Memory::Backing, for DataPage
#include <cstdint>

class Registers;
class Memory;

// sign-extend the low 32 bits of a 64-bit value to the full 64 -- the
// operation every *W-suffixed RV64 instruction ends with. Shared by
// ext_i.cpp and ext_m.cpp.
inline uint64_t sext32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

#include <vector>

// One load or store an instruction made, as lockstep.cpp logs it: the
// address the instruction used and the one it reached.
struct AccessRecord {
	uint64_t vaddr, paddr;
	uint8_t size;
	bool store;
};

class RiscvCore {
public:
	RiscvCore();

	// Commit-log bookkeeping for lockstep.cpp. enter_trap counts every trap
	// and remembers the last; translate_or_trap appends each load and store
	// to access_log while it is set.
	uint64_t trap_count = 0;
	uint64_t last_trap_cause = 0, last_trap_tval = 0, last_trap_epc = 0;
	bool last_trap_interrupt = false;
	std::vector<AccessRecord> *access_log = nullptr;
	// Whether interrupt `bit` would be taken now were it pending: the enable
	// and delegation half of check_and_take_interrupt. In lock-step the
	// pending half belongs to the reference.
	bool interrupt_enabled(Registers &regs, int bit);
	// A fetch from an address that is not a legal instruction start for the
	// extensions enabled right now: bit 1 set with C off.
	void raise_misaligned_fetch(Registers &regs, uint64_t pc) { enter_trap(regs, 0, pc); }
	// WFI and WRS ask for a wait here instead of completing, leaving pc at
	// the instruction; DoomSystem::run_wait runs it.
	enum class Wait : uint8_t { None, Wfi, WrsSto, WrsNto };
	Wait wait_request = Wait::None;
	// Sail's shouldWakeForInterrupt: an interrupt pending and enabled in mie,
	// whatever the global enables say.
	bool wake_for_interrupt(Registers &regs, Memory &mem);
	bool reservation_held() const { return reservation_valid; }

	// Pages that loads and stores inside one page can reach directly -- see
	// load_virtual. One cache for loads and one for stores, because a page
	// can be readable and not writable, and a store is what sets D.
	struct DataPage {
		uint64_t vpage = ~0ull;
		uint64_t key = ~0ull;
		uint64_t ppage = 0;      // what the translation produced, for the access log
		uint8_t *host = nullptr;
		// Which memory the page is. Both framebuffers are plain bytes too, and
		// differ from RAM only in the host-side counters a store bumps, which
		// a cached store bumps the same way -- see Memory::framebuffer_stored.
		Memory::Backing backing = Memory::Backing::Ram;
	};
	static constexpr unsigned DATA_CACHE_SIZE = 256;
	DataPage load_cache[DATA_CACHE_SIZE];
	DataPage store_cache[DATA_CACHE_SIZE];
	void raise_virtual_instruction(Registers &regs, uint64_t tval) { enter_trap(regs, 22, tval); }
	// Enter interrupt `bit`'s trap now, as check_and_take_interrupt would.
	void take_interrupt(Registers &regs, int bit)
	{
		enter_trap(regs, (1ull << 63) | (uint64_t)bit, 0, /*is_interrupt=*/true);
	}

	// Each function is responsible for leaving pc correctly set before
	// returning -- advanced by the instruction's length for straight-line
	// code, or set to the branch/jump target. No separate auto-increment
	// step exists elsewhere.
	void exec_32I(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_32M(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_32A(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_32ZICSR(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_F(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_D(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_V(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZBA(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZBB(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZBS(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZBKB(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZFH(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZICOND(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZIHINTPAUSE(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZIHINTNTL(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZIMOP(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZCMOP(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZICBOM(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZICBOP(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZICBOZ(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZAWRS(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZFA(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZFHMIN(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_SVINVAL(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_H(const DecodedOp &instr, Registers &regs, Memory &mem);
	void exec_ZIFENCEI(const DecodedOp &instr, Registers &regs, Memory &mem);

	// Sv39 address translation, shared by every load/store/AMO/FP-load/
	// vector-load call site and by DoomSystem::step()'s instruction fetch.
	// On a page fault this enters a trap itself (same as enter_trap below)
	// and returns false -- the caller should just abort the instruction
	// without touching Memory, since pc has already been redirected.
	// `size` is the number of bytes the access covers, and it matters:
	// PMP denies an access that straddles the edge of a region even when
	// both sides would permit it, and physical memory attributes likewise
	// apply to the whole access. Passing 1 for a wider access silently
	// skips both checks.
	bool translate_or_trap(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type, uint64_t &paddr, unsigned size = 1);

	// Load or store `size` bytes at a virtual address, splitting the access
	// at a page boundary when it crosses one. The two halves can land on
	// physical pages that are nowhere near each other, so a straddling
	// access cannot be done with one translation and one wide memory
	// operation however the permissions turn out.
	//
	// Both return false having already entered a trap, exactly as
	// translate_or_trap does. store_virtual checks the whole range before
	// writing any of it, so a store that runs into a read-only page leaves
	// the first page untouched instead of half-writing it.
	bool load_virtual(Registers &regs, Memory &mem, uint64_t vaddr, unsigned size, uint64_t &out);
	bool store_virtual(Registers &regs, Memory &mem, uint64_t vaddr, unsigned size, uint64_t value);

	// Called once per DoomSystem::step(), before fetch. Computes the
	// effective mip & mie, picks the highest-priority pending+enabled+
	// unmasked interrupt (if any) per the spec's fixed priority order,
	// and -- if one is actually deliverable at the current privilege/
	// mstatus.xIE state -- takes it via enter_trap. Returns true if an
	// interrupt was taken (the caller should skip the rest of this step,
	// same shape translate_or_trap already established for page faults).
	bool check_and_take_interrupt(Registers &regs, Memory &mem);

	// The same "what does a CSR read actually return" logic
	// exec_32ZICSR's read side uses (ext_zicsr.cpp) -- several CSRs are
	// computed, not plain csr[] storage (sstatus, mip, misa, time, the
	// IMSIC-backed indirect/claim registers, ...), so a caller that just
	// wants to *peek* a live value (the dashboard's CSRs panel) needs
	// this instead of Registers::read_csr directly, or it'd see stale/
	// wrong values for exactly the CSRs most worth watching. Side-effect
	// free -- topei_value() (used here) is a plain peek; claim() is a
	// separate call exec_32ZICSR only makes on an actual write.
	uint64_t read_csr_effective(Registers &regs, Memory &mem, uint16_t csr);

	// Deliver an illegal-instruction trap to the guest. Used by the
	// debugger's pause-then-trap flow: an illegal instruction first freezes
	// the machine for inspection, and only becomes a real trap once the user
	// resumes -- so a guest with a handler (Linux delivering SIGILL) can
	// carry on, while a bare-metal guest with no handler still gets caught
	// at the exact instruction instead of vanishing into a trap loop.
	void raise_illegal_instruction(Registers &regs, uint64_t tval);

	// Cause 18, the software-check exception. Zicfilp raises it with tval=2
	// for a missing or mismatched landing pad, and Zicfiss with tval=3 for
	// a shadow-stack mismatch; the tval is what tells a handler which
	// check failed.
	void raise_software_check(Registers &regs, uint64_t tval);
	static bool csr_access_permitted(Registers &regs, uint16_t csr, bool writing);

private:
	// LR/SC reservation state. Single-hart, no interrupts, so this only
	// ever needs to survive the immediate LR->SC pair a retry loop does --
	// no cross-hart invalidation logic needed. Doesn't separately track
	// LR.W vs LR.D width (a mixed-width LR->SC pair at the same address
	// would incorrectly succeed) -- not something compiled code produces.
	bool reservation_valid;
	uint64_t reservation_addr;

	// ECALL/EBREAK both funnel into the same M-mode trap entry sequence
	// (illegal-instruction detection stays a separate debugger safety net,
	// see exec_32ZICSR's comment -- this is only for the deliberate,
	// explicit trap-requesting instructions).
	void enter_trap(Registers &regs, uint64_t cause, uint64_t tval, bool is_interrupt = false);
};
