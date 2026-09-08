#include "mmu.hpp"
#include <cstdlib>
#include <cstdio>
#include "pmp.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"

namespace {
constexpr uint16_t CSR_SATP    = 0x180;
constexpr uint16_t CSR_MSTATUS = 0x300; // sstatus is a masked view of the same storage
// mstatus.MPRV: a load or store issued in M-mode is performed as though at
// mstatus.MPP -- same translation, same permission checks. Fetch is never
// affected.
constexpr uint64_t MSTATUS_MPRV = 1ull << 17;
constexpr uint64_t MSTATUS_MPV  = 1ull << 39; // the MPP privilege was virtual

constexpr uint64_t MSTATUS_SUM = 1ull << 18;
constexpr uint64_t MSTATUS_MXR = 1ull << 19;

constexpr uint64_t SATP_MODE_SV39 = 8ull;
constexpr uint64_t SATP_MODE_SV48 = 9ull;
constexpr uint64_t SATP_MODE_SV57 = 10ull;

// How many page-table levels a satp/hgatp MODE selects, or 0 for a mode
// this hart does not implement. The three differ only in depth: each step
// up adds one level and nine more bits of virtual address, and every level
// below the root is identical. Writing the walk against this number rather
// than against Sv39's three is what lets one loop serve all three -- the
// previous code hardcoded three levels and, for any other mode, returned
// the virtual address *untranslated*, which is not a fault but a silent
// bypass of the entire page table.
inline int mode_levels(uint64_t mode)
{
	switch (mode) {
	case SATP_MODE_SV39: return 3;
	case SATP_MODE_SV48: return 4;
	case SATP_MODE_SV57: return 5;
	default:             return 0;
	}
}

// Virtual address width for a mode: nine bits per level plus the 12-bit
// page offset. 39, 48 and 57 -- which is where the names come from.
inline int mode_va_bits(int levels) { return 12 + 9 * levels; }
constexpr uint64_t PAGESIZE = 4096;
constexpr int PTESIZE = 8;

constexpr uint64_t PTE_V = 1ull << 0;
constexpr uint64_t PTE_R = 1ull << 1;
constexpr uint64_t PTE_W = 1ull << 2;
constexpr uint64_t PTE_X = 1ull << 3;
constexpr uint64_t PTE_U = 1ull << 4;
constexpr uint64_t PTE_A = 1ull << 6;
constexpr uint64_t PTE_D = 1ull << 7;
// Svnapot's N bit and Svpbmt's two-bit memory type live in the top of the
// PTE, above the PPN. Bits 60:54 remain reserved and must be zero.
constexpr uint64_t PTE_N        = 1ull << 63;
constexpr uint64_t PTE_PBMT     = 3ull << 61;
constexpr int      PTE_PBMT_SHIFT = 61;
constexpr uint64_t PTE_RESERVED = 0x7Full << 54; // bits 60:54

// menvcfg bits that gate the two extensions for S-mode. Without these set,
// the corresponding PTE bits are not merely ignored -- they must read as
// reserved, i.e. a nonzero value is a page fault. That is what lets an OS
// discover whether the hardware supports them.
constexpr uint16_t CSR_MENVCFG    = 0x30A;
constexpr uint16_t CSR_SENVCFG    = 0x10A;
constexpr uint16_t CSR_VSATP      = 0x280; // the guest's satp, used by hlv/hsv
constexpr uint16_t CSR_VSSTATUS   = 0x200; // the guest's sstatus: its own SUM/MXR
constexpr uint16_t CSR_HSTATUS    = 0x600;
constexpr uint64_t HSTATUS_SPVP   = 1ull << 8;
constexpr uint16_t CSR_HGATP      = 0x680; // the second-stage root
constexpr uint16_t CSR_HTVAL      = 0x643; // faulting guest physical address
constexpr uint64_t MENVCFG_PBMTE  = 1ull << 62;
constexpr uint64_t MENVCFG_ADUE   = 1ull << 61;
constexpr uint16_t CSR_HENVCFG    = 0x60A;

// Svadu: whether *this* stage updates A/D in hardware rather than
// faulting. The two behaviours are Svade (fault, so the supervisor sets
// the bit and retries) and Svadu (hardware sets it and the access
// proceeds); RVA23S64 mandates Svade and allows Svadu on top of it, and
// the choice is per-stage rather than per-hart.
//
// menvcfg.ADUE governs the stages HS-mode owns -- its own S-stage and the
// G-stage. henvcfg.ADUE governs a guest's VS-stage, and is itself
// writable only while menvcfg.ADUE is set, so M-mode clearing one bit
// puts every stage back to Svade at once.
inline bool adue_enabled(Registers &regs, bool vs_stage)
{
	if (!(regs.read_csr(CSR_MENVCFG) & MENVCFG_ADUE)) return false;
	if (!vs_stage) return true;
	return (regs.read_csr(CSR_HENVCFG) & MENVCFG_ADUE) != 0;
}

// The bits this access needs set in the PTE. A is required by every
// access; D additionally by anything that writes. CacheBlock is
// deliberately absent from the D set: it writes nothing, so it neither
// requires nor sets D.
inline uint64_t ad_bits_needed(AccessType type, bool implicit)
{
	uint64_t need = PTE_A;
	if (!implicit && (type == AccessType::Store || type == AccessType::Amo))
		need |= PTE_D;
	return need;
}

constexpr uint64_t CAUSE_INSTR_PAGE_FAULT = 12;
constexpr uint64_t CAUSE_LOAD_PAGE_FAULT  = 13;
constexpr uint64_t CAUSE_STORE_PAGE_FAULT = 15; // AMOs fault under this cause too, per spec

// Access-fault causes, for the cases that are not page faults: an implicit
// page-table read from an address nothing answers is an *access* fault, and
// the distinction is visible to software. A walk that instead reads zeros
// from the void sees an invalid PTE and reports a page fault, which tells
// the supervisor to go fix a mapping that was never the problem.
inline uint64_t access_fault_cause(AccessType type)
{
	switch (type) {
	case AccessType::Fetch: return 1;
	case AccessType::Load:  return 5;
	default:                return 7; // Store, Amo, CacheBlock
	}
}


uint64_t pte_ppn(uint64_t pte) { return (pte >> 10) & 0xFFFFFFFFFFFull; } // bits 53:10, 44 bits

uint64_t fault_cause(AccessType type)
{
	switch (type) {
	case AccessType::Fetch: return CAUSE_INSTR_PAGE_FAULT;
	case AccessType::Load:  return CAUSE_LOAD_PAGE_FAULT;
	// A cache-block operation reports a store fault whichever permission it
	// was missing.
	default:                return CAUSE_STORE_PAGE_FAULT; // Store, Amo
	}
}

// A second-stage (G-stage) failure gets its own cause number, distinct from
// the first-stage one. That is not decoration: a hypervisor has to tell
// "the guest's own page tables rejected this" -- which is the guest kernel's
// problem to fix -- from "my page tables rejected this", which is the
// hypervisor's, and typically means a page it has not backed yet.
uint64_t guest_fault_cause(AccessType type)
{
	switch (type) {
	case AccessType::Fetch: return 20; // instruction guest-page fault
	case AccessType::Load:  return 21; // load guest-page fault
	default:                return 23; // store/AMO guest-page fault
	}
}

// G-stage translation: guest physical address -> real physical address,
// through hgatp.
//
// Sv39x4 rather than Sv39. The root table is four pages wide and its top
// index is 11 bits instead of 9, which extends the guest physical address
// space to 41 bits -- a guest may be given more physical memory than a
// single Sv39 root could describe. Only the root level differs; the two
// levels below it are ordinary Sv39.
//
// `implicit` marks a walk of the *first stage's own page tables* rather
// than the guest's final access. The distinction matters for reporting:
// spec requires such a fault to be reported against the original access,
// and it is also why a single guest load can perform a dozen memory
// accesses and fault at any of them.
bool gstage_translate(Registers &regs, Memory &mem, uint64_t gpa, AccessType type,
                      uint64_t &pa, uint64_t &cause, uint64_t &tval, bool implicit)
{
	uint64_t hgatp = regs.read_csr(CSR_HGATP);
	uint64_t mode = hgatp >> 60;
	if (mode == 0) { // bare: the guest's physical addresses are real ones
		pa = gpa;
		return true;
	}
	const int levels = mode_levels(mode);
	if (levels == 0) { // a MODE this hart does not implement
		pa = gpa;
		return true;
	}

	// Every failure below owes the hypervisor two different numbers, and
	// writing only one of them is the bug this closes. htval carries the
	// guest *physical* address, shifted right by two; stval keeps the
	// guest *virtual* address, which the caller restores. The hypervisor
	// needs both -- the VA to tell the guest what it touched, the GPA to
	// know which page to back -- so one field cannot serve for both.
	//
	// It is a lambda because there are six ways out of this function and
	// three of them used to forget: an invalid PTE, a misaligned
	// superpage and a walk that ran off the bottom all reported a guest
	// page fault with htval left at whatever the last one had put there,
	// which the trap path then cleared to zero. The hypervisor was told a
	// page was missing and not which page.
	auto gfault = [&](uint64_t c) {
		cause = c;
		// Stashed rather than written to htval here: the trap path picks
		// htval or mtval2 once it knows where the trap is going.
		regs.pending_gpa = gpa;
		tval = gpa;
		return false;
	};

	// The guest physical address space is 9 bits per level plus the 12-bit
	// offset, and two bits wider again at the root: the x4 in Sv39x4 is a
	// root table four pages wide, indexed by 11 bits instead of 9. That is
	// 41 bits for Sv39x4, 50 for Sv48x4 and 59 for Sv57x4. Anything above
	// that cannot be described by the root table and faults rather than
	// wrapping.
	const int gpa_bits = mode_va_bits(levels) + 2;
	if (gpa >> gpa_bits) return gfault(guest_fault_cause(type));

	uint64_t vpn[5] = {0, 0, 0, 0, 0};
	for (int i = 0; i < levels; i++) vpn[i] = (gpa >> (12 + 9 * i)) & 0x1FF;
	vpn[levels - 1] = (gpa >> (12 + 9 * (levels - 1))) & 0x7FF; // 11 at the root

	uint64_t a = (hgatp & 0xFFFFFFFFFFFull) * PAGESIZE;
	uint64_t pte = 0;
	int level = -1;
	uint64_t leaf_pte_addr = 0;   // for Svadu's write-back
	for (int i = levels - 1; i >= 0; i--) {
		uint64_t pte_addr = a + vpn[i] * PTESIZE;
		// The walk's own reads are subject to physical memory attributes.
		// Without this the read silently returns zero, the PTE looks
		// invalid, and a page fault is reported where the architecture
		// requires an access fault.
		if (!mem.is_backed(pte_addr, PTESIZE)) {
			cause = access_fault_cause(type);
			tval = gpa;
			return false;
		}
		pte = mem.read64(pte_addr);
		if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W)))
			return gfault(guest_fault_cause(type));
		if ((pte & PTE_R) || (pte & PTE_X)) { level = i; leaf_pte_addr = pte_addr; break; }
		// A non-leaf PTE is a pointer and nothing else; A, D, U and the
		// Svpbmt memory-type bits belong to leaves, and carrying one here
		// is a reserved encoding.
		if ((pte & (PTE_A | PTE_D | PTE_U))
		    || (Extensions.SVPBMT && (pte & PTE_PBMT)))
			return gfault(guest_fault_cause(type));
		if (i == 0) return gfault(guest_fault_cause(type));
		a = pte_ppn(pte) * PAGESIZE;
	}

	// Every G-stage leaf must be user-accessible. Both VS and VU sit below
	// HS, so from the second stage's point of view the guest is always
	// user code -- a G-stage page without U would be unreachable by any
	// guest at all, which makes it a configuration error rather than a
	// permission the hypervisor could have intended.
	if (!(pte & PTE_U)) {
		return gfault(guest_fault_cause(type));
	}

	// MXR belongs to the hypervisor's own sstatus here, not the guest's.
	// The second stage is the hypervisor's mapping, so it is the
	// hypervisor's execute-readable setting that governs whether an
	// X-only G-stage page can be loaded from. The G-stage ignored MXR
	// entirely, so an X-only guest-physical page refused every load even
	// with the bit set.
	const bool hs_mxr = (regs.read_csr(CSR_MSTATUS) & MSTATUS_MXR) != 0;

	bool perm_ok;
	switch (type) {
	case AccessType::Fetch: perm_ok = (pte & PTE_X) != 0; break;
	case AccessType::Load:  perm_ok = (pte & PTE_R) || (hs_mxr && (pte & PTE_X)); break;
	case AccessType::Store: perm_ok = (pte & PTE_W) != 0; break;
	case AccessType::CacheBlock: perm_ok = (pte & PTE_R) || (pte & PTE_W); break;
	default:                perm_ok = (pte & PTE_R) && (pte & PTE_W); break;
	}
	// A walk of the guest's page tables is a *read* of memory whatever the
	// original access was: fetching a PTE needs the page holding it to be
	// readable, not executable or writable.
	if (implicit) perm_ok = (pte & PTE_R) != 0;
	if (!perm_ok) return gfault(guest_fault_cause(type));

	// A/D on the G-stage, governed by menvcfg.ADUE alone: this stage is
	// the hypervisor's own mapping, so the guest's henvcfg has no say over
	// it. With ADUE clear the behaviour is Svade and a missing bit faults.
	{
		const uint64_t need = ad_bits_needed(type, implicit);
		if ((pte & need) != need) {
			if (!adue_enabled(regs, false) || leaf_pte_addr == 0
			    || !mem.is_backed(leaf_pte_addr, PTESIZE))
				return gfault(guest_fault_cause(type));
			pte |= need;
			mem.write64(leaf_pte_addr, pte);
		}
	}

	uint64_t ppn_full = pte_ppn(pte);
	if (level > 0) {
		uint64_t low_mask = (1ull << (9 * level)) - 1;
		if (ppn_full & low_mask) return gfault(guest_fault_cause(type));
	}
	uint64_t low_bits = 12 + 9 * level;
	pa = (ppn_full << 12) | (gpa & ((1ull << low_bits) - 1));
	return true;
}
}

// Pointer masking (Smnpm / Ssnpm / Sspm).
//
// A two-bit PMM field selects how many of an address's top bits are ignored
// on a *data* access: 0 means masking off, 2 means PMLEN=7, 3 means
// PMLEN=16. Value 1 is reserved. RVA23 requires PMLEN=0 and PMLEN=7 at
// minimum, and both of those are here.
//
// The field lives in the envcfg of the mode *above* the one being masked --
// menvcfg.PMM governs S-mode, senvcfg.PMM governs U-mode -- so a mode
// cannot exempt itself from masking its supervisor imposed.
//
// "Ignored" is not "cleared": the discarded bits are replaced by the sign
// extension of the highest retained bit. That is what keeps a masked kernel
// pointer canonical, and getting it wrong by zeroing instead would send
// every high address into the bottom half of the space.
//
// Instruction fetch is never masked, and neither are the addresses the page
// table walk itself produces -- masking applies to the effective address a
// load or store computed, and nothing further down.
int pointer_mask_len(Registers &regs, bool as_guest)
{
	uint64_t pmm;
	// An hlv/hsv is not masked by the field that governs the mode issuing
	// it. It reaches into the guest, so hstatus.HUPMM -- the hypervisor's
	// own control over the addresses it hands to those instructions --
	// selects the length instead. Using menvcfg here would have let the
	// hypervisor's S-mode masking silently rewrite guest pointers.
	if (as_guest) pmm = (regs.read_csr(CSR_HSTATUS) >> 48) & 0x3;
	else switch (regs.get_priv()) {
	case PrivMode::U: pmm = (regs.read_csr(CSR_SENVCFG) >> 32) & 0x3; break;
	case PrivMode::S: pmm = (regs.read_csr(CSR_MENVCFG) >> 32) & 0x3; break;
	default: return 0; // M-mode masking is Smmpm, which RVA23 does not mandate
	}
	switch (pmm) {
	case 2: return 7;
	case 3: return 16;
	default: return 0; // 0 = off; 1 is reserved and behaves as off
	}
}

uint64_t apply_pointer_mask(Registers &regs, uint64_t vaddr, AccessType type, bool as_guest)
{
	if (type == AccessType::Fetch) return vaddr;
	if (!Extensions.SSNPM) return vaddr;
	int pmlen = pointer_mask_len(regs, as_guest);
	if (pmlen == 0) return vaddr;
	// Sign-extend from the highest bit that survives, discarding the top
	// pmlen bits.
	return (uint64_t)((int64_t)(vaddr << pmlen) >> pmlen);
}

bool mmu_translate(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type,
                    uint64_t &paddr, uint64_t &cause, uint64_t &tval, bool as_guest)
{
	// M-mode does not translate, except when it explicitly asks to:
	// mstatus.MPRV makes a load or store behave as though issued from
	// mstatus.MPP, using that mode's translation *and* its permission
	// checks. It is how a machine-mode trap handler reaches a supervisor
	// or user buffer without hand-walking the page tables.
	//
	// MPRV never affects instruction fetch -- the handler still executes
	// its own code from its own address space, and applying it to fetch
	// would send M-mode through the guest's mappings mid-handler.
	//
	// Masking happens before anything else, including the bare-mode path
	// below: it transforms the effective address itself, not the
	// translation of one, so it applies whether or not paging is on.
	vaddr = apply_pointer_mask(regs, vaddr, type, as_guest);

	// A guest access reads the guest's own satp and runs at the guest's own
	// privilege, which is what makes hlv/hsv reach exactly the memory the
	// guest could reach -- and fault where the guest would fault -- rather
	// than whatever the hypervisor happens to have mapped.
	//
	// Two-stage translation applies to *any* access made in a virtual mode,
	// not only to hlv/hsv. That distinction was wrong and it was a large
	// hole: with the second stage gated on the explicit hypervisor
	// load/store instructions, a guest's ordinary loads and stores went
	// straight to host physical memory, unmediated by hgatp. A guest could
	// address anything.
	//
	// What hlv/hsv actually add is *whose* privilege to check against: they
	// execute in HS-mode on the guest's behalf, so the permission bits come
	// from hstatus.SPVP rather than from the current mode. A real VS access
	// is already running at the guest's own privilege.
	// Which privilege, and which *world*, this access is made in. Neither
	// is simply "the current one":
	//
	//   MPRV makes an M-mode load or store run at mstatus.MPP, which is
	//     how M-mode reaches memory the way the mode it interrupted would
	//     have. Fetches are never affected.
	//   MPV extends that to virtualisation: with MPV set, the borrowed
	//     privilege was a *guest* one, so the access goes through both
	//     stages -- the guest's vsatp and then hgatp -- rather than
	//     through satp. Reading MPRV without MPV made such an access use
	//     the hypervisor's own mapping at the guest's privilege, which is
	//     a different address, silently.
	//
	// So the two have to be resolved before the world is decided, not
	// after: virt_access used to be computed first and MPV never entered
	// into it.
	PrivMode eff_priv = regs.get_priv();
	bool mprv_virt = false;
	if (type != AccessType::Fetch && regs.get_priv() == PrivMode::M) {
		uint64_t st = regs.read_csr(CSR_MSTATUS);
		if (st & MSTATUS_MPRV) {
			eff_priv = (PrivMode)((st >> 11) & 3);
			// MPP=M is not a guest privilege however MPV reads: M-mode
			// borrowing M-mode's view is still a direct physical access.
			mprv_virt = Extensions.H && (st & MSTATUS_MPV) != 0
			            && eff_priv != PrivMode::M;
		}
	}
	const bool virt_access = Extensions.H && (as_guest || regs.get_virt() || mprv_virt);
	uint64_t satp = regs.read_csr(virt_access ? CSR_VSATP : CSR_SATP);
	if (as_guest) {
		// hstatus.SPVP says whether the guest was in VS or VU -- an hlv
		// must be checked against the guest's supervisor/user permission
		// bits, not the hypervisor's.
		eff_priv = (regs.read_csr(CSR_HSTATUS) & HSTATUS_SPVP) ? PrivMode::S : PrivMode::U;
	}
	uint64_t mode = satp >> 60;
	if (eff_priv == PrivMode::M || mode == 0) {
		// No first stage, but a guest access still owes the second one:
		// with the guest's own paging off its addresses are guest
		// *physical* addresses, which hgatp still has to place.
		if (virt_access) {
			if (!gstage_translate(regs, mem, vaddr, type, paddr, cause, tval, false)) {
				tval = vaddr; // stval reports the guest VA, htval the GPA
				return false;
			}
			return true;
		}
		paddr = vaddr;
		return true;
	}
	const int levels = mode_levels(mode);
	if (levels == 0) {
		// A MODE this hart does not implement. satp's MODE is WARL and
		// rejects these on write, so reaching here means the field was
		// set by something that bypassed that check.
		paddr = vaddr;
		return true;
	}

	// The VA must be canonical: the bits above the mode's width all equal
	// the top translated bit, i.e. the address is a sign-extended value of
	// that width -- 39 bits for Sv39, 48 for Sv48, 57 for Sv57. A
	// non-canonical VA faults before the walk even begins on real
	// hardware, which is what stops the unused top of the address space
	// from aliasing the bottom.
	const int va_bits = mode_va_bits(levels);
	const int sext_shift = 64 - va_bits;
	uint64_t sext_check = (uint64_t)((int64_t)(vaddr << sext_shift) >> sext_shift);
	if (sext_check != vaddr) {
		cause = fault_cause(type);
		tval = vaddr;
		return false;
	}

	uint64_t mstatus = regs.read_csr(virt_access ? CSR_VSSTATUS : CSR_MSTATUS);
	bool sum = mstatus & MSTATUS_SUM;
	// Under virtualisation both MXR bits are in effect on the first stage:
	// the guest's own vsstatus.MXR, and the hypervisor's sstatus.MXR, which
	// applies to every stage. Reading only the guest's made an HS-mode
	// hypervisor unable to relax the rule for a guest that had not.
	bool mxr = (mstatus & MSTATUS_MXR) != 0;
	if (virt_access) mxr = mxr || (regs.read_csr(CSR_MSTATUS) & MSTATUS_MXR) != 0;
	// The same effective privilege the walk was started with: for an hlv
	// this is the guest's, not the hypervisor's, so a U-page check tests
	// what the guest could reach.
	PrivMode priv = eff_priv;

	uint64_t vpn[5] = {0, 0, 0, 0, 0};
	for (int i = 0; i < levels; i++) vpn[i] = (vaddr >> (12 + 9 * i)) & 0x1FF;

	uint64_t a = (satp & 0xFFFFFFFFFFFull) * PAGESIZE;
	uint64_t pte = 0;
	int level = -1;
	// Where the leaf PTE was read from, so Svadu can write A/D back into
	// it. This is the *physical* address after any G-stage placement, not
	// the guest physical one the walk indexed with.
	uint64_t leaf_pte_addr = 0;
	for (int i = levels - 1; i >= 0; i--) {
		uint64_t pte_addr = a + vpn[i] * PTESIZE;
		// In a guest walk this address is a guest physical one, so the
		// second stage has to place it before the PTE can be read. This
		// is the part that makes two-stage translation expensive: a
		// single guest access performs one of these per level, plus one
		// for the final address, and any of them can fault.
		if (virt_access) {
			uint64_t pte_pa;
			if (!gstage_translate(regs, mem, pte_addr, type, pte_pa, cause, tval, true)) {
				tval = vaddr; // the access that faulted, not the PTE address
				return false;
			}
			pte_addr = pte_pa;
		}
		// A page-table read is an access like any other and is subject to
		// physical memory attributes. Skipping this check does not merely
		// miss a fault: the read returns zero, the zero looks like an
		// invalid PTE, and the walk reports a *page* fault where the
		// architecture requires an *access* fault -- sending the supervisor
		// off to repair a mapping that was never the problem.
		// The walk's own reads answer to physical memory attributes *and* to
		// PMP. PMP is checked at supervisor privilege regardless of who made
		// the original access: an implicit page-table read is the hardware's
		// access, not the program's, and M-mode's exemption does not extend
		// to it.
		if (!mem.is_backed(pte_addr, PTESIZE)) {
			cause = access_fault_cause(type);
			tval = vaddr;
			return false;
		}
		if (Extensions.SMPMP
		    && !pmp::check(regs, pte_addr, PTESIZE, pmp::ACC_LOAD, (uint8_t)PrivMode::S)) {
			cause = access_fault_cause(type);
			tval = vaddr;
			return false;
		}
		pte = mem.read64(pte_addr);
		if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W))) {
			// Invalid, or the reserved W=1/R=0 encoding.
			cause = fault_cause(type);
			tval = vaddr;
			return false;
		}
		if ((pte & PTE_R) || (pte & PTE_X)) {
			level = i; // leaf
			leaf_pte_addr = pte_addr;
			break;
		}

		// A non-leaf PTE is a pointer and nothing else. A, D and U belong to
		// leaves, and Svpbmt's memory-type bits likewise; carrying any of
		// them here is a reserved encoding and faults. Ignoring them instead
		// silently accepts a page table that names attributes it has no way
		// to apply.
		if ((pte & (PTE_A | PTE_D | PTE_U))
		    || (Extensions.SVPBMT && (pte & PTE_PBMT))) {
			cause = fault_cause(type);
			tval = vaddr;
			return false;
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
	case AccessType::CacheBlock: perm_ok = (pte & PTE_R) || (pte & PTE_W); break;
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

	// A and D. Two behaviours are architecturally permitted and this hart
	// implements both, selected by envcfg.ADUE:
	//
	//   Svade (ADUE=0, and what RVA23S64 mandates) faults on a clear A, or
	//     on a clear D for a write, and leaves it to the supervisor to set
	//     the bit and retry. Linux reads the choice out of the DT and, for
	//     an Svade hart, pre-sets A/D when it installs a PTE, which is why
	//     the boot path here never needed hardware update.
	//   Svadu (ADUE=1) sets the missing bits in the PTE itself and lets the
	//     access proceed.
	//
	// The update is a write to the page table, so it is subject to the same
	// permission checks the walk's reads were -- and, in a guest walk, it
	// must land on the *physical* address the G-stage placed the PTE at,
	// not on the guest physical address the walk indexed with. Getting
	// that wrong writes A into whatever the hypervisor happens to have at
	// that guest physical address.
	{
		const uint64_t need = ad_bits_needed(type, false);
		if ((pte & need) != need) {
			if (!adue_enabled(regs, virt_access) || leaf_pte_addr == 0
			    || !mem.is_backed(leaf_pte_addr, PTESIZE)) {
				cause = fault_cause(type);
				tval = vaddr;
				return false;
			}
			// The store answers to PMP exactly as the walk's reads did:
			// this is the hardware's own access, made at supervisor
			// privilege whoever asked for it.
			if (Extensions.SMPMP
			    && !pmp::check(regs, leaf_pte_addr, PTESIZE, pmp::ACC_STORE,
			                   (uint8_t)PrivMode::S)) {
				cause = access_fault_cause(type);
				tval = vaddr;
				return false;
			}
			pte |= need;
			mem.write64(leaf_pte_addr, pte);
		}
	}

	// Svpbmt: bits 62:61 select a memory type. This machine has no caches
	// and no distinction between cacheable and IO memory, so PMA (0), NC (1)
	// and IO (2) all behave identically -- there is nothing for the type to
	// change. Value 3 is reserved and must fault, and so must any nonzero
	// value at all when menvcfg.PBMTE is clear: an OS probes for Svpbmt by
	// setting the bits and seeing whether the access faults, so silently
	// accepting them would report support this hart does not have.
	{
		uint64_t pbmt = (pte & PTE_PBMT) >> PTE_PBMT_SHIFT;
		// Two separate gates, and both have to hold: the hart must
		// implement Svpbmt at all, and M-mode must have enabled it for
		// S-mode. Without the extension the field is simply reserved.
		bool pbmt_enabled = Extensions.SVPBMT
		                 && (regs.read_csr(CSR_MENVCFG) & MENVCFG_PBMTE) != 0;
		if (pbmt == 3 || (pbmt != 0 && !pbmt_enabled)) {
			cause = fault_cause(type);
			tval = vaddr;
			return false;
		}
	}

	// Bits 60:54 are reserved in every Sv mode and must be zero. Checking
	// this is what makes the Svnapot and Svpbmt bits above meaningful --
	// without it, a PTE with anything set up there would translate happily
	// and an OS probing for those extensions would get the wrong answer.
	if (pte & PTE_RESERVED) {
		cause = fault_cause(type);
		tval = vaddr;
		return false;
	}

	uint64_t ppn_full = pte_ppn(pte);

	// Svnapot: the N bit marks this leaf as one of a naturally-aligned
	// power-of-two contiguous range that share a single translation. Sv39
	// defines exactly one encoding, 64KB (eight 4KB pages), signalled by
	// ppn[3:0] == 0b1000; every other value of those bits with N set is
	// reserved.
	//
	// The effect is that the low bits of the PPN come from the virtual
	// address rather than the PTE, which is the opposite of the superpage
	// rule below -- so it has to be applied here, before the page offset is
	// composed, and only to a leaf at level 0.
	if (pte & PTE_N) {
		// Without Svnapot the N bit is reserved, so a PTE that sets it
		// must fault rather than translate as though the bit were absent.
		// That is what lets an OS discover the extension is missing.
		if (!Extensions.SVNAPOT || level != 0 || (ppn_full & 0xF) != 0x8) {
			cause = fault_cause(type);
			tval = vaddr;
			return false;
		}
		// A 64KB NAPOT region: take the top of the PPN from the PTE and
		// bits 15:12 of the address from the VA.
		ppn_full = (ppn_full & ~0xFull) | ((vaddr >> 12) & 0xF);
	}

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

	// What the guest's tables produced is a guest physical address, not a
	// real one. The second stage places it -- and can fault here even
	// though the guest's own tables were perfectly happy, which is exactly
	// the case the distinct guest-page-fault causes exist to report.
	if (virt_access) {
		uint64_t gpa = paddr;
		if (!gstage_translate(regs, mem, gpa, type, paddr, cause, tval, false)) {
			tval = vaddr;
			return false;
		}
	}
	return true;
}
