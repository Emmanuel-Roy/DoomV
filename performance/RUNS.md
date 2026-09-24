# Recorded runs

Written by `bench.py`, one line per run. Runs with the same workload and step count
must have the same crash.log hash: that is the proof an optimization changed speed
and nothing else.

| time | workload | label | commit | MIPS | seconds | crash.log | profile |
|---|---|---|---|---:|---:|---|---|
| 20260915-130201 | linux 300M | baseline eaa9628 | eaa9628 | 10.53 | 28.5 | `f1e71792e77d` | [histogram](runs/20260915-130201-linux-baseline-eaa9628/histogram.txt) |
| 20260915-130230 | doom 1000M | baseline eaa9628 | eaa9628 | 16.60 | 60.2 | `3d73dac132f3` | [histogram](runs/20260915-130230-doom-baseline-eaa9628/histogram.txt) |
| 20260915-131004 | linux 300M | A: interrupt cache, imsic topei, history, counters, halt check (reference) | eaa9628+ | 10.52 | 28.5 | `f1e71792e77d` |  |
| 20260915-131033 | linux 300M | A: interrupt cache, imsic topei, history, counters, halt check | eaa9628+ | 17.85 | 16.8 | `f1e71792e77d` |  |
| 20260915-131049 | doom 1000M | A: interrupt cache, imsic topei, history, counters, halt check (reference) | eaa9628+ | 16.68 | 60.0 | `3d73dac132f3` |  |
| 20260915-131149 | doom 1000M | A: interrupt cache, imsic topei, history, counters, halt check | eaa9628+ | 20.59 | 48.6 | `3d73dac132f3` |  |
| 20260915-131334 | linux 300M | A profile | eaa9628+ | 17.98 | 16.7 | `f1e71792e77d` | [histogram](runs/20260915-131334-linux-a-profile/histogram.txt) |
| 20260915-131351 | doom 1000M | A profile | eaa9628+ | 20.46 | 48.9 | `3d73dac132f3` | [histogram](runs/20260915-131351-doom-a-profile/histogram.txt) |
| 20260915-131520 | linux 300M | B: fetch cache, pmp active list, ram write32 (reference) | eaa9628+ | 10.54 | 28.5 | `f1e71792e77d` |  |
| 20260915-131549 | linux 300M | B: fetch cache, pmp active list, ram write32 | eaa9628+ | 27.24 | 11.0 | `f1e71792e77d` |  |
| 20260915-131600 | doom 1000M | B: fetch cache, pmp active list, ram write32 (reference) | eaa9628+ | 16.62 | 60.2 | `3d73dac132f3` |  |
| 20260915-131700 | doom 1000M | B: fetch cache, pmp active list, ram write32 | eaa9628+ | 34.46 | 29.0 | `3d73dac132f3` |  |
| 20260915-131827 | linux 300M | C: inline cache checks, -flto (reference; the -flto build crashed on start, so it has no row -- see README) | eaa9628+ | 10.56 | 28.4 | `f1e71792e77d` |  |
| 20260915-132051 | linux 300M | C: inline cache checks (no -flto) (reference) | eaa9628+ | 10.57 | 28.4 | `f1e71792e77d` |  |
| 20260915-132119 | linux 300M | C: inline cache checks (no -flto) | eaa9628+ | 29.56 | 10.1 | `f1e71792e77d` |  |
| 20260915-132129 | doom 1000M | C: inline cache checks (no -flto) (reference) | eaa9628+ | 16.66 | 60.0 | `3d73dac132f3` |  |
| 20260915-132229 | doom 1000M | C: inline cache checks (no -flto) | eaa9628+ | 37.91 | 26.4 | `3d73dac132f3` |  |
| 20260915-133101 | linux 300M | C + PGO experiment (reference) | eaa9628+ | 10.74 | 27.9 | `f1e71792e77d` |  |
| 20260915-133129 | linux 300M | C + PGO experiment | eaa9628+ | 36.30 | 8.3 | `f1e71792e77d` |  |
| 20260915-133137 | doom 1000M | C + PGO experiment (reference) | eaa9628+ | 16.79 | 59.6 | `3d73dac132f3` |  |
| 20260915-133237 | doom 1000M | C + PGO experiment | eaa9628+ | 50.28 | 19.9 | `3d73dac132f3` |  |
| 20260915-135113 | linux 300M | PGO build (reference) | eaa9628+ | 9.87 | 30.4 | `f1e71792e77d` |  |
| 20260915-135144 | linux 300M | PGO build | eaa9628+ | 34.79 | 8.6 | `f1e71792e77d` |  |
| 20260915-135152 | doom 1000M | PGO build (reference) | eaa9628+ | 16.43 | 60.9 | `3d73dac132f3` |  |
| 20260915-135253 | doom 1000M | PGO build | eaa9628+ | 50.17 | 19.9 | `3d73dac132f3` |  |
| 20260915-135638 | linux 300M | final: PGO build | eaa9628+ | 31.97 | 9.4 | `f1e71792e77d` | [histogram](runs/20260915-135638-linux-final-pgo-build/histogram.txt) |
| 20260915-135648 | doom 1000M | final: PGO build | eaa9628+ | 46.46 | 21.5 | `3d73dac132f3` | [histogram](runs/20260915-135648-doom-final-pgo-build/histogram.txt) |
| 20260915-175233 | linux 300M | D: data caches, inline accessors, decode by reference, batched run loop (reference) | b544ac6+ | 10.56 | 28.4 | `f1e71792e77d` |  |
| 20260915-175302 | linux 300M | D: data caches, inline accessors, decode by reference, batched run loop | b544ac6+ | 46.93 | 6.4 | `f1e71792e77d` |  |
| 20260915-175308 | doom 1000M | D: data caches, inline accessors, decode by reference, batched run loop (reference) | b544ac6+ | 16.75 | 59.7 | `3d73dac132f3` |  |
| 20260915-175408 | doom 1000M | D: data caches, inline accessors, decode by reference, batched run loop | b544ac6+ | 60.04 | 16.7 | `3d73dac132f3` |  |
| 20260915-175539 | linux 300M | D profile | b544ac6+ | 48.64 | 6.2 | `f1e71792e77d` | [histogram](runs/20260915-175539-linux-d-profile/histogram.txt) |
| 20260915-175545 | doom 1000M | D profile | b544ac6+ | 59.45 | 16.8 | `3d73dac132f3` | [histogram](runs/20260915-175545-doom-d-profile/histogram.txt) |
| 20260915-181300 | linux 300M | PGO build (reference) | b544ac6+ | 10.71 | 28.0 | `f1e71792e77d` |  |
| 20260915-181328 | linux 300M | PGO build | b544ac6+ | 59.72 | 5.0 | `f1e71792e77d` |  |
| 20260915-181333 | doom 1000M | PGO build (reference) | b544ac6+ | 16.84 | 59.4 | `3d73dac132f3` |  |
| 20260915-181433 | doom 1000M | PGO build | b544ac6+ | 82.17 | 12.2 | `3d73dac132f3` |  |
| 20260916-120929 | linux 300M | E: caches report to lock-step (reference) | 30a9663+ | 10.70 | 28.0 | `f1e71792e77d` |  |
| 20260916-120957 | linux 300M | E: caches report to lock-step | 30a9663+ | 49.22 | 6.1 | `f1e71792e77d` |  |
| 20260916-121022 | doom 1000M | E: caches report to lock-step (reference) | 30a9663+ | 16.79 | 59.5 | `3d73dac132f3` |  |
| 20260916-121122 | doom 1000M | E: caches report to lock-step | 30a9663+ | 60.52 | 16.5 | `3d73dac132f3` |  |
| 20260916-171959 | linux 300M | gcc14 (reference) | 9ce37c8+ | 9.07 | 33.1 | `f1e71792e77d` |  |
| 20260916-172032 | linux 300M | gcc14 | 9ce37c8+ | 39.83 | 7.5 | `f1e71792e77d` |  |
| 20260916-172039 | doom 1000M | gcc14 (reference) | 9ce37c8+ | 14.31 | 69.9 | `3d73dac132f3` |  |
| 20260916-172149 | doom 1000M | gcc14 | 9ce37c8+ | 54.16 | 18.5 | `3d73dac132f3` |  |
| 20260916-172253 | linux 300M | gcc14 + lto (reference) | 9ce37c8+ | 9.31 | 32.2 | `f1e71792e77d` |  |
| 20260916-172325 | linux 300M | gcc14 + lto | 9ce37c8+ | 43.66 | 6.9 | `f1e71792e77d` |  |
| 20260916-172332 | doom 1000M | gcc14 + lto (reference) | 9ce37c8+ | 14.60 | 68.5 | `3d73dac132f3` |  |
| 20260916-172441 | doom 1000M | gcc14 + lto | 9ce37c8+ | 53.55 | 18.7 | `3d73dac132f3` |  |
| 20260916-172905 | linux 300M | gcc14 + PGO (reference) | 9ce37c8+ | 9.23 | 32.5 | `f1e71792e77d` |  |
| 20260916-172938 | linux 300M | gcc14 + PGO | 9ce37c8+ | 43.59 | 6.9 | `f1e71792e77d` |  |
| 20260916-172945 | doom 1000M | gcc14 + PGO (reference) | 9ce37c8+ | 14.78 | 67.7 | `3d73dac132f3` |  |
| 20260916-173052 | doom 1000M | gcc14 + PGO | 9ce37c8+ | 60.75 | 16.5 | `3d73dac132f3` |  |
| 20260916-173444 | linux 300M | gcc14 + PGO (profiles used) (reference) | 9ce37c8+ | 10.52 | 28.5 | `f1e71792e77d` |  |
| 20260916-173513 | linux 300M | gcc14 + PGO (profiles used) | 9ce37c8+ | 59.43 | 5.0 | `f1e71792e77d` |  |
| 20260916-173518 | doom 1000M | gcc14 + PGO (profiles used) (reference) | 9ce37c8+ | 16.47 | 60.7 | `3d73dac132f3` |  |
| 20260916-173619 | doom 1000M | gcc14 + PGO (profiles used) | 9ce37c8+ | 79.24 | 12.6 | `3d73dac132f3` |  |
| 20260916-173953 | linux 300M | gcc14 + PGO + LTO (reference) | 9ce37c8+ | 10.41 | 28.8 | `f1e71792e77d` |  |
| 20260916-174022 | linux 300M | gcc14 + PGO + LTO | 9ce37c8+ | 64.79 | 4.6 | `f1e71792e77d` |  |
| 20260916-174026 | doom 1000M | gcc14 + PGO + LTO (reference) | 9ce37c8+ | 16.46 | 60.7 | `3d73dac132f3` |  |
| 20260916-174127 | doom 1000M | gcc14 + PGO + LTO | 9ce37c8+ | 86.01 | 11.6 | `3d73dac132f3` |  |
| 20260916-180136 | linux 300M | PGO build (reference) | 9ce37c8+ | 10.43 | 28.8 | `f1e71792e77d` |  |
| 20260916-180205 | linux 300M | PGO build | 9ce37c8+ | 59.50 | 5.0 | `f1e71792e77d` |  |
| 20260916-180210 | doom 1000M | PGO build (reference) | 9ce37c8+ | 16.52 | 60.5 | `3d73dac132f3` |  |
| 20260916-180311 | doom 1000M | PGO build | 9ce37c8+ | 81.03 | 12.3 | `3d73dac132f3` |  |
| 20260919-203217 | linux 300M | F: handler pointer -- INVALID PAIR, this reference is the PGO binary, not a plain build | 38bc40e+ | 58.09 | 5.2 | `f1e71792e77d` |  |
| 20260919-203222 | linux 300M | F: handler pointer -- INVALID PAIR, measures PGO vs plain, not the change | 38bc40e+ | 46.11 | 6.5 | `f1e71792e77d` |  |
| 20260919-203308 | linux 300M | F: dispatch through a handler pointer (reference) | 38bc40e+ | 47.08 | 6.4 | `f1e71792e77d` |  |
| 20260919-203314 | linux 300M | F: dispatch through a handler pointer | 38bc40e+ | 47.01 | 6.4 | `f1e71792e77d` |  |
| 20260919-203328 | doom 1000M | F: dispatch through a handler pointer (reference) | 38bc40e+ | 58.25 | 17.2 | `3d73dac132f3` |  |
| 20260919-203345 | doom 1000M | F: dispatch through a handler pointer | 38bc40e+ | 53.69 | 18.6 | `3d73dac132f3` |  |
| 20260919-203411 | doom 1000M | F: handler pointer (repeat) (reference) | 38bc40e+ | 58.75 | 17.0 | `3d73dac132f3` |  |
| 20260919-203428 | doom 1000M | F: handler pointer (repeat) | 38bc40e+ | 57.71 | 17.3 | `3d73dac132f3` |  |
| 20260919-203453 | doom 1000M | F: handler pointer (third) (reference) | 38bc40e+ | 58.50 | 17.1 | `3d73dac132f3` |  |
| 20260919-203510 | doom 1000M | F: handler pointer (third) | 38bc40e+ | 58.53 | 17.1 | `3d73dac132f3` |  |
| 20260919-203527 | linux 300M | F: handler pointer (linux repeat) (reference) | 38bc40e+ | 48.52 | 6.2 | `f1e71792e77d` |  |
| 20260919-203533 | linux 300M | F: handler pointer (linux repeat) | 38bc40e+ | 48.15 | 6.2 | `f1e71792e77d` |  |
| 20260919-203807 | linux 300M | G: one event generation, masked history (reference) | 38bc40e+ | 47.34 | 6.3 | `f1e71792e77d` |  |
| 20260919-203813 | linux 300M | G: one event generation, masked history | 38bc40e+ | 48.36 | 6.2 | `f1e71792e77d` |  |
| 20260919-203824 | doom 1000M | G: one event generation, masked history (reference) | 38bc40e+ | 58.99 | 17.0 | `3d73dac132f3` |  |
| 20260919-203841 | doom 1000M | G: one event generation, masked history | 38bc40e+ | 59.74 | 16.7 | `3d73dac132f3` |  |
| 20260919-203905 | doom 1000M | G: event gen (repeat) (reference) | 38bc40e+ | 59.02 | 16.9 | `3d73dac132f3` |  |
| 20260919-203922 | doom 1000M | G: event gen (repeat) | 38bc40e+ | 59.76 | 16.7 | `3d73dac132f3` |  |
| 20260919-203939 | linux 300M | G: event gen (linux repeat) (reference) | 38bc40e+ | 49.13 | 6.1 | `f1e71792e77d` |  |
| 20260919-203945 | linux 300M | G: event gen (linux repeat) | 38bc40e+ | 49.72 | 6.0 | `f1e71792e77d` |  |
| 20260919-205417 | doom 1000M | H: whole-instruction fetch in one lookup (reference) | 38bc40e+ | 59.84 | 16.7 | `3d73dac132f3` |  |
| 20260919-205434 | doom 1000M | H: whole-instruction fetch in one lookup | 38bc40e+ | 69.86 | 14.3 | `3d73dac132f3` |  |
| 20260919-205454 | linux 300M | H: whole-instruction fetch in one lookup (reference) | 38bc40e+ | 49.41 | 6.1 | `f1e71792e77d` |  |
| 20260919-205500 | linux 300M | H: whole-instruction fetch in one lookup | 38bc40e+ | 55.52 | 5.4 | `f1e71792e77d` |  |
| 20260919-210911 | doom 1000M | H profile: after fetch fusion | 7e8e827 | 69.56 | 14.4 | `3d73dac132f3` | [histogram](runs/20260919-210911-doom-h-profile-after-fetch-fusion/histogram.txt) |
| 20260919-211148 | doom 1000M | I: decodes in the code page's own table (reference) | 7e8e827+ | 70.03 | 14.3 | `3d73dac132f3` |  |
| 20260919-211202 | doom 1000M | I: decodes in the code page's own table | 7e8e827+ | 65.42 | 15.3 | `3d73dac132f3` |  |
| 20260919-211306 | doom 1000M | I2: page decode tables, 64, no clear (reference) | 7e8e827+ | 69.61 | 14.4 | `3d73dac132f3` |  |
| 20260919-211320 | doom 1000M | I2: page decode tables, 64, no clear | 7e8e827+ | 66.50 | 15.0 | `3d73dac132f3` |  |
| 20260919-211451 | doom 1000M | HEAD c4af636 vs 38bc40e plain (reference) | c4af636 | 58.03 | 17.2 | `3d73dac132f3` |  |
| 20260919-211508 | doom 1000M | HEAD c4af636 vs 38bc40e plain | c4af636 | 69.47 | 14.4 | `3d73dac132f3` |  |
| 20260919-211523 | linux 300M | HEAD c4af636 vs 38bc40e plain (reference) | c4af636 | 48.92 | 6.1 | `f1e71792e77d` |  |
| 20260919-211529 | linux 300M | HEAD c4af636 vs 38bc40e plain | c4af636 | 55.30 | 5.4 | `f1e71792e77d` |  |
| 20260921-112614 | doom 1000M | J: clang 23 -O3 (reference is gcc 8.1 -O3) (reference) | b97fd13 | 63.47 | 15.8 | `3d73dac132f3` |  |
| 20260921-112630 | doom 1000M | J: clang 23 -O3 (reference is gcc 8.1 -O3) | b97fd13 | 74.17 | 13.5 | `3d73dac132f3` |  |
| 20260921-112650 | linux 300M | J: clang 23 -O3 (reference is gcc 8.1 -O3) (reference) | b97fd13 | 53.17 | 5.6 | `f1e71792e77d` |  |
| 20260921-112656 | linux 300M | J: clang 23 -O3 (reference is gcc 8.1 -O3) | b97fd13 | 56.05 | 5.4 | `f1e71792e77d` |  |
| 20260921-112737 | doom 1000M | K: clang 23 + ThinLTO (reference) | b97fd13 | 77.49 | 12.9 | `3d73dac132f3` |  |
| 20260921-112750 | doom 1000M | K: clang 23 + ThinLTO | b97fd13 | 79.26 | 12.6 | `3d73dac132f3` |  |
| 20260921-112802 | linux 300M | K: clang 23 + ThinLTO (reference) | b97fd13 | 56.48 | 5.3 | `f1e71792e77d` |  |
| 20260921-112808 | linux 300M | K: clang 23 + ThinLTO | b97fd13 | 56.47 | 5.3 | `f1e71792e77d` |  |
| 20260921-113015 | doom 1000M | L: clang 23 + PGO (reference) | b97fd13 | 76.44 | 13.1 | `3d73dac132f3` |  |
| 20260921-113029 | doom 1000M | L: clang 23 + PGO | b97fd13 | 90.30 | 11.1 | `3d73dac132f3` |  |
| 20260921-113040 | linux 300M | L: clang 23 + PGO (reference) | b97fd13 | 55.49 | 5.4 | `f1e71792e77d` |  |
| 20260921-113045 | linux 300M | L: clang 23 + PGO | b97fd13 | 64.44 | 4.7 | `f1e71792e77d` |  |
| 20260921-113226 | doom 1000M | M: clang 23 + PGO + ThinLTO (reference) | b97fd13 | 92.83 | 10.8 | `3d73dac132f3` |  |
| 20260921-113237 | doom 1000M | M: clang 23 + PGO + ThinLTO | b97fd13 | 111.14 | 9.0 | `3d73dac132f3` |  |
| 20260921-113246 | linux 300M | M: clang 23 + PGO + ThinLTO (reference) | b97fd13 | 67.72 | 4.4 | `f1e71792e77d` |  |
| 20260921-113250 | linux 300M | M: clang 23 + PGO + ThinLTO | b97fd13 | 79.04 | 3.8 | `f1e71792e77d` |  |
| 20260921-114919 | doom 1000M | N: best vs best -- clang PGO+ThinLTO over gcc 8.1 PGO (reference) | b97fd13 | 88.86 | 11.3 | `3d73dac132f3` |  |
| 20260921-114930 | doom 1000M | N: best vs best -- clang PGO+ThinLTO over gcc 8.1 PGO | b97fd13 | 116.57 | 8.6 | `3d73dac132f3` |  |
| 20260921-114939 | linux 300M | N: best vs best -- clang PGO+ThinLTO over gcc 8.1 PGO (reference) | b97fd13 | 64.81 | 4.6 | `f1e71792e77d` |  |
| 20260921-114944 | linux 300M | N: best vs best -- clang PGO+ThinLTO over gcc 8.1 PGO | b97fd13 | 80.00 | 3.8 | `f1e71792e77d` |  |
| 20260921-125556 | doom 1000M | O: fflags order fix, gcc (reference is pre-fix gcc) (reference) | 5bd434d+ | 68.33 | 14.6 | `3d73dac132f3` |  |
| 20260921-125611 | doom 1000M | O: fflags order fix, gcc (reference is pre-fix gcc) | 5bd434d+ | 69.55 | 14.4 | `3d73dac132f3` |  |
| 20260921-131014 | doom 1000M | P: clang PGO+ThinLTO with fflags fix vs gcc 8.1 PGO (reference) | 5bd434d+ | 88.16 | 11.3 | `3d73dac132f3` |  |
| 20260921-131026 | doom 1000M | P: clang PGO+ThinLTO with fflags fix vs gcc 8.1 PGO | 5bd434d+ | 113.22 | 8.8 | `3d73dac132f3` |  |
| 20260921-131034 | linux 300M | P: clang PGO+ThinLTO with fflags fix vs gcc 8.1 PGO (reference) | 5bd434d+ | 63.77 | 4.7 | `f1e71792e77d` |  |
| 20260921-131039 | linux 300M | P: clang PGO+ThinLTO with fflags fix vs gcc 8.1 PGO | 5bd434d+ | 79.97 | 3.8 | `f1e71792e77d` |  |
| 20260923-123150 | doom 1000M | Q: gcc fallback vs clang, same sources (reference) | 10e0f6d+ | 78.72 | 12.7 | `3d73dac132f3` |  |
| 20260923-123203 | doom 1000M | Q: gcc fallback vs clang, same sources | 10e0f6d+ | 70.40 | 14.2 | `3d73dac132f3` |  |
| 20260923-123344 | doom 1000M | R: bare pgo.py (now clang) vs gcc 8.1 PGO (reference) | 10e0f6d+ | 89.18 | 11.2 | `3d73dac132f3` |  |
| 20260923-123355 | doom 1000M | R: bare pgo.py (now clang) vs gcc 8.1 PGO | 10e0f6d+ | 95.29 | 10.5 | `3d73dac132f3` |  |
| 20260923-124921 | doom 1000M | S: default make with ThinLTO vs without (reference) | 942eb19+ | 78.31 | 12.8 | `3d73dac132f3` |  |
| 20260923-124934 | doom 1000M | S: default make with ThinLTO vs without | 942eb19+ | 81.48 | 12.3 | `3d73dac132f3` |  |
| 20260923-124946 | linux 300M | S: default make with ThinLTO vs without (reference) | 942eb19+ | 57.27 | 5.2 | `f1e71792e77d` |  |
| 20260923-124952 | linux 300M | S: default make with ThinLTO vs without | 942eb19+ | 58.54 | 5.1 | `f1e71792e77d` |  |
| 20260924-170747 | doom 1000M | T: ram=vector | 972acde+ | 77.26 | 12.9 | `3d73dac132f3` |  |
| 20260924-170800 | doom 1000M | T: ram=pages | 972acde+ | 81.44 | 12.3 | `3d73dac132f3` |  |
| 20260924-170819 | doom 1000M | T: ram=vector rep1 | 972acde+ | 80.20 | 12.5 | `3d73dac132f3` |  |
| 20260924-170831 | doom 1000M | T: ram=pages rep1 | 972acde+ | 80.67 | 12.4 | `3d73dac132f3` |  |
| 20260924-170844 | doom 1000M | T: ram=vector rep2 | 972acde+ | 80.79 | 12.4 | `3d73dac132f3` |  |
| 20260924-170856 | doom 1000M | T: ram=pages rep2 | 972acde+ | 81.27 | 12.3 | `3d73dac132f3` |  |
| 20260924-175440 | doom 1000M | U: configurable ram + GuestRam vs fixed constexpr (reference) | 972acde+ | 78.94 | 12.7 | `3d73dac132f3` |  |
| 20260924-175453 | doom 1000M | U: configurable ram + GuestRam vs fixed constexpr | 972acde+ | 81.45 | 12.3 | `3d73dac132f3` |  |
| 20260924-181602 | linux 300M | V: linux hash check after -ram work (reference) | d54fa5e | 56.27 | 5.3 | `e76932b2b713` |  |
| 20260924-181608 | linux 300M | V: linux hash check after -ram work | d54fa5e | 59.50 | 5.0 | `e76932b2b713` |  |
