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
| 20260924-185543 | ubuntu 3000M | ubuntu-xfce boot, first | cfd8034 | 54.30 | 55.2 | `2ca0ac5022e1` |  |
| 20260924-185639 | ubuntu 3000M | ubuntu-xfce boot, repeat | cfd8034 | 53.61 | 56.0 | `2ca0ac5022e1` |  |
| 20260924-192452 | ubuntu 3000M | ubuntu boot: PGO+ThinLTO vs plain make (reference) | 8387f26 | 55.70 | 53.9 | `74c11f512233` |  |
| 20260924-192547 | ubuntu 3000M | ubuntu boot: PGO+ThinLTO vs plain make | 8387f26 | 75.61 | 39.7 | `74c11f512233` |  |
| 20260924-224320 | ubuntu 3000M | make fast (PGO+ThinLTO default) vs plain (reference) | 8387f26+ | 53.97 | 55.6 | `74c11f512233` |  |
| 20260924-224417 | ubuntu 3000M | make fast (PGO+ThinLTO default) vs plain | 8387f26+ | 75.89 | 39.5 | `74c11f512233` |  |
| 20260925-111034 | doom 1000M | W: merged branch, plain build vs pre-merge plain (reference) | 1b59d58+ | 80.67 | 12.4 | `3d73dac132f3` |  |
| 20260925-111046 | doom 1000M | W: merged branch, plain build vs pre-merge plain | 1b59d58+ | 83.71 | 11.9 | `3d73dac132f3` |  |
| 20260925-111309 | ubuntu 3000M | X: merged branch PGO vs pre-merge PGO, ubuntu boot (reference) | 1b59d58+ | 73.43 | 40.9 | `74c11f512233` |  |
| 20260925-111352 | ubuntu 3000M | X: merged branch PGO vs pre-merge PGO, ubuntu boot | 1b59d58+ | 81.53 | 36.8 | `74c11f512233` |  |
| 20260925-113922 | doom 1000M | Y: fast loop vs merged, doom (reference) | 288b1df+ | 126.17 | 7.9 | `3d73dac132f3` |  |
| 20260925-113930 | doom 1000M | Y: fast loop vs merged, doom | 288b1df+ | 247.92 | 4.0 | `3d73dac132f3` |  |
| 20260925-114051 | doom 1000M | Z: fast loop + retrained PGO vs merged (reference) | 288b1df+ | 125.73 | 8.0 | `3d73dac132f3` |  |
| 20260925-114059 | doom 1000M | Z: fast loop + retrained PGO vs merged | 288b1df+ | 281.44 | 3.6 | `3d73dac132f3` |  |
| 20260925-114102 | linux 300M | Z: fast loop + retrained PGO vs merged (reference) | 288b1df+ | 93.63 | 3.2 | `f5894f934a68` |  |
| 20260925-114106 | linux 300M | Z: fast loop + retrained PGO vs merged | 288b1df+ | 204.08 | 1.5 | `f5894f934a68` |  |
| 20260925-114107 | ubuntu 3000M | Z: fast loop + retrained PGO vs merged (reference) | 288b1df+ | 81.45 | 36.8 | `74c11f512233` |  |
| 20260925-114146 | ubuntu 3000M | Z: fast loop + retrained PGO vs merged | 288b1df+ | 161.68 | 18.6 | `74c11f512233` |  |
| 20260929-125439 | doom 1000M | research 09-29 profile | 82a34b1 | 279.64 | 3.6 | `3d73dac132f3` | [histogram](runs/20260929-125439-doom-research-09-29-profile/histogram.txt) |
| 20260929-125443 | linux 300M | research 09-29 profile | 82a34b1 | 196.12 | 1.5 | `38c23122994c` | [histogram](runs/20260929-125443-linux-research-09-29-profile/histogram.txt) |
| 20260929-125444 | ubuntu 3000M | research 09-29 profile | 82a34b1 | 156.70 | 19.1 | `9be8284ab1d9` | [histogram](runs/20260929-125444-ubuntu-research-09-29-profile/histogram.txt) |
| 20260929-125540 | linux 300M | faststats | 82a34b1 | 201.93 | 1.5 | `38c23122994c` |  |
| 20260929-125541 | ubuntu 3000M | faststats | 82a34b1 | 161.25 | 18.6 | `9be8284ab1d9` |  |
| 20260929-125602 | doom 1000M | faststats | 82a34b1 | 285.44 | 3.5 | `3d73dac132f3` |  |
| 20260929-130405 | linux 300M | proto: priv key, csr no-op writes, tlb gen+type (reference) | 82a34b1 | 192.89 | 1.6 | `38c23122994c` |  |
| 20260929-130406 | linux 300M | proto: priv key, csr no-op writes, tlb gen+type | 82a34b1 | 190.66 | 1.6 | `38c23122994c` |  |
| 20260929-130408 | doom 1000M | proto: priv key, csr no-op writes, tlb gen+type (reference) | 82a34b1 | 291.97 | 3.4 | `3d73dac132f3` |  |
| 20260929-130412 | doom 1000M | proto: priv key, csr no-op writes, tlb gen+type | 82a34b1 | 279.37 | 3.6 | `3d73dac132f3` |  |
| 20260929-130415 | ubuntu 3000M | proto: priv key, csr no-op writes, tlb gen+type (reference) | 82a34b1 | 163.02 | 18.4 | `9be8284ab1d9` |  |
| 20260929-130435 | ubuntu 3000M | proto: priv key, csr no-op writes, tlb gen+type | 82a34b1 | 172.21 | 17.4 | `9be8284ab1d9` |  |
| 20260929-130750 | ubuntu 3000M | proto2: split fetch/data keys, SUM/MXR in key, MPP only under MPRV (reference) | 82a34b1 | 165.86 | 18.1 | `9be8284ab1d9` |  |
| 20260929-130809 | ubuntu 3000M | proto2: split fetch/data keys, SUM/MXR in key, MPP only under MPRV | 82a34b1 | 175.27 | 17.1 | `9be8284ab1d9` |  |
| 20260929-130828 | linux 300M | proto2: split fetch/data keys, SUM/MXR in key, MPP only under MPRV (reference) | 82a34b1 | 204.13 | 1.5 | `38c23122994c` |  |
| 20260929-130830 | linux 300M | proto2: split fetch/data keys, SUM/MXR in key, MPP only under MPRV | 82a34b1 | 212.50 | 1.4 | `38c23122994c` |  |
| 20260929-130910 | ubuntu 3000M | proto2 profile | 82a34b1 | 174.58 | 17.2 | `9be8284ab1d9` | [histogram](runs/20260929-130910-ubuntu-proto2-profile/histogram.txt) |
| 20260929-131215 | ubuntu 3000M | proto3: history index kept local in run_fast (vs proto2) (reference) | 82a34b1 | 180.62 | 16.6 | `9be8284ab1d9` |  |
| 20260929-131234 | ubuntu 3000M | proto3: history index kept local in run_fast (vs proto2) | 82a34b1 | 188.21 | 15.9 | `9be8284ab1d9` |  |
| 20260929-131251 | linux 300M | proto3: history index kept local in run_fast (vs proto2) (reference) | 82a34b1 | 211.34 | 1.4 | `38c23122994c` |  |
| 20260929-131253 | linux 300M | proto3: history index kept local in run_fast (vs proto2) | 82a34b1 | 220.02 | 1.4 | `38c23122994c` |  |
| 20260929-131254 | doom 1000M | proto3: history index kept local in run_fast (vs proto2) (reference) | 82a34b1 | 284.89 | 3.5 | `3d73dac132f3` |  |
| 20260929-131258 | doom 1000M | proto3: history index kept local in run_fast (vs proto2) | 82a34b1 | 290.37 | 3.4 | `3d73dac132f3` |  |
| 20260929-131413 | ubuntu 3000M | proto3 (cache keys, tlb gen+type, local history) vs shipped 82a34b1 (reference) | 82a34b1 | 169.10 | 17.7 | `9be8284ab1d9` |  |
| 20260929-131432 | ubuntu 3000M | proto3 (cache keys, tlb gen+type, local history) vs shipped 82a34b1 | 82a34b1 | 191.21 | 15.7 | `9be8284ab1d9` |  |
| 20260929-131450 | linux 300M | proto3 (cache keys, tlb gen+type, local history) vs shipped 82a34b1 (reference) | 82a34b1 | 203.16 | 1.5 | `38c23122994c` |  |
| 20260929-131451 | linux 300M | proto3 (cache keys, tlb gen+type, local history) vs shipped 82a34b1 | 82a34b1 | 218.49 | 1.4 | `38c23122994c` |  |
| 20260929-131453 | doom 1000M | proto3 (cache keys, tlb gen+type, local history) vs shipped 82a34b1 (reference) | 82a34b1 | 292.86 | 3.4 | `3d73dac132f3` |  |
| 20260929-131456 | doom 1000M | proto3 (cache keys, tlb gen+type, local history) vs shipped 82a34b1 | 82a34b1 | 293.69 | 3.4 | `3d73dac132f3` |  |
| 20260929-131830 | ubuntu 3000M | cache-keys.patch (clean) vs shipped 82a34b1 (reference) | 82a34b1 | 165.15 | 18.2 | `9be8284ab1d9` |  |
| 20260929-131851 | ubuntu 3000M | cache-keys.patch (clean) vs shipped 82a34b1 | 82a34b1 | 191.81 | 15.6 | `9be8284ab1d9` |  |
| 20260929-131908 | linux 300M | cache-keys.patch (clean) vs shipped 82a34b1 (reference) | 82a34b1 | 204.99 | 1.5 | `38c23122994c` |  |
| 20260929-131910 | linux 300M | cache-keys.patch (clean) vs shipped 82a34b1 | 82a34b1 | 221.35 | 1.4 | `38c23122994c` |  |
| 20260929-131911 | doom 1000M | cache-keys.patch (clean) vs shipped 82a34b1 (reference) | 82a34b1 | 291.19 | 3.4 | `3d73dac132f3` |  |
| 20260929-131915 | doom 1000M | cache-keys.patch (clean) vs shipped 82a34b1 | 82a34b1 | 297.81 | 3.4 | `3d73dac132f3` |  |
| 20260929-145814 | ubuntu 3000M | cache keys landed, PGO retrained, vs 82a34b1 (reference) | 82a34b1+ | 168.38 | 17.8 | `9be8284ab1d9` |  |
| 20260929-145834 | ubuntu 3000M | cache keys landed, PGO retrained, vs 82a34b1 | 82a34b1+ | 187.70 | 16.0 | `9be8284ab1d9` |  |
| 20260929-145851 | linux 300M | cache keys landed, PGO retrained, vs 82a34b1 (reference) | 82a34b1+ | 195.70 | 1.5 | `38c23122994c` |  |
| 20260929-145853 | linux 300M | cache keys landed, PGO retrained, vs 82a34b1 | 82a34b1+ | 215.03 | 1.4 | `38c23122994c` |  |
| 20260929-145854 | doom 1000M | cache keys landed, PGO retrained, vs 82a34b1 (reference) | 82a34b1+ | 298.94 | 3.3 | `3d73dac132f3` |  |
| 20260929-145858 | doom 1000M | cache keys landed, PGO retrained, vs 82a34b1 | 82a34b1+ | 288.69 | 3.5 | `3d73dac132f3` |  |
| 20260929-145931 | doom 1000M | patch: old profile vs retrained profile, rep 1 (reference) | 82a34b1+ | 299.23 | 3.3 | `3d73dac132f3` |  |
| 20260929-145935 | doom 1000M | patch: old profile vs retrained profile, rep 1 | 82a34b1+ | 284.34 | 3.5 | `3d73dac132f3` |  |
| 20260929-145938 | ubuntu 3000M | patch: old profile vs retrained profile, rep 1 (reference) | 82a34b1+ | 184.60 | 16.3 | `9be8284ab1d9` |  |
| 20260929-145957 | ubuntu 3000M | patch: old profile vs retrained profile, rep 1 | 82a34b1+ | 191.65 | 15.7 | `9be8284ab1d9` |  |
| 20260929-150015 | doom 1000M | patch: old profile vs retrained profile, rep 2 (reference) | 82a34b1+ | 298.15 | 3.4 | `3d73dac132f3` |  |
| 20260929-150018 | doom 1000M | patch: old profile vs retrained profile, rep 2 | 82a34b1+ | 300.28 | 3.3 | `3d73dac132f3` |  |
| 20260929-150022 | ubuntu 3000M | patch: old profile vs retrained profile, rep 2 (reference) | 82a34b1+ | 188.64 | 15.9 | `9be8284ab1d9` |  |
| 20260929-150039 | ubuntu 3000M | patch: old profile vs retrained profile, rep 2 | 82a34b1+ | 190.69 | 15.7 | `9be8284ab1d9` |  |
| 20260929-224919 | ubuntu 3000M | fence-page (sfence.vma addr drops one page) vs 11ff5b3 (reference) | 11ff5b3+ | 188.56 | 15.9 | `9be8284ab1d9` |  |
| 20260929-224937 | ubuntu 3000M | fence-page (sfence.vma addr drops one page) vs 11ff5b3 | 11ff5b3+ | 196.22 | 15.3 | `9be8284ab1d9` |  |
| 20260929-224954 | linux 300M | fence-page (sfence.vma addr drops one page) vs 11ff5b3 (reference) | 11ff5b3+ | 216.16 | 1.4 | `38c23122994c` |  |
| 20260929-224955 | linux 300M | fence-page (sfence.vma addr drops one page) vs 11ff5b3 | 11ff5b3+ | 214.34 | 1.4 | `38c23122994c` |  |
| 20260929-224957 | doom 1000M | fence-page (sfence.vma addr drops one page) vs 11ff5b3 (reference) | 11ff5b3+ | 295.63 | 3.4 | `3d73dac132f3` |  |
| 20260929-225000 | doom 1000M | fence-page (sfence.vma addr drops one page) vs 11ff5b3 | 11ff5b3+ | 293.36 | 3.4 | `3d73dac132f3` |  |
| 20260930-135454 | ubuntu 3000M | decode cache second way vs 35dd091 (reference) | 35dd091+ | 195.15 | 15.4 | `9be8284ab1d9` |  |
| 20260930-135513 | ubuntu 3000M | decode cache second way vs 35dd091 | 35dd091+ | 181.02 | 16.6 | `9be8284ab1d9` |  |
| 20260930-135540 | linux 300M | decode cache second way vs 35dd091 (reference) | 35dd091+ | 217.50 | 1.4 | `38c23122994c` |  |
| 20260930-135542 | linux 300M | decode cache second way vs 35dd091 | 35dd091+ | 201.87 | 1.5 | `38c23122994c` |  |
| 20260930-135543 | doom 1000M | decode cache second way vs 35dd091 (reference) | 35dd091+ | 293.32 | 3.4 | `3d73dac132f3` |  |
| 20260930-135547 | doom 1000M | decode cache second way vs 35dd091 | 35dd091+ | 263.40 | 3.8 | `3d73dac132f3` |  |
| 20260930-135707 | ubuntu 3000M | decode cache second way, PGO retrained, vs 35dd091 (reference) | 35dd091+ | 201.06 | 14.9 | `9be8284ab1d9` |  |
| 20260930-135723 | ubuntu 3000M | decode cache second way, PGO retrained, vs 35dd091 | 35dd091+ | 194.98 | 15.4 | `9be8284ab1d9` |  |
| 20260930-135740 | linux 300M | decode cache second way, PGO retrained, vs 35dd091 (reference) | 35dd091+ | 222.36 | 1.3 | `38c23122994c` |  |
| 20260930-135742 | linux 300M | decode cache second way, PGO retrained, vs 35dd091 | 35dd091+ | 215.87 | 1.4 | `38c23122994c` |  |
| 20260930-135743 | doom 1000M | decode cache second way, PGO retrained, vs 35dd091 (reference) | 35dd091+ | 301.50 | 3.3 | `3d73dac132f3` |  |
| 20260930-135746 | doom 1000M | decode cache second way, PGO retrained, vs 35dd091 | 35dd091+ | 301.27 | 3.3 | `3d73dac132f3` |  |
| 20260930-135801 | ubuntu 3000M | faststats decode2 (reference) | 35dd091+ | 197.50 | 15.2 | `9be8284ab1d9` |  |
| 20260930-135819 | ubuntu 3000M | faststats decode2 | 35dd091+ | 195.65 | 15.3 | `9be8284ab1d9` |  |
| 20260930-140118 | ubuntu 3000M | decode cache second way, no swap, PGO retrained, vs 35dd091 (reference) | 35dd091+ | 201.04 | 14.9 | `9be8284ab1d9` |  |
| 20260930-140135 | ubuntu 3000M | decode cache second way, no swap, PGO retrained, vs 35dd091 | 35dd091+ | 192.21 | 15.6 | `9be8284ab1d9` |  |
| 20260930-140154 | linux 300M | decode cache second way, no swap, PGO retrained, vs 35dd091 (reference) | 35dd091+ | 219.53 | 1.4 | `38c23122994c` |  |
| 20260930-140155 | linux 300M | decode cache second way, no swap, PGO retrained, vs 35dd091 | 35dd091+ | 217.34 | 1.4 | `38c23122994c` |  |
| 20260930-140157 | doom 1000M | decode cache second way, no swap, PGO retrained, vs 35dd091 (reference) | 35dd091+ | 300.36 | 3.3 | `3d73dac132f3` |  |
| 20260930-140200 | doom 1000M | decode cache second way, no swap, PGO retrained, vs 35dd091 | 35dd091+ | 305.28 | 3.3 | `3d73dac132f3` |  |
| 20260930-142548 | ubuntu 3000M | atomics in the fast loop, PGO retrained, vs 35dd091 (reference) | df544f0+ | 200.76 | 14.9 | `9be8284ab1d9` |  |
| 20260930-142605 | ubuntu 3000M | atomics in the fast loop, PGO retrained, vs 35dd091 | df544f0+ | 200.83 | 14.9 | `9be8284ab1d9` |  |
| 20260930-142622 | linux 300M | atomics in the fast loop, PGO retrained, vs 35dd091 (reference) | df544f0+ | 221.72 | 1.4 | `38c23122994c` |  |
| 20260930-142624 | linux 300M | atomics in the fast loop, PGO retrained, vs 35dd091 | df544f0+ | 226.93 | 1.3 | `38c23122994c` |  |
| 20260930-142625 | doom 1000M | atomics in the fast loop, PGO retrained, vs 35dd091 (reference) | df544f0+ | 300.31 | 3.3 | `3d73dac132f3` |  |
| 20260930-142629 | doom 1000M | atomics in the fast loop, PGO retrained, vs 35dd091 | df544f0+ | 302.61 | 3.3 | `3d73dac132f3` |  |
| 20260930-144038 | doom 1000M | blocks v1, stale profile, vs 35dd091 (reference) | c435e02+ | 299.73 | 3.3 | `3d73dac132f3` |  |
| 20260930-144041 | doom 1000M | blocks v1, stale profile, vs 35dd091 | c435e02+ | 220.17 | 4.5 | `3d73dac132f3` |  |
| 20260930-144046 | linux 300M | blocks v1, stale profile, vs 35dd091 (reference) | c435e02+ | 221.76 | 1.4 | `38c23122994c` |  |
| 20260930-144047 | linux 300M | blocks v1, stale profile, vs 35dd091 | c435e02+ | 171.95 | 1.7 | `38c23122994c` |  |
| 20260930-144049 | ubuntu 3000M | blocks v1, stale profile, vs 35dd091 (reference) | c435e02+ | 197.40 | 15.2 | `9be8284ab1d9` |  |
| 20260930-144107 | ubuntu 3000M | blocks v1, stale profile, vs 35dd091 | c435e02+ | 156.73 | 19.1 | `9be8284ab1d9` |  |
| 20260930-144316 | doom 1000M | blocks v1, PGO retrained, vs 35dd091 (reference) | c435e02+ | 299.94 | 3.3 | `3d73dac132f3` |  |
| 20260930-144319 | doom 1000M | blocks v1, PGO retrained, vs 35dd091 | c435e02+ | 286.01 | 3.5 | `3d73dac132f3` |  |
| 20260930-144323 | ubuntu 3000M | blocks v1, PGO retrained, vs 35dd091 (reference) | c435e02+ | 201.33 | 14.9 | `9be8284ab1d9` |  |
| 20260930-144339 | ubuntu 3000M | blocks v1, PGO retrained, vs 35dd091 | c435e02+ | 177.10 | 16.9 | `9be8284ab1d9` |  |
| 20260930-144607 | doom 1000M | blocks v2 (inline compare, build on second ask, max 8), PGO, vs 35dd091 (reference) | c435e02+ | 300.56 | 3.3 | `3d73dac132f3` |  |
| 20260930-144610 | doom 1000M | blocks v2 (inline compare, build on second ask, max 8), PGO, vs 35dd091 | c435e02+ | 287.51 | 3.5 | `3d73dac132f3` |  |
| 20260930-144613 | linux 300M | blocks v2 (inline compare, build on second ask, max 8), PGO, vs 35dd091 (reference) | c435e02+ | 219.60 | 1.4 | `38c23122994c` |  |
| 20260930-144615 | linux 300M | blocks v2 (inline compare, build on second ask, max 8), PGO, vs 35dd091 | c435e02+ | 202.40 | 1.5 | `38c23122994c` |  |
| 20260930-144616 | ubuntu 3000M | blocks v2 (inline compare, build on second ask, max 8), PGO, vs 35dd091 (reference) | c435e02+ | 199.44 | 15.0 | `9be8284ab1d9` |  |
| 20260930-144633 | ubuntu 3000M | blocks v2 (inline compare, build on second ask, max 8), PGO, vs 35dd091 | c435e02+ | 181.99 | 16.5 | `9be8284ab1d9` |  |
| 20260930-144947 | doom 1000M | 16-byte fast decode table, PGO, vs 35dd091 (reference) | c435e02+ | 300.28 | 3.3 | `3d73dac132f3` |  |
| 20260930-144951 | doom 1000M | 16-byte fast decode table, PGO, vs 35dd091 | c435e02+ | 305.00 | 3.3 | `3d73dac132f3` |  |
| 20260930-144954 | linux 300M | 16-byte fast decode table, PGO, vs 35dd091 (reference) | c435e02+ | 219.58 | 1.4 | `38c23122994c` |  |
| 20260930-144955 | linux 300M | 16-byte fast decode table, PGO, vs 35dd091 | c435e02+ | 217.24 | 1.4 | `38c23122994c` |  |
| 20260930-144957 | ubuntu 3000M | 16-byte fast decode table, PGO, vs 35dd091 (reference) | c435e02+ | 200.28 | 15.0 | `9be8284ab1d9` |  |
| 20260930-145013 | ubuntu 3000M | 16-byte fast decode table, PGO, vs 35dd091 | c435e02+ | 207.92 | 14.4 | `9be8284ab1d9` |  |
| 20260930-145035 | ubuntu 3000M | 16-byte fast decode table, PGO, repeat 1 (reference) | c435e02+ | 199.44 | 15.0 | `9be8284ab1d9` |  |
| 20260930-145051 | ubuntu 3000M | 16-byte fast decode table, PGO, repeat 1 | c435e02+ | 206.72 | 14.5 | `9be8284ab1d9` |  |
| 20260930-145107 | linux 300M | 16-byte fast decode table, PGO, repeat 1 (reference) | c435e02+ | 219.43 | 1.4 | `38c23122994c` |  |
| 20260930-145109 | linux 300M | 16-byte fast decode table, PGO, repeat 1 | c435e02+ | 227.55 | 1.3 | `38c23122994c` |  |
| 20260930-145110 | doom 1000M | 16-byte fast decode table, PGO, repeat 1 (reference) | c435e02+ | 300.58 | 3.3 | `3d73dac132f3` |  |
| 20260930-145114 | doom 1000M | 16-byte fast decode table, PGO, repeat 1 | c435e02+ | 311.17 | 3.2 | `3d73dac132f3` |  |
| 20260930-145117 | ubuntu 3000M | 16-byte fast decode table, PGO, repeat 2 (reference) | c435e02+ | 198.59 | 15.1 | `9be8284ab1d9` |  |
| 20260930-145134 | ubuntu 3000M | 16-byte fast decode table, PGO, repeat 2 | c435e02+ | 207.88 | 14.4 | `9be8284ab1d9` |  |
| 20260930-145150 | linux 300M | 16-byte fast decode table, PGO, repeat 2 (reference) | c435e02+ | 221.90 | 1.4 | `38c23122994c` |  |
| 20260930-145151 | linux 300M | 16-byte fast decode table, PGO, repeat 2 | c435e02+ | 228.27 | 1.3 | `38c23122994c` |  |
| 20260930-145153 | doom 1000M | 16-byte fast decode table, PGO, repeat 2 (reference) | c435e02+ | 287.52 | 3.5 | `3d73dac132f3` |  |
| 20260930-145156 | doom 1000M | 16-byte fast decode table, PGO, repeat 2 | c435e02+ | 312.32 | 3.2 | `3d73dac132f3` |  |
| 20260930-172205 | doom 1000M | snapshots added, vs 21e9b1c (reference) | 21e9b1c+ | 307.95 | 3.2 | `3d73dac132f3` |  |
| 20260930-172208 | doom 1000M | snapshots added, vs 21e9b1c | 21e9b1c+ | 308.12 | 3.2 | `3d73dac132f3` |  |
| 20260930-172212 | linux 300M | snapshots added, vs 21e9b1c (reference) | 21e9b1c+ | 227.25 | 1.3 | `38c23122994c` |  |
| 20260930-172213 | linux 300M | snapshots added, vs 21e9b1c | 21e9b1c+ | 209.17 | 1.4 | `38c23122994c` |  |
| 20260930-172215 | ubuntu 3000M | snapshots added, vs 21e9b1c (reference) | 21e9b1c+ | 207.42 | 14.5 | `9be8284ab1d9` |  |
| 20260930-172231 | ubuntu 3000M | snapshots added, vs 21e9b1c | 21e9b1c+ | 192.55 | 15.6 | `9be8284ab1d9` |  |
| 20260930-172255 | linux 300M | snapshots added, vs 21e9b1c, repeat 1 (reference) | 21e9b1c+ | 228.06 | 1.3 | `38c23122994c` |  |
| 20260930-172256 | linux 300M | snapshots added, vs 21e9b1c, repeat 1 | 21e9b1c+ | 223.07 | 1.3 | `38c23122994c` |  |
| 20260930-172258 | ubuntu 3000M | snapshots added, vs 21e9b1c, repeat 1 (reference) | 21e9b1c+ | 208.61 | 14.4 | `9be8284ab1d9` |  |
| 20260930-172314 | ubuntu 3000M | snapshots added, vs 21e9b1c, repeat 1 | 21e9b1c+ | 193.67 | 15.5 | `9be8284ab1d9` |  |
| 20260930-172332 | linux 300M | snapshots added, vs 21e9b1c, repeat 2 (reference) | 21e9b1c+ | 230.42 | 1.3 | `38c23122994c` |  |
| 20260930-172333 | linux 300M | snapshots added, vs 21e9b1c, repeat 2 | 21e9b1c+ | 219.22 | 1.4 | `38c23122994c` |  |
| 20260930-172335 | ubuntu 3000M | snapshots added, vs 21e9b1c, repeat 2 (reference) | 21e9b1c+ | 208.53 | 14.4 | `9be8284ab1d9` |  |
| 20260930-172351 | ubuntu 3000M | snapshots added, vs 21e9b1c, repeat 2 | 21e9b1c+ | 194.02 | 15.5 | `9be8284ab1d9` |  |
| 20260930-172522 | doom 1000M | snapshots added, PGO retrained, vs 21e9b1c (reference) | 21e9b1c+ | 312.68 | 3.2 | `3d73dac132f3` |  |
| 20260930-172525 | doom 1000M | snapshots added, PGO retrained, vs 21e9b1c | 21e9b1c+ | 303.42 | 3.3 | `3d73dac132f3` |  |
| 20260930-172528 | linux 300M | snapshots added, PGO retrained, vs 21e9b1c (reference) | 21e9b1c+ | 230.05 | 1.3 | `38c23122994c` |  |
| 20260930-172530 | linux 300M | snapshots added, PGO retrained, vs 21e9b1c | 21e9b1c+ | 228.51 | 1.3 | `38c23122994c` |  |
| 20260930-172531 | ubuntu 3000M | snapshots added, PGO retrained, vs 21e9b1c (reference) | 21e9b1c+ | 209.72 | 14.3 | `9be8284ab1d9` |  |
| 20260930-172547 | ubuntu 3000M | snapshots added, PGO retrained, vs 21e9b1c | 21e9b1c+ | 162.71 | 18.4 | `9be8284ab1d9` |  |
| 20260930-172623 | ubuntu 3000M | snapshots added, PGO retrained, quiet host, run 1 (reference) | 21e9b1c+ | 207.88 | 14.4 | `9be8284ab1d9` |  |
| 20260930-172639 | ubuntu 3000M | snapshots added, PGO retrained, quiet host, run 1 | 21e9b1c+ | 205.44 | 14.6 | `9be8284ab1d9` |  |
| 20260930-172656 | linux 300M | snapshots added, PGO retrained, quiet host, run 1 (reference) | 21e9b1c+ | 230.24 | 1.3 | `38c23122994c` |  |
| 20260930-172657 | linux 300M | snapshots added, PGO retrained, quiet host, run 1 | 21e9b1c+ | 226.00 | 1.3 | `38c23122994c` |  |
| 20260930-172658 | doom 1000M | snapshots added, PGO retrained, quiet host, run 1 (reference) | 21e9b1c+ | 310.54 | 3.2 | `3d73dac132f3` |  |
| 20260930-172702 | doom 1000M | snapshots added, PGO retrained, quiet host, run 1 | 21e9b1c+ | 312.48 | 3.2 | `3d73dac132f3` |  |
| 20260930-172705 | ubuntu 3000M | snapshots added, PGO retrained, quiet host, run 2 (reference) | 21e9b1c+ | 207.26 | 14.5 | `9be8284ab1d9` |  |
| 20260930-172721 | ubuntu 3000M | snapshots added, PGO retrained, quiet host, run 2 | 21e9b1c+ | 208.28 | 14.4 | `9be8284ab1d9` |  |
| 20260930-172737 | linux 300M | snapshots added, PGO retrained, quiet host, run 2 (reference) | 21e9b1c+ | 232.35 | 1.3 | `38c23122994c` |  |
| 20260930-172738 | linux 300M | snapshots added, PGO retrained, quiet host, run 2 | 21e9b1c+ | 228.81 | 1.3 | `38c23122994c` |  |
| 20260930-172740 | doom 1000M | snapshots added, PGO retrained, quiet host, run 2 (reference) | 21e9b1c+ | 313.21 | 3.2 | `3d73dac132f3` |  |
| 20260930-172743 | doom 1000M | snapshots added, PGO retrained, quiet host, run 2 | 21e9b1c+ | 313.44 | 3.2 | `3d73dac132f3` |  |
| 20260930-182949 | desktop 3000M | desktop workload, run 1 | c8e18cc+ | 247.04 | 12.1 | `35c61483d5e3` |  |
| 20260930-183017 | desktop 3000M | desktop workload, run 2 | c8e18cc+ | 254.35 | 11.8 | `35c61483d5e3` |  |
| 20260930-190916 | session 3000M | session workload, run 1 | 1938684+ | 62.49 | 48.0 | `d749ffd00dac` |  |
| 20260930-191015 | session 3000M | session workload, run 2 | 1938684+ | 62.33 | 48.1 | `d749ffd00dac` |  |
| 20260930-191548 | session 3000M | EventGen only on a change, vs 1938684+progress (reference) | 1938684+ | 62.53 | 48.0 | `d749ffd00dac` |  |
| 20260930-191646 | session 3000M | EventGen only on a change, vs 1938684+progress | 1938684+ | 58.64 | 51.2 | `d749ffd00dac` |  |
| 20260930-191748 | doom 1000M | EventGen only on a change, vs 1938684+progress (reference) | 1938684+ | 312.15 | 3.2 | `3d73dac132f3` |  |
| 20260930-191751 | doom 1000M | EventGen only on a change, vs 1938684+progress | 1938684+ | 231.09 | 4.3 | `3d73dac132f3` |  |
| 20260930-191756 | linux 300M | EventGen only on a change, vs 1938684+progress (reference) | 1938684+ | 225.81 | 1.3 | `38c23122994c` |  |
| 20260930-191757 | linux 300M | EventGen only on a change, vs 1938684+progress | 1938684+ | 183.24 | 1.6 | `38c23122994c` |  |
| 20260930-191759 | ubuntu 3000M | EventGen only on a change, vs 1938684+progress (reference) | 1938684+ | 198.62 | 15.1 | `9be8284ab1d9` |  |
| 20260930-191816 | ubuntu 3000M | EventGen only on a change, vs 1938684+progress | 1938684+ | 168.29 | 17.8 | `9be8284ab1d9` |  |
| 20260930-191835 | desktop 3000M | EventGen only on a change, vs 1938684+progress (reference) | 1938684+ | 254.97 | 11.8 | `35c61483d5e3` |  |
| 20260930-191858 | desktop 3000M | EventGen only on a change, vs 1938684+progress | 1938684+ | 206.61 | 14.5 | `35c61483d5e3` |  |
| 20260930-191934 | doom 1000M | EventGen only on a change, repeat 1 (reference) | 1938684+ | 314.82 | 3.2 | `3d73dac132f3` |  |
| 20260930-191938 | doom 1000M | EventGen only on a change, repeat 1 | 1938684+ | 234.28 | 4.3 | `3d73dac132f3` |  |
| 20260930-191942 | linux 300M | EventGen only on a change, repeat 1 (reference) | 1938684+ | 227.66 | 1.3 | `38c23122994c` |  |
| 20260930-191943 | linux 300M | EventGen only on a change, repeat 1 | 1938684+ | 186.35 | 1.6 | `38c23122994c` |  |
| 20260930-191945 | doom 1000M | EventGen only on a change, repeat 2 (reference) | 1938684+ | 315.04 | 3.2 | `3d73dac132f3` |  |
| 20260930-191948 | doom 1000M | EventGen only on a change, repeat 2 | 1938684+ | 234.49 | 4.3 | `3d73dac132f3` |  |
| 20260930-191952 | linux 300M | EventGen only on a change, repeat 2 (reference) | 1938684+ | 227.96 | 1.3 | `38c23122994c` |  |
| 20260930-191954 | linux 300M | EventGen only on a change, repeat 2 | 1938684+ | 187.00 | 1.6 | `38c23122994c` |  |
| 20260930-192111 | session 3000M | EventGen only on a change, PGO retrained, vs 1938684+progress (reference) | 1938684+ | 62.06 | 48.3 | `d749ffd00dac` |  |
| 20260930-192238 | session 3000M | EventGen only on a change, PGO retrained, vs 1938684+progress | 1938684+ | 64.14 | 46.8 | `d749ffd00dac` |  |
| 20260930-192336 | doom 1000M | EventGen only on a change, PGO retrained, vs 1938684+progress (reference) | 1938684+ | 313.36 | 3.2 | `3d73dac132f3` |  |
| 20260930-192339 | doom 1000M | EventGen only on a change, PGO retrained, vs 1938684+progress | 1938684+ | 303.06 | 3.3 | `3d73dac132f3` |  |
| 20260930-192342 | linux 300M | EventGen only on a change, PGO retrained, vs 1938684+progress (reference) | 1938684+ | 224.42 | 1.3 | `38c23122994c` |  |
| 20260930-192344 | linux 300M | EventGen only on a change, PGO retrained, vs 1938684+progress | 1938684+ | 230.78 | 1.3 | `38c23122994c` |  |
| 20260930-192345 | ubuntu 3000M | EventGen only on a change, PGO retrained, vs 1938684+progress (reference) | 1938684+ | 195.24 | 15.4 | `9be8284ab1d9` |  |
| 20260930-192404 | ubuntu 3000M | EventGen only on a change, PGO retrained, vs 1938684+progress | 1938684+ | 207.32 | 14.5 | `9be8284ab1d9` |  |
| 20260930-192423 | desktop 3000M | EventGen only on a change, PGO retrained, vs 1938684+progress (reference) | 1938684+ | 256.43 | 11.7 | `35c61483d5e3` |  |
| 20260930-192449 | desktop 3000M | EventGen only on a change, PGO retrained, vs 1938684+progress | 1938684+ | 258.88 | 11.6 | `35c61483d5e3` |  |
| 20260930-192835 | session 3000M | F/D in the fast loop, stale profile, vs 1938684+progress (reference) | 1938684+ | 61.79 | 48.5 | `d749ffd00dac` |  |
| 20260930-192935 | session 3000M | F/D in the fast loop, stale profile, vs 1938684+progress | 1938684+ | 69.08 | 43.4 | `d749ffd00dac` |  |
| 20260930-193029 | desktop 3000M | F/D in the fast loop, stale profile, vs 1938684+progress (reference) | 1938684+ | 246.75 | 12.2 | `35c61483d5e3` |  |
| 20260930-193052 | desktop 3000M | F/D in the fast loop, stale profile, vs 1938684+progress | 1938684+ | 233.92 | 12.8 | `35c61483d5e3` |  |
| 20260930-193116 | ubuntu 3000M | F/D in the fast loop, stale profile, vs 1938684+progress (reference) | 1938684+ | 195.21 | 15.4 | `9be8284ab1d9` |  |
| 20260930-193136 | ubuntu 3000M | F/D in the fast loop, stale profile, vs 1938684+progress | 1938684+ | 185.70 | 16.2 | `9be8284ab1d9` |  |
| 20260930-193201 | linux 300M | F/D in the fast loop, stale profile, vs 1938684+progress (reference) | 1938684+ | 228.85 | 1.3 | `38c23122994c` |  |
| 20260930-193202 | linux 300M | F/D in the fast loop, stale profile, vs 1938684+progress | 1938684+ | 206.78 | 1.5 | `38c23122994c` |  |
| 20260930-193204 | doom 1000M | F/D in the fast loop, stale profile, vs 1938684+progress (reference) | 1938684+ | 314.66 | 3.2 | `3d73dac132f3` |  |
| 20260930-193207 | doom 1000M | F/D in the fast loop, stale profile, vs 1938684+progress | 1938684+ | 270.98 | 3.7 | `3d73dac132f3` |  |
| 20260930-193321 | session 3000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress (reference) | 1938684+ | 60.22 | 49.8 | `d749ffd00dac` |  |
| 20260930-193422 | session 3000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress | 1938684+ | 70.80 | 42.4 | `d749ffd00dac` |  |
| 20260930-193515 | desktop 3000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress (reference) | 1938684+ | 248.98 | 12.0 | `35c61483d5e3` |  |
| 20260930-193541 | desktop 3000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress | 1938684+ | 258.26 | 11.6 | `35c61483d5e3` |  |
| 20260930-193603 | ubuntu 3000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress (reference) | 1938684+ | 195.06 | 15.4 | `9be8284ab1d9` |  |
| 20260930-193630 | ubuntu 3000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress | 1938684+ | 207.08 | 14.5 | `9be8284ab1d9` |  |
| 20260930-193646 | linux 300M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress (reference) | 1938684+ | 227.28 | 1.3 | `38c23122994c` |  |
| 20260930-193647 | linux 300M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress | 1938684+ | 230.36 | 1.3 | `38c23122994c` |  |
| 20260930-193648 | doom 1000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress (reference) | 1938684+ | 314.79 | 3.2 | `3d73dac132f3` |  |
| 20260930-193652 | doom 1000M | EventGen on change + F/D in fast loop, PGO, vs 1938684+progress | 1938684+ | 310.89 | 3.2 | `3d73dac132f3` |  |
| 20260930-194337 | session 3000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress (reference) | 1938684+ | 62.26 | 48.2 | `d749ffd00dac` |  |
| 20260930-194436 | session 3000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress | 1938684+ | 75.94 | 39.5 | `d749ffd00dac` |  |
| 20260930-194526 | desktop 3000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress (reference) | 1938684+ | 254.56 | 11.8 | `35c61483d5e3` |  |
| 20260930-194549 | desktop 3000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress | 1938684+ | 264.69 | 11.3 | `35c61483d5e3` |  |
| 20260930-194611 | ubuntu 3000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress (reference) | 1938684+ | 195.99 | 15.3 | `9be8284ab1d9` |  |
| 20260930-194632 | ubuntu 3000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress | 1938684+ | 204.42 | 14.7 | `9be8284ab1d9` |  |
| 20260930-194651 | linux 300M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress (reference) | 1938684+ | 222.31 | 1.3 | `38c23122994c` |  |
| 20260930-194653 | linux 300M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress | 1938684+ | 224.09 | 1.3 | `38c23122994c` |  |
| 20260930-194654 | doom 1000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress (reference) | 1938684+ | 307.95 | 3.2 | `3d73dac132f3` |  |
| 20260930-194658 | doom 1000M | EventGen on change + F/D fast loop + FP loads via load_virtual, PGO, vs 1938684+progress | 1938684+ | 310.67 | 3.2 | `3d73dac132f3` |  |
| 20260930-201057 | session 3000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress (reference) | 4bf624e+ | 62.23 | 48.2 | `d749ffd00dac` |  |
| 20260930-201159 | session 3000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress | 4bf624e+ | 74.86 | 40.1 | `d749ffd00dac` |  |
| 20260930-201250 | desktop 3000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress (reference) | 4bf624e+ | 254.23 | 11.8 | `35c61483d5e3` |  |
| 20260930-201315 | desktop 3000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress | 4bf624e+ | 257.64 | 11.6 | `35c61483d5e3` |  |
| 20260930-201338 | ubuntu 3000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress (reference) | 4bf624e+ | 195.27 | 15.4 | `9be8284ab1d9` |  |
| 20260930-201359 | ubuntu 3000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress | 4bf624e+ | 198.79 | 15.1 | `9be8284ab1d9` |  |
| 20260930-201419 | linux 300M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress (reference) | 4bf624e+ | 225.84 | 1.3 | `38c23122994c` |  |
| 20260930-201420 | linux 300M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress | 4bf624e+ | 225.72 | 1.3 | `38c23122994c` |  |
| 20260930-201421 | doom 1000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress (reference) | 4bf624e+ | 309.37 | 3.2 | `3d73dac132f3` |  |
| 20260930-201425 | doom 1000M | Sail's misaligned split + reserved rm illegal, PGO, vs 1938684+progress | 4bf624e+ | 306.19 | 3.3 | `3d73dac132f3` |  |
| 20261001-235231 | doom 1000M | multi-hart, 1 hart (reference) | c4f0129+ | 309.03 | 3.2 | `3d73dac132f3` |  |
| 20261001-235234 | doom 1000M | multi-hart, 1 hart | c4f0129+ | 306.90 | 3.3 | `3d73dac132f3` |  |
| 20261001-235237 | linux 300M | multi-hart, 1 hart (reference) | c4f0129+ | 226.94 | 1.3 | `38c23122994c` |  |
| 20261001-235239 | linux 300M | multi-hart, 1 hart | c4f0129+ | 215.84 | 1.4 | `38c23122994c` |  |
| 20261001-235240 | session 3000M | multi-hart, 1 hart (reference) | c4f0129+ | 75.34 | 39.8 | `d749ffd00dac` |  |
| 20261001-235334 | session 3000M | multi-hart, 1 hart | c4f0129+ | 75.25 | 39.9 | `d749ffd00dac` |  |
| 20261001-235424 | desktop 3000M | multi-hart, 1 hart (reference) | c4f0129+ | 258.42 | 11.6 | `35c61483d5e3` |  |
| 20261001-235458 | desktop 3000M | multi-hart, 1 hart | c4f0129+ | 248.57 | 12.1 | `35c61483d5e3` |  |
| 20261001-235702 | linux 300M | multi-hart r2, 1 hart (reference) | c4f0129+ | 227.23 | 1.3 | `38c23122994c` |  |
| 20261001-235703 | linux 300M | multi-hart r2, 1 hart | c4f0129+ | 217.67 | 1.4 | `38c23122994c` |  |
| 20261001-235705 | desktop 3000M | multi-hart r2, 1 hart (reference) | c4f0129+ | 257.55 | 11.6 | `35c61483d5e3` |  |
| 20261001-235728 | desktop 3000M | multi-hart r2, 1 hart | c4f0129+ | 252.69 | 11.9 | `35c61483d5e3` |  |
| 20261001-235751 | linux 300M | multi-hart r2, 1 hart (reference) | c4f0129+ | 227.11 | 1.3 | `38c23122994c` |  |
| 20261001-235752 | linux 300M | multi-hart r2, 1 hart | c4f0129+ | 228.99 | 1.3 | `38c23122994c` |  |
| 20261001-235753 | desktop 3000M | multi-hart r2, 1 hart (reference) | c4f0129+ | 250.44 | 12.0 | `35c61483d5e3` |  |
| 20261001-235820 | desktop 3000M | multi-hart r2, 1 hart | c4f0129+ | 253.40 | 11.8 | `35c61483d5e3` |  |
| 20261002-102227 | ubuntu 3000M | boot layout at 128MB | 63ad8cf+ | 167.44 | 17.9 | `317e8cd24ffc` |  |
| 20261003-114918 | doom 1000M | virtio split + net (reference) | 5529126+ | 298.58 | 3.3 | `3d73dac132f3` |  |
| 20261003-114922 | doom 1000M | virtio split + net | 5529126+ | 293.56 | 3.4 | `3d73dac132f3` |  |
| 20261003-114925 | desktop 3000M | virtio split + net (reference) | 5529126+ | 248.72 | 12.1 | `35c61483d5e3` |  |
| 20261003-114951 | desktop 3000M | virtio split + net | 5529126+ | 242.09 | 12.4 | `35c61483d5e3` |  |
| 20261003-115015 | doom 1000M | virtio split + net (reference) | 5529126+ | 299.06 | 3.3 | `3d73dac132f3` |  |
| 20261003-115019 | doom 1000M | virtio split + net | 5529126+ | 302.26 | 3.3 | `3d73dac132f3` |  |
| 20261003-115022 | desktop 3000M | virtio split + net (reference) | 5529126+ | 248.45 | 12.1 | `35c61483d5e3` |  |
| 20261003-115046 | desktop 3000M | virtio split + net | 5529126+ | 249.50 | 12.0 | `35c61483d5e3` |  |
| 20261003-141321 | doom 1000M | rtc (reference) | 2be8a7a+ | 287.34 | 3.5 | `3d73dac132f3` |  |
| 20261003-141324 | doom 1000M | rtc | 2be8a7a+ | 298.35 | 3.4 | `3d73dac132f3` |  |
| 20261003-141328 | desktop 3000M | rtc (reference) | 2be8a7a+ | 254.19 | 11.8 | `35c61483d5e3` |  |
| 20261003-141352 | desktop 3000M | rtc | 2be8a7a+ | 248.71 | 12.1 | `35c61483d5e3` |  |
| 20261003-141421 | desktop 3000M | rtc (reference) | 2be8a7a+ | 254.63 | 11.8 | `35c61483d5e3` |  |
| 20261003-141444 | desktop 3000M | rtc | 2be8a7a+ | 248.35 | 12.1 | `35c61483d5e3` |  |
| 20261003-141508 | linux 300M | rtc-new | 2be8a7a+ | 221.37 | 1.4 | `23886d5785ac` |  |
| 20261003-141646 | desktop 3000M | rtc (reference) | 2be8a7a+ | 255.11 | 11.8 | `35c61483d5e3` |  |
| 20261003-141709 | desktop 3000M | rtc | 2be8a7a+ | 255.33 | 11.7 | `35c61483d5e3` |  |
| 20261003-141732 | desktop 3000M | rtc (reference) | 2be8a7a+ | 256.46 | 11.7 | `35c61483d5e3` |  |
| 20261003-141755 | desktop 3000M | rtc | 2be8a7a+ | 252.30 | 11.9 | `35c61483d5e3` |  |
