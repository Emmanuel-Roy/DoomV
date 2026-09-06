#include "mmu.hpp"
#include "registers.hpp"
#include "memory.hpp"

namespace {
constexpr uint16_t CSR_SATP    = 0x180;
constexpr uint16_t CSR_MSTATUS = 0x300; // sstatus is a masked view of the same storage

constexpr uint64_t MSTATUS_SUM = 1ull << 18;
constexpr uint64_t MSTATUS_MXR = 1ull << 19;

constexpr uint64_t SATP_MODE_SV39 = 8ull;
constexpr uint64_t PAGESIZE = 4096;
constexpr int PTESIZE = 8;

constexpr uint64_t PTE_V = 1ull << 0;
constexpr uint64_t PTE_R = 1ull << 1;
constexpr uint64_t PTE_W = 1ull << 2;
constexpr uint64_t PTE_X = 1ull << 3;
constexpr uint64_t PTE_U = 1ull << 4;
constexpr uint64_t PTE_A = 1ull << 6;
constexpr uint64_t PTE_D = 1ull << 7;

constexpr uint64_t CAUSE_INSTR_PAGE_FAULT = 12;
constexpr uint64_t CAUSE_LOAD_PAGE_FAULT  = 13;
constexpr uint64_t CAUSE_STORE_PAGE_FAULT = 15; // AMOs fault under this cause too, per spec

uint64_t pte_ppn(uint64_t pte) { return (pte >> 10) & 0xFFFFFFFFFFFull; } // bits 53:10, 44 bits

uint64_t fault_cause(AccessType type)
{
	switch (type) {
	case AccessType::Fetch: return CAUSE_INSTR_PAGE_FAULT;
	case AccessType::Load:  return CAUSE_LOAD_PAGE_FAULT;
	default:                return CAUSE_STORE_PAGE_FAULT; // Store, Amo
	}
}
}

bool mmu_translate(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type,
                    uint64_t &paddr, uint64_t &cause, uint64_t &tval)
{
	// M-mode never translates. Real hardware lets M-mode opt into S/U's
	// table via mstatus.MPRV for a single access -- not modeled yet, since
	// nothing needs it until OpenSBI (Stage 3) shows up doing exactly that.
	uint64_t satp = regs.read_csr(CSR_SATP);
	uint64_t mode = satp >> 60;
	if (regs.get_priv() == PrivMode::M || mode == 0) {
		paddr = vaddr;
		return true;
	}
	if (mode != SATP_MODE_SV39) {
		// Sv48/Sv57 not implemented; nothing sets a MODE other than 0/8 yet.
		paddr = vaddr;
		return true;
	}

	// Sv39 VAs must be canonical -- bits 63:39 all equal bit 38 (i.e. a
	// sign-extended 39-bit value). A non-canonical VA faults before the
	// walk even begins on real hardware.
	uint64_t sext_check = (uint64_t)((int64_t)(vaddr << 25) >> 25);
	if (sext_check != vaddr) {
		cause = fault_cause(type);
		tval = vaddr;
		return false;
	}

	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
	bool sum = mstatus & MSTATUS_SUM;
	bool mxr = mstatus & MSTATUS_MXR;
	PrivMode priv = regs.get_priv();

	uint64_t vpn[3] = {
		(vaddr >> 12) & 0x1FF,
		(vaddr >> 21) & 0x1FF,
		(vaddr >> 30) & 0x1FF,
	};

	uint64_t a = (satp & 0xFFFFFFFFFFFull) * PAGESIZE;
	uint64_t pte = 0;
	int level = -1;
	for (int i = 2; i >= 0; i--) {
		uint64_t pte_addr = a + vpn[i] * PTESIZE;
		pte = mem.read64(pte_addr);
		if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W))) {
			// Invalid, or the reserved W=1/R=0 encoding.
			cause = fault_cause(type);
			tval = vaddr;
			return false;
		}
		if ((pte & PTE_R) || (pte & PTE_X)) {
			level = i; // leaf
			break;
		}
		if (i == 0) {
			// Non-leaf pointer at the last level -- nowhere left to go.
			cause = fault_cause(type);
			tval = vaddr;
			return false;
		}
		a = pte_ppn(pte) * PAGESIZE;
	}

	bool perm_ok;
	switch (type) {
	case AccessType::Fetch: perm_ok = (pte & PTE_X); break;
	case AccessType::Load:  perm_ok = (pte & PTE_R) || (mxr && (pte & PTE_X)); break;
	case AccessType::Store: perm_ok = (pte & PTE_W); break;
	case AccessType::Amo:   perm_ok = (pte & PTE_R) && (pte & PTE_W); break;
	default:                perm_ok = false; break;
	}
	if (!perm_ok) {
		cause = fault_cause(type);
		tval = vaddr;
		return false;
	}

	bool u = pte & PTE_U;
	if (priv == PrivMode::U) {
		if (!u) { cause = fault_cause(type); tval = vaddr; return false; }
	} else {
		// S-mode touching a U-owned page: only Load/Store, and only with
		// SUM set -- fetching from a U page in S-mode is never allowed
		// regardless of SUM, per spec.
		if (u && (type == AccessType::Fetch || !sum)) {
			cause = fault_cause(type);
			tval = vaddr;
			return false;
		}
	}

	// This stage doesn't implement hardware A/D auto-set, so a page table
	// that hasn't pre-set these (as real OS page tables generally don't --
	// they rely on the CPU setting them on first access/write) will fault
	// here. Known gap, to be revisited once OpenSBI/Linux bring-up
	// actually exercises it.
	if (!(pte & PTE_A)) { cause = fault_cause(type); tval = vaddr; return false; }
	if ((type == AccessType::Store || type == AccessType::Amo) && !(pte & PTE_D)) {
		cause = fault_cause(type);
		tval = vaddr;
		return false;
	}

	uint64_t ppn_full = pte_ppn(pte);
	if (level > 0) {
		// Superpage: the PTE's own PPN must be zero in the bits a finer
		// table would otherwise have supplied -- anything else is a
		// misaligned superpage, itself a page fault per spec.
		uint64_t low_mask = (1ull << (9 * level)) - 1;
		if (ppn_full & low_mask) {
			cause = fault_cause(type);
			tval = vaddr;
			return false;
		}
	}

	// Physical address = the PTE's frame number for the high bits, and the
	// VA's own low bits (page offset, plus any superpage passthrough) for
	// the rest -- safe to just OR these together since the alignment check
	// above guarantees no overlap.
	uint64_t va_low_bits = 12 + 9 * level;
	uint64_t va_mask = (1ull << va_low_bits) - 1;
	paddr = (ppn_full << 12) | (vaddr & va_mask);
	return true;
}
