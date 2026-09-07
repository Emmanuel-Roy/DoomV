#pragma once
// Physical Memory Protection (Smpmp).
//
// PMP sits *after* address translation: it checks physical addresses, and
// it checks every one of them -- instruction fetches, loads, stores, and
// the implicit accesses a page-table walk makes on its own behalf. A region
// that PMP denies is denied even if the page tables permit it, which is the
// entire point: it is M-mode's control over what S and U mode can reach,
// and it must not be bypassable by anything S-mode can write.
//
// Until now DoomV had none of this, and the differential tests worked
// around its absence by writing a single all-permissive entry at the top of
// every privilege-dropping test. That is exactly backwards -- the tests
// were configured to match the implementation's gap.
//
// Failure is not a page fault. A PMP denial raises an *access* fault
// (cause 1/5/7 for fetch/load/store) rather than a page fault (12/13/15),
// and the distinction matters to software: a page fault invites the
// supervisor to fix a mapping and retry, while an access fault says the
// physical region is not reachable at this privilege at all.
#include <cstdint>

class Registers;

namespace pmp {

// pmpcfg byte layout. A is the address-matching mode in bits 4:3.
constexpr uint8_t CFG_R = 1 << 0;
constexpr uint8_t CFG_W = 1 << 1;
constexpr uint8_t CFG_X = 1 << 2;
constexpr uint8_t CFG_A_MASK = 3 << 3;
constexpr uint8_t CFG_A_SHIFT = 3;
constexpr uint8_t CFG_L = 1 << 7;

enum : uint8_t {
	A_OFF   = 0,  // entry disabled
	A_TOR   = 1,  // top of range: [pmpaddr[i-1], pmpaddr[i])
	A_NA4   = 2,  // naturally aligned four-byte region
	A_NAPOT = 3,  // naturally aligned power-of-two region, size from the
	              // trailing ones in pmpaddr
};

// How many entries this hart implements. 16 is the common choice and is
// what the architectural tests assume when they walk every entry; the
// remaining 48 read as zero and ignore writes, which is architecturally
// legal (unimplemented entries are hardwired to zero).
constexpr unsigned ENTRIES = 16;

// CSR numbers. pmpcfg is even-indexed only on RV64: each CSR holds eight
// bytes, so pmpcfg0 covers entries 0-7 and pmpcfg2 covers 8-15. pmpcfg1
// and pmpcfg3 exist only on RV32 and are illegal here -- a detail worth
// keeping, since software probes for them.
constexpr uint16_t CSR_PMPCFG0  = 0x3A0;
constexpr uint16_t CSR_PMPCFG15 = 0x3AF;
constexpr uint16_t CSR_PMPADDR0 = 0x3B0;
constexpr uint16_t CSR_PMPADDR63 = 0x3EF;

inline bool is_pmpcfg(uint16_t csr)  { return csr >= CSR_PMPCFG0 && csr <= CSR_PMPCFG15; }
inline bool is_pmpaddr(uint16_t csr) { return csr >= CSR_PMPADDR0 && csr <= CSR_PMPADDR63; }
inline bool is_pmp_csr(uint16_t csr) { return is_pmpcfg(csr) || is_pmpaddr(csr); }

// Reads and writes with the lock rule applied. A locked entry (cfg.L) is
// immutable until reset -- including from M-mode, which is what makes PMP
// meaningful against a compromised supervisor that has somehow reached M.
// The lock also applies to the pmpaddr of the *next* entry when that entry
// is TOR, because that address is this region's upper bound.
uint64_t read_cfg(Registers &regs, uint16_t csr);
uint64_t read_addr(Registers &regs, uint16_t csr);
void write_cfg(Registers &regs, uint16_t csr, uint64_t value);
void write_addr(Registers &regs, uint16_t csr, uint64_t value);

// The access check itself.
//
// `priv` is the privilege the access is made at, which is not always the
// current privilege: an MPRV load/store runs at mstatus.MPP, and a
// page-table walk is checked at the privilege of the access that caused it.
// Passing the wrong one is a silent hole, so callers pass it explicitly
// rather than having this read the current mode.
//
// Returns true if the access is permitted.
bool check(Registers &regs, uint64_t paddr, unsigned size, int access, uint8_t priv);

// access values, matching AccessType's ordering without depending on it
// (mmu.hpp includes this file, not the other way round).
enum { ACC_FETCH = 0, ACC_LOAD = 1, ACC_STORE = 2 };

} // namespace pmp
