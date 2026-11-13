// The packed IWAD (tools/wadpack.py), linked into flash as read-only data.
// DOOM reads lumps straight from here through the XIP cache: no copies.
	.section .rodata.doom_wad, "a"
	.balign 4
	.global doom_wad
doom_wad:
	.incbin WAD_PATH
	.global doom_wad_end
doom_wad_end:
