# Engine patches

doomgeneric is a pristine submodule. These patches are applied, in name
order, to a copy of it in the build folder when CMake configures
(`port/port.cmake`). Each is small and does one thing; almost all of them
exist to fit DOOM into the Pico 2's 520 KB of SRAM.

| Patch | What it does | RAM saved |
|---|---|---:|
| `01-renderer-limits` | Visplanes 128 → 56, openings 64 → 20 screen-widths, drawsegs 256 → 160 -- sized from the most any shareware map used (45, 3768, 102), with overflows now merging or skipping instead of crashing or corrupting memory. | 80 KB |
| `02-states-in-flash` | `states[]` is `const` (flash). The demons' tics that `-fast`/Nightmare halve live in a 13-entry RAM table. | 26 KB |
| `03-netgame-buffers` | Tic buffers for 4 players × 32 tics instead of 8 × 128; one `-statdump` slot. | 24 KB |
| `04-blockmap-and-tables` | The blockmap is used straight from the WAD (little-endian CPUs need no byte-swapped copy); `viewangletox` is `short`. | up to 19 KB per map + 8 KB |
| `05-wad-directory-in-flash` | `lumpinfo` points at the WAD's own directory in flash instead of a 28-byte-per-lump copy in RAM. | 35 KB |
| `06-texture-lookups-on-demand` | Texture column lookups are built when a texture is first drawn, and can be evicted; multi-patch columns are assembled one at a time instead of whole 32 KB composite textures. | 64 KB, and no large blocks to fragment the heap |
| `07-mobjinfo-in-flash` | `mobjinfo[]` is `const`; the three projectiles whose speed `-fast` changes have RAM copies. `r_draw.c`'s tables sized for 320×200, not 1120×832. | 19 KB |
| `08-zone-try-malloc` | `Z_TryMalloc`: an allocation that may fail, for the screen melt's optional 64 KB buffer. | (64 KB when short) |
| `09-sprite-sizes-from-flash` | Sprite width/offsets read from each sprite's patch header instead of three tables. | 6 KB |

## Proof that they change nothing

Two builds of the PC simulator -- one with the patches, one without
(`-DPORT_APPLY_PATCHES=OFF`) -- play the three shareware demos and log a
hash of every frame and of the game state:

| Demo | Frames | Game state differs | Picture differs |
|---|---:|---:|---:|
| DEMO1 | 5,065 | 0 | 1 frame, 1 pixel |
| DEMO2 | 3,875 | 0 | 0 |
| DEMO3 | 2,173 | 0 | 0 |

The one pixel is a column where stock DOOM reads past the end of a texture
into unrelated heap memory; no port could reproduce what happened to be
there.

## Making a patch

Edit a copy of the files, then `diff -u --label a/FILE --label b/FILE`
against the state after the previous patches. Mark every change with a
`Pico2Doom:` comment saying why.
