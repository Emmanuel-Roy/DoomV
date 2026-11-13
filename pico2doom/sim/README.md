# The PC simulator

The Pico port's code with a PC underneath, for answering "does it fit, and
does it still work?" without hardware. Built 32-bit with `zig cc`
(`pip install ziglang`), so DOOM's structures are the size they are on the
RP2350 and the memory it reports is the Pico's.

```sh
cmake -S sim -B build/sim -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/sim/zig-x86.cmake
cmake --build build/sim
cd build/sim
./doom_sim -wad doom1.wad -zone 271 -fastclock -explore -frames 4000 -- -warp 1 5 -skill 4
```

| Option | |
|---|---|
| `-zone KB` | DOOM's heap; 271 is what a Pico 2 has |
| `-fastclock` | every clock read advances 1 ms: fast and repeatable |
| `-explore` | invulnerable, visits every sector and spins: worst-case load |
| `-look` | turn in place at the start |
| `-frames N` | stop after N frames and print the memory peaks |
| `-dump N,N` | write those frames as PPM (`tools/ppm2png.py` converts) |
| `-zonereport` | at exit, list the heap's live blocks by allocating function |
| `-hashes FILE` | log every frame's picture and game-state hash |
| `-record FILE` | write a lock-step recording DoomV can replay |
| `-- ...` | DOOM's own arguments (`-warp 1 5`, `-timedemo demo1`) |

The WAD is mapped read-only, like flash, so an engine write into a lump
crashes here rather than misbehaving on the Pico. A crash or a hang prints
the call stack with function names and lines.

Built with `-DPORT_APPLY_PATCHES=OFF` it runs the stock engine, which is how
the patches were checked: both builds play the shareware demos with
`-hashes` and the logs are compared ([patches/README.md](../patches/README.md)).
