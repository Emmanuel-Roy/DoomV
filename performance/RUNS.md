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
