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
| Atomic misalignment cause and order | — | `ExceptionsSvZaamo`/`Zalrsc` M-mode pass | `10d7ed1` |
| **RVA23S64 overall** | **540 of 663** | **653 of 663** | current |

The full ACT4 suite is 1977 tests (`ee9cdeb`); the 663 figure is the subset
that applies to this configuration, and is the number `archtest.py` itself
cites in its polling comment.

The overall figure went from **540 of 663** when the suite was first wired
up to **653 of 663** today — 10 failures, of which at least seven are one
problem wearing three names. See [What remains](#remains).

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
110. [`SINVAL.VMA` did not obey `mstatus.TVM`](#bug110)

<a id="part-viii-toc"></a>
### Part VIII — After the certification suite: the hypervisor, and 663/663 (2026-09-07)

111. [An illegal instruction halted the debugger instead of trapping](#bug111)
112. [`sstatus` hid FS, VS, UXL and SD](#bug112)
113. [Linux was never actually booting](#bug113)
114. [Three HTIF bugs, all self-inflicted](#bug114)
115. [Seven hypervisor bits that existed and were never consulted](#bug115)
116. [Two-stage translation applied only to `hlv` and `hsv`](#bug116)

<a id="part-ix-toc"></a>
### Part IX — Widening the ISA: the crypto and half-precision extensions (2026-09-10)

117. [Headless mode exited before the signature was written](#bug117)
118. [A script that stripped debug output deleted the CSR write path](#bug118)
119. [A CSR that does not exist](#bug119)
120. [The reference was never consulted, because the model and its config did not match](#bug120)
121. [Zicfilp armed a landing-pad requirement on the wrong register](#bug121)
122. [A shadow-stack page is not a leaf by the usual test](#bug122)
123. [Zfh was wired into the wrong dispatch table](#bug123)
124. [`fclass.h` was claimed by Zfhmin](#bug124)
125. [The vector suite reported 3042 of 3042 while measuring nothing](#bug125)
126. [Every f16 NaN was canonicalised in transit](#bug126)
127. [The unsigned flag read the wrong direction](#bug127)
128. [The estimate tables were transcribed with Sail's index order intact](#bug128)
129. [The largest finite magnitude, computed by clearing the wrong bit](#bug129)
130. [`vslidedown` read past the end of the source](#bug130)
131. [`brev8` decoded in the wrong opcode](#bug131)
132. [The vector crypto opcode was not decoded at all](#bug132)
133. [GHASH multiplied in the direction it is described in](#bug133)
134. [`vaeskf2`'s round number was clamped as five bits](#bug134)
135. [vtype accepted reserved bits and unsupported SEW/LMUL ratios](#bug135)
136. [Half-precision narrowing never implemented RMM](#bug136)

<a id="part-x-toc"></a>
### Part X — The hypervisor suite, 38 of 43 to 42 of 43 (2026-09-10)

137. [VU-mode WFI never trapped](#bug137)
138. [An interrupt bound for the hypervisor was masked by its guest](#bug138)
139. [Unassigned user-level CSRs were readable and writable](#bug139)
140. [A mstateen bit hardwired to zero, and a comment that misnamed it](#bug140)
141. [A refusal from M-mode downgraded to a refusal from the hypervisor](#bug141)
142. [`henvcfg.PBMTE` did not gate the VS stage](#bug142)
143. [Pointer masking ignored MXR, and had lost the one `hlv` case that needs HUPMM](#bug143)
144. [A straddling access reported its last byte instead of the faulting page](#bug144)
- [What is left](#damo-remaining)
145. [An envcfg bit that advertised an extension the hart does not have](#bug145)
146. [`mstatus.TSR` was stored and never consulted](#bug146)
147. [The envcfg registers were a blacklist where they should be a whitelist](#bug147)
148. [virtio-blk's file offsets were 32 bits wide](#bug148)
149. [Not a bug: the missing TLB, and what it cost](#bug149)

<a id="part-xi-toc"></a>
### Part XI — Running a distribution, and the three ways a guest could not end its own run (2026-09-11)

150. [The poweroff register was implemented, acknowledged, and never read](#bug150)
151. [A headless guest console was write-only](#bug151)
152. [The fix for the poweroff was written for a kernel that could not run it](#bug152)
153. [Not a bug: a screenshot is not evidence](#bug153)

<a id="part-xii-toc"></a>
### Part XII — A keyboard and a mouse, and the constant that drifted (2026-09-11)

154. [DOOM had been unable to find its WAD since RAM grew](#bug154)
155. [The framebuffer aperture shadowed the new devices, in the byte paths only](#bug155)
156. [A console keyboard rebuilt from keysyms only works on one layout](#bug156)
157. [Not a bug: the test that measured the test](#bug157)

<a id="part-vii"></a>
### Part VII — Cross-cutting

- [Recurring patterns](#patterns)
- [Treating a reference implementation as the specification](#reference-as-spec)
- [The taxonomy used for reference disagreements](#taxonomy)
- [Bugs that were design assumptions, written down as reasoning](#assumptions)
- [Bugs in the tests, not the emulator](#test-bugs)
- [What remains](#remains)

---

# Part I — Guest bring-up: making DOOM run at all

Everything in this part predates any reference model. There was no spike,
no Sail, no signature comparison — the only oracle was "does a frame
appear". That is worth stating up front, because it explains the shape of
these bugs: every one of them was found by watching the machine run and
then instrumenting it, and several of them were only findable *because*
the first thing built was a way to see inside.

<a id="bug1"></a>
## 1. `_write` silently discarded every diagnostic the guest tried to emit

**Symptom.** The first end-to-end run of a cross-compiled DOOM against the
emulator produced nothing. Not a crash, not an error, not a partial frame —
the guest ran, retired instructions, and said nothing. `printf` produced no
output. `I_Error`, the function DOOM calls to explain why it is about to
give up, produced no output either.

**Root cause.** The guest is linked against `nosys.specs`, newlib's
"there is no operating system here" stub set. Its default `_write`
implementation returns failure and discards the buffer. That is the correct
behaviour for a target with no console, and it is also the behaviour that
makes a bring-up blind: every diagnostic the program emits to explain
itself is thrown away by the C library before it reaches the emulator at
all.

**Why it went unnoticed.** It could not be noticed. This is the bug that
hides all the other bugs — it is not a defect that produces wrong output,
it is the absence of any output channel through which a defect could be
observed. There was nothing to notice.

**Resolution.** An MMIO debug channel was added: a magic address in the
guest's memory map that the host prints a byte at a time, and a `_write`
override in `libc_shim.c` that routes stdout and stderr into it. The commit
is explicit that this whole investigation was blind until it existed.

**Evidence.** `7bf3f58` (2026-08-17, `history-before-cleanup`), which
records that "this whole investigation was blind until it existed —
`nosys.specs`'s default `_write` silently drops anything printf/I_Error
tries to output."

**Cross-references.** This is the first instance of the pattern in
[Bugs in the tests, not the emulator](#test-bugs): before you can find a
bug you need something that can distinguish "no bug" from "no
information". [Bug 35](#bug35) is the same lesson learned again, three
weeks later, in the differential harness.

---

<a id="bug2"></a>
## 2. IWAD detection went through `fopen`, not through the WAD backend

**Symptom.** With diagnostics working, the guest reported that it could not
find `DOOM1.WAD` — despite the WAD being resident in guest memory and a
purpose-built backend, `w_file_doomv.c`, existing to serve it.

**Root cause.** `d_iwad.c` locates an IWAD by calling `M_FileExists()`,
which goes through `fopen()` and therefore `_open()`. It does not go
through `W_OpenFile` at all. `W_OpenFile` is only reached *after* the IWAD
path has already been resolved — so the custom backend was correctly
implemented and correctly wired and sat entirely downstream of the step
that was failing.

**Why it went unnoticed.** Because the WAD backend was written against the
wrong mental model of DOOM's I/O layering. The assumption was that
doomgeneric's file-class indirection was the single choke point for file
access; it is not, and `M_FileExists` is the counterexample. Nothing about
the backend's own code looks wrong.

**Resolution.** Real `_open`/`_read`/`_close`/`_lseek` were implemented in
`libc_shim.c` — deliberately *scoped*: they succeed only for `IWAD_NAME`
opened read-only. The scoping is the interesting half. An unconditional
"every open succeeds" shim would have fixed the IWAD probe and broken the
config-file and savegame probes, which must fail so DOOM falls back to
defaults.

**Evidence.** `7bf3f58`.

---

<a id="bug3"></a>
## 3. `Z_Malloc` failed a 64KB allocation immediately after a 6MB one succeeded

**Symptom.** DOOM's zone allocator successfully carved out a 6MB zone and
then failed to allocate 64KB.

**Root cause.** Not determinable from outside newlib. The toolchain ships
`libc.a` as compiled objects with no source, so the allocator's internal
state could not be inspected and the failure could not be traced to a line.
The commit says so plainly: "root cause not visible from outside libc.a (no
newlib source shipped, just the compiled `.o`)".

**Why it went unnoticed.** It was noticed immediately — this one announced
itself. What made it hard was that the failing component was opaque.

**Resolution.** Rather than debug a black box, the black box was replaced.
`_sbrk` was reimplemented as a simple bump allocator bounded at `WAD_BASE`,
matching the pattern used by the reference project's `libc_backend.c`. This
is a recurring move in this project and it recurs on purpose: where the
host (or the host's library) is unreliable or opaque, compute the answer
rather than borrow it. See [Bug 53](#bug53) and [Bug 94](#bug94) for the
same decision taken about floating point, twice.

**Evidence.** `7bf3f58`.

---

<a id="bug4"></a>
## 4. The linker script's `.bss` rule did not match `.sbss*`, so the first `malloc` overlapped live globals

**Symptom.** Something between `D_CheckNetGame` and `BuildNewTic` was
corrupting `d_loop.c`'s `loop_interface` global. The corruption surfaced
hundreds of instructions after the write that caused it.

The first hypothesis was a CPU bug — that `exec_32I`'s `SW` handling was
wrong for the exact instruction and operands involved. That was tested in
isolation and found correct, which is what turned the investigation
outward. `7bf3f58` records this explicitly, as an open bug, before it was
root-caused: "verified via isolated unit test that decode+exec_32I's SW
handling is correct for the exact instruction/operands involved, so this is
a genuine memory-corruption bug elsewhere in the trace, not a CPU core
bug".

**Root cause.** `riscv.lds`'s `.bss` output section used `*(.bss*)`. That
is a wildcard on the *name* `.bss` plus a suffix — and `.sbss` is not a
suffix of `.bss`, it is a different name. GCC and newlib emit a great many
small-data `.sbss.*` sections. None of them matched. The linker therefore
orphan-placed them, and orphan placement put them *after* `_end` and
`heap_start` — which is exactly where the heap begins. So the very first
`malloc` returned a pointer into memory that still held live globals, and
writing through it corrupted them.

**Why it went unnoticed.** Three reasons compounding.

First, the symptom is maximally distant from the cause: the corrupting
write is an ordinary heap store executed by correct code, and the damage is
observed later, by a different subsystem, reading a global it has every
right to expect is intact.

Second, the linker does not warn. Orphan section placement is normal
behaviour, not an error; the linker did what it was told and told nobody.

Third, the shape of the bug frames the CPU as the suspect. A global changing
value with no visible store to it is the signature of a broken store
instruction, and that is where the investigation went first.

**Resolution.** `.bss` gained `*(.sbss*)`. In the same change, `*(.sdata*)`
and `*(.srodata*)` were added to `.data` and `.text` respectively —
pre-emptively, before they produced a quieter version of the same bug.
Found by putting a watchpoint on the corrupted global's address and
catching the exact write.

**Evidence.** `ce489d3` (2026-08-17), whose subject is "Fix .bss linker
script gap and resolution mismatch -- Doom boots".

**Cross-references.** This is the earliest instance of the project's most
common failure mode: a *silent wrong answer* rather than a crash. Almost
every bug in [Part III](#part-iii) has the same shape.

---

<a id="bug5"></a>
## 5. `DOOMGENERIC_RESX/RESY` were set by a header nobody included

**Symptom.** A corrupted, sheared title screen. The image was recognisably
DOOM and structurally wrong — a stride mismatch, though it was not
described that way until it was understood.

**Root cause.** `DOOMGENERIC_RESX` and `DOOMGENERIC_RESY` were set with a
`#define` in `doomv_mmio.h`. A `#define` in a header only affects
translation units that include that header. `doomgeneric.c` — which
allocates `DG_ScreenBuffer` — does not include it. `i_video.c` — which
writes into that buffer — does not include it either. Both therefore
compiled against doomgeneric's own 640x400 default, while every other
translation unit in the build had been told 320x200.

So the buffer was allocated at one size and written at another stride, and
the emulator scaled it as a third.

**Why it went unnoticed.** Because it *did* produce output, and the output
was recognisable. A blank screen prompts investigation; a picture of the
DOOM logo that is slightly wrong reads as "the scaler needs work". The
failure was attributed to the wrong layer.

It also went unnoticed because a per-file define has no error case. Every
translation unit compiled cleanly. There is no diagnostic for
"this constant means two different things in two different objects".

**Resolution.** Moved from the header to a global `-D` flag in the guest
Makefile, so every translation unit in the build agrees by construction.
Verified visually: pixel-perfect logo, marine and id Software branding, no
corruption.

**Evidence.** `ce489d3`.

**Cross-references.** [Bug 14](#bug14) is the same class — a constant with
more than one copy — at the emulator level rather than the guest level.

---

<a id="bug6"></a>
## 6. Instruction-to-tick scaling ran simulated time about a thousand times too fast

**Symptom.** The title screen never advanced. Thirty real seconds passed
with the CPU visibly executing new code each frame, the dashboard showing
forward progress, and the game not moving. It looked exactly like a hang,
except that nothing was hung.

**Root cause.** `MMIO_TICK` advanced 1:1 with retired instructions.
`I_GetTime()` computes `ticks * 35 / 1000`. So simulated time ran roughly a
thousand times faster than the real work being done.

The consequence is not "the game runs fast". DOOM's tic logic is a
catch-up loop: `D_PageTicker` and `TryRunTics` process however many tics
they believe have elapsed since the last frame. With simulated time running
away, the backlog was enormous and always growing, so the loop spent every
frame processing tics it thought had already happened and never reached the
present. Externally indistinguishable from a freeze.

**Why it went unnoticed.** Because "frozen" and "processing an unbounded
backlog" look identical from outside, and the obvious diagnostic — is the
CPU still executing? — returns yes in both cases. The dashboard was
actively misleading here: it showed a healthy machine.

**Resolution.** `Memory::INSTR_PER_MS = 1200`, calibrated from the observed
throughput of roughly 20,000 instructions per render at ~60fps, so
simulated time tracks approximately real time given the throughput actually
achieved.

**Evidence.** `eedaf98` (2026-08-17). The verification note is worth
keeping: end-to-end confirmation needed *real* input, via `SendInput`
rather than `PostMessage`, because SDL's Windows keyboard handling requires
genuine OS-level focus and silently ignores synthetic messages without it.
That is a second silent-failure trap encountered while verifying the fix
for the first one.

**Cross-references.** [Bug 21](#bug21) is the same mistake at a different
scale — an instruction-count clock meeting software that assumes wall
time — and it cost far more to diagnose the second time.

---

<a id="bug7"></a>
## 7. `-specs=nosys.specs` passed twice on one command line hard-errored the link

**Symptom.** `attempt to rename spec ... already defined` on every guest
build, surfacing while retargeting the guest to `-march=rv32imac`.

**Root cause.** `CFLAGS` and `LDFLAGS` both carried `-nostartfiles
-specs=nosys.specs`. The guest target is a single combined compile-and-link
invocation — `$(CC) $(CFLAGS) ... $(LDFLAGS)` — so both copies land on one
command line. This toolchain's spec-file `%rename` directives are not
idempotent: applying the same spec file twice attempts to rename a spec
that the first application already renamed, and that is a hard error.

**Why it went unnoticed.** It was pre-existing and had never been triggered
because the guest had not been rebuilt from clean in a while. It surfaced
during an unrelated change and was initially attributed to that change.
Reverting to the old `-march=rv32ima` reproduced it identically, which is
what proved it was never about the arch flag.

**Resolution.** `LDFLAGS` reduced to `-T riscv.lds`, the only thing it was
contributing beyond what `CFLAGS` already had.

**Evidence.** `7025243` (2026-08-23). Verified by rebuilding clean and
confirming via `objdump` that ~52% of the resulting instructions were
2-byte compressed encodings — i.e. that the C extension was genuinely being
exercised by the real DOOM binary rather than only by tests.

---

<a id="bug8"></a>
## 8. Host FPU NaN payloads leaked into guest registers

**Symptom.** Signature mismatches against spike on the F and D
riscv-arch-test families, in the very first run of the very first
arch-test harness.

**Root cause.** F and D arithmetic was computed on the host FPU. When an
operation produces a NaN, the *value* of that NaN — its payload bits — is
not fully specified by IEEE-754 and differs between implementations.
RISC-V specifies a canonical quiet NaN and requires that operations
producing a NaN produce exactly that one. Passing the host's result
straight through leaks whatever payload the host FPU chose into the guest's
`f` registers, where it is architecturally visible.

**Why it went unnoticed.** DOOM does not care. Nothing in a bare-metal
game distinguishes one NaN payload from another; a NaN is a NaN as far as
any code that is not a conformance test is concerned. The bug is only
observable to something that dumps register bits and compares them, which
is precisely what did not exist until the arch-test harness was built.

**Resolution.** Explicit canonicalisation in the shared FP plumbing: a NaN
result is replaced by RISC-V's canonical quiet NaN before being written
back.

**Evidence.** `e35c777` (2026-08-25, `history-before-cleanup`), which
records this as one of "two real correctness bugs surfaced by building a
spike-based riscv-arch-test harness". Also narrated in `aa38824` and
`0b7f866` on `main`.

**Cross-references.** This canonicalisation was eventually *deleted* — see
[Bug 94](#bug94). SoftFloat's default NaN already is RISC-V's canonical
one, so once arithmetic moved into SoftFloat the explicit fix-up became
dead code that could only introduce bugs.

---

<a id="bug9"></a>
## 9. `fcvt.lu.{s,d}` had undefined behaviour above 2^63

**Symptom.** Signature mismatch against spike on float-to-unsigned-long
conversions, same first arch-test run as [Bug 8](#bug8).

**Root cause.** The conversion used `std::llrint()`, which returns a
*signed* `long long`. For inputs at or above 2^63 the correct unsigned
result does not fit in a signed 64-bit integer, so `llrint` overflows —
which in C++ is undefined behaviour, not a defined saturation. The observed
result therefore depended on the compiler, the optimisation level and the
host.

**Why it went unnoticed.** The top of the unsigned range is exactly the
region no real program visits. DOOM never converts a float above 2^63 to an
unsigned long. Conformance tests do, deliberately, because that is what a
conformance test is for.

**Resolution.** The conversion path was rewritten to handle the range above
2^63 explicitly rather than routing it through a signed intermediate.

**Evidence.** `e35c777`.

**Cross-references.** This is the first of four separate bugs in the
float-to-integer conversion path. See [Bug 53](#bug53) (inexact never
reported), [Bug 90](#bug90) (range-checked before rounding) and
[Bug 94](#bug94). Four bugs in one function over six weeks is a signal
about that function, and the eventual response was to stop computing it on
the host.

---

<a id="bug10"></a>
## 10. A freshly linked `riscv_doom.exe` could not launch

**Symptom.** After a genuine clean rebuild, the emulator binary would not
start at all — not a crash inside the program, a failure to load.

**Root cause.** The Makefile was missing static-runtime-linking flags, so
the produced executable had dynamic dependencies on `libstdc++`, `libgcc`
and the UCRT DLLs. Those are present on the build machine's `PATH` in a
development shell and absent in the environment the binary is actually
distributed and run in — only `SDL2.dll` is vendored alongside it.

**Why it went unnoticed.** This is the most instructive part. The binary
used throughout all the prior verification work *predated the problem*. It
had been built before the Makefile drifted, and every test run since had
been exercising that older executable. Nothing rebuilt from clean, so
nothing observed the breakage.

**Resolution.** `libgcc`, `libstdc++` and `libwinpthread` are now statically
linked; SDL2 stays dynamic since its DLL is vendored.

**Evidence.** `fa6102c` (2026-08-25), which is explicit that this was
"surfaced by actually rebuilding end-to-end for the first time this
session (the binary used throughout prior verification predated both
problems)".

**Cross-references.** This is exactly [Bug 98](#bug98), six weeks apart:
the build system quietly hands the harness a binary that is not the one the
source describes, and the test results measure the wrong thing. It happened
twice, in two different ways, and the second time it "cost a wrong
diagnosis here before being spotted".

---

# Part II — Booting Linux: firmware, devices and the silent hangs

This part is where the project stops being an emulator that runs one
program and starts being a machine. The bugs change character accordingly.
DOOM is a cooperative guest: it does what the emulator supports and nothing
else. Firmware and a kernel are adversarial guests — they probe the machine
to discover what it is, and a machine that answers those probes wrongly
does not fail loudly, it gets *quietly lied to* and then behaves according
to the lie.

Almost every bug in this part is of that shape. The emulator does not
crash. The guest does not crash. The guest simply stops, or spins, on the
strength of something it was told.

<a id="bug11"></a>
## 11. `misa` did not report S, so OpenSBI's cold-boot hart lottery was unwinnable

**Symptom.** OpenSBI's `fw_jump.elf` was loaded, started executing, and hart
0 spun forever in `wait_for_coldboot`. No output, no fault.

**Root cause.** `sbi_init()` decides whether a hart is eligible to be the
cold-boot hart by, among other things, calling `misa_extension('S')`. DoomV's
`misa` did not report the S extension, so that check always failed. Every
hart therefore concluded it was not the cold-boot hart, and waited for a
cold-boot hart that would never exist.

**Why it went unnoticed.** There was no earlier point at which anything
read `misa`. DOOM never does. `misa` had been implemented as a derived
value from the extension table, which was correct in structure and wrong in
content, and nothing had ever asked.

**Resolution.** The `misa` derivation was corrected to report S.

**Evidence.** `58ceb82` (2026-08-31), which describes it as "a real `misa`
CSR gap that made OpenSBI's coldboot-hart lottery unwinnable".

---

<a id="bug12"></a>
## 12. OpenSBI pinned to v1.3 because everything newer needs a PIE-capable linker

**Symptom.** Building current OpenSBI fails with `Your linker does not
support creating PIEs`.

**Root cause.** Post-v1.3 OpenSBI unconditionally requires a linker with
RISC-V PIE support. Neither bare-metal toolchain available in this
environment — `riscv-none-elf-gcc` or MSYS2's `riscv64-unknown-elf-gcc` —
has it. A full `riscv64-*-linux-gnu` toolchain reliably does, because Linux
userspace needs it, but building one from source in this environment was
not practical.

**Why this is a bug worth recording.** It is an environment constraint that
determines a version pin, and a version pin that is not written down turns
into a phantom defect the next time somebody bumps it. It also has a direct
consequence elsewhere: OpenSBI v1.3 hardcodes its reported SBI version to
1.0, which is [Bug 18](#bug18) — a bug that would not exist on a newer
OpenSBI.

**Resolution.** Pinned to tag `v1.3`, with the reason documented in
`tools/linux/opensbi/README.md` and restated in the commit that
republishes the firmware setup.

**Evidence.** `252333b` (2026-08-27), `212b7a2` (`main`),
`tools/linux/opensbi/README.md`. The boot model is `fw_jump` rather than
`fw_dynamic` or `fw_payload`, chosen because it is the simplest — a fixed,
non-relocatable image at a known address, which suits an emulator whose
memory map never varies and does not need `fw_dynamic`'s relocation support.

---

<a id="bug13"></a>
## 13. OpenSBI's `bool` typedef collides with GCC 15's C23 default

**Symptom.** OpenSBI v1.3 does not compile under the toolchain's GCC, with
type-mismatch errors around `bool`, `true` and `false`.

**Root cause.** OpenSBI v1.3 is a few-years-old codebase. Its
`sbi_types.h` does `typedef int bool`, `#define true 1`, `#define false 0`.
GCC 15 defaults to C23, in which `bool`, `true` and `false` are real
keywords. The typedef and the defines collide with the language.

**Why it went unnoticed.** It is not a DoomV bug at all — it is an
old-codebase-meets-new-compiler problem that only appears when you build
someone else's pinned source with a toolchain newer than they targeted.
It is recorded here because it is a real obstacle that cost time and
because its *fix* has a property worth keeping.

**Resolution.** `build.sh` `sed`-patches `-std=gnu11` into the submodule's
Makefile and reverts it afterwards via a trap, so the submodule remains
pinned to a clean, unmodified upstream commit. Forcing the older standard
is simpler and more robust than patching every resulting type mismatch
individually.

Separately, `PLATFORM_RISCV_ISA` needs `zifencei` spelled out explicitly:
the default `-march` string this OpenSBI version derives does not include
it, and `sbi_tlb.c` uses `fence.i` directly.

**Evidence.** `252333b`, `212b7a2`, `tools/linux/opensbi/README.md`. The
patch-and-revert-under-a-trap pattern is reused for [Bug 23](#bug23).

---

<a id="bug14"></a>
## 14. The RAM_BASE relocation missed a fourth hardcoded copy and a stale test constant

**Symptom.** After relocating guest RAM to `0x80000000` with a 256MB span —
required to match OpenSBI generic platform's hardcoded, 2MB-aligned
`FW_TEXT_START` — parts of the system continued to behave as though RAM
were somewhere else.

**Root cause.** `RAM_BASE` existed in four places. Three were updated. The
fourth was in `doomv_mmio.h`, on the guest side of the boundary. Separately,
`test2_paging.S` contained an identity-mapped superpage index computed for
the old base, which the relocation invalidated.

**Why it went unnoticed.** A constant duplicated across a language boundary
has no mechanism to keep the copies in step, and no diagnostic when they
diverge. The host side and the guest side each compiled cleanly against
their own value.

**Resolution.** Both remaining copies corrected, and `load_blob` added to
the loader so non-ELF images (the kernel `Image`, the DTB, the initramfs)
can be placed without going through the ELF path.

**Evidence.** `58ceb82`.

**Cross-references.** Structurally identical to [Bug 5](#bug5) — a constant
that means two things in two places — and to
[Bug 82](#bug82)/[Bug 86](#bug86), where a path or a filename is duplicated
rather than a number.

---

<a id="bug15"></a>
## 15. `medlow` cannot address 0x80000000

**Symptom.** After the RAM relocation, the guest build produced
relocation-overflow errors or code that could not reach its own data.

**Root cause.** The RISC-V `medlow` code model assumes all symbols are
addressable within a ±2GB window of address zero, which is to say the
bottom 2GB. `0x80000000` is exactly the boundary: with RAM based there,
`medlow` cannot form the addresses.

**Resolution.** `-mcmodel=medany` added to the guest build.

**Evidence.** `58ceb82`.

---

<a id="bug16"></a>
## 16. `satp` had no WARL semantics, so Linux "detected" Sv57 and ran under it

**Symptom.** The kernel crashed shortly after entry, in early paging setup.

**Root cause.** Linux probes the widest supported paging mode by *writing*
it to `satp` and reading it back — `set_satp_mode()` in
`arch/riscv/mm/init.c` tries Sv57, then Sv48, then Sv39, taking the first
one that sticks. That probe depends entirely on the hart implementing
`satp.MODE` as a WARL field that rejects modes it does not support.

DoomV stored the written value verbatim. So the Sv57 write stuck, the
kernel concluded Sv57 was supported, enabled it, and began executing under
a translation mode `mmu.cpp` does not implement.

**Why it went unnoticed.** Nothing had ever written `satp` with an
unsupported mode. Bare-metal DOOM runs with `satp = 0`; the hand-written
paging tests write Sv39 because that is what they are testing. A
never-exercised WARL field is indistinguishable from a correct one.

More broadly, this is the first appearance of a theme that dominates the
rest of the project: **WARL fields are how software interrogates
hardware.** A stored-verbatim WARL field does not merely fail to constrain
a write — it actively lies about what the machine is. Every WARL bug in
this document has that same character.

**Resolution.** A real WARL `write_satp()` in `ext_zicsr.cpp` that rejects
unsupported MODE values, matching real hardware. Confirmed by breakpoint
bisection that the kernel then correctly falls back to Sv39 and boots
through `relocate_enable_mmu` and into `start_kernel`.

**Evidence.** `a13e551` (2026-08-31). The comment in `ext_zicsr.cpp:304`
still narrates it: DoomV "implements MODE 0 (bare) and 8 (Sv39) --
everything else used to just" be stored.

**Cross-references.** WARL bugs recur throughout: [Bug 44](#bug44),
[Bug 60](#bug60), [Bug 61](#bug61), [Bug 68](#bug68), [Bug 79](#bug79).
By [Part V](#part-v) it had become clear that *all eight* mismatches Sail
found across nineteen suites were WARL fields and nothing else.

---

<a id="bug17"></a>
## 17. The device tree never declared AIA or Sstc, so Linux refused to use them

**Symptom.** Linux booted but silently fell back to SBI-only IPIs. The
IMSIC driver did not attach.

**Root cause.** `tools/dts/doomv.dts`'s `riscv,isa` string did not include
`smaia`, `ssaia` or `sstc`, even though DoomV genuinely implements all
three. Linux trusts the device tree string here, not OpenSBI's own
correctly runtime-detected extension set, so `riscv_isa_extension_available`
returned false and `irq-riscv-intc` declined to attach the IMSIC driver.

**Why it went unnoticed.** Because the fallback works. SBI-based IPIs are a
legitimate path and the kernel takes it without complaint. The machine
implemented a feature, failed to advertise it, and the guest correctly
declined to use it — which produces a working system that is quietly less
capable than the hardware it is running on. There is no error anywhere in
that chain.

**Resolution.** The missing tokens added to the DTS. Confirmed via the boot
log: `providing IPIs using interrupt 1` and `available via sstc extension`.

**Evidence.** `a13e551`.

**Cross-references.** This is the first instance of the
device-tree-understates-the-machine bug. It recurs, larger, as
[Bug 43](#bug43), where the missing tokens are `zba`/`zbb`/`zbs`/`zicond`
and the cost is Linux declining its own optimised string and bitmap
routines. Fixing this one is also what created [Bug 22](#bug22): once the
DT advertises `smaia`, Linux installs a *different* interrupt handler,
which promptly exposed a CSR that had never been implemented.

---

<a id="bug18"></a>
## 18. OpenSBI v1.3 reports SBI 1.0 unconditionally, so Linux's DBCN console never activated

**Symptom.** Completely silent console. Zero output. Not a hang — the
machine was running, and breakpoint bisection could prove it was making
progress — simply nothing printed.

**Root cause.** OpenSBI v1.3 hardcodes `SBI_ECALL_VERSION_MAJOR`/`MINOR` to
1.0 and reports that regardless of what it actually implements. It *does*
implement SBI DBCN (the debug console extension). Linux only attempts the
DBCN console path when the reported SBI spec version is 2.0 or later. So
the kernel saw "SBI 1.0", concluded DBCN was unavailable, and both the
earlycon and hvc drivers declined the path that would have worked.

**Why it went unnoticed.** This is a firmware/kernel version-negotiation
mismatch with no DoomV involvement at all, and its failure mode is the
absence of output — which is indistinguishable from every other reason a
bring-up produces no output. Silence is the least informative symptom
available.

**Resolution.** Rebuild the kernel with `CONFIG_RISCV_SBI_V01=y`, so both
earlycon and the real console fall back to the legacy SBI v0.1
`console_putchar` path, which OpenSBI v1.3 does support.

**Evidence.** `a13e551`, restated in `45279c5` on `main`: "OpenSBI reports
SBI spec v1.0, and Linux only attempts the DBCN console path when the
reported version is 2.0 or later ... Without it the kernel boots with a
completely silent console: not a hang, just no output at all."

**Cross-references.** This bug is a direct consequence of [Bug 12](#bug12) —
the v1.3 pin is forced by the toolchain's missing PIE support, and the
version report is a v1.3 property. Two environment constraints, one
compounding the other. And `CONFIG_RISCV_SBI_V01` alone is still not enough
for a usable console; see [Bug 19](#bug19).

---

<a id="bug19"></a>
## 19. `CONFIG_HVC_RISCV_SBI` is unreachable from defconfig, so the console died partway through boot

**Symptom.** Console output started, ran for some lines, and stopped. Not
silence from the beginning — silence from a point.

**Root cause.** `CONFIG_HVC_RISCV_SBI`'s Kconfig entry depends on
`NONPORTABLE`, which is off by default. A symbol whose dependencies are
unmet is never *offered*, so it does not appear in `.config` as
`# CONFIG_HVC_RISCV_SBI is not set` — it does not appear at all.

Earlycon covers early boot without it. So the observable failure is output
stopping at the moment the kernel hands over from the boot console to the
real console, rather than output never starting.

**Why it went unnoticed.** The absent-versus-disabled distinction is the
whole bug. Grepping `.config` for the symbol returns nothing, which reads
as "not in this kernel version" rather than "gated behind a dependency you
have not enabled". And the partial-output symptom points at whatever the
kernel was doing at the point it went quiet, not at the console
configuration.

**Resolution.** `CONFIG_HVC_RISCV_SBI` enabled explicitly, along with
`NONPORTABLE`. The handover is then visible in the log:

```
printk: legacy console [hvc0] enabled
printk: legacy bootconsole [sbi0] disabled
```

and `rdinit=/bin/sh` reaches an interactive prompt.

**Evidence.** `8cb38df` (`main`), `0075339` (`history-before-cleanup`,
2026-09-04, which documents the hvc0 console config).

---

<a id="bug20"></a>
## 20. `CSR_TIME` was never implemented, so every timer deadline was computed from zero

**Symptom.** Boot hung immediately after `local_irq_enable()` in
`start_kernel`. Found by temporarily instrumenting
`check_and_take_interrupt()` to print every interrupt taken, which showed
the same S-mode timer interrupt (bit 5) firing from the same PC — the
instruction right after `local_irq_enable()` — every single time, forever,
with no forward progress.

**Root cause.** DoomV never implemented `CSR_TIME` (0xC01), the
unprivileged read-only `mtime` alias every real implementation provides.
Unimplemented CSRs fell through to generic, always-zero `csr[]` storage.

Linux's `get_cycles()`/`get_cycles64()`
(`arch/riscv/include/asm/timex.h`) reads that CSR to compute "now" when
arming `stimecmp` (`riscv_clock_next_event` in
`drivers/clocksource/timer-riscv.c`). So every computed deadline was
`0 + delta` — while real `mtime` was already tens of millions of retired
instructions in. The timer interrupt became pending the instant it was
armed, every time.

**Why it went unnoticed.** `mtime` and `mtimecmp` were implemented
correctly, and the hand-written timer test passed: it armed `mtimecmp`
directly from `mtime` and confirmed the interrupt fired and could be
disarmed. That test never read `CSR_TIME`, because the test author knew
where the clock lived. Linux does not; it uses the architectural
unprivileged alias, which is the correct thing for it to do.

This is a general hazard with hand-written tests: they exercise the
interface the implementer had in mind, not the interface the architecture
specifies. See [Bug 82](#bug82) and the note in
`tools/verification/tests/archtest/README.md`: "The differential suites are
hand-written: they cover what I thought to test, which is exactly their
limitation as evidence."

**Resolution.** A real read intercept for `CSR_TIME` returning
`Timer::get_mtime()`.

**Evidence.** `fda9255` (2026-08-31).

---

<a id="bug21"></a>
## 21. `timebase-frequency` of 10MHz made a kernel tick unservicable

**Symptom.** The same interrupt storm as [Bug 20](#bug20), surviving the
fix for it.

**Root cause.** `tools/dts/doomv.dts` declared
`timebase-frequency = 10000000` — a placeholder from an earlier stage,
written before `mtime`'s actual semantics on this machine had this
consequence.

DoomV's `mtime` advances **once per retired instruction**, not by wall
clock. That is a deliberate design choice: it makes guest timing
deterministic and reproducible run to run instead of varying with host
speed. But it means the device tree's `timebase-frequency` is not a
statement about time, it is a statement about *instructions*.

The pinned kernel's `.config` has `CONFIG_HZ=250`. A periodic tick is
therefore `10000000 / 250 = 40000` raw `mtime` units apart — which under
these semantics means 40,000 retired instructions apart. A full generic
kernel tick handler (RCU, scheduler, entropy) legitimately costs more than
40,000 instructions to execute. So by the time any tick handler finished,
`mtime` had already passed the *next* tick's deadline. A permanent,
unrecoverable storm, regardless of how correct the interrupt delivery path
is.

**Why it went unnoticed.** The number was a plausible placeholder and
looked like a description of the machine rather than a constraint on it. It
had also been silently correct for as long as nothing armed a periodic
timer.

The deeper reason is that this is a *systems* bug rather than a code bug —
neither the DTS nor the timer nor the kernel is wrong in isolation. The
constraint only exists at the intersection of an instruction-count clock, a
particular `CONFIG_HZ`, and the cost of the handler.

**Resolution.** Raised to 1GHz, giving 4,000,000 raw units per tick and
comfortable headroom, **with the reasoning documented inline** so the next
person to touch the constant understands the constraint rather than just
the number.

**Evidence.** `fda9255`. After the fix, boot reached real incrementing
kernel timestamps for the first time: `[0.000045] Timer interrupt in S-mode
is available via sstc extension`, `[0.000181] Console: colour dummy device
80x25`, `pid_max`, LSM init, dentry and mount-cache hash tables — all
previously unreachable.

**Cross-references.** [Bug 6](#bug6) is the same collision — an
instruction-count clock meeting software that reasons in wall time — two
weeks earlier and one layer down. The first time it cost a title screen;
the second time it cost a kernel boot.

---

<a id="bug22"></a>
## 22. `mtopi`/`stopi` were missing, so the AIA interrupt handler never dispatched anything

**Symptom.** A silent livelock partway through `kernel_init`. The machine
was still executing. There was no illegal instruction for the debugger to
catch. The console simply went quiet, which looked like a plain hang.

**Root cause.** Once the device tree advertises `smaia`/`ssaia` — which it
does, as of the fix for [Bug 17](#bug17) — Linux's `irq-riscv-intc`
installs `riscv_intc_aia_irq` instead of `riscv_intc_irq`. That handler
dispatches entirely out of `CSR_TOPI`:

```c
while ((topi = csr_read(CSR_TOPI)))
    generic_handle_domain_irq(intc_domain, topi >> TOPI_IID_SHIFT);
```

DoomV implemented the IMSIC's `mtopei`/`stopei` (external-interrupt claim)
but never `mtopi`/`stopi` (0xFB0/0xDB0), the AIA top-interrupt CSRs.
Unimplemented CSRs read as zero, so the `while` condition was false
immediately and the loop body never ran. The interrupt was neither
dispatched nor acknowledged. `STIP` stayed asserted, because only the timer
handler re-arms `stimecmp`, and the hart re-trapped forever.

**Why it went unnoticed.** Two compounding reasons.

The CSR read as zero rather than trapping, because `Registers::csr[]` backs
all 4096 addresses generically. So the missing register was
indistinguishable from a register reporting "no interrupt pending" — which
is a perfectly valid thing for it to say.

And the handler that used it was only installed *because* an earlier bug
was fixed. Before [Bug 17](#bug17), Linux used `riscv_intc_irq`, which does
not read `TOPI`. Fixing the device tree changed which code path the kernel
took, and the new path immediately fell into a hole that had always been
there.

**Resolution.** `mtopi`/`stopi` implemented as read-only, reporting
`{IID[27:16], IPRIO[7:0]}` for the highest-priority interrupt that is both
pending and enabled at the requested level, in AIA default major priority
order (external > software > timer), masked by `mideleg` for the S-level
view.

One detail worth keeping: `IPRIO` is reported as 1, the default when no
priority has been programmed. Linux reads only the IID field, but a zero
`IPRIO` combined with a zero IID would be indistinguishable from "no
interrupt pending", and the loop condition tests the whole register.

**Evidence.** `35b4a7c` (2026-09-04). Found by `-break` bisection against
`vmlinux` symbols: `rest_init` and `kernel_init` were reached but
`run_init_process` was not; `handle_riscv_irq` was reached with
`scause=0x8000000000000005` and `mtime` past `stimecmp`, proving the timer
interrupt itself was delivered correctly, while `riscv_intc_irq` was never
reached — which identified the AIA handler as the one actually installed.

With this fix Linux 6.12 boots through devtmpfs, clocksource switchover,
initramfs unpack and driver init to `Run /bin/sh as init process`.

**Cross-references.** `fbdaf84` later corrects a comment in the source that
had argued `mtopi`/`stopi` "aren't implemented ... not worth the extra
surface right now" — see [Bug 46](#bug46). That comment is one of the
clearest examples of the [design-assumption pattern](#assumptions).

---

<a id="bug23"></a>
## 23. GNU Make truncated OpenSBI's `ar` recipe at 8191 characters, silently

**Symptom.** OpenSBI's build failed at the *firmware link*, several steps
after the actual problem, with:

```
fw_base.S:252: undefined reference to `platform'
```

**Root cause.** Archiving `libplatsbi.a` passes roughly 132 absolute object
paths in a single recipe, about 10.7KB of command line. GNU Make truncates
the recipe when spawning the shell, at cmd.exe's 8191-character limit.

The truncation is silent, and it lands on a clean path boundary. So `ar`
receives a well-formed command with about 100 of the 132 paths, builds a
perfectly valid archive from them, and exits successfully. `platform.o` is
among the ~32 that were dropped.

**Why it went unnoticed.** Every step succeeds. The truncation produces a
valid command; the command produces a valid archive; the archive is simply
incomplete. The first thing that fails is the firmware link, and it fails
with a message about a symbol — which points at the source that references
`platform`, not at the archive step thirty commands earlier that quietly
omitted it.

It is also platform-specific: a no-op on Linux and macOS, where the long
recipe was never a problem, so it appears only on Windows and looks like a
DoomV-specific breakage of somebody else's build.

**Resolution.** Confirmed it was Make and not the shell first — running the
identical 10,715-character command directly in bash archives all 132
objects. So the object list must not travel through the recipe at all.
`$(file >)` has Make write a response file at expansion time, leaving a
short recipe for `ar` to expand itself.

The edit stays a `sed` patch reverted by the existing trap, so the
submodule remains pinned to a clean, unmodified v1.3.

**Evidence.** `d3ccdd5` (2026-09-04).

---

<a id="bug24"></a>
## 24. OpenSBI's build script only worked on the machine it was written on

**Symptom.** `tools/opensbi/build.sh` fails on a second machine with a
confusing compiler-not-found error.

**Root cause.** `TC_BIN` defaulted to an absolute path under one specific
user's home directory. Same for `python3`, which OpenSBI's kconfig needs
via `/usr/bin/env`.

**Resolution.** Honour `TC_BIN` if set, otherwise use whatever
`riscv-none-elf-gcc` is already on `PATH`, and fail with an actionable
message rather than letting the missing tool produce a downstream error.

**Evidence.** `d3ccdd5`. Also `6dbf419` (2026-09-04), which does the same
for PLAN.md's toolchain notes.

---

<a id="bug25"></a>
## 25. `make clean` was broken in one shell or the other, depending on which Makefile

**Symptom.** `make clean` fails, in a way that depends on which shell you
invoked it from and which directory you were in.

**Root cause.** The root Makefile's `clean` used `del`, which fails in Git
Bash. The guest Makefile's used `rm -f`, which fails in cmd and PowerShell.
GNU Make's built-in `$(RM)` is no help — it is hardcoded to `rm -f`.

The underlying mechanism is that Make selects `sh.exe` when one is on
`PATH` and silently falls back to `cmd` otherwise, so the shell a recipe
runs under is a property of the environment rather than of the Makefile.

**Resolution.** Probe for the same `sh` Make itself would select, and pick
the matching deleter. Recipes prefixed with `-` so deleting an
already-absent file is not an error. Verified in both shells, for both
Makefiles.

**Evidence.** `9175c16` (2026-09-04).

---

<a id="bug26"></a>
## 26. `core.autocrlf` broke every POSIX shell script in the tree, invisibly

**Symptom.** The differential harness and the toolchain build scripts die
immediately inside WSL with:

```
set: pipefail: invalid option name
```

**Root cause.** `core.autocrlf=true` converts every file to CRLF on
checkout. That is right for the C++ sources MinGW builds and wrong for
anything a POSIX shell has to execute: bash reads `set -euo pipefail\r`,
treats the carriage return as part of the option name, and the script dies
before doing anything.

**Why it went unnoticed, and why this one is nasty.** It is a
*checkout-dependent* failure. The file in the repository is correct. The
file on disk is wrong. So the bug appears and disappears with no change to
any file, depending on the git configuration of whoever checked it out —
which means it cannot be reproduced by reading the source and cannot be
bisected by looking at history.

**Resolution.** A `.gitattributes` marking those files `eol=lf`, scoped to
this project's own paths. The vendored trees under `tools/*/src` are
submodules and keep whatever their upstream uses.

The commit is careful to state that the rest of it is whitespace only, and
gives the check: `git diff --ignore-all-space <this commit>` is empty.

**Evidence.** `b1eceee` (2026-09-05).

**Cross-references.** The same Windows-checkout-semantics class produces
[Bug 73](#bug73) (a CRLF shebang inside a submodule, which `.gitattributes`
deliberately does not cover) and [Bug 88](#bug88) (symlinks arriving as
text files). Three distinct bugs from one property of the filesystem.

# Part III — Differential testing against spike: the silent wrong answers

This part is the turning point of the project. Everything before it was
verified by observation: DOOM draws pixels, Linux prints lines. Everything
in it was found by taking a signature dump from DoomV and a signature dump
from spike and diffing them byte for byte.

The character of the bugs changes completely, and `f91df1e` names the
change precisely: "Every bug it found was a silent wrong answer rather than
a crash, which is the case a differential test is uniquely good at."

None of these would have been found by running a guest. Several of them
*were* being run by a guest — glibc executes all of them — and the guest
was producing wrong answers and carrying on.

<a id="bug27"></a>
## 27. Unrecognised `funct7` fell through to the base-I meaning: `sh3add` executed as `OR`

**Symptom.** A `SIGSEGV` in a distro-built userspace, hundreds of
instructions after the instruction that caused it, inside glibc's `memset`.

**Root cause.** The R-type decode path only ever inspected `funct7` to tell
`SUB` from `ADD` and `SRA` from `SRL`. Every other `funct7` value fell
through to the base-I meaning of its `funct3`.

So an unimplemented bitmanip operation did not fault. It quietly executed
as something else. `sh3add a4,a4,a5` — which should compute
`(a4 << 3) + a5` — ran as `OR`, returning `a5 | 4`.

In glibc's `memset`, that produced a loop bound the pointer could never
equal. The runaway 8-byte stores walked off the end of the stack VMA and
surfaced as a segmentation fault hundreds of instructions later, with
nothing left pointing at the cause.

**Why it went unnoticed.** Because the decoder's fall-through was
structurally invisible. There is no missing case to notice — the switch has
a default and the default is a legal instruction. An unimplemented
extension therefore behaves like a *different implemented* extension rather
than like an absence.

And RVA23 mandates Zba/Zbb/Zbs, so distro glibc emits them unconditionally
rather than behind a runtime check. There was never going to be a graceful
degradation path; the only options were "implement them" and "compute
plausible wrong numbers".

**Resolution.** Two halves, and the first matters more than the second.

`classify()` now returns `ILLEGAL` for unrecognised `funct7`/`funct6`
across OP, OP-32, OP-IMM and OP-IMM-32. A missing extension now halts on
its first instruction with a crash log instead of computing a plausible
wrong number. That change alone found the next two gaps immediately.

Then Zba, Zbb, Zbs, Zcb and Zicond were implemented. All default on,
unlike V: bare-metal DOOM never emits them, but every distro binary does,
so defaulting them off would just be another way to configure a userspace
into failing.

**Evidence.** `8d790c4` (2026-09-04, `history-before-cleanup`). The comment
survives in `src/riscv_decoder.cpp:102`: "this path used to check funct7
only for SUB/SRA, so an unimplemented Zb* op silently executed as its
base-I funct3 twin (sh3add as OR)".

The principle is restated in `911be26` on `main`, as a design property
rather than a bug fix: "Anything unrecognised is ILLEGAL rather than
falling through to a similar-looking base instruction. That distinction
matters more than it sounds: an unimplemented op that silently aliases onto
another produces a wrong answer that surfaces hundreds of instructions
later, while an illegal one stops on the instruction that caused it."

**Cross-references.** This is the *archetype* of the project's dominant bug
class, and it recurs exactly twice more in the vector unit, where the same
fall-through structure existed and had not been fixed:
[Bug 49](#bug49) (17 Zvbb instructions falling into `exec_v_int`) and
[Bug 52](#bug52) (14 VFUNARY0 widening/narrowing instructions ignored).
`e414423` makes the connection explicitly and revises the reasoning that
had permitted it.

---

<a id="bug28"></a>
## 28. Every mask-producing vector instruction wrote `v0` instead of its named `vd`

**Symptom.** glibc called `abort()` partway through starting `/bin/sh`.

**Root cause.** `set_mask_bit()` hardcoded `regs.write_v(0)`. So every
mask-producing vector instruction wrote its result into `v0` rather than
into the `vd` it names — and clobbered the active mask while doing it.

Eighteen call sites were affected: the eight integer compares,
`vmadc`/`vmsbc`, the six FP compares, the `vm*.mm` mask-logic operations,
and `vmsbf`/`vmsof`/`vmsif`.

Concretely: `vmsne.vv v9, v1, v2` left `v9` all zero. `vmor.mm` then OR-ed
that zero. `vfirst.m` reported -1 (no bits set) instead of 0.

**Why it went unnoticed.** The confusion is architectural, not clerical.
`v0` genuinely is the mask register — it is the register that *consumers*
of a mask read implicitly. It is not the register a mask *producer* writes.
Those are different roles and the encoding names the destination
explicitly.

The commit on `main` states the distinction as the design rule it should
always have been: "Mask-producing instructions write the vd they name. v0
is the register that *consumers* of a mask read implicitly, which is not
the same thing."

And it looked correct whenever `vd` happened to be `v0`, which in
hand-written test code it often is.

In real userspace this is the difference between a working `strlen` and
`memchr` and ones that silently return the wrong answer.

**Resolution.** `set_mask_bit()` takes the destination register.

**Evidence.** `5eff60b` (2026-09-04). This commit also introduces
`tools/vtest/vector/`, the differential harness that found it — the V
extension has no upstream riscv-arch-test suite, so this filled that gap.
One assembly test covering the ~50 distinct vector operations a
distro-built glibc actually emits, harvested with `objdump` over BusyBox,
dumped into the same `begin_signature`/`end_signature` region the arch-test
harness already uses and diffed byte-for-byte against spike.

Result: 47/47 vector tests matching spike, and Linux booting all the way to
an interactive BusyBox prompt.

The comment survives at `src/extensions/ext_v_common.hpp:126`.

---

<a id="bug29"></a>
## 29. The vector load/store `mop` field was extracted one bit too high

**Symptom.** Found on the first run of an extended differential test — the
one added immediately after [Bug 28](#bug28) to cover the addressing modes
the first pass had skipped.

**Root cause.** `mop` — the addressing mode — is instruction bits [27:26].
`funct7` starts at bit 25, so `mop` sits at `funct7` bits [2:1].
`ldst_mop` shifted by 2 instead of 1, reading `{mew, mop[1]}`.

That silently remapped two of the four addressing modes:

| encoding | meaning | read as |
| --- | --- | --- |
| `00` unit-stride | unit-stride | unit-stride (unchanged) |
| `01` indexed-unordered | indexed-unordered | **unit-stride** |
| `10` strided | strided | **indexed** |
| `11` indexed-ordered | indexed-ordered | indexed-ordered (unchanged) |

So `vlse`/`vsse` used the stride *register number* as if it were an index
vector, and `vluxei`/`vsuxei` ignored their index vector entirely and
walked memory sequentially.

**Why it went unnoticed.** Two of the four modes survive the mangling
unchanged — and they are the two common ones. Unit-stride is what
compilers emit by default; indexed-ordered is the other one that happens to
be a fixed point of the transformation. So the code paths that get
exercised most work, and the two that break do so by reading the wrong
addresses rather than by trapping.

The commit that introduces vector load/store on `main` calls this out as a
standing hazard rather than as history: "mop (the addressing mode) is
instruction bits[27:26], which is funct7 bits[2:1] -- easy to shift by one
bit too many, and the failure is quiet: strided silently becomes indexed,
so the stride *register number* gets used as an index vector."

**Resolution.** Shift corrected to 1. The differential test was extended in
the same commit to the vector operations BusyBox uses that the first pass
skipped: strided and indexed loads, a segment store/load round trip
(`vsseg4e8`/`vlseg4e8`), a gather whose index vector is a different EEW
than its data (`vrgatherei16.vv`), fault-only-first with nothing faulting,
and `vzext.vf8` at LMUL=8 where the source EMUL works out to 1 and the
encoding is finally legal.

That extension is what surfaced the bug, on its first run.

**Evidence.** `5d89782` (2026-09-04). 58/58 vector tests matching spike
afterwards. Comment at `src/extensions/ext_v_common.hpp:159`.

**Cross-references.** This bug is why every subsequent encoding in the
project was taken from the assembler rather than from memory.
`7bf217a` states the resulting policy: "Every encoding here came out of the
assembler rather than out of memory -- this project has found enough bugs
that were a mis-shifted field to stop trusting a remembered opcode table."
See also [Bug 50](#bug50), where two Zvbb encodings would have been got
wrong by a remembered table.

---

<a id="bug30"></a>
## 30. `mstatus.VS` was not enforced, and vector state was never marked Dirty

**Symptom.** The V differential test ran fine under DoomV and trapped on
its first `vsetivli` under spike.

That is the wrong way round from how a bug usually presents, and it is
worth dwelling on: DoomV was *more permissive* than the architecture, so
the test was written against the permissive behaviour, and the reference
was the thing that appeared to be broken.

**Root cause.** DoomV executed vector instructions regardless of
`mstatus.VS`. Real hardware and spike both trap those as illegal until
software turns the unit on.

The second half is the one with teeth: executing a vector instruction must
set `VS` to Dirty. Linux decides whether a task has live vector state to
save on a context switch by checking whether `VS` reads Dirty. A hart that
never sets it lets the kernel skip saving registers that really are live —
so vector state leaks between tasks, and surfaces as impossible-looking
corruption long after the fact.

**Why it went unnoticed.** Nothing had ever left the unit off and then used
it, because the only guests were DOOM (which contains no vector
instructions at all) and a kernel booted by OpenSBI, which sets
`mstatus.VS` itself during hart init whenever `misa` reports V
(`lib/sbi/sbi_hart.c`). Firmware enables the unit before Linux runs, so
the enable was always already on.

**Resolution.** `VS` Off makes every vector instruction illegal; executing
one sets `VS` to Dirty.

The placement is the interesting part: **the check deliberately sits after
the decode cache.** `enabled` is cached per (address, encoding), and `VS`
is runtime state. Caching it would freeze whatever the vector unit's enable
happened to be the first time an address executed.

**Evidence.** `1f8bec0` (2026-09-04). The commit records an expectation
that turned out wrong — that this would break the Linux boot, since the DTS
does not advertise `v` and so the kernel would never enable the unit for
userspace. It does not, because OpenSBI enables it first, and the commit
records *why* rather than just that it passed.

Verified three ways: a vector instruction with `VS=Off` halts as illegal;
all 128 differential tests still match spike (they enable `VS` explicitly);
DOOM and the boot to an interactive shell both unaffected.

Comment at `src/extensions/ext_xstate.hpp:20`.

---

<a id="bug31"></a>
## 31. `mstatus.FS` had the same gap on the floating-point side

**Symptom.** None observed. This one was found by looking, not by failing —
having closed the `VS` gap, the obvious question was whether `FS` had it
too. It did.

**Root cause.** Identical to [Bug 30](#bug30): FP instructions executed
regardless of `mstatus.FS`, and `FS` was never marked Dirty for a
supervisor to notice live FP state at context-switch time.

**Why it went unnoticed.** Same reason, and one more: DOOM's guest is built
`-march=rv64imac_zicsr` and its ELF contains zero FP instructions. Its
`start.S` never touches `mstatus`, so `FS` genuinely is Off there — it
simply never executes anything that cares.

**Resolution.** Same treatment. The helpers moved to a new
`ext_xstate.hpp` rather than staying in the vector header, since `FS` has
nothing to do with the V extension and both fields are checked in the same
place in the decoder.

The commit is explicit that this was the riskier of the two and was checked
before anything was changed, on the reasoning that "enforcing an enable can
only break things that were relying on not having one". Neither consumer
was: DOOM contains no FP instructions, and OpenSBI sets `FS` from `misa`
exactly as it does `VS`.

**Evidence.** `e86aa01` (2026-09-04).

One incidental C++ trap recorded in the same commit, because it cost time:
the include has to sit at file scope in `ext_v_common.hpp`, not inside
`namespace vcommon`, or the helpers end up as `vcommon::vcommon::`.

---

<a id="bug32"></a>
## 32. Fault-only-first loads were executed as plain unit-stride loads

**Symptom.** With Sv39 paging and a real userspace, glibc's `strlen` took a
page fault in code that was behaving correctly.

**Root cause.** `vle<eew>ff.v` was treated as a plain unit-stride load. Its
entire purpose is the opposite: a fault on any element after the first must
*not* trap, but instead trim `vl` to that element's index and complete the
instruction, leaving what was already loaded in place.

That is what lets `strlen`-style code read a whole vector of bytes without
first knowing whether the tail crosses into an unmapped page. And it is
exactly what glibc's `strlen` does.

Treating it as a normal load means such a read takes a real page fault
where the hardware would have quietly shrunk `vl`.

**Why it went unnoticed.** With no MMU it is harmless — with `satp = 0`
every address translates and nothing can fault, so a fault-only-first load
and a plain load are observationally identical. The bug only becomes real
once there is paging *and* a guest that relies on the semantics.

The commit is candid about why it had been left undone, and the reason is
an architectural one rather than laziness: `translate_or_trap` cannot
express "would this fault?" — it takes the trap as a side effect of asking.

**Resolution.** `mmu_translate`, which `translate_or_trap` wraps, already
*reports* the fault instead of entering it. So the fault-only-first path
calls that directly for elements past the first. Element 0 still faults
normally.

This split is described on `main` as a deliberate design property rather
than a workaround: "mmu_translate reports a fault rather than taking it,
and translate_or_trap wraps it for callers that just want the trap. That
split is not decoration: fault-only-first needs to ask 'would this fault?'
without the asking being the fault."

**Evidence.** `3732a60` (2026-09-05), and `4a72670`/`9959089` on `main`.

The commit is explicit about the limits of its own verification: only the
non-faulting case is covered by the vector suite, because making an access
fault on purpose needs Sv39 paging set up inside the test binary. So the
faulting path was verified only indirectly, by Linux continuing to boot —
"worth being explicit about rather than implying the tests cover it".

That gap was closed the next day by `799b7f3`, which builds a page table,
drops to S-mode, and runs a fault-only-first load off the end of the mapped
region on purpose. One 2MB megapage covers `0x80000000`–`0x801fffff` and
nothing above it, so a load starting at `0x801ffff8` has its first 8
elements mapped and element 8 onwards in a hole. Results matching spike
byte for byte:

| observation | value |
| --- | --- |
| `vl` before | 16 (VLMAX for e8/m1) |
| `vl` after crossing | 8 — trimmed, no trap |
| `vl` wholly inside page | 16 — untrimmed |
| bytes loaded | `0x0102030405060708` |

The third line is what makes the second mean anything: without it, "vl
becomes 8" is equally consistent with an implementation that trims on every
fault-only-first load. The fourth confirms it loaded the right eight bytes
rather than merely stopping at the right index.

---

<a id="bug33"></a>
## 33. A faulting vector access left `vstart` at 0, so it was not restartable

**Symptom.** None observed directly. Found by asking what a supervisor
would do with a faulting vector load, and checking.

**Root cause.** A vector load or store that faulted partway through stopped
touching memory but left `vstart` at 0. So the instruction was not
restartable: a supervisor that mapped the missing page and re-executed it
would redo *every* element from the beginning, repeating the accesses that
had already succeeded.

**Why it went unnoticed.** Most of the semantics were already there —
`for_each_active` starts the element loop at `vstart` and resets it to 0 on
normal completion. The missing piece was recording the element that
faulted, and that has to happen *after* the loop, since the helper's own
reset would otherwise wipe it. So the code looked complete and the one
place the value needed to be written was in the one place the existing
structure did not reach.

And nothing exercised it. Restartability is only observable to a supervisor
that actually restarts the instruction, which nothing did.

**Resolution.** The faulting element index is recorded into `vstart` after
the loop.

**Evidence.** `4b506de` (2026-09-05), which describes this as "the last
known spec deviation on the list".

Verified directly rather than by inspection: `vtest_trap` runs a plain
(non-FOF) `vle8.v` across the end of the mapped page, and the handler
records `scause`, `stval` and `vstart` for every trap it takes. The
faulting load reports `scause` 13, `stval` `0x80200000`, `vstart` 8 — the
exact element it stopped on — matching spike. The other five traps report
`vstart` 0, which is what makes the 8 meaningful rather than coincidental.

**Cross-references.** The commit is careful that this proves a weaker claim
than it appears to: it shows the value is *written*, not that re-executing
resumes from there. Those are different claims. `d5197a8` closes the gap
the following day with a handler that maps the missing page and returns
without advancing `sepc`, so the same instruction runs a second time.

The data is what makes that one conclusive: `v1[0..7]` holds
`0102030405060708`, loaded before the fault and never revisited by the
restarted execution; `v1[8..15]` holds `1112131415161718`, from the page
that did not exist on the first pass. One fault, `vstart` 8 during it and 0
after. That result is only reachable if both halves ran, in order, across
two executions of one instruction.

`f91df1e` summarises why both tests exist: "Those are different claims and
only the second means the instruction is genuinely restartable." Writing
that pair of tests then produced two more bugs — [Bug 38](#bug38) and
[Bug 39](#bug39) — both in the test rather than the emulator, and both
things a real supervisor has to get right.

---

<a id="bug34"></a>
## 34. `fmin(+0,-0)` returned the wrong zero

**Symptom.** Signature mismatch against spike.

**Root cause.** `fmin`/`fmax` delegated to `std::fmin`/`std::fmax`. For
zeros of opposite sign, the C library functions may return *either*
operand — the standard does not pin it, because for most purposes `+0` and
`-0` are equal. RISC-V pins it: `fmin(+0,-0)` is `-0.0` and `fmax` is
`+0.0`.

**Why it went unnoticed.** `+0 == -0` compares true, so any test written
in terms of numeric equality passes regardless. The difference is only
visible in the bit pattern, which is only visible to something that dumps
registers and compares them.

**Resolution.** `fmin`/`fmax` handle the signed-zero case explicitly rather
than delegating.

**Evidence.** Recorded in `f91df1e` (`main`) as one of the bugs the
differential suite found — "fmin returning +0.0 where the spec requires
-0.0" — and stated as a design constraint in `0b7f866`: "fmin/fmax cannot
delegate to std::fmin/std::fmax: for zeros of opposite sign those may
return either operand, while RISC-V pins fmin(+0,-0) to -0.0 and fmax to
+0.0."

**Cross-references.** This is the second of what eventually became a long
list of ways the host FPU is not the RISC-V FPU. See [Bug 8](#bug8),
[Bug 9](#bug9), [Bug 48](#bug48), [Bug 53](#bug53), [Bug 89](#bug89),
[Bug 90](#bug90), [Bug 91](#bug91), and finally [Bug 94](#bug94), which
stops using it.

---

<a id="bug35"></a>
## 35. `compare.py` reported "MATCH: all 0 words identical" on an empty dump

**Symptom.** The harness announced a pass for a run that had produced no
signature at all.

**Root cause.** `compare.py` compared two lists element-wise and reported
the number of matching words. Two empty lists have zero mismatches. So a
run in which the emulator never reached the signature-dump point, or
crashed, or was never started, reported as a clean match — and reported the
count as evidence.

**Why this is the worst possible failure.** Every other bug in this
document produces a wrong answer. This one produces a *right* answer for
the wrong reason, and it does so exactly in the case where something has
gone badly wrong. It converts a total failure into a green result.

**Resolution.** Empty dumps report `NO DATA` and exit 2. A length mismatch
fails even when the common prefix agrees.

**Evidence.** `3778b72` (2026-09-05): "compare.py reported 'MATCH: all 0
words identical' when a run produced no signature at all, which is the most
misleading thing it could do."

**Cross-references.** This guard then earned its keep three separate times,
each recorded:

- `d5197a8`, two commits later — a recursive fault in a test handler
  produced nothing from *both* simulators, "reported as NO DATA rather than
  a match, which is the compare.py fix from two commits ago earning its
  keep."
- `f4d66aa` (RVA23 phase 5) — a test case for `vfncvt.rod.f.f.w`, which
  Zvfhmin does not permit, made spike trap with no handler installed and
  spin. "The harness reported NO DATA rather than a false match — the guard
  added earlier after compare.py once announced 'all 0 words identical' on
  empty dumps."
- `7991e11` (RVA23 phase 7) — spike exiting immediately on an unrecognised
  ISA name, leaving an empty signature. This one prompted a *further*
  guard: `spike_sig.sh` now checks for spike failing to start and reports
  its own words, which "turns that diagnosis into one line". See
  [Bug 57](#bug57).

The lesson generalises: a harness must distinguish "no difference" from "no
information". This is the same lesson as [Bug 1](#bug1), where the guest
had no output channel at all.

---

<a id="bug36"></a>
## 36. spike's `+signature` plusarg silently writes nothing in this build

**Symptom.** No signature file, no error, exit status 0.

**Root cause.** spike's `+signature` plusarg never populates `sig_file` in
this build. `htif_t::stop` then writes nothing, silently, while still
exiting successfully.

**Why it matters.** It is a reference model failing silently, which is the
one component a differential harness cannot afford to have fail silently.
Combined with [Bug 35](#bug35) — which had not yet been fixed at this point
— it produces a green "all 0 words identical" result from a run that did
nothing.

**Resolution.** The dump goes through spike's `-d` debug mode with
`--debug-cmd` instead.

Two further constraints on driving spike this way, both found the hard way
and both recorded in the harness:

- Commands must come from `--debug-cmd` and not stdin, where every line is
  treated as "step one instruction".
- The HTIF `tohost` write terminates spike on the spot, so the signature
  region has to be read at a label placed *before* it, not at the DoomV
  spin loop after it.

**Evidence.** `5eff60b` (2026-09-04), which records all three. `537e6ba`
later replaced `run_vtest_v.sh`, "which was written against spike's
+signature plusarg before that turned out not to work in this build".

**Cross-references.** The eventual answer to this whole class of problem
appears in [Bug 74](#bug74): Sail has a `--test-signature` option that does
exactly this job, and an earlier version of `sail_sig.sh` hand-rolled a
memory dump and got the file naming wrong. "Reading the option list would
have been faster than writing the parser."

---

<a id="bug37"></a>
## 37. A test's unaligned `ld` trapped before the MMU, then spun forever in M-mode

**Symptom.** Under spike's debug mode: *no output at all*. Not a mismatch,
not an error — nothing.

**Root cause.** A trap-delegation test wanted to check that a fault partway
into a page reports the exact address rather than the page base. The
address chosen for that was not 8-byte aligned.

An unaligned `ld` raises misaligned-access *before* the MMU is consulted.
That exception was not delegated, so it went to M-mode — where `mtvec` was
still zero. Jumping to address zero produced an instruction-access fault,
which also went to `mtvec`, which is zero. The machine spun in
instruction-access faults forever.

In spike's debug mode that shows up as no output whatsoever, because
`until` is silent and never matched.

**Why it went unnoticed.** It was noticed immediately; it just pointed
nowhere. A total absence of output from a debug-mode run gives no
information about which of the many things that could have gone wrong did.

**Resolution.** The mid-page test address kept 8-byte aligned, so the test
measures the thing it is about. And `mtvec` now points at a handler that
ends the run rather than being left at zero — so an undelegated trap
produces a terminated run with a diagnosis instead of a silent spin.

**Evidence.** `3778b72` (2026-09-05). Both constraints are now recorded in
the test itself rather than only in the commit.

---

<a id="bug38"></a>
## 38. A trap handler used `t1` — the faulting instruction's own base register

**Symptom.** The restart test's element 8 came back holding a page-table
entry.

**Root cause.** The handler for the restart test used `t1` as scratch. `t1`
is the faulting instruction's base register. On `sret` the load re-executed
against the handler's value, so it read from wherever the handler had left
`t1` pointing — which was the page-table entry the handler had just
written.

**Why this belongs in a bug document about an emulator.** Because it is not
an emulator bug and it is exactly the thing the emulator is for. A restart
only works if the interrupted context survives, which is why real handlers
save registers before doing anything. The test was written as a supervisor
would have to be written, and got it wrong in the way a real supervisor
would.

**Resolution.** The handler saves and restores what it uses. Kept in the
file with a comment rather than quietly fixed, "because both are things a
real supervisor has to get right".

**Evidence.** `d5197a8` (2026-09-05).

---

<a id="bug39"></a>
## 39. `sp` pointed outside the pages the test mapped, so the handler faulted recursively

**Symptom.** Both simulators produced nothing at all.

**Root cause.** `sp` still pointed at `__stacktop`, the top of the 256MB
the linker script describes, while the paging the test sets up covers only
the two 2MB pages it maps. M-mode never noticed, because translation is off
there. The moment the handler pushed a register in S-mode it faulted —
and the fault handler is the thing that faulted, so it recursed forever.

**Why it went unnoticed.** Because the same code works in M-mode, where the
whole point of the test — translation — is switched off. The test sets up
translation, drops privilege, and only then does the stack pointer become a
problem, at which point the failure is in the handler rather than in the
code under test.

**Resolution.** `sp` moved inside the mapped region.

**Evidence.** `d5197a8`. This is the case that made [Bug 35](#bug35)'s
guard pay for itself: both simulators produced nothing, and the harness
correctly said `NO DATA` instead of `MATCH`.

---

<a id="bug40"></a>
## 40. An illegal instruction halted the debugger permanently, killing a guest that was behaving correctly

**Symptom.** A guest that executes an illegal instruction and has a handler
for it — which is to say, Linux — is killed by the emulator instead of
handling it.

**Root cause.** An illegal instruction halted the debugger permanently.

That was the *right* decision when it was made, and the commit says so:
nothing on this CPU had a trap handler to land in, so routing an illegal
instruction through `enter_trap` would just have spun re-trapping forever,
and freezing at the offending instruction was strictly more useful for
debugging.

The decision stopped being right when a real OS started running. Linux
expects to catch `SIGILL`. A permanent halt turns a guest that is behaving
correctly into a dead machine.

**Why it went unnoticed.** It was not unnoticed — it was a deliberate,
documented choice whose premise expired. This is the
[design-assumption pattern](#assumptions) in its purest form: the reasoning
was sound, was written down, and became wrong when the world changed
underneath it. Nothing about the code drifted; the environment did.

**Resolution.** It now does both. The machine freezes at the instruction,
so its state can still be read straight off the dashboard, and F9 then
delivers the trap the guest would really have taken: cause 2, `tval` = the
instruction encoding, `epc` = the faulting pc — which is still correct
because a rejected encoding is never executed, so nothing advanced pc.

A guest with a handler carries on. A bare-metal guest without one is still
caught at the exact instruction rather than vanishing into a trap loop.

The threading detail is worth keeping: registers belong to the CPU thread,
so the input thread cannot deliver the trap itself. F9 sets an atomic flag
that the CPU thread consumes, preserving the existing ownership split — the
two threads share only the snapshot, the key queue, and now this one flag.

**Evidence.** `6e39820` (2026-09-05), restated as a design property in
`2f75559` on `main`.

The commit is honest about what is not covered: "The F9 keypress itself is
not covered -- there is no way to inject an SDL event from the test
harness, so that path rests on inspection rather than a test."

---

<a id="bug41"></a>
## 41. The breakpoint skip was disarmed on first use, and `should_halt()` is called twice

**Symptom.** A breakpoint became impossible to get past. Resuming from it
re-froze immediately, every time.

**Root cause.** Resuming from a breakpoint needs the breakpoint ignored
while `pc` still sits on it, or it re-fires immediately. The first
implementation disarmed that skip on first use.

`DoomSystem::step()` calls `should_halt()` **twice** for the same
instruction — once before the fetch and once after the decode. So the first
call spent the skip and the second re-froze.

**Why it went unnoticed.** It was found immediately, in the same commit
that introduced it. It is recorded because the underlying fact — that a
policy hook is called more than once per instruction — is exactly the kind
of thing that makes a "do this once" flag wrong in a way that reads
correctly.

**Resolution.** The skip disarms when `pc` actually changes, rather than on
first use.

**Evidence.** `6e39820`.

---

<a id="bug42"></a>
## 42. Misaligned scalar accesses are a legal choice, not a conformance property

**Symptom.** Written as a differential test, misaligned accesses showed
DoomV reporting cause 9 where spike reported 4 — plus six later lines
wrong, from the handler being skipped and every subsequent result shifting
by one trap.

**Root cause.** There is no bug. The specification permits an
implementation either to support misaligned accesses natively or to trap.
DoomV does the former, spike the latter. Both are conformant.

Linux agrees with DoomV: it probes at boot and reports "unaligned accesses
are fast".

**Why this is in a bug document.** Because a one-mismatch difference
cascaded into seven wrong lines, and seven wrong lines looks like a serious
defect. The failure mode of comparing against a reference on an *open
choice* is not a single benign disagreement — it is a disagreement that
desynchronises everything downstream of it and produces a large, alarming
diff.

Diagnosing that correctly is the skill this project kept having to
exercise: distinguishing "we are wrong" from "we chose differently".

**Resolution.** Misaligned accesses are deliberately not compared. The
commit states the reason as a rule: "A differential test there compares a
choice, not a correctness property."

**Evidence.** `3778b72` (2026-09-05).

**Cross-references.** This is the first appearance of what became the
project's [taxonomy for reference disagreements](#taxonomy). It is category
2 — an open architectural choice — and the correct response is to stop
diffing it and test the closed property instead. See [Bug 76](#bug76)
(VLEN), [Bug 77](#bug77) (GEILEN) and [Bug 79](#bug79) (which legal value a
WARL field substitutes) for the same judgement applied later, and
[Bug 109](#bug109) for a case where the two permitted answers turned out to
matter after all.

# Part IV — RVA23 conformance: the extensions, and the gaps they exposed

This part is the RVA23 phase plan: eight phases adding the extensions the
profile mandates, each verified against spike before the next began.

The pattern that dominates it is not "extension was missing". It is
**adding an extension exposed a defect in something that had been
implemented for weeks**. Phase 4 found a base-ISA NaN bug. Phase 5 found
three pre-existing gaps wider than the phase itself. Phase 8a found a
privilege-check bug that had been latent since phase 2 and unreachable
until VS-mode CSR redirection existed.

That is the argument for implementing an extension you have no consumer
for: not because the extension matters, but because implementing it walks
code paths nothing else walks.

<a id="bug43"></a>
## 43. The device tree described a smaller machine than the one it runs on

**Symptom.** None visible. Linux booted, ran, worked — and declined to use
four extensions the hardware implements.

**Root cause.** `doomv.dts` advertised:

```
rv64imafdc_zicsr_zifencei_smaia_ssaia_sstc
```

which does not mention Zba, Zbb, Zbs or Zicond — all implemented and
differentially tested for some time by then. Linux picks its optimised
string and bitmap routines from exactly that list at boot, so it had been
declining them.

**Why it went unnoticed.** Because the failure mode is *a working system
that is quietly slower and less exercised than it should be*. There is no
error. There is no wrong answer. The kernel does what it was told the
machine can do, and the machine can do more.

This is the second instance of the same bug — [Bug 17](#bug17) was the
first, with `smaia`/`ssaia`/`sstc` missing — and the reason it recurred is
that the device tree is not derived from the extension table. It is a
separate hand-maintained description of the same fact, and nothing keeps
the two in step.

**Resolution.** The missing tokens added, plus the
`riscv,isa-base`/`riscv,isa-extensions` pair Linux 6.12 prefers (it only
falls back to parsing the string), plus the `cbom`/`cbop` block-size
properties Linux requires before it will use the cache-block instructions
at all.

Vector is deliberately *not* advertised: it is real but opt-in at runtime,
and a device tree that promises vectors to a kernel booted without them
produces illegal instructions inside the kernel's own routines.

**Evidence.** `7bf217a` (2026-09-05, RVA23 phase 1).

The verification here is the interesting part, because "the DT now lists
more things" is not evidence of anything. The commit proves the DT is
genuinely being consumed by booting the *same DTB* on a CPU without Zbb
(via an `-march` lacking it):

> Linux still boots to a shell, and now demonstrably uses the new DT:
> booting the same DTB on a CPU without Zbb (-march lacking it) dies at
> "riscv: ELF capabilities", where the kernel first calls a Zbb-patched
> string routine, instead of reaching /bin/sh

That is a negative control: the DT change is only meaningful if removing
the underlying capability now breaks something, and it does — the boot dies
at 81 lines instead of reaching a shell at 236.

**Cross-references.** The same commit corrects a comment in `mmu.cpp` that
described faulting on a clear A/D bit rather than setting it in hardware as
a "Known gap, to be revisited". It is not a gap — it is Svade, one of the
two behaviours RVA23 permits, and the one Linux accommodates by pre-setting
A/D and re-walking on fault. A correct implementation had been
mis-documented as a defect, which is the mirror image of
[the design-assumption pattern](#assumptions).

---

<a id="bug44"></a>
## 44. `MOP.R.n` and `MOP.RR.n` must write zero to `rd`, not leave it alone

**Symptom.** None. This is a bug that was *avoided* rather than found,
and it is recorded because the commit identifies precisely why it would
have been invisible.

**Root cause (potential).** Zimop and Zcmop define "may-be-operations":
encodings reserved for some future extension to claim, which until claimed
must retire with a defined effect. That effect is to write **zero** to
`rd`.

Leaving `rd` untouched would look identical in every test that only asks
whether anything broke — nothing crashes, nothing traps, the program
continues. And it destroys the entire point of the encoding: software is
allowed to rely on reading a zero back, which is what makes these encodings
safe for a future extension to claim.

**Why it would have gone unnoticed.** Because "retires without
architectural effect" and "writes zero to rd" are easy to conflate, and
five of the six extensions in that phase genuinely are the former. PAUSE is
a FENCE. The `ntl.*` hints are `C.ADD` into `x0`. `prefetch.*` is `ORI`
into `x0`. All five were already retiring correctly before they had names.
Zimop and Zcmop are the one member of the group with a rule that can be
got wrong.

**Resolution.** Implemented to write zero.

The gating direction is the other detail worth keeping, because two of
these extensions gate in *opposite* directions depending on whose encoding
space they borrow. `prefetch.*` and `ntl.*` sit on encodings base I and RVC
already define as reserved HINTs, so `classify()` only claims them when the
extension is on — turning it off must leave a legal result-discarding
instruction behind, not an illegal one. Zicbom is the other way round:
MISC-MEM `funct3=010` is empty in base I, so disabling it genuinely makes
`cbo.*` trap.

**Evidence.** `7bf217a`.

The test for this group needed a different shape from every other suite,
because there is no result to diff. It loads sentinels into every register
a mis-decode could clobber, runs the hints, and dumps the register file —
which catches the failure that actually threatens this group, a hint
decoding as its host instruction with a live destination. Confirmed to
discriminate: with the six extensions disabled, 10 of its 18 checks fail.

---

<a id="bug45"></a>
## 45. CSR privilege enforcement was entirely absent

**Symptom.** None, for a long time. Every mode could read *and write* every
CSR.

**Root cause.** No privilege check existed on CSR access at all. The three
rules the CSR address encoding already states were simply not implemented:

- `csr[11:10] == 11` marks a read-only register — writing one is illegal.
- `csr[9:8]` is the lowest privilege that may touch it.
- The unprivileged counters are further gated by the
  `mcounteren`/`scounteren` chain.

**Why it went unnoticed.** The comment in `ext_zicsr.cpp:368` states it
exactly: "That is invisible while only M-mode firmware runs, and becomes
very visible the moment a guest kernel deliberately probes a CSR expecting
a trap."

M-mode may touch everything, so a machine that never checks is
indistinguishable from a correct one for as long as only M-mode code runs.
And the Linux boot passes through S-mode without ever deliberately
attempting something it expects to be denied.

It also made every test of the counters *vacuous*: `mcounteren` cannot deny
anything to a machine that never checks. So the counter work in the same
phase could not be verified at all until this was fixed first, which is why
the two landed together.

**Resolution.** `RiscvCore::csr_access_permitted()` in `ext_zicsr.cpp`
enforces all three.

One subtlety that had to be got right: **whether an instruction writes has
to be decided before the check, not after.** `CSRRS`/`CSRRC` with `rs1==0`
— the `csrr` pseudo-instruction — is a read despite being encoded as a
read-modify-write. Deciding otherwise would make every `csrr` of a
read-only counter trap.

**Deliberately not added**, and this is a design decision rather than an
omission: trapping on CSR numbers this machine gives no meaning to.
`Registers::csr[]` backs all 4096 addresses generically, and OpenSBI
detects hart features precisely by reading a spread of CSRs and seeing
which ones trap (`sbi_hart.c`, `hart_detect_features`). Making unknown CSRs
illegal is a much larger behaviour change than making privilege boundaries
real, and belongs in its own step.

**Evidence.** `fbdaf84` (2026-09-05, RVA23 phase 2).

The verification is careful, because this is the change in that commit most
likely to have broken the Linux boot:

> Linux still boots to a shell with CSR enforcement active, which is the
> part of this commit most likely to have broken it. OpenSBI writes
> mcounteren=-1 and scounteren=7 before dropping to S (sbi_hart.c:63) --
> checked in its source rather than assumed, and its own priv-version probe
> still works because unknown CSRs stay readable.

Checking OpenSBI's source rather than assuming its behaviour is a habit
that recurs — see also `e60ec1c`, which checks that OpenSBI sets
`menvcfg.PBMTE` unconditionally on RV64 (`sbi_hart.c:142`) before
advertising Svpbmt in the device tree, and `1f8bec0`, which explains an
unexpectedly passing boot by finding that OpenSBI sets `mstatus.VS` itself.

**Cross-references.** Enforcing this created [Bug 59](#bug59) — a latent
misreading of `csr[9:8]` that could not be reached until VS-mode CSR
redirection existed three phases later.

---

<a id="bug46"></a>
## 46. The comment arguing `mtopi`/`stopi` were not worth the surface was exactly backwards

**Symptom.** Already fixed by then — this is the *comment* being corrected
after the fact, not the code.

**Root cause.** The source had carried a comment claiming `mtopi`/`stopi`
"aren't implemented ... not worth the extra surface right now".

That reasoning was exactly backwards, as [Bug 22](#bug22) had already
proved at considerable cost: a device tree advertising `smaia`/`ssaia`
makes Linux dispatch and acknowledge interrupts *solely* through a
`csr_read` of TOPI. Returning zero was not a missing convenience. It was a
silent livelock.

**Why this is recorded as a bug.** Because a wrong comment is a bug with a
long half-life. The code was fixed on 2026-09-04 in `35b4a7c`; the comment
justifying its absence survived until `fbdaf84` on 2026-09-05. Anyone
reading the file in between would have found a confident argument for not
doing the thing that had just been proved necessary.

**Resolution.** Comment corrected. It survives, rewritten, at
`src/extensions/ext_zicsr.cpp:105`: the old reasoning "turned out to be
exactly wrong. A device tree advertising..."

**Evidence.** `fbdaf84`.

**Cross-references.** See [Bugs that were design assumptions](#assumptions).
This is one of five comments in this codebase that argued for an absence
and were later corrected: this one, `mmu.cpp`'s A/D "known gap"
([Bug 43](#bug43)), `ext_zvbb.cpp`'s "guessing at implementations nothing
exercises" ([Bug 49](#bug49)), `ext_fp_common.hpp`'s "setting NV more often
than required never changes the returned comparison result"
([Bug 48](#bug48)), and `mstatus.MPRV`'s "not modeled yet, since nothing
needs it" ([Bug 96](#bug96)).

---

<a id="bug47"></a>
## 47. `cbo.zero`'s address must be aligned *down* to the block

**Symptom.** None observed — caught by a test written specifically to
provoke it.

**Root cause (potential).** `cbo.zero` zeroes a whole cache block, and its
address is aligned *down* to the 64-byte block boundary. So `cbo.zero`
issued against `base+8` still zeroes from `base`.

Missing that mask corrupts the tail of the previous page on a
page-clearing path — which is precisely the use `cbo.zero` exists for.

**Why it would have gone unnoticed.** Any test that passes an
already-aligned address cannot distinguish a correct implementation from
one that does not mask at all. The natural way to write the test is with an
aligned address, because that is how software uses the instruction.

**Resolution.** Implemented with the align-down, and the test deliberately
passes an *unaligned* address with guard words on both sides.

Two further properties of `cbo.zero` that had to be right: it is a store,
so it takes store faults and needs write permission. Translating it as a
load would let a read-only page be silently zeroed. The comment survives at
`src/extensions/ext_zicboz.cpp:17`.

**Evidence.** `fbdaf84`.

**Cross-references.** Zicboz is "the one member of the cbo.* family with
real behaviour", which is exactly the reasoning that left Zicbom doing
nothing at all — see [Bug 100](#bug100).

---

<a id="bug48"></a>
## 48. `FEQ`/`FMIN`/`FMAX` raised NV for a quiet NaN

**Symptom.** A per-operation fflags dump in the new Zfa test disagreed with
spike on the first run.

**Root cause.** `FMIN`, `FMAX` and `FEQ` raised the invalid-operation flag
for a *quiet* NaN. The specification reserves that for a *signalling* NaN.

**Why it went unnoticed, and why the original reasoning was wrong.** This
was a deliberate, documented simplification. The comment argued that
setting NV more often than required "never changes the returned comparison
result".

That is true, and beside the point. The comment survives, rewritten, at
`src/extensions/ext_fp_common.hpp:246`: "This used to be simplified to 'any
NaN sets NV' on the grounds that it..."

The flag is the *only* observable difference between `FEQ` and `FLT`, and
between both of those and Zfa's `fleq`/`fltq`. Aliasing them makes the
quiet comparisons unimplementable. So a simplification that was
argued to be unobservable turned out to be the single observable that
distinguishes four instructions from one another.

It survived because of a testing artifact worth naming: `vtest_fd` dumps
only *accrued* fflags at the end of the run. Other instructions in the same
test set NV legitimately, which masked it. `vtest_zfa` dumps flags *per
operation* and caught it on the first run.

That is a general property of accumulator-style state: an accrued flag
register hides any spurious set as long as some legitimate set also
occurred. Per-operation dumps are strictly stronger evidence.

**Resolution.** NV is raised only for a signalling NaN.

**Evidence.** `eda4103` (2026-09-05, RVA23 phase 4).

The commit is careful to demonstrate this is a *correction* rather than a
behaviour change: "all 10 suites (405 cases) match spike, vtest_fd included
-- the 62 existing FP cases still pass with the narrowed NV rule".

**Cross-references.** [Bug 89](#bug89) is the inverse error in the same
area — an operation that should raise NV on a signalling NaN and did not.
Both are the same underlying confusion about what NV reports: it reports
the *quieting* of a signalling NaN, not the presence of a NaN.

---

<a id="bug49"></a>
## 49. 17 Zvbb instructions fell through to `exec_v_int` and did nothing

**Symptom.** None. A guest executing `vrol` or `vwsll` got its destination
register left holding whatever was there before, with no crash and no trap.

**Root cause.** An unimplemented `funct6` in the OPIV* space did not raise
an illegal instruction. It fell through to `exec_v_int`, which did nothing
and advanced `pc`.

Seventeen instructions — `vbrev`, `vbrev8`, `vrev8`, `vclz`, `vctz`,
`vcpop`, `vrol`, `vror` and `vwsll` in all their operand forms — were
silently no-ops.

**Why it went unnoticed, and the reasoning that permitted it.** This is the
most explicitly self-correcting entry in the whole history, so it is worth
quoting at length. The file had carried a comment arguing that the rest of
Zvbb was deliberately left out because "guessing at implementations nothing
exercises is how the mask-register bug got in" — a reference to
[Bug 28](#bug28).

`e414423` rejects that reasoning outright:

> The caution was aimed at the wrong risk. Writing an untested
> implementation is a risk a differential test removes; a fall-through that
> makes a missing instruction indistinguishable from a working one is a
> risk nothing removes, and it is the worse of the two.

The same argument had appeared in `f8fbd04` on `main`, phrased as a virtue:
"implementing operations nothing exercises is how subtle bugs get in
unnoticed". The rebuttal is that *not* implementing them, in a dispatch
structure that falls through, is worse — because the first risk is
detectable and the second is not.

The measurement is exact. Reverting the dispatch to its previous form and
re-running the new test: **34 of 37 cases fail, and the 3 that pass are the
`vandn` forms** — the one instruction that had been implemented.

**Resolution.** All 17 implemented. Comment at
`src/extensions/ext_zvbb.cpp:3`: "This file used to hold vandn alone, on
the reasoning that it was the one..." and at line 9, that the rest "fell
through to exec_v_int and was silently ignored, which is worse than..."

**Evidence.** `e414423` (2026-09-05, RVA23 phase 3). `vtest_zvbb` matched
spike 37/37 on the first run; all 9 suites (290 cases) matching.

**Cross-references.** Structurally identical to [Bug 27](#bug27) in the
scalar decoder, which had been fixed by making unrecognised `funct7`
illegal — a fix that was never applied to the vector `funct6` space.
[Bug 51](#bug51) and [Bug 52](#bug52) are the same structure again in
`exec_v_fp`, found two phases later, and `f4d66aa` names the connection:
"implementing a subset and leaving the rest silently ignored is precisely
what created this class of bug in Zvbb."

---

<a id="bug50"></a>
## 50. Zvbb's `funct6` 0x12 is shared with `vzext`/`vsext`, and `vror.vi` hides `imm[5]` in `funct6`

**Symptom.** Two encoding collisions that a remembered opcode table gets
wrong.

**Root cause.**

- `funct6` 0x12 is shared with base V's `vzext`/`vsext`, which use `vs1`
  values 2..7. Zvbb's unary group takes `vs1` 8..14 in the same slot. The
  split is on `vs1`, and the previous dispatch sent the *whole slot* to
  `vzext`.

- `vror.vi` needs a 6-bit rotate amount, but OPIVI's immediate field is 5
  bits. So `imm[5]` lives in the low bit of `funct6`. `vror.vi` therefore
  appears at *both* 0x14 and 0x15 — and 0x15 means `vrol` for `.vv`/`.vx`
  but `vror` for `.vi`. Only `funct3` separates them.

**Why it would have gone unnoticed.** Neither is inferable from a mnemonic
list. Both are the kind of thing that reads as a transcription error when
you find it and as obviously-correct when you write it.

**Resolution.** Encodings taken from the assembler rather than from memory
— the policy established after [Bug 29](#bug29).

**Evidence.** `e414423`.

The test built around this is worth recording because it targets two
failure modes that are element-width dependent, and both have a natural
wrong implementation that looks correct at e64:

- `clz`/`ctz`/`cpop` are defined over SEW, so an all-zero element must
  yield SEW. A host `__builtin_clzll` answers 64 at every width. Run at e8
  and e32, all-zero element included.
- Rotates are modulo SEW and must happen *inside* it — a 64-bit host rotate
  of an 8-bit element drags in the zeros above bit 7. Run at e8, including
  an amount larger than SEW.

It also covers `vwsll`'s widened two-register destination, dumping both
halves, since writing only the narrow width leaves the second register
untouched — and two masked operations, "v0 handling being where this
project's worst vector bug lived" ([Bug 28](#bug28)).

---

<a id="bug51"></a>
## 51. `exec_v_fp` returned silently for any SEW other than 32 or 64

**Symptom.** Every vector FP instruction at e16 was a no-op.

**Root cause.** `exec_v_fp` had an early return for `SEW != 32 && SEW != 64`.

**Why it went unnoticed.** Because half-precision was not implemented, so
no test ran at e16, so the early return was never taken. The guard was
correct for a machine without Zvfhmin and became a silent-no-op the moment
one was needed.

**Resolution.** It now admits the two conversions Zvfhmin defines.
Everything else at e16 still returns silently, and **the comment says so
plainly** rather than leaving it to be inferred, since making it illegal is
a separate behaviour change.

That is the fix for this class applied deliberately: the remaining silence
is documented as a known state rather than left as an accident.

**Evidence.** `f4d66aa` (2026-09-06, RVA23 phase 5). Comment at
`src/extensions/ext_v_fp.cpp:170`: leaving a subset "silently ignored is
what caused the problem in the..."

---

<a id="bug52"></a>
## 52. VFUNARY0's widening and narrowing subcodes were ignored — fourteen instructions

**Symptom.** Fourteen instructions left their destination register
untouched with no trap, at *every* width, not just half precision.

**Root cause.** VFUNARY0's widening (`vs1` 0x08–0x0F) and narrowing (`vs1`
0x10–0x17) subcodes were all ignored — they fell off the end of the
same-width handler.

**Why it went unnoticed.** Same structure as [Bug 49](#bug49): a dispatch
that falls through rather than rejecting. And these are conversions between
widths, which a bare-metal game and a booting kernel do not exercise.

**Resolution.** The *whole family* is implemented, rather than only the two
RVA23 names that phase 5 strictly needed — explicitly because
"implementing a subset and leaving the rest silently ignored is precisely
what created this class of bug in Zvbb".

That is the [Bug 49](#bug49) lesson being applied prospectively rather than
retroactively, one phase later.

**Evidence.** `f4d66aa`. Comment at `src/extensions/ext_v_fp.cpp:164`:
these "used to fall off the end of the same-width handler below and" leave
the destination holding what "it held before, silently, which is the same
failure Zvbb had".

---

<a id="bug53"></a>
## 53. Float-to-integer conversions never reported inexact

**Symptom.** NX was dropped from every `fcvt` in the emulator — scalar and
vector alike.

**Root cause.** `collect_fflags()` read MXCSR, which covers SSE arithmetic.
But `std::llrint` on this toolchain goes through **x87**, and leaves its
status in the x87 status word rather than in MXCSR. So the inexact flag
raised by the conversion was written to a register nothing read.

**Why it went unnoticed.** Because the *values* were right. Only the flag
was missing, and flags are only observable to something that dumps
`fflags`. The hand-written FP suite dumps accrued flags at the end of the
run, where — exactly as in [Bug 48](#bug48) — other instructions setting NX
legitimately masked the omission.

**Resolution.** The interesting part. Rather than reading a different host
status register, inexactness is now **decided from the values**: a
conversion is inexact exactly when its result converts back to something
different.

That is correct for every rounding mode and independent of which host FP
unit the library happens to use. The commit states the principle: "Same
reasoning as writing the f16 conversions by hand: where the host is
unreliable or absent, compute the answer rather than borrow it."

**Evidence.** `f4d66aa`.

The verification targets the risk directly: "vtest_v and vtest_fd matter
most here: 122 existing cases that exercise fcvt_to_* and still agree with
spike now that NX is reported where it was previously dropped."

**Cross-references.** This is the third bug in the float-to-integer path
([Bug 9](#bug9), [Bug 53](#bug53), [Bug 90](#bug90)) and the second caused
by reading the wrong host status register. [Bug 91](#bug91) is the same
root cause found again, in `fma`, and fixed at the shared cause rather than
one call site at a time — the `collect_fflags` comment records that
"std::llrint had already been caught doing the same thing, and was worked
around one call site at a time; this fixes the shared cause instead."

[Bug 94](#bug94) is the point at which borrowing the host's answer was
abandoned entirely.

---

<a id="bug54"></a>
## 54. The Svnapot and Svpbmt config flags were added but `mmu.cpp` never consulted them

**Symptom.** `-march` could not actually disable the features. The flags
were decorative.

**Root cause.** The `SVNAPOT` and `SVPBMT` extension-table flags were added
in the same commit that implemented the features, and `mmu.cpp` — which
implements them — never read them.

**Why it went unnoticed... except it did not.** This was caught by the
phase's own negative control, and it is the clearest demonstration in the
project of why negative controls are worth the trouble. Every positive test
passed. The features worked. The only thing that could detect the problem
was checking that turning them *off* breaks something, and it did not.

**Resolution.** Both gate properly. And when a feature is off, its PTE bit
is treated as *reserved* — it must fault rather than translate as though
the bit were absent.

That distinction is the same "don't claim what you don't implement" rule as
the phase 1 device tree fix ([Bug 43](#bug43)), one level down. A hart with
Svpbmt disabled that quietly ignores a PBMT-encoded PTE is reporting
support it does not have.

**Evidence.** `e60ec1c` (2026-09-06, RVA23 phase 6).

Each feature then discriminates independently: disabling `svinval` fails 10
of 16 cases, `svnapot` 7, `svpbmt` 2.

---

<a id="bug55"></a>
## 55. PTE bits 60:54 were not checked as reserved

**Symptom.** None directly — but without it, the entire Svnapot/Svpbmt
implementation is unverifiable.

**Root cause.** Bits 60:54 of a page table entry are reserved and must
cause a fault if set. They were not checked.

**Why this is not a separate nicety.** The commit is explicit: "without it
the two above are meaningless, since a PTE with anything set in the high
bits would translate happily either way."

Svnapot's N bit and Svpbmt's memory-type field live in the high bits of the
PTE. The properties that make those extensions *real* are what must fault —
Svpbmt's reserved type 3, and any nonzero type while `menvcfg.PBMTE` is
clear, which is how an OS probes for the extension. A machine that ignores
unknown high bits cannot fault on any of them.

**Resolution.** Reserved-bit check added in `mmu.cpp`.

**Evidence.** `e60ec1c`.

**Cross-references.** [Bug 105](#bug105) is the same omission for *non-leaf*
PTEs specifically, found a day later by riscv-arch-test: A, D and U belong
to leaves, and Svpbmt's memory-type bits likewise, so carrying any of them
in a pointer PTE is a reserved encoding that must fault.

Also worth recording from this commit: Svnapot's failure mode if the N bit
is ignored "is not a fault: translation still succeeds, it just aliases
every address in the range onto the same physical page" — which is why
`vtest_sv` writes distinct values through the first, middle and last pages
of a NAPOT range rather than testing one address.

And Svinval's three instructions retire without effect, on the same
argument `fence.i` uses — `mmu_translate` walks the table in guest memory
on every access, so a translation can never be stale. The commit draws the
distinction that matters: "between doing nothing because nothing is
required and doing nothing because it was never implemented; these are the
first kind, and ext_svinval.cpp exists to say so."

That distinction is the entire subject of [Bug 100](#bug100), where the
same argument was made about Zicbom and was wrong.

---

<a id="bug56"></a>
## 56. The pointer-masking test never left M-mode, where masking correctly does not apply

**Symptom.** A test that would have passed while measuring nothing.

**Root cause.** Pointer masking's PMM field lives in the `envcfg` of the
mode *above* the one being masked: `menvcfg.PMM` governs S-mode,
`senvcfg.PMM` governs U-mode. So a mode cannot exempt itself from the
masking its supervisor imposed — and M-mode, having no mode above it, is
correctly not masked at all.

The first draft of the test ran entirely in M-mode.

**Why it went unnoticed.** It did not — it was caught by running the thing
rather than reading it, and the commit says so: "Two errors of my own, both
caught by running the thing rather than reading it."

It is recorded because a test that passes while measuring nothing is the
same failure as [Bug 35](#bug35) and [Bug 66](#bug66), and it happened
three separate times in this project.

**Resolution.** The test drops privilege.

**Evidence.** `7991e11` (2026-09-06, RVA23 phase 7).

---

<a id="bug57"></a>
## 57. `sspm` is not a name spike recognises, and spike failing to start looked like a hang

**Symptom.** An empty signature that looked exactly like the hang seen in
the previous phase for an unrelated reason.

**Root cause.** The spike ISA string named `sspm`, which spike does not
recognise. Sspm is a *profile-level* name meaning "S-mode pointer masking
is available" — the thing that provides it is Smnpm. spike exited
immediately.

**Why it was hard.** Because the observable is identical to a genuine hang:
no signature file. The previous phase had produced an empty signature for a
completely different reason (a test case spike refused and trapped on), so
the diagnosis was primed toward the wrong answer.

**Resolution.** ISA name corrected — and, more usefully, `spike_sig.sh` now
checks for spike failing to start and reports its own words, "which turns
that diagnosis into one line".

**Evidence.** `7991e11`.

**Cross-references.** The same class recurred immediately in `07a0090`,
which "corrects the spike ISA name: it spells this extension smstateen, not
ssstateen. The 'spike did not start' check added in phase 7 reported that
in one line instead of an empty signature." See [Bug 70](#bug70). A harness
improvement paying for itself within one phase is unusually fast.

---

<a id="bug58"></a>
## 58. The pointer-masking test used "does it fault" as a proxy, and measured a missing PMA check instead

**Symptom.** Five mismatches against spike, all of which looked like
pointer-masking bugs and none of which were.

**Root cause.** The test originally used "does the tagged access fault" as
a proxy for "is the tag still there".

That proxy does not hold. A 7-bit tag necessarily puts the address outside
any RAM this machine has. spike raises an access fault for a physical
address with no memory behind it. DoomV had **no physical-memory-attribute
checking at all** and let the access evaporate.

So every one of the five mismatches was that difference, not masking.

**Why it went unnoticed.** Because the proxy is a reasonable one on any
machine that checks PMAs, and the fact that DoomV did not was not yet
known. The test was diagnosing a real defect through an unrelated
observable, and reporting it under the wrong name.

**Resolution.** The cases now state the property directly: a store through
a tagged pointer must leave the clean location unchanged. Both machines
agree on that — one because the store faulted, the other because it landed
somewhere harmless.

The commit then records the real defect explicitly as a known gap rather
than folding a large change into a phase about something else:

> That leaves a known gap, deliberately not fixed here: DoomV does not
> raise access faults for unbacked physical addresses. It is a real
> conformance issue, but it would touch every memory access Doom and Linux
> make and belongs in its own change with its own regression.

**Evidence.** `7991e11`.

That gap was closed eight commits and one day later — see
[Bug 102](#bug102) and [Bug 103](#bug103), where it turns out to have a
second, sharper consequence in the page-table walk that nobody had
predicted.

The commit also records something the machine cannot prove
differentially, which is worth keeping as an example of stating a
limitation rather than implying coverage: sign-extension versus zero-fill
in the single case where they differ needs an address that is both high
*and* backed by memory, and all of DoomV's RAM is low. The test checks the
consequence it can see — a zeroing implementation would land back near the
clean address and corrupt the sentinel — "which is weaker than observing
the masked address itself".

---

<a id="bug59"></a>
## 59. `csr[9:8] == 2` was read as a privilege level; it is the hypervisor/VS-CSR encoding

**Symptom.** A guest's entirely ordinary `csrw stvec` trapped.

**Root cause.** `csr[9:8]` normally encodes the lowest privilege that may
access a register: 0 for U, 1 for S, 3 for M. The value **2 is not a
privilege level at all** — it is the hypervisor and VS-CSR encoding, and
those registers are reachable from HS-mode and M.

Read as a literal privilege number, 2 exceeds every `PrivMode`, so the
check denied every VS CSR to every mode.

**Why it went unnoticed.** It was unreachable. No CSR in the emulator lived
in the `0x2xx` range until phase 8a introduced VS-mode CSR redirection —
which rewrites an S-mode CSR number into the VS range before privilege
checks see it. That redirection made the dead code live.

The symptom then points nowhere near the cause. A guest writing `stvec`
trapping is a *privilege* symptom in appearance and a *field-decoding*
symptom in reality, and the connection between them runs through a
redirection layer that had been added the same day.

**Resolution.** `csr_access_permitted()` maps `min_priv == 2` to
`PrivMode::S`. The comment at `ext_zicsr.cpp:382` narrates the whole thing,
including its unreachability: "That is invisible until something actually
redirects an S-mode CSR name into the 0x2xx range, at which point a guest's
ordinary csrw stvec starts trapping."

**Evidence.** `bb1cc50` (2026-09-06, RVA23 phase 8a), which describes it as
"latent since phase 2".

**Cross-references.** [Bug 45](#bug45) introduced the check this bug lives
in. This is the clearest case in the project of a bug that was *created* by
a correct change and *exposed* by a later one — enforcement added in phase
2, made reachable in phase 8a, three weeks of correct-looking behaviour in
between.

---

<a id="bug60"></a>
## 60. `hstatus.VSBE` and `VGEIN` were writable

**Symptom.** Signature mismatch against spike on a write-all-ones test.

**Root cause.** Both fields must read as zero on this hart. `VSBE` selects
guest endianness and this hart is little-endian only. `VGEIN` names a guest
external interrupt controller, and GEILEN is 0 here — there are none — so a
nonzero value would name something that does not exist.

**Why it went unnoticed.** It was found in the phase that introduced the
register. Recorded because it is the same WARL class as
[Bug 16](#bug16), [Bug 61](#bug61), [Bug 68](#bug68) and [Bug 79](#bug79),
and because `VGEIN` recurs in [Bug 77](#bug77) as a *non*-defect once Sail
enters the picture: a reference configured with guest interrupt files is
equally correct, so the field was excluded from the comparison rather than
changed.

**Resolution.** Both forced read-only zero.

**Evidence.** `bb1cc50`.

---

<a id="bug61"></a>
## 61. `vstvec` and `vsatp` never got the WARL treatment their S-mode twins had

**Symptom.** A written `vstvec` MODE value stored raw.

**Root cause.** `stvec`/`mtvec` MODE is WARL and only direct mode is
implemented, so the S-mode write path clamps it. `vstvec` needed the same
clamp and did not get it — **because redirection rewrites the CSR number
before the write site sees it.** The guest's write arrives as `0x205`, not
as `0x105`, so it does not reach the code keyed on the S-mode number.

**Why it went unnoticed.** The redirection design is otherwise a strength:
`bb1cc50` chose to rewrite the CSR number once, before privilege checks or
dispatch see it, "rather than special-casing nine registers at their own
read and write sites -- which is what stops one of them being missed."

That is correct, and it has exactly one failure mode: any code keyed on the
S-mode *number* rather than on the register's identity silently stops
applying. The clamp was such code.

**Resolution.** `vstvec` gets the clamp. `vsatp` is routed through `satp`'s
WARL handling for the same reason — though the commit is honest that it
"stays unverified until two-stage translation reads it", which it did two
phases later.

The commit then generalises the rule rather than just fixing the instance:

> That last pair generalises: any code keyed on an S-mode CSR number has a
> VS-mode twin needing identical treatment. stvec/vstvec and satp/vsatp are
> done; sstatus/vsstatus and sie/sip when interrupt delegation lands.

**Evidence.** `bb1cc50`.

---

<a id="bug62"></a>
## 62. Three hypervisor decode collisions, each of which mis-decodes silently

**Symptom.** Caught during implementation of `hlv`/`hsv`, by taking
encodings from the assembler.

**Root cause.** Three separate collisions in the hypervisor load/store
encoding space:

- **`funct3=100` is shared with Zimop's `MOP.R`**, split on bit 31. Without
  that split, `hlv` decodes as a may-be-operation and — because of
  [Bug 44](#bug44)'s correct implementation — writes **zero to `rd`**
  instead of reading guest memory. No trap. Just a wrong answer. This is a
  case where two correct implementations collide into a silent failure.

- **`hfence.gvma`'s `funct7` (0x31) is the same as `hsv.b`'s**; `funct3`
  separates them.

- **`hlvx` asks for `AccessType::Fetch` rather than `Load`**, because it
  probes *execute* permission — it exists so a hypervisor can fetch a guest
  instruction from a page the guest may execute but not read. Using `Load`
  would wrongly succeed on a read-only page and wrongly fail on an
  execute-only one.

**Resolution.** All three handled in `classify()` and
`src/extensions/ext_h_ldst.cpp`, whose header comment records two of them:
"Two collisions worth stating, because both would silently mis-decode".

**Evidence.** `463ac7c` (2026-09-06, RVA23 phase 8b).

The test construction for this phase is worth recording, because a naive
`hlv` that translates through `satp` instead of `vsatp` works perfectly
whenever the two page tables agree — which in any normal setup they largely
do. So the test deliberately maps **the same virtual address to two
different physical pages**, one in the hypervisor's table and one in the
guest's. An ordinary `ld` must reach the first and `hlv` the second; only
the value distinguishes them.

Negative control: making `hlv` read `satp` instead of `vsatp` fails 14 of
22 and crashes nothing — "which is exactly the failure the two-mapping
construction exists to catch."

---

<a id="bug63"></a>
## 63. The two-stage translation test passed 14/14 with the intermediate PTE translation removed

**Symptom.** A negative control that failed to be negative.

**Root cause.** Two-stage translation means every guest page-table
*pointer* is itself a guest physical address that the G-stage must
translate before the PTE can be read — a single guest load walks three
levels, and each level's walk is an address needing second-stage
translation.

The test's G-stage identity-mapped the region holding the guest's page
tables. So translating those addresses or not gave identical answers, and
that entire code path went unexercised.

**Why this is the more useful outcome.** The commit says so directly: "The
negative controls found a defect in the *test* rather than the
implementation, which is the more useful outcome." The implementation was
correct; the evidence for it was not.

An identity mapping is the natural thing to write when constructing a
G-stage by hand, and it is precisely the mapping under which the second
stage is unobservable.

**Resolution.** The guest's tables now live behind a G-stage relocation —
`vsatp` names guest physical `0x80240000`, which the second stage maps onto
the real pages — so the walk genuinely depends on it.

**Evidence.** `9a5c416` (2026-09-06, RVA23 phase 8c).

Both halves are then independently controlled: removing the final G-stage
fails 12 of 14; removing the intermediate PTE translation fails 8 of 14
(**it failed 0 before the test was corrected**).

**Cross-references.** [Bug 66](#bug66) is the other test that passed while
proving nothing, and [Bug 56](#bug56) is a third. Three in three
consecutive phases.

Also from this phase: Sv39x4 rather than Sv39 for the G-stage — the root
table is four pages wide with an 11-bit top index, extending guest physical
addresses to 41 bits. And every G-stage leaf must have `U` set, because
both VS and VU sit below HS, so the second stage always sees a guest access
as user code: a G-stage page without `U` is unreachable by any guest at
all, which makes it a configuration error rather than a permission the
hypervisor could have meant.

---

<a id="bug64"></a>
## 64. `hedeleg` has read-only-zero bits, and they were not implemented at all

**Symptom.** Signature mismatch against spike.

**Root cause.** `hedeleg` looks like an ordinary delegation register and is
not. Bits 10, 20, 21, 22 and 23 are read-only zero, each naming a trap the
hypervisor **structurally cannot give away**:

- An ecall from VS is how a guest *calls* its hypervisor. Delegating it
  back to the guest would leave the guest unable to call out.
- A guest-page fault means the second stage refused, which the guest cannot
  see, let alone fix.
- A virtual instruction exception is raised precisely because the guest
  attempted something only the hypervisor may do.

`hideleg` likewise only accepts the three VS-level interrupts.

**Why it went unnoticed.** Because the *shape* of the register is familiar.
`medeleg` delegates anything; `hedeleg` looks like `medeleg` one level
down. The reason it is different is semantic rather than structural, and
nothing in the encoding hints at it.

**Resolution.** Read-only-zero mask implemented for both registers.

**Evidence.** `efe8823` (2026-09-06, RVA23 phase 8d), listed as the first
of "three things this increment got wrong first and spike corrected".

---

<a id="bug65"></a>
## 65. `stval` and `htval` carry different addresses; one field was serving both

**Symptom.** On a guest-page fault, the first pass reported the *guest
physical* address as `stval`.

**Root cause.** On a guest-page fault the hypervisor needs two different
numbers: the guest *virtual* address, to tell the guest what it touched;
and the guest *physical* address, to know which page to back.

They are unrelated numbers. The fault happens *precisely because* the first
stage succeeded and the second did not — so the GVA and the GPA have no
useful relationship. One field cannot serve both.

Reporting the GPA in `stval` shows a hypervisor an address the guest never
used.

**Why it went unnoticed.** In a one-stage world `stval` is the faulting
address and there is only one candidate. The distinction is created by
two-stage translation and did not exist before phase 8c.

**Resolution.** The MMU writes `htval` itself, being the only code that
knows the GPA. `enter_trap` only *clears* it for causes with no
second-stage address, so a stale value cannot be mistaken for a fresh one.

That split — the MMU writes, the trap path clears — is the design decision
that makes the two fields independently trustworthy.

**Evidence.** `9a5c416` and `efe8823`.

Also from `efe8823`: guest-page-fault causes (20/21/23) are distinct from
ordinary page faults for an operational reason — "they say the guest's own
tables were satisfied and the *hypervisor's* were not, which means the
hypervisor has not backed that page. Reporting an ordinary page fault there
would send the guest kernel hunting for a bug in tables that are correct."

And `htinst` remains read-zero, which the spec permits and spike agrees
with. That is recorded as a deliberate choice rather than an oversight: a
real hypervisor uses it to avoid re-fetching and re-decoding the faulting
instruction, so zero costs performance rather than correctness.

---

<a id="bug66"></a>
## 66. The `hdeleg` test's labels asserted the opposite of what it checked — and it passed

**Symptom.** None. The test passed.

**Root cause.** The test's own labels asserted the opposite of what it
checked, reading `delegated: guest handler ran (1)` against an expected
value of 0.

**Why this is a real bug rather than cosmetic.** Because the labels are the
only thing that tells a future reader what a mismatch means. A test that
passes with inverted labels is a trap set for whoever next sees it fail:
the diff will name a behaviour, and the behaviour it names will be the
wrong one.

**Resolution.** The labels and the test's comments now say what the ecall
stages actually demonstrate: setting the bit and observing nothing change.

**Evidence.** `efe8823`.

Two test constructions from the same commit are worth keeping:

- The guest handler returns control by reading `hstatus` — a virtual
  instruction, deliberately never delegated — rather than by ecall, since
  ecall is one of the causes under test and a delegated one would send the
  handler back to itself.

- The traps are `ebreak` and `ecall` rather than a page fault, because an
  unmapped physical address would run into the then-known difference
  between the implementations (DoomV had no physical-memory-attribute
  checking, so it did not fault where spike does), "producing a failure
  that looked like a delegation bug".

That second one is [Bug 58](#bug58)'s lesson applied prospectively: a known
divergence, routed around deliberately so it cannot masquerade as the thing
under test.

**Cross-references.** [Bug 56](#bug56), [Bug 63](#bug63) — tests that
passed while proving nothing or proving the wrong thing. And
[Bug 78](#bug78), a test that passed for a reason that had nothing to do
with what it claimed.

---

<a id="bug67"></a>
## 67. Sscofpmf and Ssstateen were missing after the mandatory set was declared complete

**Symptom.** The RVA23 mandatory extension set had been declared complete
after phase 8. It was not.

**Root cause.** Auditing DoomV against riscv-arch-test's
`sail-RVA23S64.yaml` — the profile's own machine-readable extension list —
found two mandatory extensions simply absent.

**Why it went unnoticed.** The commit is precise about this, and the reason
generalises well beyond these two:

> Both are CSRs and an interrupt with no instructions of their own, which is
> exactly why a plan built from reading the profile document missed them:
> there is no unimplemented opcode to trip over, and nothing fails until
> software goes looking for the registers.

A phase plan derived from reading a specification is organised around
*things you can execute*. An extension that adds only state is invisible to
that organising principle. Nothing traps, nothing decodes wrongly, nothing
is missing from any dispatch table.

The fix for this class is not more careful reading — it is auditing against
a machine-readable list rather than a document. `mkconfig.py` does exactly
that, and the commit that introduces it (`761291a`) says this audit "is
what earned its keep: it is what found Sscofpmf and Ssstateen missing after
the mandatory set was believed complete."

**Resolution.** Both implemented.

Ssstateen gates access to state that newer extensions add. It exists for a
specific hypervisor problem: switching guests means saving every piece of
state a guest could have touched, and a hypervisor written before some
extension existed does not know that extension's state needs saving — so a
guest using it silently corrupts another guest across a context switch. The
stateen bits let the hypervisor deny access to state it does not
understand, **turning silent corruption into a clean trap**. Comment at
`src/extensions/ext_ssstateen.cpp:14`.

Sscofpmf adds an overflow bit per counter, mode filtering, `scountovf`, and
the local counter-overflow interrupt, so a profiler can sample rather than
poll. This hart counts no events, so nothing overflows spontaneously — but
the registers, their WARL behaviour and `scountovf`'s derived read are real
and observable, and software can drive an overflow directly.

**Evidence.** `07a0090` (2026-09-06). `vtest_stateen` matches spike 19/19;
all 19 suites (639 cases) match.

---

<a id="bug68"></a>
## 68. The `stateen` WARL masks were guessed rather than checked

**Symptom.** Four of the five errors in the first Ssstateen implementation.

**Root cause.** The masks were guessed. The commit lists the corrections:

| register | wrong | right |
| --- | --- | --- |
| `mstateen0` | excluded bit 62 (ENVCFG) and bit 60 (JVT); included bit 57 (CSRIND) | includes 62 and 60; excludes 57 |
| `mstateen1-3` | — | only SE0; the rest name nothing yet |
| `sstateen0-3` | — | **no writable bits at all** |
| `hstateen0-3` | — | none on this hart |

`sstateen` is the instructive one: it has no SE0, because U-mode has no
state-enable register of its own to aggregate. The bit exists in the other
three because it gates access to the *next level down's* stateen register,
and below S there is no next level.

**Why it went unnoticed.** WARL masks are exactly the kind of detail that
looks like transcription and is actually semantics. Each excluded bit has a
reason; guessing produces a mask that is plausible bit by bit and wrong
overall.

**Resolution.** Corrected against the specification, and the test changed
to check the *gating behaviour* rather than diffing an
implementation-defined value — see [the taxonomy](#taxonomy).

**Evidence.** `07a0090`, and finished in `2667bf1`: "The stateen masks were
corrected the same way in the previous round and are finished here:
mstateen0 is SE0|ENVCFG, mstateen1-3 read-only zero, and the suite checks
SE0's gating behaviour rather than diffing an implementation-defined
value."

**Cross-references.** The problem framing given for this project describes
these masks as "copied from spike, claiming state DoomV lacks". The commit
history says "guessed rather than checked" and does not mention spike as
the source. The *effect* described — claiming state the hart does not have
— is accurate for `sstateen`/`hstateen`; the *provenance* is
**[unverified]**.

---

<a id="bug69"></a>
## 69. LCOFI was derived from the OF bits rather than latched

**Symptom.** The fifth error in the first Sscofpmf implementation, and the
one that was not a transcription mistake.

**Root cause.** LCOFI — the local counter-overflow interrupt — was made
*derived* from the `mhpmevent` OF bits rather than *latched*.

The reasoning, written down in a comment at the time, was that a latched
edge would stay asserted after its cause was gone.

That is wrong for this extension, and the commit explains why in a way that
generalises:

> hardware raises LCOFI at the moment a counter overflows, and it stays
> pending until software clears it, like any device interrupt. Deriving it
> from OF makes it impossible for a handler to clear the interrupt without
> destroying the overflow record it needs in order to know which counter
> fired. The two are related signals, not the same signal.

**Why it went unnoticed.** Because the derived implementation is *simpler*
and looks more principled — no duplicated state, no possibility of the
interrupt and the flag disagreeing. The argument for it is an argument from
elegance, and it happens to remove a capability software depends on.

**Resolution.** LCOFI is latched.

**Evidence.** `07a0090`, explicitly framed as "a design decision I argued
for in a comment and got backwards".

**Cross-references.** [Bugs that were design assumptions](#assumptions).
This is the only one in the group where the mistaken reasoning was about
*hardware behaviour* rather than about what software needs.

The verification note matters here too: "all 19 suites (639 cases) match
spike -- including vtest_trap and vtest_csr, which the LCOFI change touches
through mip."

---

<a id="bug70"></a>
## 70. spike spells the extension `smstateen`, not `ssstateen`

**Symptom.** spike exits immediately, empty signature.

**Root cause.** The ISA string named `ssstateen`; spike spells it
`smstateen`.

**Why it is recorded.** Because of how it was *diagnosed*, not because the
name was wrong. The "spike did not start" check added one phase earlier for
[Bug 57](#bug57) reported it in one line instead of producing an empty
signature that would have needed the same investigation from scratch.

**Resolution.** Name corrected.

**Evidence.** `07a0090`.

# Part V — Sail as the reference: what a formal model found

Up to this point the oracle was spike: an independent, well-respected,
widely-used reimplementation of the architecture. It found a great many
real bugs and it is still worth running.

It is also not the specification. `761291a` states the change of authority
plainly:

> Sail is the formal RISC-V specification: its model is generated from the
> same source the architecture is defined in, rather than being an
> independent reimplementation like spike. Where the two disagree, Sail is
> the stronger authority -- and it is the only one of them with RVA23
> profile configurations at all.

This part contains two kinds of entry. The first is the friction of getting
a formal model to run at all, which cost real time and is worth recording
so it is not paid again. The second is what it found — and what it found is
remarkably concentrated: **eight mismatches across nineteen suites, none
arithmetic, all WARL fields.**

<a id="bug71"></a>
## 71. riscv-arch-test's own Sail config cannot be loaded by any tagged Sail release

**Symptom.** `config/sail/sail-RVA23S64` fails to load.

**Root cause.** Its required platform properties postdate every tagged Sail
release: `simple_interrupt_generator` (2026-04-22), `reservation`
(2026-04-24), `wfi_available_to_user_mode` (2026-06-03). Pinning Sail to
before those dates makes it demand *different* properties the config also
lacks.

It appears to track an untagged revision. There is no version of Sail that
can load it.

**Resolution.** `mkconfig.py` generates the configuration instead. The
reasoning is worth keeping: the extension list in the profile's YAML is the
authoritative content, and the JSON is one serialisation of it — so
generating from the list is both more robust and closer to the source.

That decision then paid off twice over, because the generator also *audits*
DoomV against the same list, which is what found [Bug 67](#bug67).

**Evidence.** `761291a` (2026-09-06).

---

<a id="bug72"></a>
## 72. z3 must be on `PATH`, not merely installed

**Symptom.** `SMT solver returned unexpected status 127`.

**Root cause.** Sail shells out to z3. Exit status 127 is "command not
found", wrapped in a message about solver status that does not say so.

**Why it cost time.** The error names the *result* of the failure and not
the failure. "Unexpected status" reads as a solver disagreement rather than
as a missing binary.

**Resolution.** z3 on `PATH`, documented in `setup.sh`.

**Evidence.** `761291a`, listed as one of "four things ... each of which
cost time".

---

<a id="bug73"></a>
## 73. Sail's `build_simulator.sh` has a CRLF shebang on an autocrlf checkout

**Symptom.** The submodule's build script cannot execute.

**Root cause.** Same mechanism as [Bug 26](#bug26): `core.autocrlf`
converts the file on checkout, and a shebang line ending in `\r` names an
interpreter that does not exist.

But the fix for Bug 26 deliberately does *not* apply here. The
`.gitattributes` added there is scoped to this project's own paths,
because the vendored trees under `tools/*/src` are submodules and keep
whatever their upstream uses. Fixing this by widening that scope would mean
this repository silently rewriting a submodule's working tree.

**Resolution.** `build.sh` issues Sail's two `cmake` calls directly rather
than editing a submodule's working tree.

**Evidence.** `761291a`.

**Cross-references.** [Bug 26](#bug26), [Bug 88](#bug88). Three distinct
failures from Windows checkout semantics, each needing a different fix
because each sits at a different point of the repository/submodule
boundary.

---

<a id="bug74"></a>
## 74. `sail_sig.sh` hand-rolled a memory dump that Sail has a flag for

**Symptom.** Zero words of signature, silently.

**Root cause.** An earlier version of `sail_sig.sh` dumped memory and
sliced it by hand, guessing the dump file naming wrong. Sail has
`--test-signature`, which does exactly this job.

**Resolution.** Use the flag.

**Evidence.** `761291a`, with the lesson stated flatly: "Reading the option
list would have been faster than writing the parser."

**Cross-references.** [Bug 36](#bug36) is the same shape with the opposite
resolution: spike's `+signature` plusarg *is* the right flag and does not
work in this build, so the harness had to hand-roll it through `--debug-cmd`
instead. Same question — "does the reference have a signature mechanism" —
opposite answers, and both cost a wrong turn.

---

<a id="bug75"></a>
## 75. Eleven differential suites depended on spike's permissive PMP default

**Symptom.** Every test that drops to S-mode takes a fetch access fault
under Sail, at the S-mode entry address, before its first instruction runs.

**Root cause.** With PMP implemented and **no entry configured**, the
specification denies S and U mode access outright. That is the rule people
get backwards — the intuition is that an unconfigured protection unit
protects nothing, and the architecture says the opposite.

spike happens to default permissively. So for as long as spike was the only
reference, nine suites (eleven by the time they were counted for the fix)
dropped privilege and depended on that default without saying so.

**Why it went unnoticed.** Because a dependency on a reference's *default*
is invisible while that reference is the only one. Nothing in the test
source mentions PMP; nothing needed to; the tests worked.

`761291a` names it correctly on discovery: "That last one is a defect in
this project's tests rather than in DoomV."

**Resolution.** Two stages, deliberately separated.

First, as a diagnostic workaround, the generated Sail config set
`pmp.count` to 0.

Then `24a17b1` did it properly: the eleven suites that drop privilege
install a permit-all entry themselves — `pmpaddr0` all-ones with A=NAPOT
covers the whole space, `0x1F` adds R, W and X — "which is what real
software does and makes the suite independent of any reference's defaults."
The `pmp.count=0` workaround was then removed, so both references run the
same configuration.

The commit gives the forward-looking reason as well: whisper and VeeR-ISS
are now in the tree as further oracles, and neither is likely to share
spike's permissiveness either.

**Evidence.** `761291a` (found), `24a17b1` (2026-09-07, fixed).
`vtest_csr` reaches SUCCESS under Sail with PMP fully enabled, and all 19
suites (639 cases) still match spike.

**Cross-references.** `24a17b1` also records, correctly at the time, that
"DoomV implements no PMP at all, so it ignores these writes. That is not a
conformance gap -- Smpmp is optional in RVA23 -- but it does belong on the
known-gaps list next to the missing physical-memory-attribute checking."

Both of those known gaps were closed within four commits:
[Bug 95](#bug95) and [Bug 102](#bug102). And the PMP commit is scathing
about the same test workaround in retrospect: "every privilege-dropping
test opens an all-permissive entry at the top, which is the tests being
shaped to match the implementation rather than the architecture."

That is a fair criticism of a fix this document has just praised, and both
readings are correct. Installing a permit-all entry is what real software
does and is the right way to make the tests reference-independent. It is
*also* the thing that let a missing PMP implementation stay comfortable.

---

<a id="bug76"></a>
## 76. VLEN 256 versus 128: a configuration difference, not a defect

**Symptom.** Three vector suites differed against Sail.

**Root cause.** The generated Sail config defaulted to VLEN 256 where DoomV
has 128. Nothing was wrong with either machine.

**Why it is recorded.** Because this is category 1 of
[the taxonomy](#taxonomy), and category 1 has a specific failure mode:
three failing suites is an alarming result, and it is entirely artificial.
The cost of misdiagnosing a configuration difference as a defect is
chasing an arithmetic bug that does not exist.

**Resolution.** VLEN pinned in `mkconfig.py`.

**Evidence.** `2667bf1` (2026-09-07).

**Cross-references.** `a27950a` later fetches `riscv-vector-tests` "at the
VLEN=128 that matches this machine" — the same lesson applied when
selecting a new pre-existing suite rather than after being bitten by it.

---

<a id="bug77"></a>
## 77. GEILEN: an open architectural choice, wrongly diffed

**Symptom.** `hstatus.VGEIN` differed against Sail.

**Root cause.** `hstatus.VGEIN` is as wide as the platform's guest external
interrupt count. DoomV has no guest interrupt file, so zero is the only
legal value and its read-only zero is **correct**. A reference configured
with guest interrupt files is **equally correct**.

**Resolution.** Excluded from the comparison. Not changed — there is
nothing to change.

**Evidence.** `2667bf1`.

**Cross-references.** [Bug 60](#bug60) is the same field, one phase
earlier, where making it read-only zero *was* the fix. Both are right: the
field must be read-only zero on this hart, and a reference on which it is
not read-only zero is not thereby wrong. Distinguishing "our value is
correct" from "the difference is a defect" is the whole skill.

[Bug 42](#bug42) (misaligned accesses) is the earliest instance of the same
judgement.

---

<a id="bug78"></a>
## 78. A test used `sfence.vma` where `hfence.gvma` was required, and DoomV never noticed

**Symptom.** `vtest_hgatp` differed against Sail. It had been passing
against spike and against DoomV.

**Root cause.** The test edited G-stage page tables and then issued
`sfence.vma`. `sfence.vma` does not order G-stage translations —
`hfence.gvma` does.

DoomV has **no TLB**. `mmu_translate` walks the table in guest memory on
every access, so a translation can never be stale and no fence is ever
required for correctness. So DoomV never noticed the wrong fence, because
no fence would have made any difference.

Sail caches, and correctly refused.

**Why this is a good bug.** It is the clearest possible example of a test
passing for a reason unrelated to what it claims. The test asserts a
property about G-stage translation after a table edit. On DoomV that
property holds unconditionally, for reasons that have nothing to do with
the fence the test issues. The test could have issued no fence at all and
still passed.

It is also a case where a *simplification* in the implementation — no TLB,
which `4a726708` defends as "simple to get right and cheap enough to matter
only if something measures it" — makes an entire class of software error
undetectable. That is a genuine cost of the simplification and it is
recorded here as one.

**Resolution.** The test uses `hfence.gvma`.

**Evidence.** `2667bf1`, categorised as one of "two [that] were the test's
own fault, and DoomV had been passing them for the wrong reason".

**Cross-references.** [Bug 66](#bug66), [Bug 63](#bug63), [Bug 56](#bug56).
And [Bug 106](#bug106), where the absence of a TLB is again *not* an excuse
— `mstatus.TVM`'s point "is not the fence -- there is no TLB here to flush
-- but that a hypervisor running a guest supervisor traps on both".

---

<a id="bug79"></a>
## 79. `menvcfg/senvcfg/henvcfg.PMM` stored the reserved value 1

**Symptom.** `vtest_pm` differed against Sail. The test asserted that a
reserved PMM value "behaves as masking off", which quietly assumed the
reserved value gets *stored*.

**Root cause.** PMM is WARL. Writing the reserved value 1 must not store 1.
WARL means the field must read back a value the hart implements — and
reading back is exactly how software probes it.

DoomV stored the written value. So the field reported a length the hart
does not implement.

**Why it went unnoticed.** Because the *behaviour* was defensible: with the
reserved value stored, masking was off, which is a reasonable thing for an
unimplemented length to do. The test asserted that behaviour and passed.
What was wrong was the field's read-back, which nothing had checked.

This is [Bug 16](#bug16) again — the `satp.MODE` bug — in a different
register, sixteen days later. A stored-verbatim WARL field lies about the
machine.

**Resolution.** A write of 1 now retains the previous legal field value.

The test was then changed in a way that is the whole point of
[the taxonomy](#taxonomy): **which** legal value a hart substitutes is its
own choice, and the two references make different ones. So the test now
checks the property that is *not* open — whatever PMM reads back is the
length actually in force.

**Evidence.** `2667bf1`, categorised as one of "two [that] were DoomV, and
both are the same mistake -- a reference was treated as the specification".

---

<a id="bug80"></a>
## 80. `hstatus.HUPMM` was missing entirely

**Symptom.** `vtest_h` differed against Sail on a write-all-ones test.

**Root cause.** `hstatus.HUPMM` is the hypervisor's own pointer-masking
control over the addresses `hlv`/`hlvx`/`hsv` compute. RVA23S64 mandates
both H and Ssnpm, so a hart with the pair owes the field. It did not exist.

**Why it went unnoticed.** Because it sits at the intersection of two
extensions. The pointer-masking phase (7) implemented `menvcfg.PMM` and
`senvcfg.PMM` and had no hypervisor. The hypervisor phase (8) implemented
`hstatus` and was not thinking about pointer masking. Neither phase's
checklist contained it.

An extension-by-extension plan has exactly this blind spot: obligations
created by *combinations* belong to no phase.

**Resolution.** `pointer_mask_len()` now takes `as_guest` and reads HUPMM
for a guest access. The commit notes the consequence of not doing so:
"using menvcfg there would have let the hypervisor's S-mode masking
silently rewrite guest pointers." Comment at `src/mmu.cpp:258`.

**Evidence.** `2667bf1`.

---

<a id="bug81"></a>
## 81. spike leaves `hstatus.HUPMM` read-only zero — recorded, not worked around

**Symptom.** With [Bug 80](#bug80) fixed, DoomV matches Sail and differs
from spike.

**Root cause.** spike does not implement `hstatus.HUPMM`. This is a gap in
a *reference*, not in DoomV.

**Why this entry exists.** Because the tempting response — relax the test
so both references pass — destroys the evidence. `2667bf1` introduces a
`KNOWN_DIVERGENCES` table in `compare.py` specifically to avoid that:

> compare.py grows a KNOWN_DIVERGENCES table for the one case left where a
> *reference* departs from the architecture: spike leaves hstatus.HUPMM
> read-only zero. It is reported and does not fail the run, and a stale
> entry is reported too, so a reference's gap stays visible instead of
> being hidden by weakening the test.

The stale-entry check is the part that makes it a mechanism rather than a
suppression list. If spike ever implements HUPMM, the harness says
`STALE KNOWN DIVERGENCE: ... no longer differs -- drop the entry` rather
than silently continuing to excuse a difference that no longer exists.

The table entry itself, in
`tools/verification/tests/differential/vector/compare.py:732`, carries the
full argument rather than a one-line note — including why DoomV follows
Sail here and that this suite matches Sail on all 22 tests.

**Evidence.** `2667bf1`. Final state: 19/19 suites match Sail with no
divergences; 19/19 match spike with this one.

**Cross-references.** This is category 3 of [the taxonomy](#taxonomy) —
a genuine reference gap — and it is the only category-3 instance in the
project.

---

<a id="bug82"></a>
## 82. The golden reference could not run without the model it outranks being installed

**Symptom.** A Sail run invoked `spike_sig.sh`.

**Root cause.** `run_diff.sh` invoked `spike_sig.sh` on *every* run,
including Sail runs, because the ELF build and the per-test `-march` table
lived inside it.

So making Sail the golden reference did not, on its own, make Sail
independent. The model that had just been demoted was still a hard
dependency of the model that had replaced it.

**Why it matters.** Not for the obvious reason. The problem is not the
install requirement — it is that a shared script means a change made for
one reference's benefit silently affects the other's runs, and the whole
value of a second oracle is that it is independent.

**Resolution.** The ELF build moves to `build_elf.sh`. `run_diff.sh` builds
once and calls exactly one reference. A Sail run now never touches spike.

**Evidence.** `ee9cdeb` (2026-09-07). spike moves behind `--ref spike` and
"is still worth running -- an independent implementation disagreeing is a
signal even when it turns out to be the one that is wrong -- but it no
longer decides anything."

---

<a id="bug83"></a>
## 83. A timeout is the normal ending, and reading it as the result fails every passing test

**Symptom.** Every passing test reported as "timed out".

**Root cause.** DoomV writes its signature on reaching the `-break` address
and then **keeps its SDL window open** rather than exiting. It is a GUI
emulator; not exiting is the correct behaviour for it. So every harness run
has to be killed, and every run therefore ends in a timeout.

**Why it is a real trap.** Because exit status is the universal convention
for "did this work", and here it carries no information whatsoever. Reading
it the normal way inverts the entire result set.

There is a second-order cost too, recorded in `archtest.py`: naively
running with a timeout and waiting means *every* test, passing or not,
burns the full budget. At even 150 seconds each, 663 tests is over a day.

**Resolution.** The verdict comes from whether `signature.log` appeared,
never from the exit status. `archtest.py` polls for the file and kills
DoomV as soon as it has finished writing it, with the timeout kept only as
the backstop for a test that never reaches the halt address at all.

One further detail in that polling loop: it waits for the file *size to
stop changing* before killing, "or a large signature gets truncated
mid-write and the diff blames DoomV for a harness race."

**Evidence.** `ee9cdeb`, and
`tools/verification/tests/archtest/README.md`, which leads its
"Things that are not obvious" section with it.

---

<a id="bug84"></a>
## 84. Substituting `riscv64-linux-gnu-gcc` 15.2.0 makes Sail's *own* reference run die in a trap loop

**Symptom.** With the "correct" compiler version, the tests compile
successfully and then Sail's own reference run dies in a trap loop at
`rvtest_boot_to_smode`, before reaching the first test case.

**Root cause.** ACT4 checks the toolchain version and asks for GCC 15 or
later. Ubuntu packages no newlib toolchain that new. The obvious shortcut
is to point the framework at `riscv64-linux-gnu-gcc` 15.2.0, which
satisfies the version check.

It is the wrong compiler. A Linux-targeting toolchain produces test
binaries whose startup does not work under the bare-metal boot sequence the
framework's assembly expects.

**Why this is the most dangerous entry in Part V.** Because the failure
does not point at the compiler. It points at *Sail*, or at the test, or —
worst — at DoomV, since the natural next step from "the reference run
fails" is to look at what the reference is being compared against.

An unstated environment deviation is what turns a pairing artifact into a
phantom DoomV bug. That sentence is in the commit, and it is the reason
this whole section of the README exists.

**Resolution.** The GCC floor is lowered to the newlib 14.2.0 toolchain
Ubuntu actually packages, **in the open**, in `setup.sh` and `README.md`.

Verified both ways on Zicond:

| compiler | Sail reference run |
| --- | --- |
| newlib `riscv64-unknown-elf-gcc` 14.2.0 | SUCCESS |
| `riscv64-linux-gnu-gcc` 15.2.0 | trap loop at `rvtest_boot_to_smode` |

**Evidence.** `ee9cdeb`, and
`tools/verification/tests/archtest/README.md`.

---

<a id="bug85"></a>
## 85. Sail must be pinned to 0.13.1, in a second build, and the deviation must be written down

**Symptom.** ACT4 refuses to run against the Sail build the differential
harness uses.

**Root cause.** ACT4 checks the reference model version exactly and refuses
anything else. The 4.0.0 tag wants Sail 0.10; the `act4` branch wants
0.13.1. The differential harness uses 0.14.

**Resolution.** A separate 0.13.1 build at `/root/build/sail-0131`,
alongside the 0.14 the differential harness uses. The arch-test submodule
moves to the `act4` branch, which is where the RVA23S64 configuration and
the 0.13.1 pairing live.

The reason the pin is documented rather than just applied: "The pin is what
keeps a mismatch attributable to DoomV instead of to a framework/model
pairing artifact."

**Evidence.** `ee9cdeb`,
`tools/verification/tests/archtest/README.md`.

**Cross-references.** `804988f` had already retargeted the arch-test
submodule once — from the abandoned `old-framework-3.x` branch to `main`,
where the profile configurations live. It was uninitialised and referenced
by nothing at the time, so nothing depended on the old pin.

---

<a id="bug86"></a>
## 86. Both harnesses share `./signature.log`, and releasing the lock too early misattributes a run

**Symptom.** One run's output attributed to another test.

**Root cause.** DoomV hardcodes `signature.log` relative to its own working
directory, so both the differential harness and the arch-test harness write
to the same path.

Taking a lock for the duration of the *run* is not enough. Releasing it
before claiming the file leaves a window in which another run's output can
land there and be picked up as this test's result.

**Why it matters more than a normal race.** Because the failure does not
look like a collision. It looks like a wrong answer — this test's expected
signature versus some other test's actual output — and it is
non-deterministic, so it appears and disappears.

**Resolution.** `archtest.py` takes the same `.signature.lock` as
`run_diff.sh` and holds it across the **move**, not just the run. The lock
is a `mkdir`, because "mkdir is atomic even over a Windows filesystem,
where flock is not dependable".

**Evidence.** `ee9cdeb`, `archtest.py`,
`tools/verification/tests/archtest/README.md`.

---

<a id="bug87"></a>
## 87. Sail writes 64-bit signature words and DoomV writes 32-bit

**Symptom.** Every signature mismatches.

**Root cause.** Different natural word widths in the two dumps.

**Why it is recorded.** For the reason the README gives: "comparing at
different widths is how a byte-order bug hides." A naive comparison at
mismatched widths does not merely fail — it fails in a way that could
equally be an endianness defect, so the diagnosis has two candidates and
one of them is a real class of emulator bug.

**Resolution.** `archtest.py` recombines DoomV's pairs into doublewords
before comparing, so both sides are diffed at the same width.

**Evidence.** `tools/verification/tests/archtest/README.md`.

---

<a id="bug88"></a>
## 88. 65 files git records as symlinks arrived as text files containing their targets

**Symptom.** The assembler reports `unknown pseudo-op: '..'`.

**Root cause.** The checkout is on a Windows filesystem with
`core.symlinks` false. Each of the 65 files git records as a symlink arrived
as a plain text file whose contents are the target's path. The assembler
then tried to assemble a relative path as source.

**Why the error is unhelpful.** `unknown pseudo-op: '..'` names a token
from the *path string* — the leading `..` of a relative target — and says
nothing about symlinks. It reads as a corrupt source file.

**Resolution.** `setup.sh` copies each target's contents over its pointer,
which works from both sides of the WSL boundary in a way a real symlink
would not — the checkout is read from Windows and the toolchain runs in
WSL, and a Windows symlink and a WSL symlink are not the same object.

**Evidence.** `tools/verification/tests/archtest/README.md`.

**Cross-references.** [Bug 26](#bug26), [Bug 73](#bug73). Three separate
consequences of the same environment fact, each requiring a different
remedy: a `.gitattributes` for this repository's own files, a script
rewrite for a submodule's file, and a materialisation pass for a
submodule's symlinks.

# Part VI — riscv-arch-test: the certification suite

The hand-written differential suites and the certification suite answer
different questions, and the archtest README states the difference in the
sharpest available terms:

> The differential suites are hand-written: they cover what I thought to
> test, which is exactly their limitation as evidence. This suite is the
> architecture's own certification tests, and its pass criterion is
> literally "produces the same signature as the reference model" -- which
> is what makes a claim like *matches Sail* mean anything.

The result is stark. **All 19 hand-written suites passed throughout.** Not
one of the bugs in this part was reachable by them. Several — PMP,
mstatus.SD, physical memory attributes, `mstatus.TVM` — are entire features
that did not exist, and the suite that had been declared comprehensive
noticed none of them.

`a27950a` puts it plainly for the floating-point group: "Three real defects
first, all found by the certification suite and none reachable by the
hand-written suites, which pass throughout."

<a id="bug89"></a>
## 89. `FCVT.D.S` never touched fflags

**Symptom.** Signature mismatch on `D-fcvt.d.s-00` against Sail.

**Root cause.** Widening single to double is exact and never rounds — which
is what the code reasoned about, and it is true. But a **signalling** NaN
input still raises invalid, because quieting a signalling NaN is precisely
the event NV reports. The conversion canonicalised the NaN silently.

**Why it went unnoticed.** Because the reasoning was correct as far as it
went. "This operation cannot round, therefore it cannot be inexact,
therefore it raises no flags" is sound for four of the five flags. NV is
not about rounding.

**Resolution.** `FCVT.D.S` raises NV for a signalling NaN input. Comment at
`src/extensions/ext_d.cpp:259`: it is "the event NV exists to report.
Canonicalising it silently, as..." — and at line 263, "Found by
riscv-arch-test D-fcvt.d.s-00 against Sail".

**Evidence.** `a27950a` (2026-09-07).

**Cross-references.** [Bug 48](#bug48) is the mirror image — raising NV
where a quiet NaN does not warrant it. Both come from the same imprecision
about what NV means. Getting one right did not get the other right, because
they are errors in opposite directions.

---

<a id="bug90"></a>
## 90. Float-to-integer conversions range-checked the *unrounded* value

**Symptom.** Signature mismatches on `D-fcvt.lu.d-00` and its relatives.

**Root cause.** The architecture **rounds first and then checks range**.
The implementation checked range first. Those two orders disagree at both
ends of the range, in opposite directions:

- `-0.5` converted to unsigned: rounds to `-0.0`, which is *in range* and
  merely inexact. The unrounded value is out of range, so it was reported
  **invalid**.
- `2147483647.6` converted to int32: rounds to 2^31, which is *out of
  range*. The unrounded value is in range, so it was **admitted and cast**
  — which is undefined behaviour.

**Why it went unnoticed.** Both cases sit within one ulp of a boundary.
Every value more than half an ulp from the limit gives the same answer
under either order, which is to say: everything a real program converts.

A conformance suite aims at exactly these points, and nothing else does.

**Resolution.** Round, then range-check. Comment at
`src/extensions/ext_fp_common.hpp:374`: "Found by riscv-arch-test
D-fcvt.lu.d-00 and friends against Sail."

**Evidence.** `a27950a`.

**Cross-references.** The fourth bug in the float-to-integer path.
[Bug 9](#bug9) (UB above 2^63), [Bug 53](#bug53) (inexact never reported),
this one, and then [Bug 94](#bug94), which stops computing conversions the
way that produced all three. Four bugs in one function over six weeks is
not four unrelated mistakes; it is a signal about the approach.

---

<a id="bug91"></a>
## 91. `collect_fflags` read MXCSR directly, and MinGW's double `fma()` never touches it

**Symptom.** Every `FMADD`/`FMSUB`/`FNMADD`/`FNMSUB.D` reported **no
exceptions whatsoever** — inexact and invalid alike.

**Root cause.** `collect_fflags` read MXCSR directly. MXCSR covers SSE
arithmetic. MinGW's double-precision `fma()` is a **software routine** that
never writes MXCSR at all.

So the flags for an entire instruction family were being read from a
register that instruction family never writes.

**Why it went unnoticed.** Single-precision `fmaf()` happens to go through
SSE and looked fine. So half the family worked, which is exactly the
pattern that makes a defect look like an edge case rather than a structural
problem.

The hand-written suite dumps accrued flags, so the missing sets were masked
by the legitimate sets around them — the same masking as
[Bug 48](#bug48) and [Bug 53](#bug53).

**Resolution.** Read `<cfenv>` instead of MXCSR, since on this toolchain
`<cfenv>` covers the x87 status word *and* MXCSR.

Two things about this fix are worth keeping.

First, it *revises a decision recorded in that same file* — an earlier
change had deliberately bypassed `<cfenv>` for direct MXCSR access, and
that change had fixed real, reproducible bugs. The note was **updated
rather than deleted**, because its reasoning was sound for what it was
responding to. Both comments now stand in
`src/extensions/ext_fp_common.hpp`, and reading them in sequence is a
better record than either alone.

Second, it fixes the *shared cause* rather than the instance:
"`std::llrint` had already been caught doing the same thing, and was worked
around one call site at a time; this fixes the shared cause instead." That
earlier workaround is [Bug 53](#bug53).

**Evidence.** `a27950a`. Comment at `ext_fp_common.hpp:99`: "Found by
riscv-arch-test D-fnmadd.d-* against Sail."

Those three fixes together — [Bug 89](#bug89), [Bug 90](#bug90) and this
one — took the D/F families from **103 failures to 78**.

---

<a id="bug92"></a>
## 92. RMM has no x86 encoding at all — 390 cases in a single file

**Symptom.** 78 failures that would not move.

**Root cause.** RISC-V has five rounding modes. MXCSR has four.
Round-to-nearest-ties-away (RMM) has no encoding on x86 at all.

`host_round_mode()` mapped it to `FE_TONEAREST` — round-to-nearest-even —
and documented the approximation as "a defensible simplification since RMM
is rarely used in practice and only differs from RNE on exact halfway
ties".

That is true of real programs and false of a conformance suite. RMM and RNE
differ on **exactly the cases a conformance test aims at**: every exact tie,
off by one ulp.

**One arch-test file carries 390 RMM cases.**

**Why it went unnoticed.** Because the approximation was correct for every
consumer the emulator had ever had. DOOM does not use RMM. Linux does not
use RMM. glibc does not use RMM. The rounding mode exists for
decimal-arithmetic and financial-rounding use cases that this machine never
saw.

And it was *documented as an approximation*, in the source, honestly. It
was not a hidden defect — it was a known simplification whose cost was
believed to be near zero and turned out to be 390 test cases.

**Resolution.** Not fixable inside the wrapper. See [Bug 94](#bug94).

**Evidence.** `a27950a`, and the surviving comment in
`src/extensions/ext_softfloat.hpp`:

> RMM cannot be expressed on x86. RISC-V has five rounding modes; MXCSR has
> four. Round-to-nearest-ties-away has no encoding, so it was approximated
> by round-to-nearest-even, which differs on exactly the cases a
> conformance test aims at -- every exact tie, off by one ulp. A single
> arch-test file contains 390 RMM cases.

---

<a id="bug93"></a>
## 93. Host exception flags are not trustworthy on an ARM64 machine running x86-64 under Prism

**Symptom.** NX and UF silently under-reported for real test cases under
non-default rounding modes. Computed *values* were always correct.

**Root cause.** The development machine is ARM64 — a Snapdragon — so every
x86-64 binary on it, **including spike**, runs under Windows' Prism
x86-to-ARM64 translation layer. MXCSR is emulated rather than silicon, and
NX/UF do not reliably survive the translation.

spike gets exception flags right anyway, and the reason is the whole point:
it computes F and D entirely in software, via Berkeley SoftFloat, and never
touches a host FPU flag at all. DoomV's design computed via genuine host
`float`/`double` arithmetic and read the host's flags — which is exactly
the piece Prism does not faithfully preserve.

**Why it took so long to identify.** Because the symptom is
mode-dependent, flag-only, and partial. Values are right. Most flags are
right. Some flags are wrong under some rounding modes. That pattern reads
as an implementation bug in the wrapper, and it was chased as one — the
`<cfenv>`-to-MXCSR change mentioned in [Bug 91](#bug91) was part of that
chase, and it did fix real bugs without closing the gap.

The root cause was only found by chasing it "all the way down" to the host
architecture, which is not a place emulator bugs are normally found.

**Resolution.** Not fixable from inside the wrapper. See
[Bug 94](#bug94).

The original comment, which survives in
`src/extensions/ext_fp_common.hpp:53` alongside its successor, is careful
about the limits of its own claim: "On genuine x86-64 hardware this whole
class of problem shouldn't exist, since MXCSR would be real silicon, not
translated; unverified directly, no such hardware was available to test
on here."

**Evidence.** `a27950a`, `ext_fp_common.hpp`.

---

<a id="bug94"></a>
## 94. Berkeley SoftFloat: the design change, and why the integration was small

This is the headline item of the whole conformance effort, and it is a
design change rather than a bug fix. It is recorded here because it is the
resolution of [Bug 92](#bug92) and [Bug 93](#bug93), and because the
reasoning that led to it is the most transferable thing in this document.

**The situation.** After three genuine defects were fixed
([Bug 89](#bug89), [Bug 90](#bug90), [Bug 91](#bug91)), the D and F
families went from 103 failures to 78. The remaining 78 would not move,
because they were not defects in the wrapper at all. They were consequences
of two facts:

1. RMM cannot be expressed on x86 ([Bug 92](#bug92)).
2. Host exception flags are not trustworthy on this machine
   ([Bug 93](#bug93)).

Both are properties of **computing on someone else's FPU and reading its
status register**. Neither is reachable by adjusting the wrapper, because
the wrapper is not where the information is lost.

The original design — use the host's IEEE-754 hardware through `<cfenv>`
rather than a from-scratch soft-float library — was a reasonable one and
`0b7f866` defends it well. It is fast. It is almost always right. And the
project had already built substantial shared plumbing around it for
rounding modes, exception flags and NaN canonicalisation, precisely because
the host's raw behaviour is not RISC-V's.

Accumulating that plumbing was itself the signal. By the time of
`a27950a` the wrapper contained: NaN canonicalisation ([Bug 8](#bug8)),
special-cased signed-zero min/max ([Bug 34](#bug34)), a hand-written
inexact determination from values rather than flags ([Bug 53](#bug53)), an
NV rule narrowed to signalling NaNs ([Bug 48](#bug48)), a
round-then-range-check reordering ([Bug 90](#bug90)), and an entire
hand-written f16 implementation because MinGW's GCC 8.1 has no `_Float16`
on x86 at all.

That is not a thin wrapper over a hardware FPU. It is most of a soft-float
library, written incrementally, one conformance failure at a time.

**The change.** `fp_binop`, `fp_sqrt` and `fp_fma` now compute with
Berkeley SoftFloat — which is what spike uses, and why Sail and spike agree
with each other: neither touches a host FPU.

**Why the integration was small.** This is the part worth understanding,
and `src/extensions/ext_softfloat.hpp` explains it directly. SoftFloat
carries a **RISC-V specialization**, and three things follow from that:

- Its rounding enum is *numerically identical* to the RISC-V `rm` field,
  including `near_maxMag` for RMM:

  | value | SoftFloat | RISC-V |
  | --- | --- | --- |
  | 0 | `near_even` | RNE |
  | 1 | `minMag` | RTZ |
  | 2 | `min` | RDN |
  | 3 | `max` | RUP |
  | 4 | `near_maxMag` | **RMM** |

- Its exception flags are *numerically identical* to `fflags`: 1 NX, 2 UF,
  4 OF, 8 DZ, 16 NV.

- Its default NaN is *already* RISC-V's canonical one.

None of that is coincidence. So all three are **passed straight through
rather than translated** — and the file says why: "A translation table here
would be code that looks meaningful and can only introduce bugs."

The explicit NaN canonicalisation added back in [Bug 8](#bug8) was
**deleted** for the same reason. Correct code that is now redundant is not
harmless; it is a second place the canonical NaN is defined, which can
drift.

**The library is spike's own vendored copy via the existing submodule**,
not a second checkout, "so the two cannot drift apart."

**Result.** riscv-arch-test D and F: **103 failures → 0. 196 of 196
matching Sail.**

**Evidence.** `a27950a` (2026-09-07), `src/extensions/ext_softfloat.hpp`,
`src/softfloat/`.

**What was deliberately left.** Conversions still compute on the host, and
the commit says so and says why: "they pass today because the three fixes
above cover them." That is an honest statement of a known remaining
inconsistency rather than a claim of completeness.

**Also in this commit.** `fetch.sh` for the precompiled suites the Sail
model's own test tree uses — riscv-tests, `damo-rv-priv-ats` (43 RV64
hypervisor groups, which ACT4 has no coverage for at all), and
riscv-vector-tests at the VLEN=128 that matches this machine. The reasoning
is worth keeping as a general principle: "Pre-existing coverage is the
first thing to reach for; a hand-written test of ground an established
suite already covers is weaker evidence and duplicated effort."

That is the same conclusion as the archtest README's opening, arrived at
from the other direction.

---

<a id="bug95"></a>
## 95. PMP did not exist, and the tests had been shaped around its absence

**Symptom.** PMP families: **0 of 81** passing.

**Root cause.** There was no physical memory protection at all.

**Why it went unnoticed, and this is the uncomfortable part.** The
differential tests had been *quietly configured around its absence*. Every
privilege-dropping test opens an all-permissive PMP entry at the top.

That change was made for a good reason ([Bug 75](#bug75)): the tests had
been depending on spike's permissive default without saying so, and Sail
correctly refused them. Installing a permit-all entry is what real software
does and makes the suite independent of any reference's defaults.

It is also, in `fdd74c3`'s words, "the tests being shaped to match the
implementation rather than the architecture". A suite in which every test
begins by disabling the feature cannot detect that the feature is missing.

Both readings are true simultaneously. That is worth sitting with: the
correct fix for one problem produced a blind spot for another, and nothing
about the fix was wrong.

`24a17b1` did explicitly note at the time that "DoomV implements no PMP at
all, so it ignores these writes" and put it on the known-gaps list. So it
was not hidden — it was recorded and deprioritised on the grounds that
Smpmp is optional in RVA23. The certification suite tests it anyway.

**Resolution.** `src/pmp.{hpp,cpp}` implements 16 entries with all four
matching modes, NAPOT size decoding, and the lock rules that make the
feature mean anything:

- A locked entry is immutable **even from M-mode**.
- A locked TOR entry also freezes the *previous* entry's address, since
  that address is its own lower bound.

Checks run on physical addresses after translation and raise **access
faults (1/5/7), not page faults**. The distinction is stated in `pmp.hpp`:
"a page fault invites the supervisor to fix a mapping and retry, while an
access fault says the region is unreachable at this privilege however the
tables are arranged."

And the default is now the architectural one: **with no entries configured,
S and U mode reach nothing.** The commit calls this out as "the rule people
get backwards, and it is why a bare-metal test that drops privilege has to
program an entry first" — which is exactly what [Bug 75](#bug75) had
discovered from the other side, before there was an implementation to test.

16 entries is chosen because "16 is the common choice and is what the
architectural tests assume when they walk every entry; the remaining 48
read as zero and ignore writes".

**Evidence.** `fdd74c3` (2026-09-07). **PMP families: 0 → 76 of 81.**

---

<a id="bug96"></a>
## 96. `mstatus.MPRV` was documented as "not modeled yet, since nothing needs it"

**Symptom.** riscv-arch-test tests it directly.

**Root cause.** `mstatus.MPRV` was not modelled. The comment recording that
said it was "not modeled yet, since nothing needs it until OpenSBI shows up
doing exactly that".

MPRV means: an M-mode load or store is performed as though at
`mstatus.MPP`, using that mode's translation and its permission checks. It
is how M-mode firmware safely dereferences a supervisor's pointer.

**Why it went unnoticed.** The comment is the reason. It is a correct
observation about the *guests this emulator had*, presented as a reason not
to implement something. OpenSBI did not, in fact, show up doing it, so the
condition the comment set was never met and the note stayed accurate right
up until something else needed it.

This is the [design-assumption pattern](#assumptions) at its most explicit:
the scope of "nothing needs it" was the set of programs that had been run,
and the architecture is not that set.

**Resolution.** Implemented. Fetch is **deliberately excluded** — applying
MPRV to fetch would send a machine-mode handler through the guest's
mappings while it is still executing its own code.

**Evidence.** `fdd74c3`.

---

<a id="bug97"></a>
## 97. `FSH` and `FMV.X.H` applied NaN-boxing to a raw bit transfer

**Symptom.** Zfhmin/ZfhminD: 8 of 12 passing.

**Root cause.** Both instructions applied the NaN-boxing rule. They must
not: **both are raw bit transfers**, and the specification is explicit that
`FSH` does not modify the bits it transfers and does not canonicalise NaNs.

Unboxing turned every improperly-boxed pattern into `0x7E00` on the way
out.

**Why it went unnoticed.** Because NaN-boxing is a real and correct rule
that applies to most half-precision instructions, and applying it uniformly
is the natural implementation. The exception is not visible from the
instruction's name or shape — a store and a bit-move look like the same
kind of operation as everything around them.

The rule that separates them, stated in the commit: **NaN-boxing belongs to
instructions that interpret the value as a number.** A store and a bit-move
do not.

**Resolution.** Both stop unboxing.

**Evidence.** `fdd74c3`. **Zfhmin/ZfhminD: 8 of 12 → 12 of 12.**

---

<a id="bug98"></a>
## 98. The Makefile reported success without relinking, so a stale binary was tested

**Symptom.** `make` reports success. The test run measures the previous
binary. A source edit appears to have no effect.

**Root cause.** Introduced with SoftFloat: `$(OUT)` listed only the
SoftFloat objects as prerequisites. So after editing a C++ source, `make`
found `$(OUT)` newer than every prerequisite it knew about, declared it up
to date, and reported success without relinking.

**Why it had never mattered before.** Before SoftFloat, `all` was phony and
always relinked. Nothing had ever depended on the prerequisite list being
right, so nothing noticed when it became wrong.

**Why it is dangerous.** Because it makes the build system lie about what
is being tested, and the lie is silent and looks like success. The commit
says it "cost a wrong diagnosis here before being spotted" — a real
debugging session spent on a defect that had already been fixed in source
and was still present in the binary under test.

The commit also names the ancestor of this failure: "This is the same
failure mode as the old `echo \"BUILD OK\"` masking a failed link".

**Resolution.** `$(OUT)` depends on all objects.

**Evidence.** `fdd74c3`.

**Cross-references.** [Bug 10](#bug10) is the same class six weeks earlier —
the binary under test was not the binary the source described — and there
the discovery was that "the binary used throughout prior verification
predated both problems". Twice, in two different mechanisms, in one
project.

---

<a id="bug99"></a>
## 99. `mstatus.SD` was never published

**Symptom.** Sv families: 30 failures.

**Root cause.** `mstatus.SD` is the read-only summary of the extension
state fields — one when FS or VS reads Dirty. It exists so a context switch
can test a single sign bit instead of extracting two fields, which is what
supervisors actually do.

DoomV tracked FS and VS correctly — [Bug 30](#bug30) and [Bug 31](#bug31)
were fixed weeks earlier — and then never published the summary. So every
supervisor asking the cheap question was told no extension state was live.

**Why it went unnoticed.** Because the underlying fields *were* correct.
Anything that read FS or VS directly got the right answer. Only the
derived summary was missing, and the summary exists purely as an
optimisation for software that would otherwise read the fields.

Linux boots either way — it just does the extraction. So the bug is
invisible to every guest and visible only to a test that reads the bit.

**Resolution.** Derived on read rather than stored, and the reason is a
good one: "storing it would let a write to mstatus set a summary that
contradicts the fields it summarises."

That is the same design instinct as [Bug 69](#bug69), applied in the
opposite direction — there, a derived value was wrong and needed latching;
here, a latched value would be wrong and needs deriving. The distinction is
whether the value is a *summary* of current state (derive) or a *record* of
a past event (latch).

**Evidence.** `cc7d184` (2026-09-07). **Sv families: 30 failures → 9.**

---

<a id="bug100"></a>
## 100. Zicbom did nothing at all, on the argument that a machine with no cache satisfies it trivially

**Symptom.** Cache-block management operations never faulted, whatever
their address.

**Root cause.** The file argued that a machine with no cache satisfies
these instructions trivially.

That is true of their **effect** and false of their **observable
behaviour**. A cache-block operation still translates its address and still
checks permissions. An unmapped or protected block faults whether or not
there is a cache behind it.

So `cbo.clean` on an unmapped address, or on a page with no permissions,
retired successfully.

**Why it went unnoticed.** The argument is genuinely good, and it is the
*same argument* that is correct for `fence.i` and for Svinval. `aade4aa`
defends it for `fence.i`: "Implemented as a genuine no-op, which is the
correct emulation rather than a shortcut. FENCE.I asks that instruction
fetches see prior stores; there is no instruction cache here, since every
fetch reads live guest memory, so that already holds unconditionally."
`e60ec1c` defends it for Svinval and draws the distinction that matters:
"between doing nothing because nothing is required and doing nothing
because it was never implemented; these are the first kind, and
ext_svinval.cpp exists to say so."

Zicbom looked like a third member of that set and is not. The difference is
that `fence.i` and `sinval.vma` take no address that could fault —
`fence.i` names nothing, and `sinval.vma`'s address argument is a hint
about what to invalidate rather than an access. `cbo.*` names an address it
must be permitted to reach.

Being right about a pattern twice is what makes the third case
convincing. That is worth naming as its own hazard.

**Resolution.** Zicbom translates and permission-checks.

**Evidence.** `cc7d184`.

---

<a id="bug101"></a>
## 101. Zicbom's permission rule was got wrong by reasoning from plausibility

**Symptom.** riscv-arch-test case 4 — RX permissions, all three
instructions, no fault — failed.

**Root cause.** The first attempt reasoned from plausibility: `CBO.INVAL`
can discard data, so it surely needs write permission.

It does not. The actual rule has three parts and each one differs from what
a Load or a Store would do:

1. **Read or write permission suffices** for all three instructions.
2. **The D bit is neither required nor set**, since nothing is written.
3. **A failure is reported as a store fault** regardless of which permission
   was missing.

**Why the plausible answer was wrong.** Because it reasoned about what the
instruction *does to data* rather than about what the architecture says.
Invalidating a cache line discards data, so requiring write permission
feels like the safe conservative choice. The specification says otherwise
and riscv-arch-test's case 4 says so directly.

**Resolution.** Three separate divergences from both Load and Store means
it is neither. It gets its own `AccessType::CacheBlock`.

The reasoning for a new enumerator rather than reusing one is exact:
"Modelling it as a Store demands write permission and D; modelling it as a
Load reports the wrong cause."

**Evidence.** `cc7d184`.

**Cross-references.** [Bug 62](#bug62)'s `hlvx` is the same judgement made
correctly the first time — it asks for `AccessType::Fetch` rather than
`Load` because it probes execute permission, and using `Load` "would
wrongly succeed on a read-only page and wrongly fail on an execute-only
one."

---

<a id="bug102"></a>
## 102. No physical-memory-attribute checking: unbacked reads returned zero

**Symptom.** An access to an address no RAM or device answers returned zero
on read and vanished on write.

**Root cause.** No PMA checking existed at all.

**Why this is the most forgiving possible behaviour, and why that is bad.**
The commit states it directly: "That is the most forgiving behaviour
available and it hides precisely the bugs this matters for: a wild pointer
or a page table pointing into nothing both simply appeared to work."

An emulator that silently absorbs bad accesses is an emulator in which
memory-safety bugs in the *guest* are undetectable. Every category of wild
pointer produces plausible behaviour.

**Why it went unnoticed for so long.** It was noticed. [Bug 58](#bug58)
found it on 2026-09-06, correctly diagnosed it, and deliberately deferred
it:

> That leaves a known gap, deliberately not fixed here: DoomV does not
> raise access faults for unbacked physical addresses. It is a real
> conformance issue, but it would touch every memory access Doom and Linux
> make and belongs in its own change with its own regression.

That was the right call. The fix does touch every memory access, and it
converts previously silent accesses into traps, so a single missing region
in the backing map would break the Linux boot rather than fail a test.

**Resolution.** `Memory::is_backed` lists the regions the read and write
paths already decode. `translate_or_trap` consults it **before PMP** — an
address nothing answers is an access fault regardless of what PMP would
have said.

Comment at `src/memory.hpp:89`: "not a silent zero. Reads used to return
zero and writes used to" vanish, which hid "exactly the bugs this matters
for -- a wild pointer, or a page table" into nothing.

**Evidence.** `936df17` (2026-09-07).

The verification chosen is the one that matters for a change of this shape:
"Verified Linux still boots to /bin/sh in 238 lines, which is the check
that matters for this change -- it turns previously silent accesses into
traps, so a missing region here would show up as an early boot failure
rather than as a test result."

---

<a id="bug103"></a>
## 103. A page-table walk into unbacked memory reported a *page* fault where the architecture requires an *access* fault

**Symptom.** A dozen riscv-arch-test cases require a store **access** fault
and got a **page** fault.

**Root cause.** This is the second-order consequence of [Bug 102](#bug102)
and it is genuinely non-obvious.

Skipping the PMA check on a page-table walk does not merely miss a fault.
The chain is:

1. The implicit read of the PTE returns **zero**, because unbacked reads
   returned zero.
2. A zero PTE has V clear, so it **looks like an invalid PTE**.
3. The walk therefore reports a **page fault**.

Where the architecture requires an **access fault**.

Those say different things to a supervisor. A page fault invites it to fix
a mapping and retry — which it will do, and the retry will fail identically,
forever. An access fault says the physical region is unreachable however
the tables are arranged. The wrong one "sends it off to repair something
that was never broken."

**Why it went unnoticed.** Because a fault *was* raised, and it was raised
at the right instruction with the right address. Only the cause number was
wrong, and only in a case — a page table pointing at nothing — that a
correct guest never produces.

riscv-arch-test probes it deliberately: `RVMODEL_ACCESS_FAULT_ADDRESS` is
physical address 0, and a dozen tests map a valid, permissive PTE onto it
and require a store access fault.

**Resolution.** The page-table walk checks `is_backed` too. Comment at
`src/mmu.cpp:152`: "Without this the read silently returns zero, the PTE
looks" invalid.

**Evidence.** `936df17`.

**Cross-references.** This is the second time in the project that a missing
check produced a *plausible wrong cause* rather than a missing fault.
[Bug 109](#bug109) is the third, and there the wrong cause comes from
check *ordering* rather than from a check being absent.

---

<a id="bug104"></a>
## 104. PMP was checked on the final address but not on the walk's own reads

**Symptom.** riscv-arch-test PMP and Sv families.

**Root cause.** PMP was checked on the final translated physical address
and not on the implicit reads the walk makes to fetch each PTE.

The architectural rule is precise and slightly counterintuitive: an
implicit page-table read is **the hardware's access rather than the
program's**, so it is checked at **supervisor privilege** whatever mode
made the original access. M-mode's exemption from unlocked entries does not
extend to it.

**Why it went unnoticed.** Because PMP had existed for exactly one commit
([Bug 95](#bug95)) when this was found. The initial implementation checked
"every access" in the sense of every access the *program* makes, which is
the natural reading and is not what the architecture means.

**Resolution.** The walk's reads are PMP-checked, at supervisor privilege.

**Evidence.** `0543dfa` (2026-09-07). **663-test suite: 30 failures → 14**
across this commit's three fixes.

---

<a id="bug105"></a>
## 105. Non-leaf PTEs were not validated for reserved bits

**Symptom.** Same commit, same suite.

**Root cause.** A, D and U belong to *leaf* PTEs. Svpbmt's memory-type bits
likewise. Carrying any of them in a **pointer** PTE — a non-leaf entry that
names the next level of the table — is a reserved encoding that must fault.

Non-leaf PTEs were not validated at all.

**Why it went unnoticed.** [Bug 55](#bug55) added reserved-bit checking for
PTE bits 60:54, which is the check needed to make Svnapot and Svpbmt
meaningful. That check was correct and applied to leaves, where the
attribute bits live. The non-leaf case is a *different* set of bits being
reserved in a *different* position of the walk, and nothing about the first
check suggests the second.

**Resolution.** Non-leaf PTEs validated. Comment at `src/mmu.cpp:422`:
accepting them "silently accepts a page table that names attributes it has
no way" to apply, "and walks it anyway".

**Evidence.** `0543dfa`.

---

<a id="bug106"></a>
## 106. `mstatus.TVM` did not exist

**Symptom.** Same commit, same suite.

**Root cause.** `mstatus.TVM` makes `SFENCE.VMA` and `satp` illegal in
S-mode. It did not exist.

**Why the obvious dismissal is wrong.** The obvious argument is the one
this project had already used correctly three times: there is no TLB here
to flush, so a fence is a no-op, so trapping it changes nothing.

The commit rejects that directly: "the point is not the fence -- there is
no TLB here to flush -- but that a hypervisor running a guest supervisor
traps on both and so sees every attempt the guest makes to manage its own
translation. A hart that quietly succeeds reports that nothing happened."

TVM is not about the fence's *effect*. It is about **observability**: it is
the mechanism by which a hypervisor intercepts a guest's translation
management. A hart that lets the guest through gives the hypervisor no way
to know it happened.

**Why it went unnoticed.** Because nothing had run a hypervisor. The whole
H extension was implemented in phases 8a–8d and verified against spike, and
none of that verification involved a *guest supervisor managing its own
page tables* — which is the one workload TVM exists for.

**Resolution.** Implemented in `csr_access_permitted()` — it closes `satp`
to S-mode, reads included — and in the fence path. Comment at
`src/extensions/ext_zicsr.cpp:398`.

**Evidence.** `0543dfa`.

**Cross-references.** [Bug 78](#bug78) is the other place the absent TLB
made a real architectural obligation invisible. And [Bug 110](#bug110) is
the direct sequel: TVM was implemented for `SFENCE.VMA` and not for
`SINVAL.VMA`, which leaves exactly the hole TVM exists to close.

---

<a id="bug107"></a>
## 107. Instruction fetch translated once and read four bytes

**Symptom.** riscv-arch-test failures on tests that place an instruction
across a page or PMP boundary.

**Root cause.** Instruction fetch translated once, at `pc`, and then read
four bytes.

A four-byte instruction may straddle a page boundary or the edge of a PMP
region. With the C extension an instruction can begin at **any even
address**, so this is ordinary rather than exotic.

The old shape took the second half from whatever followed the first page,
silently — no fault, no translation of the second half, just bytes from
wherever the first page's mapping happened to lead.

**Why it went unnoticed.** Because guests do not usually place instructions
across page boundaries in a way that matters. When they do — and with C
they do constantly — the second page is almost always mapped, contiguous,
and holds exactly what the naive read produced. The bug only bites when the
second half is *not* reachable, which a working program never arranges.

**Resolution.** Fetch goes a **halfword at a time**, which is the unit the
architecture checks, with the length read from the low two bits of the
first halfword so the second fetch only happens when there is a second
halfword.

**Evidence.** `b9474a0` (2026-09-07). **663-test suite: 14 failures →
~10.** Comment at `src/doom_system.cpp:191`: the second half used to come
from "what happened to follow the first page, silently."

---

<a id="bug108"></a>
## 108. `translate_or_trap` had no size parameter, so every straddle check was dead code

**Symptom.** No PMP straddle denial ever fired.

**Root cause.** `translate_or_trap` took an address and no size. Callers
that were checking a wide access effectively passed 1.

PMP denies an access straddling the edge of a region **even when both sides
would permit it**, and physical memory attributes apply to the whole access.
Passing 1 for a wider access skipped both checks — so `pmp::check`'s
straddle logic, which was written and looks correct, could never fire.

**Why it went unnoticed.** Because the straddle logic *existed*. Reading
`pmp.cpp` shows a correct implementation. The defect is in the interface
between it and its callers, which is the hardest place to see a bug: both
halves look right, and the information is lost in transit.

**Resolution.** `translate_or_trap` grows a size parameter. Comment at
`src/riscv_core.hpp:55`: PMA checks "apply to the whole access. Passing 1
for a wider access silently" skipped them.

**Evidence.** `b9474a0`.

---

<a id="bug109"></a>
## 109. Atomics were never alignment-checked, and the check order was got wrong twice

**Symptom.** Atomic accesses to misaligned addresses simply happened.

**Root cause (the easy half).** Every A-extension access must be naturally
aligned. DoomV checked nothing and quietly did the access.

Unlike an ordinary load or store this is not softened by Zicclsm: an atomic
spanning two naturally-aligned units is not something hardware can do, so
the architecture requires the exception rather than emulating it. See
[Bug 42](#bug42) for why the ordinary-load case is different — there,
supporting misaligned accesses natively is a legal choice.

**Root cause (the hard half).** Two details are not what they look like,
and **both were got wrong before being read off the reference** — the
second one twice, in opposite directions, across two commits.

*The cause.* It is an **access** fault (5/7), not address-misaligned (4/6).
The A extension explicitly permits either — "an address-misaligned
exception or an access-fault exception will be generated" — and the access
fault is the one to raise when the misaligned access is not going to be
emulated, which is this machine. Cause 4/6 **looks like the more specific
answer** and is a mismatch.

*The order.* `b9474a0` put translation first, on the evidence that an
atomic to an unreachable address reported the access fault where checking
alignment first gave cause 6 against Sail's 7. That produced the right
observable for the wrong reason: the reference was reporting an access
fault because of the *misalignment*, not because of the unreachability.

`10d7ed1` then reversed it, and this is the ordering that is correct: the
misalignment check comes **before** translation. An atomic to an address
that is both misaligned and unmapped reports the **access fault**, not the
page fault — the misalignment follows from the effective address alone and
never reaches the page tables.

**Why the intuition is wrong.** The commit names the trap precisely:

> The previous commit had this ordering backwards, having reasoned from the
> synchronous exception priority table, which puts address-misaligned above
> access fault and so suggests exactly the wrong thing here.

The priority table ranks *which exception wins when several are raised*.
Reading it as a prescription for *what order to perform checks in* is a
natural mistake and produces the opposite of the right answer, because here
the misalignment does not raise an address-misaligned exception at all — it
raises an access fault, which the table ranks below the page fault that
translation would have produced.

Two wrong turns from the same source: first the cause (specific-looking and
wrong), then the order (derived from a table that answers a different
question).

**Resolution.** In `src/extensions/ext_a.cpp`: misalignment checked first,
raising `CAUSE_LOAD_ACCESS` (5) for LR and `CAUSE_STORE_ACCESS` (7) for
everything else, before `translate_or_trap` is called.

**Evidence.** `b9474a0` (first attempt, right order for the wrong reason),
`10d7ed1` (2026-09-07, corrected). The latter records the result:
"ExceptionsSvZaamo/ExceptionsSvZalrsc M-mode variants now pass."

**Cross-references.** This is the third bug in the project where a missing
or misordered check produced a **plausible wrong cause** rather than a
missing fault — [Bug 103](#bug103) and [Bug 101](#bug101) are the others.
It is also the closest relative of the failures that [remain](#remains),
which are all exception priority when a fault is over-determined.

---

<a id="bug110"></a>
## 110. `SINVAL.VMA` did not obey `mstatus.TVM`

**Symptom.** riscv-arch-test `Svinval_mstatus_tvm`.

**Root cause.** [Bug 106](#bug106) implemented `mstatus.TVM` for
`SFENCE.VMA` and not for `SINVAL.VMA`.

Trapping `SFENCE.VMA` under TVM while letting `SINVAL.VMA` through leaves a
guest supervisor a way to do the same job **unobserved — in bulk**, which
is precisely what Svinval exists for. The architecture governs both by the
same rule.

`SFENCE.W.INVAL` and `SFENCE.INVAL.IR` are deliberately **not** affected:
they only order the invalidations around them and name no address, so there
is nothing for TVM to observe.

**Why it went unnoticed.** Because TVM was implemented as a property of
`SFENCE.VMA` and `satp` — the two things the field's description names —
rather than as a property of *guest translation management*. Svinval is a
separate extension implemented three phases earlier, in a separate file,
correctly retiring without effect ([Bug 55](#bug55)), and nothing connected
the two.

It is the same shape as [Bug 80](#bug80): an obligation created by the
combination of two extensions, belonging to neither one's checklist.

**How it was found, which is the good part.** The arch-test signature
records the **trapping instruction word**. Sail trapped on `0x16000073`
(`SINVAL.VMA`); DoomV trapped on `0x12000073` (`SFENCE.VMA`). The
signature named the missing case exactly.

That is worth recording as a property of good test design: a signature that
carries *which instruction faulted* rather than merely *that something
faulted* turns a count mismatch into a diagnosis.

**Resolution.** `SINVAL.VMA` obeys `mstatus.TVM`.

**Evidence.** `7d1078f` (2026-09-07). The commit is explicit that the suite
is not yet clean: "That suite still differs on a remaining count and is not
yet clean." See [What remains](#remains).

# Part VII — Cross-cutting

<a id="patterns"></a>
## Recurring patterns

Reading 149 bugs in order, the same small number of mechanisms account for
nearly all of them. They are listed here in rough order of how much they
cost.

### 1. The silent wrong answer

The dominant failure mode of this entire project. An instruction, a field,
or a check does something plausible instead of something correct, retires,
and the damage surfaces somewhere else entirely.

`f91df1e` names it as the thing differential testing is uniquely good at:
"Every bug it found was a silent wrong answer rather than a crash."

Instances: [4](#bug4) (heap over live globals), [27](#bug27) (`sh3add` as
`OR`), [28](#bug28) (`v0` instead of `vd`), [29](#bug29) (`mop` shifted),
[49](#bug49) (17 Zvbb no-ops), [51](#bug51) (e16 no-op), [52](#bug52) (14
VFUNARY0 no-ops), [62](#bug62) (`hlv` decoding as `MOP.R`), [97](#bug97)
(NaN-boxing a raw transfer), [102](#bug102) (unbacked reads returning
zero), [107](#bug107) (fetch across a page boundary).

The structural cause, in most of them, is **a dispatch that falls through
rather than rejecting.** `911be26` states the design rule that fixes it:
"Anything unrecognised is ILLEGAL rather than falling through to a
similar-looking base instruction ... an unimplemented op that silently
aliases onto another produces a wrong answer that surfaces hundreds of
instructions later, while an illegal one stops on the instruction that
caused it."

That rule was applied to the scalar decoder in `8d790c4` and **not** to the
vector `funct6` space, which then produced the same bug twice more
([49](#bug49), [52](#bug52)) across two extensions and one month.

### 2. WARL fields as the machine's self-description

A Writable-Any Read-Legal field is not merely a constrained store. It is
**how software interrogates the hardware.** A field that stores what it is
given does not fail to constrain a write; it *lies about what the machine
is*, and software then acts on the lie.

Instances: [16](#bug16) (`satp.MODE`, Linux ran under Sv57), [44](#bug44)
(Zimop writing zero), [60](#bug60) (`hstatus.VSBE`/`VGEIN`),
[61](#bug61) (`vstvec`/`vsatp` missing the clamp), [68](#bug68) (stateen
masks), [79](#bug79) (`PMM` storing the reserved value).

`2667bf1` found that **all eight** mismatches across nineteen suites were
WARL fields and nothing else. Not one arithmetic error remained by that
point. That is a striking concentration and it says something about where
the residual risk lives in a mature implementation.

### 3. The premise that expired

A decision that was correct when made, documented with its reasoning, and
became wrong when the environment changed — with nothing in the code
drifting at all.

Instances: [40](#bug40) (halting on illegal instructions, correct until an
OS with a `SIGILL` handler existed), [92](#bug92) (RMM approximated as RNE,
correct until a conformance suite arrived), [96](#bug96) (MPRV "until
OpenSBI shows up doing exactly that"), [22](#bug22)/[46](#bug46)
(`mtopi`/`stopi` "not worth the extra surface", correct until the device
tree advertised AIA).

See [Bugs that were design assumptions](#assumptions) for the full
treatment. The distinguishing feature is that **the comment is the bug** —
these are not oversights, they are arguments, and the argument is what has
to be revisited.

### 4. The constant with more than one copy

Instances: [5](#bug5) (`DOOMGENERIC_RESX` via a header nobody included),
[14](#bug14) (a fourth hardcoded `RAM_BASE`), [17](#bug17)/[43](#bug43)
(the device tree, a hand-maintained second copy of the extension table),
[86](#bug86) (`./signature.log` shared by two harnesses).

The device tree case is the instructive one because it recurred: the DTS is
not derived from `extensions.hpp`, so the two say different things about
the same machine and nothing keeps them in step. It went wrong twice
([17](#bug17), [43](#bug43)) and was fixed twice by hand.

### 5. The test that passed while measuring nothing

Instances: [35](#bug35) (empty dump reported as a match), [56](#bug56)
(pointer masking tested in M-mode), [63](#bug63) (G-stage identity mapping
made the intermediate translation unobservable — negative control failed 0
of 14), [66](#bug66) (labels asserting the opposite), [78](#bug78)
(`sfence.vma` where `hfence.gvma` was required, which DoomV could not
notice), [75](#bug75) (tests depending on a reference's default).

The defence that actually worked was **negative controls**: not "does the
test pass" but "does the test fail when I break the thing it claims to
check". Every RVA23 phase from 1 onward ran one, and it caught real defects
in phase 6 ([Bug 54](#bug54), where config flags were decorative) and phase
8c ([Bug 63](#bug63), where a test proved nothing).

### 6. The build system testing something other than the source

Instances: [10](#bug10) (verification had been running a binary that
predated the problems), [98](#bug98) (`$(OUT)` prerequisites, "cost a wrong
diagnosis here before being spotted").

Both are the same failure: `make` reports success, the harness runs, and the
results describe a binary the source no longer produces.

### 7. Windows checkout semantics

Instances: [26](#bug26) (CRLF killing `set -euo pipefail`), [73](#bug73)
(CRLF shebang inside a submodule), [88](#bug88) (65 symlinks arriving as
text files), [23](#bug23) (Make truncating a recipe at cmd.exe's
8191-character limit), [25](#bug25) (`del` vs `rm -f`).

Five bugs, one environment, and each needing a different remedy because
each sits at a different point of the repository/submodule/WSL boundary.
[Bug 26](#bug26) is the nastiest of the set because it is
*checkout-dependent*: the file in the repository is correct and the file on
disk is not, so the bug appears and disappears with no change to any file
and cannot be bisected.

### 8. Reading the wrong host status register

Instances: [8](#bug8), [9](#bug9), [34](#bug34), [53](#bug53),
[91](#bug91), [93](#bug93).

Six bugs, all from computing floating point on the host and reading its
flags. Resolved by [Bug 94](#bug94), which stopped doing that.

---

<a id="reference-as-spec"></a>
## Treating a reference implementation as the specification

`2667bf1` categorises two of its eight findings under a single heading:
"Two were DoomV, and both are the same mistake -- a reference was treated as
the specification."

That mistake has three distinct forms in this project, and they are worth
separating because they have different remedies.

**Form 1: matching a reference's permissiveness.** The tests depended on
spike's permissive PMP default ([Bug 75](#bug75)) without saying so, and
that dependency was invisible for exactly as long as spike was the only
reference. The remedy is a second reference, and it is why whisper and
VeeR-ISS are in the tree.

**Form 2: shaping the implementation to what the reference happens to do.**
`menvcfg.PMM` storing a reserved value ([Bug 79](#bug79)) and
`hstatus.HUPMM` being absent ([Bug 80](#bug80)) are both this. The remedy
is a reference generated from the specification rather than an independent
reimplementation — which is the entire argument for making Sail the golden
model:

> Sail is the formal RISC-V specification: its model is generated from the
> same source the architecture is defined in, rather than being an
> independent reimplementation like spike. Where the two disagree, Sail is
> the stronger authority.

**Form 3: shaping the *tests* to what the implementation does.** PMP
([Bug 95](#bug95)) is the sharpest instance — "the tests being shaped to
match the implementation rather than the architecture" — and it is
uncomfortable because the change that produced it was itself correct. The
remedy is a suite nobody in this project wrote, whose pass criterion is
literally "produces the same signature as the reference model".

That is what the certification suite is for, and its yield is the argument:
every bug in [Part VI](#part-vi) was invisible to nineteen hand-written
suites that all passed.

The counter-discipline is not "trust Sail instead of spike". It is that a
disagreement with a reference is **evidence, not a verdict** — and
[the taxonomy](#taxonomy) is how it gets adjudicated.

---

<a id="taxonomy"></a>
## The taxonomy used for reference disagreements

When a reference model and DoomV disagree, the disagreement falls into one
of three categories. Getting the category wrong is expensive in both
directions: treating a configuration difference as a defect wastes a day
chasing a bug that does not exist; treating a defect as a configuration
difference hides it permanently.

### Category 1 — Configuration differences

The two machines are differently configured. Neither is wrong. **Stop
diffing.**

- **VLEN 256 vs 128** ([Bug 76](#bug76)). Three vector suites differed for
  this reason alone. Pinned in `mkconfig.py`.
- **GEILEN** ([Bug 77](#bug77)). DoomV has no guest external interrupt
  file, so `hstatus.VGEIN`'s read-only zero is correct; a reference
  configured with them is equally correct. Excluded from the compare.

The tell is that the difference is *uniform* — it affects every case
touching that parameter, and the values differ in a way that tracks the
configuration rather than the operation.

### Category 2 — Open architectural choices

The architecture permits more than one behaviour and the two
implementations chose differently. Neither is wrong. **Test the closed
property instead.**

- **Misaligned scalar accesses** ([Bug 42](#bug42)). DoomV supports them
  natively; spike traps. Both are conformant, and Linux agrees with DoomV
  — it probes at boot and reports "unaligned accesses are fast". The commit
  states the rule: "A differential test there compares a choice, not a
  correctness property."
- **Which legal value a WARL field substitutes** ([Bug 79](#bug79)). A
  write of a reserved PMM value must not be stored; *which* legal value
  replaces it is the hart's own choice, and the two references make
  different ones. So the test now checks the property that is not open:
  **whatever PMM reads back is the length actually in force.**
- **Counter values** (`fbdaf84`). spike counts simulated cycles, DoomV
  counts retired instructions, and neither is wrong. `vtest_csr` therefore
  "dumps no counter value at all, only the properties both must agree on:
  that a counter never runs backwards, and that reading one the enable
  chain has not permitted raises mcause=2."
- **Ssstateen mask values** ([Bug 68](#bug68)). The suite "checks SE0's
  gating behaviour rather than diffing an implementation-defined value."

The move in every case is the same: replace the observable that is open
with a nearby observable that is closed. That is a stronger test, not a
weaker one — it asserts the property that actually matters instead of an
incidental encoding of it.

Category 2 has a dangerous failure mode worth flagging: a single open-choice
difference can **desynchronise everything downstream**. [Bug 42](#bug42)
produced one genuine disagreement plus six later lines wrong, because the
handler was skipped and every subsequent result shifted by one trap. Seven
wrong lines reads as a serious defect and was one benign choice.

### Category 3 — Genuine reference gaps

The reference departs from the architecture. **Record it in
`KNOWN_DIVERGENCES` rather than weakening the test.**

There is exactly one instance: spike leaves `hstatus.HUPMM` read-only zero
([Bug 81](#bug81)). RVA23S64 mandates both H and Ssnpm, so a hart with the
pair owes the field; Sail implements it and DoomV follows Sail.

The mechanism matters as much as the judgement. From `2667bf1`:

> It is reported and does not fail the run, and a stale entry is reported
> too, so a reference's gap stays visible instead of being hidden by
> weakening the test.

The stale-entry check is what makes this a mechanism rather than a
suppression list. If spike ever implements HUPMM, `compare.py` prints
`STALE KNOWN DIVERGENCE: ... no longer differs -- drop the entry` instead
of silently continuing to excuse a difference that no longer exists.

The table entry itself carries the full argument rather than a one-line
note. That is deliberate: a suppression whose justification is not written
down is indistinguishable from a suppression that was never justified.

### The fourth category, which is not a category

**DoomV is wrong.** This is the default and it needs no machinery. The
value of the taxonomy is that it stops the other three from being
misfiled as this one — and, more importantly, stops this one from being
misfiled as one of the other three.

---

<a id="assumptions"></a>
## Bugs that were design assumptions, written down as reasoning

A recurring and distinctive feature of this codebase: several bugs were not
oversights at all. They were **arguments**, written into comments,
defending a decision — and the argument was wrong, or stopped being right.

These are harder to find than ordinary defects, because reading the code
does not reveal them. The code does exactly what the comment says it does,
and the comment explains why that is correct. Finding the bug means
disagreeing with a case that has already been made.

There are at least six.

**1. `mstatus.MPRV`: "not modeled yet, since nothing needs it until OpenSBI
shows up doing exactly that"** ([Bug 96](#bug96)).

A correct observation about the guests this emulator had, presented as a
reason not to implement an architectural feature. OpenSBI never did show
up doing it, so the condition the comment set was never met and the note
stayed accurate — right up until riscv-arch-test tested it directly.

**2. Zicbom: a machine with no cache satisfies these instructions
trivially** ([Bug 100](#bug100)).

True of their *effect*, false of their *observable behaviour*. And the same
argument is genuinely correct for `fence.i` (`aade4aa`) and for Svinval
(`e60ec1c`) — which is what made it convincing the third time. Being right
about a pattern twice is its own hazard.

The distinction `e60ec1c` draws is the right one and Zicbom is on the wrong
side of it: "between doing nothing because nothing is required and doing
nothing because it was never implemented."

**3. Zvbb: "guessing at implementations nothing exercises is how the
mask-register bug got in"** ([Bug 49](#bug49)).

A lesson correctly learned from [Bug 28](#bug28) and applied to the wrong
risk. `e414423`'s rebuttal is the sharpest single paragraph in the history:

> The caution was aimed at the wrong risk. Writing an untested
> implementation is a risk a differential test removes; a fall-through that
> makes a missing instruction indistinguishable from a working one is a
> risk nothing removes, and it is the worse of the two.

The same reasoning appears as a stated virtue in `f8fbd04`: "implementing
operations nothing exercises is how subtle bugs get in unnoticed." Both
sentences are defensible. Only one of them accounts for the dispatch
structure the code actually had.

**4. Quiet-NaN NV: "setting NV more often than required never changes the
returned comparison result"** ([Bug 48](#bug48)).

True, and beside the point. The flag is the *only* observable that
distinguishes `FEQ` from `FLT` and both from `fleq`/`fltq`. A
simplification argued to be unobservable turned out to be the single
observable separating four instructions.

**5. LCOFI derived rather than latched: "a latched edge would stay asserted
after its cause was gone"** ([Bug 69](#bug69)).

An argument from elegance — no duplicated state, no possibility of
disagreement between the interrupt and the flag — that removes a capability
software depends on. `07a0090` is unusually direct about it: "a design
decision I argued for in a comment and got backwards."

**6. RMM approximated as RNE: "a defensible simplification since RMM is
rarely used in practice"** ([Bug 92](#bug92)).

Entirely true of real programs and entirely false of a conformance suite,
which aims at exactly the halfway ties where the two modes differ. One
arch-test file carries 390 cases.

### And the mirror image

There is one instance of the opposite error: **a correct implementation
documented as a defect.** `mmu.cpp` described faulting on a clear A/D bit
rather than setting it in hardware as a "Known gap, to be revisited"
(`7bf217a`). It is not a gap. It is Svade — one of the two behaviours RVA23
permits, and the one Linux accommodates by pre-setting A/D and re-walking on
fault.

That is the same class of error as the six above, and it is worth noting
that it is *cheaper*: a comment that understates the implementation costs
somebody an afternoon, while a comment that overstates the reasoning for an
absence costs a conformance failure.

### What actually worked against this class

Not code review — the reasoning survives review, because it is reasoning.
What found these was **running an oracle that had no opinion about the
argument.** Every one of the six was found by a differential or
certification test, and four of them by riscv-arch-test specifically.

The secondary discipline, visible throughout the history, is that when one
of these was corrected the **comment was corrected too, and the old
reasoning kept where it was sound.** `a27950a` on the `<cfenv>` question:
"the note is updated rather than deleted, since its reasoning was sound for
what it was responding to." Both comments now stand in
`ext_fp_common.hpp`, and reading them in sequence is a better record than
either alone.

---

<a id="test-bugs"></a>
## Bugs in the tests, not the emulator

A striking fraction of this document is not about the emulator. Roughly a
quarter of the entries are defects in tests, harnesses, or build tooling —
and several of them were *passing tests* that proved nothing.

That is not incidental. A conformance effort's evidence is only as good as
its instruments, and an instrument that reads "correct" when disconnected
is worse than no instrument at all.

### Tests that passed while measuring nothing

- [Bug 35](#bug35) — `compare.py` reporting "MATCH: all 0 words identical"
  on an empty dump. "The most misleading thing it could do." The guard
  added to fix it then earned its keep three separate times, each recorded.
- [Bug 56](#bug56) — the pointer-masking test running entirely in M-mode,
  where masking correctly does not apply.
- [Bug 63](#bug63) — the two-stage translation test passing 14 of 14 with
  the intermediate PTE translation removed, because the G-stage
  identity-mapped the page tables. "It failed 0 before the test was
  corrected."
- [Bug 66](#bug66) — labels asserting the opposite of what was checked,
  and passing that way.
- [Bug 78](#bug78) — `sfence.vma` where `hfence.gvma` was required, which
  DoomV structurally could not notice because it has no TLB.

### Tests depending on something other than the architecture

- [Bug 75](#bug75) — eleven suites depending on spike's permissive PMP
  default.
- [Bug 42](#bug42) — comparing a legal implementation choice.
- [Bug 58](#bug58) — using "does it fault" as a proxy, and measuring an
  unrelated missing PMA check.

### Harness and environment defects

- [Bug 36](#bug36) — spike's `+signature` writing nothing, silently, while
  exiting 0.
- [Bug 74](#bug74) — hand-rolling a dump that Sail has a flag for.
- [Bug 83](#bug83) — reading a timeout as the result, which reports every
  passing test as a failure.
- [Bug 84](#bug84) — a compiler substitution that kills Sail's own
  reference run.
- [Bug 86](#bug86) — a shared output path with a lock released too early.
- [Bug 87](#bug87) — comparing 64-bit and 32-bit words, "how a byte-order
  bug hides".
- [Bug 82](#bug82) — the golden reference unable to run without the model
  it outranks.
- [Bug 98](#bug98) / [Bug 10](#bug10) — the build system handing the
  harness a stale binary.

### Test-authoring mistakes that are also supervisor mistakes

Two are worth keeping precisely because they are not test-specific:
[Bug 38](#bug38), a handler clobbering the faulting instruction's base
register, and [Bug 39](#bug39), a stack pointer outside the mapped region
causing recursive faults. `d5197a8` keeps both in the file with comments
"because both are things a real supervisor has to get right".

### The defences that worked

**Negative controls.** Every RVA23 phase ran one, and they are the reason
several of the above were caught rather than shipped. Representative
results, all from commit messages:

| control | result |
| --- | --- |
| hints suite, six extensions disabled | 10 of 18 fail |
| `vtest_csr` without zicboz | 7 of 16 fail |
| `vtest_csr` without zawrs | 5 of 16 fail |
| Zvbb dispatch reverted | 34 of 37 fail (the 3 passing are `vandn`) |
| `vtest_sv` without svinval | 10 of 16 fail |
| `vtest_sv` without svnapot | 7 of 16 fail |
| `vtest_sv` without svpbmt | 2 of 16 fail |
| pointer masking disabled | 5 of 12 fail |
| `hlv` reading `satp` instead of `vsatp` | 14 of 22 fail |
| G-stage final translation removed | 12 of 14 fail |
| intermediate PTE translation removed | 8 of 14 fail (**0 before the test was fixed**) |

**Making the harness fail loudly.** `NO DATA` on empty dumps
([Bug 35](#bug35)); a "spike did not start" check ([Bug 57](#bug57)) that
paid for itself one phase later ([Bug 70](#bug70)); a length-mismatch
failure even when the common prefix agrees.

**Writing constraints into the test file rather than the commit message.**
Repeatedly: `SIGV` must save and restore `vtype`/`vl` or its own `vsetvli`
becomes the `vtype` for the next instruction under test; a widening
operation's wide operand must be even-numbered; the mid-page fault address
must stay 8-byte aligned; DoomV's `-march` resets every extension it does
not name, so each test must spell out its full set.

---

<a id="remains"></a>
## What remains

**riscv-arch-test RVA23S64: 653 of 663 passing. 10 failures.**

That is up from 540 of 663 when the suite was first wired up. All 19
hand-written differential suites match Sail across 639 cases with no
divergences (and match spike with the one recorded in
[Bug 81](#bug81)). Linux boots to `/bin/sh` in 238 lines. DOOM runs.

The 10 remaining failures are:

| test | shape |
| --- | --- |
| `sv39_exceptions_Smode` | DoomV terminates early |
| `sv39_exceptions_Umode` | DoomV terminates early |
| `sv39_exceptions_Zaamo_Smode` | DoomV terminates early |
| `sv39_exceptions_Zaamo_Umode` | DoomV terminates early |
| `sv39_exceptions_Zalrsc_Smode` | DoomV terminates early |
| `sv39_exceptions_Zalrsc_Umode` | DoomV terminates early |
| `Svinval` (2 tests) | DoomV takes fewer traps than Sail |
| `PMPZca_misaligned_napot` | — |
| `PMPZicbo_cbo_wr_02` | — |

### The `sv39_exceptions` cluster — six of the ten, one problem

These six are S-mode and U-mode variants of the same three test bodies, and
they fail the same way: **DoomV terminates early**, writing `deadbeef` into
the remainder of the signature region where Sail records further trap
entries.

The underlying problem is **exception priority when a fault is
over-determined**. These tests construct addresses that are simultaneously
misaligned, unmapped, and outside any backed physical region — deliberately
— and then ask which exception the hart reports. Every one of those three
conditions independently justifies a fault, and they justify *different*
faults:

- misaligned → address-misaligned (4/6), or access fault (5/7) on a machine
  that will not emulate the access
- unmapped → page fault (12/13/15)
- unbacked or PMP-denied → access fault (1/5/7)

DoomV reports misaligned or page fault where Sail reports access fault. The
divergence in cause then desynchronises the test: DoomV's handler takes a
different path, the signature entries stop lining up, and the run ends
before Sail's does — which is why the symptom is early termination rather
than a clean value mismatch. That cascade is the same one
[Bug 42](#bug42) produced from a single benign disagreement.

The check-ordering work in [Bug 109](#bug109) is exactly this problem,
solved for one instruction family. It took two attempts and the second one
reversed the first, because the intuitive source of truth — the synchronous
exception priority table — answers a *different question* than the one
being asked. The table ranks which exception wins when several are raised.
It does not prescribe the order in which checks are performed, and reading
it as though it does produces the wrong answer here.

Generalising that ordering across fetch, load, store and atomic paths is
the remaining work.

### The Svinval pair

DoomV takes **fewer traps than Sail**. [Bug 110](#bug110) improved this —
`SINVAL.VMA` now obeys `mstatus.TVM`, which was found because the signature
records the trapping instruction word and named the missing case exactly —
but `7d1078f` is explicit that it is not finished: "That suite still
differs on a remaining count and is not yet clean."

### `PMPZca_misaligned_napot` and `PMPZicbo_cbo_wr_02`

Two isolated PMP cases. The first is misalignment interacting with NAPOT
region matching, which puts it in the same family as the `sv39_exceptions`
cluster. The second exercises cache-block write permissions against PMP —
the intersection of [Bug 101](#bug101)'s `AccessType::CacheBlock` and
[Bug 95](#bug95)'s PMP implementation, which are four days and one commit
apart and were never designed against each other.

### The honest summary

Ten failures, of which at least seven are one problem wearing three names.
Everything else in RVA23S64 that this configuration exercises — the base
ISA, M, A, C, F, D, the bitmanip families, Zicond, Zicbo\*, Zfa, Zfh\*, the
vector extensions, the hypervisor extension, pointer masking, the
supervisor address-translation extensions, Sscofpmf, Ssstateen, PMP,
counters and CSR privilege — matches the formal model signature for
signature.

The remaining problem is not an instruction computing the wrong number.
Every one of those is gone. It is the machine disagreeing with the
specification about **which of several simultaneously-valid complaints to
make first** — which is, appropriately, the last thing left after
everything that produces a wrong *value* has been fixed.

---

<a id="part-viii"></a>
## Part VIII — After the certification suite: the hypervisor, and 663/663

Part VII closed on ten arch-test failures and called them "the machine
disagreeing with the specification about which of several simultaneously-valid
complaints to make first". That reading was half right. Two of them were
exactly that. The rest were three more missing features and a bug in the
harness, and finding them took a change of method: diffing Sail's
`--trace-instr` and `--trace-gpr` output against DoomV's own instruction
history, instead of reading signatures and guessing.

Signature-guessing had by then produced three wrong fixes in a row —
`cbo.inval` "must need write permission", misaligned atomics "must report
misaligned", a split fetch "must name the instruction" — each plausible, each
contradicted by the reference. A trace diff names the divergent instruction
directly, and ended the guessing.

### 111. An illegal instruction halted the debugger instead of trapping
<a id="bug111"></a>

**Symptom.** Six `sv39_exceptions` tests stopped after 743 instructions,
writing `deadbeef` where the reference recorded about 61 trap records.

**Root cause.** `Debugger::should_halt` halted unconditionally on any illegal
instruction, raising the trap only if a human resumed. The reasoning was
written down and was true when written: nothing in the project had an
illegal-instruction handler, so trapping would spin re-trapping instead of
surfacing a crash log.

**Why it went unnoticed.** The premise expired silently. OpenSBI installs a
handler, Linux installs one and turns it into SIGILL, and every arch-test
image installs one and then deliberately executes an illegal instruction to
check that the trap works. Halting stops the guest at exactly the moment it is
testing that it can recover — and the emulator looks correct by its own
lights, which is why three rounds of signature inspection missed it.

**How it was found.** Diffing DoomV's instruction history against Sail's
trace: the two agreed for 743 instructions, then at #742 the test jumped to
`0x140009002`, whose encoding is `0x00000000` — `c.illegal`, on purpose — and
Sail entered the M-mode handler while DoomV simply stopped.

**Resolution.** The trap is raised inline. The halt survives as
`Debugger::break_on_illegal`, off by default, because during bare-metal
bring-up before any handler exists a crash log really is more useful than a
silent loop — which is what made the original decision right at the time.
`sv39_exceptions_Smode` went from 351 differing words to passing.

**Evidence.** `e98ef88`. Same expired-premise shape as [Bug 96](#bug96) and
[Bug 100](#bug100).

### 112. sstatus hid FS, VS, UXL and SD
<a id="bug112"></a>

**Symptom.** A page-table entry recorded with its physical page number
zeroed — an MMU bug, to all appearances.

**Root cause.** `SSTATUS_MASK` carried SIE, SPIE, SPP, SUM and MXR and nothing
else, so sstatus read zero everywhere else. sstatus is a view of mstatus and
the view is wider: FS (14:13) and VS (10:9) carry the floating-point and
vector state, UXL (33:32) the U-mode XLEN, SD (63) the summary of FS and VS.

**Why it matters beyond the test.** A supervisor decides whether a task has
live FP or vector registers worth saving on a context switch by reading FS and
VS here — it has no access to mstatus. Masking them out told every supervisor
that no extension state was ever live, which corrupts state across task
switches and surfaces much later with no obvious cause.

**How it was found.** `--trace-gpr` showed the trap handler doing
`csrrs x7, sstatus` and rebuilding a PTE out of bits 16:0. Sail read
`0x...6600` (FS=3, VS=3), DoomV read 0, and the handler faithfully stored the
PTE that follows from that. The cause was three steps upstream of the symptom.

**Resolution.** The mask carries the full view. UXL and SD are read-only
through it, via a separate `SSTATUS_WMASK`, since UXL is fixed at 64-bit and
SD is derived. **riscv-arch-test: 663 of 663, zero failures.**

**Evidence.** `895667c`.

### 113. Linux was never actually booting
<a id="bug113"></a>

**Symptom.** None, and that is the point. "Linux boots to /bin/sh in 238
lines" had been reported as a healthy result for most of a working session.

**Root cause.** The log stopped right after `Run /bin/sh as init process`
because the emulator halted there — [Bug 111](#bug111)'s debugger halt, on the
first instruction userspace executed. A halt and a successful boot produce an
identical-looking log: the output simply ends.

Fixing the halt exposed the real failure. Userspace issued a `vsetivli`; the
hart raised an illegal instruction, entirely correctly, because the device
tree never mentioned V; Linux therefore never enabled `mstatus.VS` for the
task, turned the trap into SIGILL, and killed init.

**Resolution.** Two halves of one mistake. The device tree gains `v`, `zvbb`
and `zvfhmin` — the identical omission as the earlier `zba`/`zbb`/`zbs`/
`zicond` one ([Bug 43](#bug43)), with the same signature: the guest refuses to
use a feature that is present, and the refusal looks like a guest problem. And
a Linux boot with no `-march` now selects the RVA23S64 profile, because
advertising V to a hart built with `V = false` produced a second panic, in the
kernel this time, on a `vsetvli` the hart was configured not to have.

Linux now reaches an interactive shell prompt, which it had never done.

**Evidence.** `051cdd1`.

### 114. Three HTIF bugs, all self-inflicted
<a id="bug114"></a>

Running `damo-rv-priv-ats` — the only hypervisor coverage that exists
anywhere — needed HTIF, and the first implementation had three distinct bugs
worth recording because each produced a convincing wrong answer.

**Infinite recursion.** Acknowledging a console write means storing zero to
the port, and those stores come straight back into the handler. Clearing the
low half first leaves the high half still reading device 1, command 1, so the
re-entered call saw a console packet with a zero payload, printed a NUL,
cleared again, and never stopped. It looked exactly like the guest hanging.

**A stale upper word.** `write64` decomposes into two 32-bit stores, low
first, so evaluating on the low one compares a fresh payload against the
previous packet's upper word. A carriage return — `0x0d`, odd — then reads as
device 0, command 0 with bit 0 set: an exit, with the character as its code.
This is the "subtest 36169534507319302 failed" that the very first run
reported. The check now runs only after the high half lands.

**Dumping the wrong memory.** The runner used the tohost word as its signature
range, but acknowledging a console write zeroes that word, so by the time
anything read it the verdict was gone. DoomV writes the captured value to
`tohost.log` instead.

**Evidence.** `89cbeb5`. The suites now print their own results, which names
the failing assertion directly and is how every hypervisor bug below was
found.

### 115. Seven hypervisor bits that existed and were never consulted
<a id="bug115"></a>

`riscv-arch-test` has no hypervisor tests, no testplan and no coverpoints,
while RVA23S64 requires H and the `Sh*` sub-extensions. The first damo file
found seven defects, every one a defined, writable bit that nothing read:

* **`hstatus.VTSR`** — a guest supervisor's `SRET` must trap to the hypervisor
  as a virtual instruction so the hypervisor can emulate the return. DoomV
  performed the return, to whatever `sepc` held.
* **`SRET` read the hypervisor's `sepc` and `sstatus` in VS-mode** — those
  names mean the guest's `vsepc` and `vsstatus` there, the same redirection
  every other S-mode CSR access already went through.
* **`hstatus.VTVM`** — a guest's `satp` access, `SFENCE.VMA` and `SINVAL.VMA`
  now trap. Without it the guest manages its own translation with the
  hypervisor none the wiser.
* **`hstatus.VTW`** and **`mstatus.TW`** — a guest's `WFI`. TW outranks VTW:
  when M-mode has closed WFI to everything below it, the guest gets an illegal
  instruction and the trap goes to M, not a virtual instruction handled by a
  hypervisor that is itself denied the instruction.
* **`hstatus.GVA` and `mstatus.GVA`** — whether tval holds a guest virtual
  address. One classification function serves both paths deliberately: the
  same fault reports GVA in hstatus when delegated and in mstatus when not,
  and a hypervisor reading either has to see the same answer.
* **`hgatp` MODE was not WARL** — reserved encodings were stored, so software
  probing the field was told this hart implements a second-stage mode nobody
  has defined. `mstatus.TVM` also left `hgatp` open while closing `satp`;
  M-mode withholding translation control has to withhold all of it.

Most of these trap as cause 22 rather than cause 2, and the difference carries
the meaning: illegal says the guest did something nobody may do, virtual says
it did something only the hypervisor may do and which can be emulated on its
behalf.

**Evidence.** `3d6f3b5`, `110e5d5`, `45d7308`, `97b281d`. damo-tests went from
hanging, to 5 of 43 groups, to 12 of 43.

### 116. Two-stage translation applied only to hlv and hsv
<a id="bug116"></a>

**Symptom.** `GHIGH-01: GPA bit 41 set -> load guest-page-fault` reported
cause 5 (load access fault) instead of 21.

**Root cause.** The G-stage was gated on an `as_guest` flag that only `hlv`
and `hsv` set. A guest's ordinary loads and stores never went through `hgatp`
at all — they addressed host physical memory directly. A guest could reach
anything.

**Why it went unnoticed.** `hlv` and `hsv` are the rare case, and they were
the only case tested: `vtest_hgatp` exercises two-stage translation through
exactly those instructions and passes 14 of 14. The comment in `mmu.cpp` still
read "Second-stage translation through hgatp is not applied here yet. With
hgatp left at zero the second stage is bare" — an assumption that stopped
being true the moment hgatp was implemented, in a different commit, without
this line being revisited.

**Resolution.** Two-stage translation applies to any access made in a virtual
mode. What `hlv` and `hsv` actually add is whose privilege to check against:
they execute in HS-mode on the guest's behalf, so permissions come from
`hstatus.SPVP` rather than the current mode, while a real VS access is already
running at the guest's own privilege.

**Note.** This is the fourth entry in this document — with [43](#bug43),
[96](#bug96) and [100](#bug100) — where a comment asserting "not needed yet"
outlived the condition that made it true. It is the most reliable single
predictor of a bug in this codebase.

---

<a id="part-ix"></a>
## Part IX — Widening the ISA: the crypto and half-precision extensions

663/663 on the certification suite is a statement about one ISA string. The
suites in this part measure a different one. `riscv-tests` and
`riscv-vector-tests` between them cover Zfh, Zvfh, bf16, the scalar crypto
bitmanip and the whole vector crypto family — none of which RVA23S64
mandates, and none of which arch-test has a single test for.

Two things characterise every bug in this part. The first is that they were
all invisible to the suite that was being watched: arch-test stayed at
663/663 through every one of them. The second is that most of them are
*transcription* errors rather than reasoning errors — a table copied with
its index order intact, a round number masked to the wrong width, a decoder
arm attached to the wrong dispatch table. Reasoning errors are found by
thinking harder. Transcription errors are found by diffing against the
reference, and only by that.

### 117. Headless mode exited before the signature was written
<a id="bug117"></a>

**Symptom.** arch-test dropped from 663/663 to 574/663 the moment the suites
started running headless, with 89 tests reporting a signature mismatch in
the last few words.

**Root cause.** The headless exit waited on `debugger.halted` and then called
`std::exit`. `halted` is set by the CPU thread when the guest stops; the
signature, the crash log and the tohost log are written *after* that, by the
same thread. So the process was racing its own output files and usually won.

**Why it looked like 89 architectural failures.** Because a truncated
signature is a mismatch, and a mismatch is what an architectural failure
looks like. The failures were spread across families with nothing in common,
which is the tell — 89 unrelated tests do not regress together — but the
diff for any one of them reads exactly like a real bug.

**Resolution.** A separate `run_finished` flag, set only once every output
file is closed. `halted` answers "has the guest stopped", which is not the
question. `2e8d015`.

### 118. A script that stripped debug output deleted the CSR write path
<a id="bug118"></a>

**Symptom.** Every CSR without an explicit case in `write_csr`'s dispatch
silently discarded its write.

**Root cause.** A one-off script removing debug prints turned

```cpp
else regs.write_csr(csr, updated);
```

into `else { }`. The `else` had no braces, so the print *was* the statement,
and deleting the print deleted the branch.

**Why it went unnoticed for as long as it did.** It did not: it was caught
within the same session. It is recorded because the class matters. Every
other entry in this document is a bug in code that was written; this is a
bug in code that was *edited by a program*, where the edit was syntactically
valid and semantically a deletion. A braceless `else` whose body is the line
being removed is the exact shape that lets that happen, and there are a lot
of them in this codebase.

**Resolution.** Restored, and found by grepping for the line rather than by
any test — nothing in the suites writes a CSR this machine gives no meaning
to.

### 119. A CSR that does not exist
<a id="bug119"></a>

**Symptom.** The damo hypervisor suite went from passing groups to failing
outright.

**Root cause.** `vscounteren` was invented at address 0x206 and `scounteren`
accesses in VS-mode were redirected to it, by analogy with `vsstatus`,
`vsie`, `vsip` and every other S-level CSR the H extension shadows.

There is no such register. The H extension does not shadow `scounteren`:
there is one `scounteren`, and a guest supervisor writing it writes the
same register the host supervisor reads. So the redirect sent every counter
enable the guest kernel set into a register nothing consulted, and VU-mode
was denied `cycle`, `time` and `instret` — which its own kernel had just
enabled.

**Why it looked right.** The pattern is real and nearly universal. Nine
S-level CSRs are shadowed by a VS twin. Extending it to the tenth is the
obvious generalisation, and the obvious generalisation is wrong.

**Resolution.** Reverted. The lesson is narrower than "check the spec": the
H extension's CSR list is *enumerated*, not derived, and an enumeration has
to be read rather than extrapolated from.

### 120. The reference was never consulted, because the model and its config did not match
<a id="bug120"></a>

**Symptom.** A design decision was built on the claim that Sail does not
implement Zicfilp or Zicfiss. Both are in its RVA23S64 config with
`supported: true`.

**Root cause.** The Sail binary and the configuration file being handed to
it came from different builds. The config had a `lrsc` key the installed
model's schema rejected, so every invocation failed its schema check before
executing an instruction. A model that refuses to start looks a great deal
like a model that does not implement the extension you are asking about.

**Why it went unnoticed.** Because the failure was upstream of the question.
The reference was not disagreeing; it was not running. And a claim about
what a reference model does not implement is exactly the kind of claim that
does not get double-checked, because the natural check — run it and see —
is the thing that is broken.

**Resolution.** `mkconfig.py` now generates the config from the *installed*
model's own defaults, so the pair cannot drift, and both extensions were
implemented against a working reference. `4f243be`, `bb50a2d`.

**Note.** This is the second-order version of the pattern in
[Category 3](#taxonomy): a reference gap has to be established, not
assumed. Here nothing was even established falsely — the reference had
never spoken.

### 121. Zicfilp armed a landing-pad requirement on the wrong register
<a id="bug121"></a>

**Symptom.** Ordinary un-instrumented binaries trapped with a landing-pad
fault on their first indirect call.

**Root cause.** The exemption was implemented as `rd == 0 && (rs1 == 1 ||
rs1 == 5)`. The spec's exemption is on `rs1` alone: an indirect jump through
`x1`, `x5` or `x7` is a *return*, and returns do not need landing pads
because the shadow stack already protects them.

Including `rd` in the test broke exactly one case, and it is the common
one: `jalr ra, off(ra)`, which a compiler emits for a call through a
resolved function pointer. `rd` is `ra` there, so the exemption did not
apply, so a landing pad was required at a target that no un-instrumented
binary has one at.

**Why it looked right.** `rd == 0` is what distinguishes `jr` from `jalr`
in the assembler's mnemonics, so it reads like part of "is this a return".
It is not — the architecture's return test is about which register holds
the address, not about whether the instruction also links.

**Resolution.** `rs1 != 1 && rs1 != 5 && rs1 != 7`. `4f243be`.

### 122. A shadow-stack page is not a leaf by the usual test
<a id="bug122"></a>

**Symptom.** 29 Zicfiss assertions failed, all of them on the first access
to a shadow stack.

**Root cause.** The page-table walk decides a PTE is a leaf with
`(pte & PTE_R) || (pte & PTE_X)`, which is correct for every page type that
existed before Zicfiss. A shadow-stack page is encoded as `W=1, R=0` — an
encoding that was reserved precisely so it could be given this meaning — so
it granted neither R nor X and the walk treated it as a pointer to a
non-existent next level.

**Why it went unnoticed.** Because the leaf test is not part of Zicfiss and
was not touched when Zicfiss was added. The extension's own logic — the
`sspush`/`sspopchk` instructions, the enable bits, the fault causes — was
all implemented and all correct. One line in a file the change never opened
made every bit of it unreachable.

**Resolution.** `(pte & PTE_R) || (pte & PTE_X) || ss_page`. One clause,
29 assertions. `bb50a2d`.

### 123. Zfh was wired into the wrong dispatch table
<a id="bug123"></a>

**Symptom.** `fadd.h` returned 0.

**Root cause.** `decode_zfh` was added to the OP / OP-IMM arm of the
decoder's dispatch switch. Half-precision arithmetic is in OP-FP. So
classify() correctly identified the extension, `decode()` never routed to
it, and every half op fell through to `decode_f` — where its funct7 named a
single-precision instruction, which then executed on the operands as though
they were floats.

**Why it looked right.** Because the extension *was* wired in, and to a
real arm, and the code compiled and ran. The decoder has two dispatch
tables — `classify()` maps an encoding to an extension, `decode()` maps an
opcode to a decoder — and agreeing with one of them is not agreeing with
both. Every other extension added to this decoder touched both tables in
the same commit, which is why nothing about the shape of the change looked
wrong.

**Resolution.** Moved to the OP-FP arm alongside Zfa and Zfhmin.
`cd4c125`.

### 124. fclass.h was claimed by Zfhmin
<a id="bug124"></a>

**Symptom.** `fclass.h` decoded as `fmv.x.h`.

**Root cause.** Both live in funct7 0x72 and differ only in funct3 — 000 for
the move, 001 for the classify. Zfhmin owns the move (it is one of the four
instructions Zfhmin consists of) and had claimed the whole funct7.

**Why it went unnoticed.** Zfhmin passed 663/663 for as long as Zfh did not
exist, because nothing could emit the encoding it was stealing.

**Resolution.** Split on funct3. `cd4c125`.

### 125. The vector suite reported 3042 of 3042 while measuring nothing
<a id="bug125"></a>

**Symptom.** `riscv-vector-tests` passed completely, before Zvfh, Zvbc or
any of the vector crypto family existed.

**Root cause.** The harness has two verdicts: a signature diff against Sail,
and the test's own tohost word. A test that exports signature symbols took
the signature path — and if no reference signature existed for it, the
signature branch returned pass. Almost none of them had references.

**Why it went unnoticed.** Because 3042/3042 is not a suspicious number when
you believe the vector unit is finished. The honest baseline, once the
branch fell back to the tohost verdict, was 2147 pass and 895 fail.

**Resolution.** A signature test with no reference falls back to the test's
own pass/fail, which every one of these suites reports. Never to an
unconditional pass. `db995f5`.

**Note.** This is the third harness bug in this document that manufactured
passes rather than failures — see [Bugs in the tests, not the emulator](#test-bugs). A harness bug that produces failures costs an
afternoon. One that produces passes costs however long it takes for someone
to disbelieve the number.

### 126. Every f16 NaN was canonicalised in transit
<a id="bug126"></a>

**Symptom.** After Zvfh brought the vector suite from 2147 to 2887, 99 of
the remaining failures were NaN-payload and invalid-flag mismatches.

**Root cause.** `ext_v_fp.cpp` computes in `double` and converts at the
element boundaries. The f16-to-double conversion returns a canonical quiet
NaN for any NaN input — which is correct for arithmetic and destroys two
things the architecture requires be preserved: the operand's payload, which
propagates through a NaN-producing operation, and whether it was signalling,
which decides whether invalid is raised.

**Resolution.** A half NaN is carried through the double as
`0x7FF8000000000000 | bits`, with the original sixteen bits in the low half.
It is still a NaN to every operation in between, and `d_to_h` recovers the
encoding exactly. `39ce9ac`, 99 failures to 68.

**Why this is worth an entry.** The lossy step is in a conversion function
whose job is to be lossless for every value that is not a NaN, and it is.
The bug is entirely in the interaction between "canonicalise NaNs" — a
correct thing for a conversion to do — and "use this conversion as a
transport", which is a use it was never designed for.

### 127. The unsigned flag read the wrong direction
<a id="bug127"></a>

**Symptom.** Every `vfcvt.f.xu.v` at e16 produced a signed conversion.

**Root cause.** One `is_unsigned` predicate was reused for both conversion
directions. VFUNARY0's sub-opcodes do not agree about which are unsigned:
0x00 and 0x06 are the unsigned *float-to-int* forms, while 0x02 is the
unsigned form going the other way. Testing for the first pair read every
int-to-float unsigned conversion as signed.

**Resolution.** Two predicates. `45d037f`.

### 128. The estimate tables were transcribed with Sail's index order intact
<a id="bug128"></a>

**Symptom.** `vfrec7(2.0)` returned 0.25 where the reference returns
0.498046875.

**Root cause.** Sail's lookup reads `table[127 - unsigned(idx)]`. Sail
vectors default to **descending** index order, so that expression is the
*ascending* element `idx` — `table[127 - idx]` in Sail and `table[idx]` in a
C array are the same lookup. The tables were transcribed together with the
`127 - idx`, which reverses all 128 entries.

**Why it went unnoticed.** Because it moved the suite count by exactly zero.
The tests that cover vfrec7 and vfrsqrt7 were failing before the tables
existed and kept failing after, so the only signal was the count, and the
count said nothing. A single hand-computed probe found it immediately.

**Resolution.** `TABLE[idx]`. `4d6659a`.

**Note.** The same convention governs the AES and SM4 S-box lookups
(`table[255 - unsigned(x)]`) and the SM4 key-constant table
(`table[31 - unsigned(x)]`). Having been caught here, it was got right
there — which is the only reason [132](#bug132) was a one-line fix rather
than three reversed tables.

### 129. The largest finite magnitude, computed by clearing the wrong bit
<a id="bug129"></a>

**Symptom.** `vfrec7` of a value that overflows returned 0x3FFF at
round-toward-zero where the answer is 0x7BFF.

**Root cause.** "The largest finite magnitude" was written as
`emask >> 1`. Shifting the exponent mask right clears its *top* bit, not
its bottom one — it halves the exponent field rather than decrementing it.

**Why it went unnoticed.** Five f32 probes at round-to-nearest-even all
matched, because RNE takes the infinity branch and never reaches this line.
The line is only live in the three directed rounding modes, which is a third
of the rounding modes and a much smaller fraction of the tests.

**Resolution.** `emask - 1` in the significand, exponent explicit.
`b340e5a`.

### 130. vslidedown read past the end of the source
<a id="bug130"></a>

**Symptom.** Large slide offsets produced elements from beyond the source
register group.

**Root cause.** `vslidedown` reads element `i + offset`, and both `i +
offset` and `offset` alone can exceed VLMAX. The spec's answer is zero for
any such element; the code computed the index and read it.

**Resolution.** Both bounds checked. Elements sourced from beyond VLMAX are
zero, not whatever is adjacent in the register file. `865284d`.

### 131. brev8 decoded in the wrong opcode
<a id="bug131"></a>

**Symptom.** `brev8` was unrecognised.

**Root cause.** It was routed in OP. It lives in OP-IMM, sharing `rev8`'s
funct6 and distinguished only by the value in the rs2 field of the immediate
— 0b11000 against `rev8`'s 0b00111.

**Why it looked right.** `brev8` is a bit-reversal of a register, and every
other register-to-register bitmanip operation in this codebase is in OP.
Being an immediate-form instruction that takes no immediate is unusual
enough that nothing about the instruction suggests where it lives.

**Resolution.** OP-IMM, split on the immediate. `cd4c125`.

### 132. The vector crypto opcode was not decoded at all
<a id="bug132"></a>

**Symptom.** Every one of the 24 vector crypto instructions raised an
illegal instruction.

**Root cause.** Zvkned, Zvkg, Zvknh, Zvksed and Zvksh are not in OP-V with
the rest of the vector ISA. They were given a major opcode of their own,
OP-VE (0b1110111), and `classify()` had no case for it.

**Why this is a better failure than the alternative.** An unclaimed major
opcode fails loudly and completely: nothing decodes, nothing executes,
nothing produces a plausible wrong answer. Compare [123](#bug123), where a
misrouted extension executed as its single-precision twin and returned
numbers. Given the choice, the whole family being illegal is the failure
mode you want.

**Resolution.** OP-VE classified as `Extension::V`, which gets it the vector
unit's `mstatus.VS` enable check and the `vill`/`vstart` handling unchanged,
and routed by funct6 to the three implementation files. The one subtlety is
that funct6 0x28 and 0x29 hold both the AES round instructions and SM4's,
told apart only by the vs1 field — 16 is SM4, 0 through 3 and 7 are AES, 17
is Zvkg's `vgmul` — so the split cannot be on funct6 alone.

### 133. GHASH multiplied in the direction it is described in
<a id="bug133"></a>

**Symptom.** `vgmul` and `vghsh` were the last two of the 24 crypto tests
to fail, after all thirteen AES tests passed.

**Root cause.** GF(2^128) multiplication for GHASH was written the way
GHASH is usually *explained*: walk the multiplier from bit 127 down,
shifting the accumulator right and folding the reduction polynomial in at
the top. The architecture specifies it the other way — both operands pass
through `brev8`, the multiplier is walked from bit 0 up, the multiplicand
shifts *left*, and 0x87 folds into the low byte. GCM stores its field
elements bit-reversed within each byte, and the two formulations are
different functions.

**Why it looked right.** Because it is a correct implementation of GHASH as
described in the GCM literature, and it produces output with the right
statistical shape. There is no partial credit in a field multiply: it is
either the same permutation as the reference or it is noise, and noise and
correctness are indistinguishable without the reference.

**Resolution.** Transcribed from `zvkg_insts.sail`, including the `brev8` on
the way in and out.

### 134. vaeskf2's round number was clamped as five bits
<a id="bug134"></a>

**Symptom.** `vaeskf2.vi` mismatched on 16 of 1352 signature words.

**Root cause.** Out-of-range round numbers are not illegal; the spec folds
them back into range by inverting bit 3, so that every encoding decodes to
something defined. That fold is specified on `rnd[3:0]`, and it was applied
to the whole five-bit immediate. For `rnd = 0x10` the two differ: four bits
gives 0 → 8, five bits gives 16 → 24, and the round constant is then indexed
at `(24 >> 1) - 1 = 11` in a ten-entry table.

**Why it went unnoticed in vaeskf1.** Its sibling reads the same immediate
and passed first time, because Sail writes *its* clamp as a test on
`rnd_val[3..0]` with the flip on the five-bit value — which is equivalent to
four-bit arithmetic for every input, where vaeskf2's is not.

**Resolution.** Mask to four bits, and make the rcon table return 0 rather
than read out of bounds for an index the clamp should have made
unreachable — a defence, not a behaviour, since no input reaches it now.

### 135. vtype accepted reserved bits and unsupported SEW/LMUL ratios
<a id="bug135"></a>

**Symptom.** None visible. This was found by reading Sail's
`vext_vset_insts.sail` alongside `exec_v_config`, not by a failing test.

**Root cause.** `vset{i}vl{i}` implemented two of the five conditions that
set `vill` — the reserved `vsew` encodings and the reserved `vlmul` — and
none of the other three:

* **The reserved field.** Bits above the four defined ones are reserved,
  and a nonzero one makes the whole vtype illegal rather than being
  ignored. They live in a different place in each form: bits[10:8] of
  `vsetvli`'s 11-bit immediate, bits[9:8] of `vsetivli`'s 10-bit one, and
  bits[62:8] plus `vill` itself of the register `vsetvl` reads. All three
  were accepted, and `vsetvl` then stored the request back verbatim, so a
  reserved bit could be read out of `vtype` again afterwards.

* **SEW against LMUL.** SEW may not exceed ELEN, and for a fractional
  LMUL it may not exceed LMUL × ELEN. At ELEN=64 that makes every
  fractional LMUL illegal at SEW=64, `mf4` and `mf8` illegal at SEW=32,
  and `mf8` illegal at SEW=16. All were accepted, and VLMAX was then
  computed by integer division: `mf8` at SEW=64 gives
  (128 × 1) / (64 × 8) = 0. An accepted configuration in which no vector
  instruction can address an element.

* **The `rd=x0, rs1=x0` form.** It keeps `vl`, which is only meaningful if
  VLMAX has not changed — so it is illegal to use it to change the
  SEW/LMUL ratio, or to use it at all while `vtype` is already `vill`.
  Neither was checked.

**Why it went unnoticed.** Every one of these is a rule about what must be
*rejected*, and a test that only ever asks for legal configurations cannot
see the difference. The three `vset` tests in riscv-vector-tests pass both
before and after this change; what settled it was the reference's source,
which states each condition as one line of a single `if`.

**Resolution.** All three conditions added, and the stored `vtype` masked
to the four defined fields rather than written back raw.

**Note.** The rule this belongs to is the one in
[Bugs that were design assumptions](#assumptions): the absence of a
failing test is not evidence. It is also the second entry in Part IX —
with [120](#bug120) — where the fix came from reading the reference model
rather than from running it.

### 136. Half-precision narrowing never implemented RMM
<a id="bug136"></a>

**Symptom.** `rv64vfncvt_f_f_w-1` and `-2`, the last two failures in
riscv-vector-tests. Subtest 137 expected `0xc081` and got `0xc080` —
one bit, in the last place.

**Root cause.** `frm` was 4, RMM: round to nearest, ties away from zero.
The f32→f16 conversion went through `fp16::f64_bits_to_h`, which takes a
*host* rounding mode, and `host_round_mode` had to map RMM onto something
x86 can express. There is nothing: x86 has no RMM encoding. It folded to
round-to-nearest-even, which differs from RMM on exactly the ties — and
`0xc080` is even.

This is the same defect that moved F and D onto Berkeley SoftFloat
wholesale, recorded at the top of this document. Vector FP followed them,
one operation at a time; this one conversion was the last caller left
behind, and it survived because nothing else in the file still consulted a
host rounding mode for a *result*.

**Resolution.** SoftFloat's `f32_to_f16` and `f64_to_f16`, chosen on the
source's own width so the conversion is a single rounding for either pair
of formats. Round-to-odd (`vfncvt.rod.f.f.w`) still rounds toward zero and
then forces the low bit on inexact, which is what it is defined as.

**And the fix read the wrong variable.** The first attempt made the
conversion correct and the results were still wrong, differently: an
overflow under round-toward-zero returned an infinity where the largest
finite magnitude was required. The arm being edited declared its own
`int rm` holding a host `FE_*` value, shadowing the function's `uint8_t rm`
holding the RISC-V mode. The new SoftFloat call read the shadow.
`FE_TOWARDZERO` is `0xC00` on x86; narrowed to a `uint8_t` parameter
expecting a RISC-V rounding mode, that is `0` — round-to-nearest-even
again. So the symptom moved from "RMM ties round the wrong way" to "every
directed-rounding overflow returns an infinity", and both were the same
sentence read twice.

Two variables of different types, both named `rm`, both meaning "rounding
mode", in units that are not interchangeable and where the wrong one
silently truncates to a valid value of the other. The shadow is now named
`host_rm`.

**Evidence.** riscv-vector-tests 3040/3042 to **3042/3042**.

---

<a id="part-x"></a>
## Part X — The hypervisor suite, 38 of 43 to 42 of 43

damo-rv-priv-ats is the only H-extension test suite that exists, and the
five groups still failing after Part VIII had thirteen named assertions
between them. This part closes twelve of the thirteen.

The pattern here is different from Part IX's. Nothing was a transcription
error: every one of these was a rule that had been *reasoned about* and
reasoned about wrongly, or a rule whose enforcement was written against a
value nothing could produce. Three of them were found by reading Sail's
source rather than by running it, and one was found by reading Sail's
source *after* running it disagreed — the trace said which address the
reference faulted on, and that was the whole answer.

### 137. VU-mode WFI never trapped
<a id="bug137"></a>

**Symptom.** `VINST-11: VU executes WFI with TW=0` — no virtual-instruction
trap.

**Root cause.** WFI had arms for `mstatus.TW` (illegal from anything below
M) and for `hstatus.VTW` (virtual instruction from VS-mode). VU-mode had
none, so a WFI from guest user code fell through to the no-op and the guest
carried on.

Sail states it in four lines: VU never waits. `TW=1` is an illegal
instruction, `TW=0` is a virtual instruction, and `VTW` does not enter into
it at all.

**Why the missing arm looked complete.** Because the two bits that *are*
checked are the two the spec talks about, and VTW's description — "traps
WFI from VS-mode" — reads like the complete statement of what
virtualisation adds. The VU rule is not about either bit: it is that a
guest *user* process halting the hart is never something the guest kernel
decided, so the trap has to go somewhere that can decide, and the
hypervisor is the nearest such place. That reasoning does not appear in
either bit's description, so an implementation guided by the bits misses
it.

### 138. An interrupt bound for the hypervisor was masked by its guest
<a id="bug138"></a>

**Symptom.** `TINST-01` — a VS software interrupt, injected through `hvip`
with `hideleg` bit 2 clear so that HS-mode keeps it, never fired.

**Root cause.** The global-enable test for an S-targeted interrupt read
`priv == PrivMode::S && !(mstatus & SIE)`. `PrivMode::S` is true of HS-mode
*and* VS-mode — DoomV carries virtualisation in a separate `virt` flag — so
while a guest was running, an interrupt destined for the hypervisor was
gated on the hypervisor's own `sstatus.SIE`.

That is the wrong register and the wrong mode. VS and VU are both strictly
below HS: the hypervisor is not the mode being interrupted, so its SIE has
no say. Sail writes the condition as
`(priv == Supervisor & mstatus[SIE]) | priv == User | priv ==
VirtualSupervisor | priv == VirtualUser` — the three modes below HS are
unconditional.

**Why it went unnoticed.** Because the interrupt it drops is the one a
hypervisor keeps for *itself*. Everything a hypervisor normally injects it
also delegates, and a delegated VS interrupt goes down the other branch
where the enable is correctly the guest's `vsstatus.SIE`. Only the
undelegated case — the hypervisor asking to be told about its guest's
timer or software interrupt rather than passing it through — reaches this
line, and only while the hypervisor happens to be running with its own
interrupts off, which is exactly when it most needs to be told.

### 139. Unassigned user-level CSRs were readable and writable
<a id="bug139"></a>

**Symptom.** `TENT-14` and `MSTAT-02` both failed on "trap triggered" —
their trigger is `csrw 0x004, x0` from HS-mode, and no trap happened.
`HTVAL-CLR-01` and `HTVAL-CLR-06`, which need an illegal instruction to
observe what it does *not* modify, failed the same way.

**Root cause.** `csr_access_permitted` enforced privilege boundaries and
read-only-ness, and answered anything else from the generic `csr[]` backing
store. A comment said so, and said why: OpenSBI probes a spread of CSRs to
see which trap, and getting that list wrong breaks the boot rather than a
test.

That reasoning is sound for the machine and supervisor quarters of the
address space. It does not apply to the user quarter, which is small enough
to enumerate exactly: RVA23 assigns nine addresses there plus the counters,
and nothing in a boot path goes looking in it. And the addresses that are
*not* assigned include the ones that used to be — 0x000-0x005 were
`ustatus`/`uie`/`utvec`/`uscratch`/`uepc`/`ucause`, the N extension's
user-level trap registers, and N was removed from the ISA. Software probing
for it has to see a trap.

**Resolution.** `is_unimplemented_user_csr`, in the shape of the existing
`is_rv32_high_half`: no such register, illegal at every privilege including
M. The other three quarters are deliberately untouched. Linux still boots.

**Note.** Four assertions across two groups turned on this, and none of
them is *about* CSR addressing. Three needed an illegal instruction as an
instrument — something guaranteed to trap, so the test could look at what
the trap did to `htval` or `mstatus.MPV`. A machine that will not trap on
demand cannot be tested for what it does when it traps.

### 140. A mstateen bit hardwired to zero, and a comment that misnamed it
<a id="bug140"></a>

**Symptom.** `SRMCFG-22: V=0 HS-mode normal access srmcfg` — HS-mode could
neither read nor write `srmcfg`.

**Root cause.** `srmcfg` is gated below M-mode by `mstateen0` bit 55, and
`csr_access_permitted` checked that bit correctly. `STATEEN0_M_WMASK` did
not include it, so nothing could ever set it, and the check could only ever
refuse.

The reason it was excluded is written down, and is the interesting part.
The mask carries a long comment about not copying masks from a reference,
because each bit names a specific extension's state and the honest mask is
a statement about what *this* machine has — and it ends by dismissing "bit
55 (CSRIND), which DoomV does not implement either". Bit 55 is SRMCFG.
CSRIND is bit 60. DoomV implements srmcfg.

**Why it survived.** The comment's reasoning was right, was applied
carefully to four registers, and got a specific fact wrong in passing. A
reader checking the *argument* finds nothing to object to; only a reader
who looks up the bit number finds the error. This is the same failure shape
as [96](#bug96) and [116](#bug116) — a comment asserting something that
stopped being true, or was never true — except that here the comment is
what created the bug rather than what concealed it.

**Note.** `hstateen0` genuinely has no SRMCFG bit — the field is absent
from the register in the architecture, not merely unimplemented here — and
that is what makes `srmcfg` unreachable from every virtual mode no matter
what a hypervisor writes. Resource-control identities are assigned *to*
guests, never by them. So the mask above it is correct as it stands, for a
reason, and the two registers had to be treated differently.

### 141. A refusal from M-mode downgraded to a refusal from the hypervisor
<a id="bug141"></a>

**Symptom.** `SRMCFG-23: mstateen0[55]=0 VS-mode -> illegal-inst` reported
cause 22 (virtual instruction) where cause 2 was required.

**Root cause.** Any `srmcfg` access from a virtual mode took the
virtual-instruction path unconditionally. But with `mstateen0.SRMCFG`
clear, HS-mode cannot reach `srmcfg` either — there is no hypervisor
standing behind the register and nothing for one to emulate on the guest's
behalf, so the answer is "no such access".

**Why it is worth its own entry.** DoomV already had this exact rule,
written out, fifteen lines below the bug: the `senvcfg`/`henvcfg` case
notes that "mstateen0 closing it first stays illegal: the machine said no
and no one is underneath". The same ordering appears a third time in WFI,
where TW outranks VTW. Three instances of one principle — *a refusal from
above is never downgraded into a refusal from the middle* — and the third
site did not get it, in a file where the other two are adjacent and
commented.

### 142. henvcfg.PBMTE did not gate the VS stage
<a id="bug142"></a>

**Symptom.** `HENV-05: PBMTE=0 disables VS-stage Svpbmt` — no page fault.

**Root cause.** The Svpbmt leaf check read `menvcfg.PBMTE` for every stage.
`menvcfg.PBMTE` governs the stages HS-mode owns — its own S-stage and the
G-stage; a guest's VS-stage is governed by `henvcfg.PBMTE`. So a hypervisor
that cleared `henvcfg.PBMTE` to hide Svpbmt from its guest found the
guest's own page tables honouring the memory-type bits anyway, and the
guest probing successfully for an extension it had been denied.

**Why it went unnoticed.** Because the identical rule for Svadu was already
implemented, correctly, twenty lines above — `adue_enabled(regs, vs_stage)`
exists precisely because "the choice is per-stage rather than per-hart",
and its comment says so. The PBMT check sat in the same function, took the
same `virt_access` flag that the ADUE call was already passing, and read
one register.

**Resolution.** `pbmte_enabled(regs, vs_stage)`, in the shape of its
neighbour.

### 143. Pointer masking ignored MXR, and had lost the one hlv case that needs HUPMM
<a id="bug143"></a>

**Symptom.** Two assertions, one function. `HZPM-TRAP-05: MXR suppresses PM
in VS-mode` — a trap that should not have happened; `HZPM-HLV-08:
senvcfg.PMM ineffective for HLV in U-mode` — likewise.

**Root cause, first half.** Pointer masking does not apply while MXR is in
effect, and DoomV had no such rule. The two features would otherwise fight:
MXR exists so a supervisor can read an execute-only page, at an address it
worked out itself, and masking rewrites that address out from under it.
M-mode is exempt from the suppression — Smmpm's masking is M's own and MXR
does not govern it — and under virtualisation *either* MXR counts, HS-level
`mstatus.MXR` across both stages or `vsstatus.MXR` across the VS-stage.

**Root cause, second half.** An `hlv`/`hsv` issued from **U-mode** — which
`hstatus.HU` permits — with `SPVP=0` acts as VU, and a VU access would
ordinarily take the guest's `senvcfg.PMM`. That is the wrong register: the
address came from a U-mode process running under the hypervisor, not from
the guest, so `hstatus.HUPMM` supplies the length.

DoomV had a case for this once, and it applied HUPMM to *every* `hlv`,
which was wrong in the other direction. The fix removed the case entirely
and made hlv follow `SPVP` like an ordinary access — correct for every hlv
but this one.

**Why both errors have the same symptom.** A tagged pointer masked by a
field that does not govern it faults on an address the program never named.
Over-applying HUPMM and under-applying it produce identical-looking
failures on different accesses, which is why the second error looked like a
clean simplification of the first.

**Note.** `hlvx` needs no case: it arrives at the MMU as a `Fetch` because
it borrows a fetch's *permission*, and masking's early return for fetch
already covers it. The PMM fields are defined over `hlv`/`hsv` only, so
that is the right answer for the right reason — by luck rather than by
design, but it holds.

### 144. A straddling access reported its last byte instead of the faulting page
<a id="bug144"></a>

**Symptom.** `HTVAL-STR-01: load straddle into V=0 page reports htval =
GPA>>2`, and the test printed its own diagnosis: `htval 0x20080401 is
neither op>>2=0x200803ff nor victim>>2=0x20080400`.

**Root cause.** An access that crosses a page boundary is two accesses to
the page tables, and DoomV checked the second page by translating the
access's **last byte**. For an eight-byte load at offset 0xFFF that byte is
at offset 0x006 of the next page, so the guest physical address reported to
the hypervisor was six bytes past the page that actually faulted — telling
it to back a page at the wrong address.

The reference reports the page boundary itself: Sail's trace gives
`mtval = 0x80201000` and `mtval2 = 0x20080400` for this access, both naming
the first byte of the access that lies in the faulting page.

**How it was found, which is the point of the entry.** Three wrong models
of the test were built and discarded before any of this. What settled it
was running the same ELF under Sail with `--trace-instr --trace-csr` and
reading the six CSR writes at the faulting instruction. That gave the
cause, the tval, the GPA and the `mepc` in one line each — the whole answer,
with no inference. The instrumentation added to DoomV afterwards only
confirmed which of its own faults produced the value.

**Why stores were right and loads were wrong.** They take different paths.
`store_virtual` translates a straddle byte-at-a-time, ascending, so the
first byte to fault is the boundary one by construction. `load_virtual`
probes the whole range once through `translate_or_trap`, which is where the
last-byte address came from. `HTVAL-STR-02`, the store version of the same
test, passed throughout.

**Resolution.** `last & ~0xFFF` — the second page's first byte. Nothing
here is wider than eight bytes, so an access spans at most two pages and
there is only ever one boundary. The comment that used to sit above this
code claimed the fault "has to name the *original* address, not the start
of the second page"; it named neither, and the reference names the second.

### What is left
<a id="damo-remaining"></a>

The conclusion recorded here was that the one remaining assertion --
`HENV-17: DTE=1 enables vsstatus.SDT` -- is not a bug: that it wants
Ssdbltrp, which DoomV does not implement, and that it should stay red until
the extension is real.

That was wrong, and [145](#bug145) is what it actually was. The argument
about not advertising state the machine does not have was sound; it was
pointed at the wrong register. Ssdbltrp is still unimplemented and the
suite is at 43/43.

### 145. An envcfg bit that advertised an extension the hart does not have
<a id="bug145"></a>

**Symptom.** `HENV-17: DTE=1 enables vsstatus.SDT` — the last failing
assertion in the hypervisor suite, and the one [Part X](#part-x) closed by
saying it was not a bug.

That was wrong, and the way it was wrong is the entry.

**The reasoning that got it wrong.** `henvcfg.DTE` is Ssdbltrp's, and
Ssdbltrp is not implemented here: no `mstatus.MDT`, no `vsstatus.SDT`, no
double-trap escalation. So the test appeared to be asking for an extension
that does not exist, and the conclusion recorded was that it should stay
red until the extension is real — with an argument attached about how
making `vsstatus.SDT` writable to turn the test green would be advertising
state the machine does not have.

The argument is right. It was pointed at the wrong register. The test never
asked for `vsstatus.SDT` unprompted: it wrote `henvcfg.DTE`, read it back,
found it had stuck, and *only then* went looking for the state that bit
promises. `henvcfg.DTE` reading back set is itself the false advertisement.

**Root cause.** The envcfg write path already has a block for exactly this,
with the rule written out: "an envcfg bit for an extension this hart does
not have reads as zero... leaving LPE or SSE writable on a hart with no
landing pads or shadow stack would have software enable a protection that
then does not happen -- worse than not offering it, because the enable
appears to succeed." DTE belongs in that block and was not in it.

**What settled it.** Running the group under Sail. The suite reports
`[sail: PASS (119 passed)]` where DoomV reported 118 and one failure, and
Sail has Ssdbltrp *less* implemented than DoomV does — its `henvcfg.DTE`
legalization is commented out with a `TODO: Add Ssdbltrp part once
supported`, so the write is dropped and the bit reads zero. The reference
passes the test by reporting the extension absent, which is what the test
is for. The `--sail-verdict` flag exists precisely to ask "does the
reference pass this?", and it had not been asked.

**Resolution.** One line, in the block that already stated the rule.
damo-rv-priv-ats goes 42/43 to **43/43**.

**Note.** Two lessons, and the second is the useful one. First: an
extension is reported absent by making its *enable* read zero, not by
leaving the enable alone and omitting the behaviour — the second reads as
"enabled and broken". Second: "this test wants a feature we do not have" is
a conclusion that needs the same evidence as any other, and the evidence is
one command. Part X shipped the opposite conclusion with a paragraph of
justification and no measurement, which is a more expensive mistake than
being wrong quietly would have been.

### 146. mstatus.TSR was stored and never consulted
<a id="bug146"></a>

**Symptom.** `rv64mi-p-illegal` subtest 2 — the last failing test in
riscv-tests, and the last red anywhere in the project. It had been carried
as "pre-existing" for the whole of Parts IX and X without being looked at.

**Root cause.** `mstatus.TSR` closes SRET to HS-mode, so that M-mode can
interpose on a supervisor's return the same way a hypervisor interposes on
its guest's. The bit was in the writable mask and nothing ever read it, so
an S-mode SRET with TSR set simply returned.

The test's shape makes that immediately destructive. It traps an illegal
instruction from S-mode, and the handler sets `mstatus.TSR` before
returning — so the very next thing the machine does after TSR becomes set
is execute an SRET in S-mode expecting cause 2. DoomV performed the return
instead, to a `sepc` chosen for a trap that never happened, and the
resulting fetch fault arrived at the handler as cause 1 where cause 2 was
required.

**Why it went unnoticed.** Because its virtualised counterpart was
implemented, and implemented *because a test caught it*. `hstatus.VTSR` is
handled fifteen lines above, with a comment recording that it "was defined
and made writable and then never consulted, so a guest with VTSR set simply
returned -- to whatever sepc held". That is a verbatim description of the
bug still sitting below it. The hypervisor suite exercises VTSR; nothing
in arch-test exercises TSR; and the one suite that does had its failure
filed as environmental.

**Resolution.** One condition, matching Sail's `sret_illegal`, with three
deliberate exclusions: M-mode is exempt because TSR is M's own control and
a mode does not trap itself with it; VS-mode is exempt because VTSR governs
there; U-mode needs no case because SRET is illegal there under any
setting.

**Note.** This is the fourth entry in this document where one bit of a pair
was implemented and the other was not, and the third where the *comment on
the implemented half* describes the unimplemented one exactly — see
[115](#bug115), [140](#bug140), [141](#bug141). The pattern is specific
enough to act on: when a bit is found unconsulted, the next thing to check
is whatever bit it is the analogue of.

**And the method note, since this one is unflattering.** "Pre-existing" is
not a diagnosis. This failure was reported in five separate regression runs
across two parts of this document, named as a known M-mode gap each time,
and it took one `--sail-verdict` (Sail passes), one disassembly and one
breakpoint to find. The cost of carrying it was not the test -- it was that
every "all suites green except one known failure" summary written in the
meantime was wrong about what the exception was.

**Evidence.** riscv-tests 376/377 to **377/377**. With it, every suite this
project runs is at 100%: arch-test 663/663, riscv-vector-tests 3042/3042,
differential 19/19, damo-rv-priv-ats 43/43.

### 147. The envcfg registers were a blacklist where they should be a whitelist
<a id="bug147"></a>

**Symptom.** None. No test catches this; it was found by asking what else
was wrong in the same way [145](#bug145) was.

**Root cause.** The envcfg write path cleared individual bits whose
extension is absent — LPE without Zicfilp, SSE without Zicfiss, and, after
145, DTE. That is the right rule applied one bit at a time, and one bit at a
time is exactly how DTE came to be writable in the first place. Two more
were still writable on a hart that implements neither:

* **60 CDE** — Smcdeleg, counter delegation.
* **8 UKTE** — Ssctr's constant-timing enable.

Plus every reserved bit in all three registers, and every enable a future
extension will put there.

The reference states the rule as a whole rather than per bit: its
legalization names the fields it implements and takes everything else from
the register's old value, which starts at zero and stays there. Its comment
says so outright — "other extensions are not implemented yet so all other
fields are read only zero".

**Resolution.** The same rule turned round. Each register admits exactly
the fields DoomV has, so an unimplemented enable reads zero without having
to be remembered individually. The masks differ per register rather than
being one shared constant: STCE, PBMTE and ADUE exist in `menvcfg` and
`henvcfg` but not in `senvcfg`, because there is no S-level control over a
guest's timer, page types or A/D updates.

**Why this is worth an entry with no failing test behind it.** Three of the
last four entries were one bit that was writable while the thing it enables
did not exist — [140](#bug140), [145](#bug145), [146](#bug146). Two of them
cost a failing assertion each and one cost a wrong conclusion published in
this document. A blacklist gets that class of bug wrong once per future
extension; a whitelist gets it right by construction. The fix is worth more
than the bugs it closes today.

### 148. virtio-blk's file offsets were 32 bits wide
<a id="bug148"></a>

**Symptom.** A 4 GiB Ubuntu image would not boot: the guest reported

```
virtio_blk virtio0: [vda] 0 512-byte logical blocks (0 B/0 B)
VFS: Cannot open root device "/dev/vda1" or unknown-block(254,1): error -6
Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(254,1)
```

The same code had been mounting a 192 MiB image correctly for two commits.

**Root cause.** Two truncations on the same path, both from `long` being 32
bits on this toolchain.

`std::ftell` was used to measure the image. 4 GiB is *exactly* 2^32, so it
returned -1, the `end > 0` guard turned that into a capacity of zero, and the
device honestly advertised a disk with no sectors on it. The kernel's panic
names the partition it could not find, which says nothing about the device
having no size -- and 254,1 is exactly what a *missing partition table* looks
like too, which is the wrong place to start looking.

The second one is worse and had not been triggered yet:
`std::fseek(file, (long)off, SEEK_SET)` wrapped every access past 2 GiB back
into the low half of the image. That does not fail. It reads and writes the
wrong sectors, so any filesystem larger than 2 GiB would have corrupted
itself quietly while appearing to work.

**Why it went unnoticed.** Every image used until now was small. The
partitioned-root test two commits earlier was 192 MiB, chosen because it was
quick to build -- which is exactly the size that cannot exercise either bug.
A 32-bit offset is invisible until the file is bigger than the type, and
"bigger than the type" for a disk image means "the first realistic one".

**Resolution.** `_fseeki64`/`_ftelli64` on Windows, `fseeko`/`ftello`
elsewhere, behind two one-line helpers. Capacity now reads 8388608 sectors
and the partition mounts.

**Note.** The interesting part is the pairing. One truncation produced a loud
failure that stopped the boot; the other produced silent data corruption on
the same line of reasoning, and only the loud one was reachable with the
images in use. Finding the first is what made anyone look at the second,
which is an argument for chasing a bug to its cause rather than to its
symptom.

### 149. Not a bug: the missing TLB, and what it cost
<a id="bug149"></a>

This one is here because the reasoning that left it out was written down,
turned out to be right at the time, and expired without anyone noticing.

`mmu.hpp` said translation was "stateless on purpose (no TLB) -- every call
re-walks the page table directly out of guest RAM via `mem`, which is simple
to get right and cheap enough for now; only worth revisiting if it's an
actual measured bottleneck once something heavier than Doom is running".

Every clause of that is correct. Statelessness *is* simpler to get right: no
cache means no invalidation, which is why `SFENCE.VMA`, `SINVAL.VMA` and
`fence.i` could all be honest no-ops, and why several entries in this
document about fences are about privilege checks rather than about
flushing. And Doom does not stress it: it runs bare-metal with `satp` at
zero, so it never walks a page table at all.

Then something heavier turned up. Booting Linux measures **1.67 MIPS** --
206 million instructions in 123 seconds -- and building an Ubuntu root
filesystem on this machine is a multi-hour job, almost all of it re-walking
the same handful of pages. With Sv39 every fetch and every load or store was
three dependent reads out of guest RAM before the access itself, and an
instruction that crosses into a second halfword pays for two translations.

A 4096-entry direct-mapped TLB takes it to **4.24 MIPS**, and the guest's
own clock reports the identical instruction count either way -- 0.206266
seconds of guest time before and after -- which is the first evidence that
nothing about what executed has changed.

**What makes it safe is what it refuses to cache.** The temptation is to
cache every translation; the design caches four narrow cases out of many,
and each exclusion removes a way to be wrong:

* **Faults are never cached**, so a mapping that appears later is seen at
  once.
* **Two-stage translation is never cached** -- no VS-mode, no `hlv`/`hsv`,
  nothing under H. That has a second set of tables and a second set of
  fences, none of it modelled here, and excluding it means the hypervisor
  suite still exercises the code it was written against.
* **M-mode is never cached**, which is trivial because M-mode does not
  translate.
* **A translation whose walk set A or D is never cached.** This is the
  subtle one. A hit skips the walk, and the walk is where the accessed and
  dirty bits get set -- so caching a translation that needed an update would
  mean the *next* access silently failed to set a bit the architecture
  requires. Only walks that found A (and D, for anything that writes)
  already set are eligible, which means the second touch of a page is the
  one that gets cached. That is exactly the access that repeats.

The key carries everything else that changes the answer for one address:
the privilege it is made at, the access type, and SUM and MXR. Anything not
in the key must flush, and the flush list is short by design -- `satp`,
`vsatp` and `hgatp` writes, the fences, and an `envcfg` write, since
`PBMTE` decides whether a nonzero memory-type field faults and a cached
entry was validated under the old setting.

`SFENCE.VMA` and `SINVAL.VMA` drop the whole table rather than the address
or ASID the operands name. Invalidating more than asked is always permitted,
and it keeps the argument for correctness down to one sentence instead of a
table.

**Evidence.** 663/663 arch-test, 43/43 damo, 377/377 riscv-tests, 3042/3042
riscv-vector-tests, 19/19 differential -- every suite unchanged, and Linux
still boots. The suites are what made this worth attempting at all: an
optimisation to the MMU is exactly the change you do not make without them.

## Part XI — Running a distribution, and the three ways a guest could not end its own run

Part X was the last of the conformance work. This part comes from a
different kind of testing: building and booting Ubuntu 24.04, which runs
riscv64 `dpkg`, its maintainer scripts, and the perl and shell those fork,
for about four hours. No reference model is involved. It either configures a
hundred packages or it does not.

It found nothing wrong with the ISA. What it found was that the *machine*
was missing three things, and all three were the same thing wearing
different clothes: a guest running an unattended job had no way to stop.
Each one on its own looks like a footnote. Together they are the difference
between a build you can run and a build you have to sit and watch.

The other lesson is about what counts as evidence. Two of these three were
diagnosed by *reading a configuration file* and one by grepping the kernel
binary -- after a session of reasoning confidently about code that could not
have worked. The measurement was available the whole time and took a minute.

### 150. The poweroff register was implemented, acknowledged, and never read
<a id="bug150"></a>

**Symptom.** A guest powers off. The kernel says so:

```
[    0.296394] sysrq: Power Off
[    0.299056] reboot: Power down
[    1.341319] Unable to poweroff system
```

...and the emulator keeps running. The four-hour Ubuntu build reached its
completion marker and then sat in the sleep loop its own script falls into
when poweroff fails, forever, with the work already done.

**Root cause.** `Memory` has had the `sifive,test0` device since the device
tree got a `test@100000` node, and it does the right thing with a write:

```cpp
if (addr >= TEST_BASE && addr < TEST_BASE + TEST_SIZE) {
        if (cmd == 0x5555 || cmd == 0x7777 || cmd == 0x3333) poweroff = true;
```

and `Memory::poweroff_requested()` returns that flag. Nothing anywhere
called it. `grep -rn poweroff_requested src/` returned exactly one line: the
definition.

The device had been added for OpenSBI's benefit rather than for the
emulator's -- OpenSBI's generic platform implements SBI SRST by *finding*
that compatible string in the device tree, so without the node a guest's
`poweroff` returns "not supported". That is a real requirement and it was
met, which is precisely why the other half was easy to miss: the node made
the kernel's poweroff path work, right up to the last step, and the last
step was a flag being set for nobody.

The kernel's "Unable to poweroff system" is the giveaway in hindsight. It
does not mean the write failed; it means the write returned. A power-off
register is the one MMIO store in the machine that is not supposed to have
a next instruction.

**Fix.** `cpu_loop` reads the flag once per 200000-instruction burst and
ends the run. Once per burst rather than once per instruction because the
guest is in its poweroff path by then and has nothing left to do, so the
overrun is free and the hot path stays clean. In windowed mode the render
loop closes the window on the same flag -- specifically on that flag and
not on `run_finished`, which is also set by a debugger halt and by a test
finishing, both of which want the window to stay up so the dashboard can be
read and F9 can resume. A machine that has been switched off is the one
case where there is nothing left to look at.

**Evidence.**

```
$ printf 'mount -t proc proc /proc\necho o > /proc/sysrq-trigger\n' \
    | ./riscv_doom.exe -ng -expect='~ # ' -kernel=build/linux/Image ...
~ # echo o > /proc/sysrq-trigger
[    0.296394] sysrq: Power Off
~ # [    0.299056] reboot: Power down
guest requested poweroff
$ echo $?
0
```

Before the fix the same command exited 124 -- `timeout` killing it after
five minutes.

### 151. A headless guest console was write-only
<a id="bug151"></a>

**Symptom.** Not a wrong answer; a thing that could not be done. A `-ng`
Linux boot could be read and never answered. The UART's receive ring was
fed from exactly one place -- the SDL keyboard handler in the render loop --
so every interaction with a guest past the kernel log needed a window and a
person. Logging in, running a command and reading what it printed, telling a
guest to shut down: all of it manual, none of it scriptable, and therefore
none of it in any suite.

That is why 150 survived as long as it did. The poweroff path is reachable
from a shell in one line, and there was no way to type that line except by
hand.

**Fix.** Two pieces, and the second is the one that matters.

`-ng` now runs a thread feeding host stdin into the receive ring, so a guest
is drivable from a pipe. Newlines are translated to CR on the way in: a pipe
carries LF, a terminal sends CR when you press return, and the kernel's line
discipline is set up for a terminal -- `ICRNL` turns CR into LF on input and
nothing turns LF into anything, so a raw LF is typed and never runs. The
feed blocks until each byte fits rather than dropping on a full ring, which
also paces it to whatever rate the guest is draining at. Dropping stays the
right behaviour for the keyboard path, where a human cannot outrun sixteen
bytes and stalling the render thread would be the wrong answer.

Then `-expect=<text>`, which is not a convenience. The first attempt sent
its command at reset and the guest ran `cho o > /proc/sysrq-trigger`:

```
~ # cho o > /proc/sysrq-trigger
/bin/sh: cho: not found
```

Input sent before the guest's tty exists is not queued anywhere. It is read
out of the ring by OpenSBI, handed to a console with no line discipline yet,
and dropped. `-expect` holds the feed until the guest's *transmit* side has
printed a given string, which `Uart` matches incrementally because the guest
writes one character per store and any needle straddles many writes. The
alternative is a delay, and a delay is a guess that has to be re-guessed
every time the guest gets slower or faster.

### 152. The fix for the poweroff was written for a kernel that could not run it
<a id="bug152"></a>

**Symptom.** The stage-2 build script ends with

```sh
echo o > /proc/sysrq-trigger 2>/dev/null || true
```

on the reasoning -- correct as far as it goes -- that `poweroff` in an
Ubuntu rootfs is systemd's, that systemd's `poweroff` wants to talk to a
running systemd, and that there is none when the stage-2 script is itself
PID 1. sysrq is handled in the kernel with no userspace involved, so it is
the one thing that works from there.

It would have worked on a kernel with sysrq. This one had:

```
$ grep MAGIC_SYSRQ .../linux-*/.config
# CONFIG_MAGIC_SYSRQ is not set
```

riscv `defconfig` leaves it off. So `/proc/sysrq-trigger` did not exist, the
redirect failed into `/dev/null`, and the `|| true` swallowed it -- and the
two fallbacks after it are systemd's `poweroff` and `reboot`, which is
exactly what had failed in the first place. A fix that changed nothing, with
its own failure suppressed twice.

**Fix.** `--enable MAGIC_SYSRQ` in `scripts/build_linux.sh`, next to the
`FB_SIMPLE` line and for the same reason: the feature the machine needs is
one riscv defconfig does not build.

And then the same thing again from the host side, because a four-hour build
should not hinge on which kernel happens to be in `build/linux`:
`boot-stage2.sh` runs the emulator in the background and ends the run itself
when the completion marker appears in the log. The guest still tries to
power itself off and now can, but the build no longer depends on it. That
also makes the script's exit status mean "stage 2 finished" rather than "the
emulator stopped", which are not the same claim.

One related change in the guest script: the marker is now printed *after*
`sync`, not before. The host ends the run on that marker, so it has to mean
"the image is on disk" and not "the script got this far" -- otherwise the
gap between the two is a window in which dpkg's work is still in the guest's
page cache and the emulator is being killed.

**Evidence.** `sysrq-trigger` is in the rebuilt kernel:

```
$ strings -a build/linux/Image | grep -c sysrq-trigger
1
```

and the end-to-end chain is [150](#bug150)'s evidence block: sysrq to SBI
SRST to `sifive,test0` to an emulator that exits 0.

### 153. Not a bug: a screenshot is not evidence
<a id="bug153"></a>

Worth writing down because it cost more than any bug in this part, and
because the mistake is not about emulators at all.

The framebuffer console was finished and the question was whether Ubuntu was
drawing on it. I took a screenshot. It was black. I then spent two changes
-- a `getty@tty1` fix and a kernel-path fix, both of which turned out to be
real and necessary for other reasons -- reasoning about *why* the screen was
black, without ever having established *that* it was.

Screenshots of this window are not evidence, twice over.
`CopyFromScreen` copies a rectangle of the screen, so it captures whatever
is on top of that rectangle -- one of them captured a browser playing
minesweeper. `PrintWindow` asks the window to draw itself and returns black
for GPU-composited SDL content, which is what SDL2 renders by default. So
the "blank screen" was consistent with a perfectly good framebuffer and with
no framebuffer at all, which makes it not an observation.

`-fbdump=<path>` writes the framebuffer out of `Memory::linux_framebuffer()`
itself, as a PPM plus a count of non-black pixels. It cannot be the wrong
window and it cannot be composited away. The Ubuntu boot, which had produced
the black screenshot:

```
framebuffer: 100023 of 786432 pixels non-black, written to fbu.ppm
```

and at the login prompt, rendered as text:

```
Ubuntu 24.04 LTS doomv tty1

doomv login: _
```

The pixel count is the part worth keeping. A cleared console with a login
prompt on it lights about a thousand of 786432 pixels, and two lines of 8x16
text disappear entirely when a 1024x768 image is scaled down to glance at.
The bounding box and the count are the measurement; the thumbnail is not.

## Part XII — A keyboard and a mouse, and the constant that drifted

Adding real input devices -- `virtio-input` keyboard and mouse for Linux,
MMIO registers for DOOM -- was a feature, not a bug hunt. It turned up
three things anyway, and the first is the worst bug in this document that
nothing was testing.

### 154. DOOM had been unable to find its WAD since RAM grew to 1GB
<a id="bug154"></a>

**Symptom.**

```
W_Init: Init WADfiles.
 adding doom1.wad
Wad file doom1.wad doesn't have IWAD or PWAD id
```

The WAD on disk is fine: 4196020 bytes, first four bytes `IWAD`.

**Root cause.** Two copies of one constant, and one of them moved.

The host places the WAD immediately above RAM:

```cpp
static constexpr uint64_t WAD_BASE = RAM_BASE + RAM_SIZE;
```

The guest had its own copy in `tools/doom/doombuild/doomv_mmio.h`:

```c
#define RAM_SIZE     0x10000000u   // 256MB
#define WAD_BASE     0x90000000u
```

`RAM_SIZE` was raised from 256MB to 1GB so that a distribution would fit --
systemd in 256MB is not workable and `apt` is hopeless -- which moved
`WAD_BASE` from `0x90000000` to `0xC0000000`. The guest went on reading
`0x90000000`, a gigabyte short, where it found zeros.

So the change that made Ubuntu possible broke DOOM, and DOOM is the thing
this emulator is named after. Nothing caught it because no suite runs DOOM:
the six suites cover the ISA, the privileged architecture, the vector unit
and a Linux boot, and the bare-metal guest is checked by looking at it.
Between one look and the next, it stopped working.

**Fix.** Not "update the other copy", which would leave the same trap armed
for the next time either number changes. The guest asks:

```c
#define MMIO_WAD_BASE 0x10000014u
#define MMIO_WAD_SIZE 0x10000018u
```

Nothing can `static_assert` across the emulation boundary, so the only
durable fix for a constant that must match on both sides is for one side to
stop having a copy. Publishing the length as well removed a second
duplication -- it had been compiled in as `-DWAD_LENGTH` from a table of
four hardcoded file sizes in the guest Makefile -- and as a side effect the
four output binaries now differ only in name, and any of them runs whichever
WAD it is handed.

**Evidence.** `I_InitGraphics: framebuffer: x_res: 320, y_res: 200` and a
title screen in the window, from the same command that produced the error
above.

### 155. The framebuffer aperture shadowed the new devices, in the byte paths only
<a id="bug155"></a>

**Symptom.** Both `virtio-input` devices probed, registered, and were
useless:

```
I: Bus=0006 Vendor=0000 Product=0000 Version=0000
N: Name=""
H: Handlers=event0
B: EV=1
```

No name, and `EV=1` is `EV_SYN` alone -- the bit the kernel sets itself. A
device that is present and answers nothing, which is a worse failure than a
device that is absent: absent is diagnosable.

**Root cause.** I had put the devices at `0x10009000` and `0x1000A000`,
following the QEMU convention of virtio slots next to the disk's
`0x10008000`. DOOM's framebuffer is `320*200*4` bytes at `0x10001000`, so
its aperture runs to `0x1003F7FF` and covers all of them.

The disk had been living inside that aperture the whole time without
trouble, because `read32`/`write32` happen to test the virtio range before
`MMIO_FB`. `read8`/`write8` test `MMIO_FB` first -- and `virtio-input` is
the first device here whose config space is read a byte at a time, because
it is a packed struct of `u8`s and a 128-byte union rather than a few
words. So every config read returned a framebuffer byte, which is zero.

The one non-zero field was a red herring that cost twenty minutes:
`Bus=0006` is `BUS_VIRTUAL`, which the driver assigns itself and only
overwrites if the device's `ID_DEVIDS` answer is long enough. It looked like
evidence that config space was working.

**Fix.** Moved to `0x10100000` and `0x10101000`, past the aperture, with a
`static_assert` that they are:

```cpp
static_assert(VIRTIO_KBD_BASE >= MMIO_FB + FB_SIZE,
              "input devices must not sit inside DOOM's framebuffer aperture");
```

Reordering the branch chain would also have worked and would have left the
next device to rediscover this. The overlap that remains -- the disk at
`0x10008000` -- is noted where it is, since moving a working device's
address is a device-tree-compatibility change and not a bug fix.

### 156. A console keyboard rebuilt from keysyms only works on one layout
<a id="bug156"></a>

Not a crash; a design that could not be right. Worth recording because the
wrong version looked completely reasonable and passed every test anyone
would think to run on a US keyboard.

`translate_console_key` turned an SDL keysym plus a shift bit into a
character, with a hand-written table of the shifted punctuation:

```cpp
if (shift) {
        switch (sdl_keysym) {
        case '1': return '!';
        case '2': return '@';
        ...
```

Every line of that table is a claim about the *host's* keyboard layout.
Shift+2 is `@` on a US board, `"` on a UK one, `~` on a French one. And a
layout is not a table of pairs anyway -- dead keys compose over the
following keystroke, and AltGr is a third level the shift bit cannot
express.

The replacement uses the two facts SDL already knows. `SDL_TEXTINPUT`
delivers composed characters with layout, modifiers and dead keys already
resolved, which is the entire problem solved by the platform that owns it.
And `SDL_KEYDOWN`'s *scancode* names the physical key, which is what an
evdev keyboard reports and what lets the guest apply its own keymap -- so a
guest running `loadkeys dvorak` behaves the way it would on hardware. The
only thing left in the keysym path is what genuinely has no character: the
arrows and navigation block as CSI sequences, and Ctrl+letter.

The 96-entry chord table that remains in `replay_input_script` is the same
assumption, deliberately: a script that says `type a` has to pick a layout
to turn that into a key. It is test scaffolding, it is labelled as such, and
it is not on the path any real keystroke takes.

### 157. Not a bug: the test that measured the test
<a id="bug157"></a>

The mouse appeared not to work. Events were reaching the guest's queue --
instrumentation showed seven delivered into 8-byte writable descriptors, the
used ring published, the APLIC source configured (`cfg=6`), the interrupt
pending and enabled in the IMSIC (`eie=1 thr=0 deliv=1`) -- and `dd
if=/dev/input/event1` sat blocked forever.

Every layer measured as working and the whole did not, which is the shape of
a problem that is not in any of the layers. It was in the test:

```
~ # (dd if=/dev/input/event1 bs=24 coinput script finished
unt=6 2>/dev/null | od -An -tx1; echo MOUSE
-READ-DONE) &
```

`input script finished` is interleaved into the middle of the shell's echo
of the command. The script had sent every event and exited while the guest
was still *typing the command that would read them* -- the guest runs at
about 6.6 MIPS, and forking a subshell takes it a while. `virtio_input`
drops events when no client has the device open, correctly, so all seven
went in the bin.

The fix was `sleep 45000` instead of `sleep 6000`. Then:

```
 02 00 00 00 28 00 00 00     type=EV_REL code=REL_X value=+40
 02 00 01 00 ec ff ff ff     type=EV_REL code=REL_Y value=-20
```

which is `rel 40 -20`, exactly as sent.

Two things worth keeping. Instrumenting every layer was not wasted -- it is
what turned "the mouse does not work" into "every layer works", which is the
observation that points at the harness. And a guest whose clock is driven by
retired instructions makes timing assumptions from the host side wrong in a
way that has nothing to do with how fast the host is: the same script passes
or fails depending on how much work the guest has to do first, and the only
robust answer is to key off something the guest emits rather than off a
delay. `-expect` does that for the console; the input script still cannot,
because a mouse has nothing to say.

