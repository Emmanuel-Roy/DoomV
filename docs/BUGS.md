# DoomV: every bug that stood between this emulator and the conformance suite

This is the bug record. Not a changelog — a changelog says what was added.
This says what was *wrong*, why it looked right at the time, why nothing
caught it for as long as it went uncaught, and what the fix actually
touched.

It is ordered oldest to newest, which is the only ordering that makes the
recurring patterns visible. The same handful of mistakes are made over and
over in this project under different names, and reading them in sequence is
the only way to see that. There is a taxonomy of them in
[Recurring patterns](#patterns), but do not read it first; it means more
after the evidence.

Every entry cites a commit sha. Where a number is quoted (test counts,
failure counts, boot-line counts) it comes from the commit message or the
harness README that recorded it, and the entry says so. Where something
could not be verified from the repository it is marked **[unverified]**
rather than smoothed over.

---

<a id="how-to-read"></a>
## How to read this document, and where the history lives

The repository carries two histories, and both are needed.

`history-before-cleanup` (81 commits, 2025-12-17 through 2026-09-05) is the
original development history. It is where the guest bring-up, the Linux
boot, and the first differential testing actually happened, in real time,
with commit messages written while the bug was still fresh. Its commits are
the primary evidence for [Part I](#part-i), [Part II](#part-ii) and
[Part III](#part-iii).

`main` (75 commits, 2026-09-05 through 2026-09-07) is a curated
republication of the codebase — one commit per subsystem — with the RVA23
conformance work built on top of it. The two branches are disjoint: there
is no merge base, `git log main..history-before-cleanup` returns all 81 and
`git log history-before-cleanup..main` returns all 75. Commits on `main`
dated 2026-09-05 that describe a subsystem are republications; the bug they
narrate was often found weeks earlier on the other branch. Where that is
the case this document cites both shas and dates the bug by when it was
*found*, not by when the narrative was written.

The conformance work — RVA23 phases 1 through 8, the move to Sail, the
riscv-arch-test harness, SoftFloat, PMP — is `main`-only and genuinely
dates from 2026-09-05 to 2026-09-07. That is [Part IV](#part-iv) onward.

A note on the earliest commits. The repository's first 18 commits
(2025-12-17 through 2026-01-04) are README edits and an SDL2 template; the
emulator proper begins at `6306596` (2026-08-17). Nothing before that
contains a bug worth recording.

---

<a id="results"></a>
## Results

Numbers as recorded in the commits that produced them.

### Hand-written differential suites (vs spike, then vs Sail)

| Milestone | Suites | Cases | Commit |
| --- | --- | --- | --- |
| First V differential suite | 1 | 47 | `5eff60b` |
| Addressing modes added | 1 | 58 | `5d89782` |
| Zb\* families verified | 2 | 126 | `537e6ba` |
| RVA23 phase 1 (hints) | 7 | 237 | `7bf217a` |
| RVA23 phase 2 (counters, CSR privilege) | 8 | 253 | `fbdaf84` |
| RVA23 phase 3 (Zvbb) | 9 | 290 | `e414423` |
| RVA23 phase 4 (Zfa) | 10 | 405 | `eda4103` |
| RVA23 phase 5 (half precision) | 12 | 518 | `f4d66aa` |
| RVA23 phase 6 (Sv\* extensions) | 13 | 534 | `e60ec1c` |
| RVA23 phase 7 (pointer masking) | 14 | 546 | `7991e11` |
| RVA23 phase 8a (H CSRs) | 15 | 568 | `bb1cc50` |
| RVA23 phase 8b (hlv/hsv) | 16 | 590 | `463ac7c` |
| RVA23 phase 8c (two-stage translation) | 17 | 604 | `9a5c416` |
| RVA23 phase 8d (guest delegation) | 18 | 620 | `efe8823` |
| Sscofpmf + Ssstateen | 19 | 639 | `07a0090` |
| **All 19 matching Sail, no divergences** | **19** | **639** | `2667bf1` |

### Official riscv-arch-test (ACT4, RVA23S64, Sail as reference)

| Family | Before | After | Commit |
| --- | --- | --- | --- |
| D and F | 103 failures | 0 (196 of 196) | `a27950a` |
| PMP families | 0 of 81 | 76 of 81 | `fdd74c3` |
| Zfhmin / ZfhminD | 8 of 12 | 12 of 12 | `fdd74c3` |
| Sv families | 30 failures | 9 failures | `cc7d184` |
| 663-test suite | 30 failures | 14 failures | `0543dfa` |
| 663-test suite | 14 failures | ~10 failures | `b9474a0` |

The full ACT4 suite is 1977 tests (`ee9cdeb`); the 663 figure is the subset
that applies to this configuration, and is the number `archtest.py` itself
cites in its polling comment.

The framing figures sometimes quoted for this project — arch-test 540 → 651
of 663, and 12 remaining failures — are **[unverified]** against the
repository as committed. What the commits record is the sequence above:
30 → 14 → ~10 on the 663-test suite, with D/F, PMP, Zfhmin and Sv broken
out separately as they were fixed. The residual failures are all one
problem; see [What remains](#remains).

### Profile conformance audit

`mkconfig.py` audits DoomV against riscv-arch-test's own
`sail-RVA23S64.yaml` extension list. As of `761291a`: **78 mandatory
extensions, 61 implemented, 17 satisfied as structural guarantees, 0
missing.** An earlier run of the same audit is what found Sscofpmf and
Ssstateen absent after the mandatory set had been declared complete
([Bug 67](#bug67)).

### Guests

DOOM boots to a playable E1M1. Linux 6.12 boots to `/bin/sh` in 238 lines
of console output (`2667bf1`, re-verified in `936df17`).

---

<a id="toc"></a>
## Table of contents

<a id="part-i"></a>
### Part I — Guest bring-up: making DOOM run at all (2026-08-17)

1. [`_write` silently discarded every diagnostic the guest tried to emit](#bug1)
2. [IWAD detection went through `fopen`, not through the WAD backend](#bug2)
3. [`Z_Malloc` failed a 64KB allocation immediately after a 6MB one succeeded](#bug3)
4. [The linker script's `.bss` rule did not match `.sbss*`, so the first `malloc` overlapped live globals](#bug4)
5. [`DOOMGENERIC_RESX/RESY` were set by a header nobody included](#bug5)
6. [Instruction-to-tick scaling ran simulated time about a thousand times too fast](#bug6)
7. [`-specs=nosys.specs` passed twice on one command line hard-errored the link](#bug7)
8. [Host FPU NaN payloads leaked into guest registers](#bug8)
9. [`fcvt.lu.{s,d}` had undefined behaviour above 2^63](#bug9)
10. [A freshly linked `riscv_doom.exe` could not launch](#bug10)

<a id="part-ii"></a>
### Part II — Booting Linux: firmware, devices and the silent hangs (2026-08-27 → 2026-09-04)

11. [`misa` did not report S, so OpenSBI's cold-boot hart lottery was unwinnable](#bug11)
12. [OpenSBI pinned to v1.3 because everything newer needs a PIE-capable linker](#bug12)
13. [OpenSBI's `bool` typedef collides with GCC 15's C23 default](#bug13)
14. [The RAM_BASE relocation missed a fourth hardcoded copy and a stale test constant](#bug14)
15. [`medlow` cannot address 0x80000000](#bug15)
16. [`satp` had no WARL semantics, so Linux "detected" Sv57 and ran under it](#bug16)
17. [The device tree never declared AIA or Sstc, so Linux refused to use them](#bug17)
18. [OpenSBI v1.3 reports SBI 1.0 unconditionally, so Linux's DBCN console never activated](#bug18)
19. [`CONFIG_HVC_RISCV_SBI` is unreachable from defconfig, so the console died partway through boot](#bug19)
20. [`CSR_TIME` was never implemented, so every timer deadline was computed from zero](#bug20)
21. [`timebase-frequency` of 10MHz made a kernel tick unservicable](#bug21)
22. [`mtopi`/`stopi` were missing, so the AIA interrupt handler never dispatched anything](#bug22)
23. [GNU Make truncated OpenSBI's `ar` recipe at 8191 characters, silently](#bug23)
24. [OpenSBI's build script only worked on the machine it was written on](#bug24)
25. [`make clean` was broken in one shell or the other, depending on which Makefile](#bug25)
26. [`core.autocrlf` broke every POSIX shell script in the tree, invisibly](#bug26)

<a id="part-iii"></a>
### Part III — Differential testing against spike: the silent wrong answers (2026-09-04 → 2026-09-05)

27. [Unrecognised `funct7` fell through to the base-I meaning: `sh3add` executed as `OR`](#bug27)
28. [Every mask-producing vector instruction wrote `v0` instead of its named `vd`](#bug28)
29. [The vector load/store `mop` field was extracted one bit too high](#bug29)
30. [`mstatus.VS` was not enforced, and vector state was never marked Dirty](#bug30)
31. [`mstatus.FS` had the same gap on the floating-point side](#bug31)
32. [Fault-only-first loads were executed as plain unit-stride loads](#bug32)
33. [A faulting vector access left `vstart` at 0, so it was not restartable](#bug33)
34. [`fmin(+0,-0)` returned the wrong zero](#bug34)
35. [`compare.py` reported "MATCH: all 0 words identical" on an empty dump](#bug35)
36. [spike's `+signature` plusarg silently writes nothing in this build](#bug36)
37. [A test's unaligned `ld` trapped before the MMU, then spun forever in M-mode](#bug37)
38. [A trap handler used `t1` — the faulting instruction's own base register](#bug38)
39. [`sp` pointed outside the pages the test mapped, so the handler faulted recursively](#bug39)
40. [An illegal instruction halted the debugger permanently, killing a guest that was behaving correctly](#bug40)
41. [The breakpoint skip was disarmed on first use, and `should_halt()` is called twice](#bug41)
42. [Misaligned scalar accesses are a legal choice, not a conformance property](#bug42)

<a id="part-iv"></a>
### Part IV — RVA23 conformance: the extensions, and the gaps they exposed (2026-09-05 → 2026-09-06)

43. [The device tree described a smaller machine than the one it runs on](#bug43)
44. [`MOP.R.n` and `MOP.RR.n` must write zero to `rd`, not leave it alone](#bug44)
45. [CSR privilege enforcement was entirely absent](#bug45)
46. [The comment arguing `mtopi`/`stopi` were not worth the surface was exactly backwards](#bug46)
47. [`cbo.zero`'s address must be aligned *down* to the block](#bug47)
48. [`FEQ`/`FMIN`/`FMAX` raised NV for a quiet NaN](#bug48)
49. [17 Zvbb instructions fell through to `exec_v_int` and did nothing](#bug49)
50. [Zvbb's `funct6` 0x12 is shared with `vzext`/`vsext`, and `vror.vi` hides `imm[5]` in `funct6`](#bug50)
51. [`exec_v_fp` returned silently for any SEW other than 32 or 64](#bug51)
52. [VFUNARY0's widening and narrowing subcodes were ignored — fourteen instructions](#bug52)
53. [Float-to-integer conversions never reported inexact](#bug53)
54. [The Svnapot and Svpbmt config flags were added but `mmu.cpp` never consulted them](#bug54)
55. [PTE bits 60:54 were not checked as reserved](#bug55)
56. [The pointer-masking test never left M-mode, where masking correctly does not apply](#bug56)
57. [`sspm` is not a name spike recognises, and spike failing to start looked like a hang](#bug57)
58. [The pointer-masking test used "does it fault" as a proxy, and measured a missing PMA check instead](#bug58)
59. [`csr[9:8] == 2` was read as a privilege level; it is the hypervisor/VS-CSR encoding](#bug59)
60. [`hstatus.VSBE` and `VGEIN` were writable](#bug60)
61. [`vstvec` and `vsatp` never got the WARL treatment their S-mode twins had](#bug61)
62. [Three hypervisor decode collisions, each of which mis-decodes silently](#bug62)
63. [The two-stage translation test passed 14/14 with the intermediate PTE translation removed](#bug63)
64. [`hedeleg` has read-only-zero bits, and they were not implemented at all](#bug64)
65. [`stval` and `htval` carry different addresses; one field was serving both](#bug65)
66. [The `hdeleg` test's labels asserted the opposite of what it checked — and it passed](#bug66)
67. [Sscofpmf and Ssstateen were missing after the mandatory set was declared complete](#bug67)
68. [The `stateen` WARL masks were guessed rather than checked](#bug68)
69. [LCOFI was derived from the OF bits rather than latched](#bug69)
70. [spike spells the extension `smstateen`, not `ssstateen`](#bug70)

<a id="part-v"></a>
### Part V — Sail as the reference: what a formal model found (2026-09-06 → 2026-09-07)

71. [riscv-arch-test's own Sail config cannot be loaded by any tagged Sail release](#bug71)
72. [z3 must be on `PATH`, not merely installed](#bug72)
73. [Sail's `build_simulator.sh` has a CRLF shebang on an autocrlf checkout](#bug73)
74. [`sail_sig.sh` hand-rolled a memory dump that Sail has a flag for](#bug74)
75. [Eleven differential suites depended on spike's permissive PMP default](#bug75)
76. [VLEN 256 versus 128: a configuration difference, not a defect](#bug76)
77. [GEILEN: an open architectural choice, wrongly diffed](#bug77)
78. [A test used `sfence.vma` where `hfence.gvma` was required, and DoomV never noticed](#bug78)
79. [`menvcfg/senvcfg/henvcfg.PMM` stored the reserved value 1](#bug79)
80. [`hstatus.HUPMM` was missing entirely](#bug80)
81. [spike leaves `hstatus.HUPMM` read-only zero — recorded, not worked around](#bug81)
82. [The golden reference could not run without the model it outranks being installed](#bug82)
83. [A timeout is the normal ending, and reading it as the result fails every passing test](#bug83)
84. [Substituting `riscv64-linux-gnu-gcc` 15.2.0 makes Sail's *own* reference run die in a trap loop](#bug84)
85. [Sail must be pinned to 0.13.1, in a second build, and the deviation must be written down](#bug85)
86. [Both harnesses share `./signature.log`, and releasing the lock too early misattributes a run](#bug86)
87. [Sail writes 64-bit signature words and DoomV writes 32-bit](#bug87)
88. [65 files git records as symlinks arrived as text files containing their targets](#bug88)

<a id="part-vi"></a>
### Part VI — riscv-arch-test: the certification suite (2026-09-07)

89. [`FCVT.D.S` never touched fflags](#bug89)
90. [Float-to-integer conversions range-checked the *unrounded* value](#bug90)
91. [`collect_fflags` read MXCSR directly, and MinGW's double `fma()` never touches it](#bug91)
92. [RMM has no x86 encoding at all — 390 cases in a single file](#bug92)
93. [Host exception flags are not trustworthy on an ARM64 machine running x86-64 under Prism](#bug93)
94. [Berkeley SoftFloat: the design change, and why the integration was small](#bug94)
95. [PMP did not exist, and the tests had been shaped around its absence](#bug95)
96. [`mstatus.MPRV` was documented as "not modeled yet, since nothing needs it"](#bug96)
97. [`FSH` and `FMV.X.H` applied NaN-boxing to a raw bit transfer](#bug97)
98. [The Makefile reported success without relinking, so a stale binary was tested](#bug98)
99. [`mstatus.SD` was never published](#bug99)
100. [Zicbom did nothing at all, on the argument that a machine with no cache satisfies it trivially](#bug100)
101. [Zicbom's permission rule was got wrong by reasoning from plausibility](#bug101)
102. [No physical-memory-attribute checking: unbacked reads returned zero](#bug102)
103. [A page-table walk into unbacked memory reported a *page* fault where the architecture requires an *access* fault](#bug103)
104. [PMP was checked on the final address but not on the walk's own reads](#bug104)
105. [Non-leaf PTEs were not validated for reserved bits](#bug105)
106. [`mstatus.TVM` did not exist](#bug106)
107. [Instruction fetch translated once and read four bytes](#bug107)
108. [`translate_or_trap` had no size parameter, so every straddle check was dead code](#bug108)
109. [Atomics were never alignment-checked, and the check order is not the obvious one](#bug109)

<a id="part-vii"></a>
### Part VII — Cross-cutting

- [Recurring patterns](#patterns)
- [Treating a reference implementation as the specification](#reference-as-spec)
- [The taxonomy used for reference disagreements](#taxonomy)
- [Bugs that were design assumptions, written down as reasoning](#assumptions)
- [Bugs in the tests, not the emulator](#test-bugs)
- [What remains](#remains)

---
