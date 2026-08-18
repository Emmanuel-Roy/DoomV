# Cross-compiles doomgeneric for DoomV's RV32IMA target.
# Needs riscv-none-elf-gcc on PATH (xPack GNU RISC-V Embedded GCC).
#
# Usage: make free       (shareware, DOOM1.WAD -- default)
#        make doom       (Ultimate Doom, DOOM.WAD)
#        make doom2      (DOOM2.WAD)
#        make finaldoom  (Final Doom, TNT.WAD)
#        make all        (all four)

CC = riscv-none-elf-gcc

# -nostartfiles (not -nostdlib) + -specs=nosys.specs: drop the toolchain's
# own crt0 (we have our own _start in start.S) but keep newlib's real
# libc.a/libnosys.a -- gets memcpy/strlen/printf/malloc/etc "for free"
# instead of hand-writing ~50 functions, at the cost of only needing to
# stub the small syscall surface nosys.specs doesn't already cover
# (currently just mkdir, see libc_shim.c). _sbrk works via the `end`
# symbol PROVIDE'd in riscv.lds.
# DOOMGENERIC_RESX/RESY as global -D flags, not a header #define -- every
# translation unit must agree (doomgeneric.c allocates DG_ScreenBuffer at
# this size, i_video.c writes into it assuming this stride). See
# doomv_mmio.h's comment for what happens if they disagree.
MARCH   = -march=rv32ima -mabi=ilp32
RESFLAGS = -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200
CFLAGS  = $(MARCH) -nostartfiles -specs=nosys.specs -O2 $(RESFLAGS) -I. -I$(DOOMGENERIC_DIR)
LDFLAGS = -T riscv.lds -nostartfiles -specs=nosys.specs

DOOMGENERIC_DIR = doomgeneric/doomgeneric

# doomgeneric's own engine sources, minus the platform files we replace
# (w_file.c/w_file_stdc.c -> w_file_doomv.c, doomgeneric_xlib.c -> doomgeneric_doomv.c).
# i_video.c/i_input.c stay -- despite the file headers, they're the
# generic I_*-to-DG_* bridge (I_FinishUpdate calls DG_DrawFrame,
# I_GetEvent calls DG_GetKey), not X11-specific.
ENGINE_SRCS = \
	$(DOOMGENERIC_DIR)/dummy.c \
	$(DOOMGENERIC_DIR)/am_map.c \
	$(DOOMGENERIC_DIR)/doomdef.c \
	$(DOOMGENERIC_DIR)/doomstat.c \
	$(DOOMGENERIC_DIR)/dstrings.c \
	$(DOOMGENERIC_DIR)/d_event.c \
	$(DOOMGENERIC_DIR)/d_items.c \
	$(DOOMGENERIC_DIR)/d_iwad.c \
	$(DOOMGENERIC_DIR)/d_loop.c \
	$(DOOMGENERIC_DIR)/d_main.c \
	$(DOOMGENERIC_DIR)/d_mode.c \
	$(DOOMGENERIC_DIR)/d_net.c \
	$(DOOMGENERIC_DIR)/f_finale.c \
	$(DOOMGENERIC_DIR)/f_wipe.c \
	$(DOOMGENERIC_DIR)/g_game.c \
	$(DOOMGENERIC_DIR)/hu_lib.c \
	$(DOOMGENERIC_DIR)/hu_stuff.c \
	$(DOOMGENERIC_DIR)/info.c \
	$(DOOMGENERIC_DIR)/i_cdmus.c \
	$(DOOMGENERIC_DIR)/i_endoom.c \
	$(DOOMGENERIC_DIR)/i_joystick.c \
	$(DOOMGENERIC_DIR)/i_scale.c \
	$(DOOMGENERIC_DIR)/i_sound.c \
	$(DOOMGENERIC_DIR)/i_system.c \
	$(DOOMGENERIC_DIR)/i_timer.c \
	$(DOOMGENERIC_DIR)/memio.c \
	$(DOOMGENERIC_DIR)/m_argv.c \
	$(DOOMGENERIC_DIR)/m_bbox.c \
	$(DOOMGENERIC_DIR)/m_cheat.c \
	$(DOOMGENERIC_DIR)/m_config.c \
	$(DOOMGENERIC_DIR)/m_controls.c \
	$(DOOMGENERIC_DIR)/m_fixed.c \
	$(DOOMGENERIC_DIR)/m_menu.c \
	$(DOOMGENERIC_DIR)/m_misc.c \
	$(DOOMGENERIC_DIR)/m_random.c \
	$(DOOMGENERIC_DIR)/p_ceilng.c \
	$(DOOMGENERIC_DIR)/p_doors.c \
	$(DOOMGENERIC_DIR)/p_enemy.c \
	$(DOOMGENERIC_DIR)/p_floor.c \
	$(DOOMGENERIC_DIR)/p_inter.c \
	$(DOOMGENERIC_DIR)/p_lights.c \
	$(DOOMGENERIC_DIR)/p_map.c \
	$(DOOMGENERIC_DIR)/p_maputl.c \
	$(DOOMGENERIC_DIR)/p_mobj.c \
	$(DOOMGENERIC_DIR)/p_plats.c \
	$(DOOMGENERIC_DIR)/p_pspr.c \
	$(DOOMGENERIC_DIR)/p_saveg.c \
	$(DOOMGENERIC_DIR)/p_setup.c \
	$(DOOMGENERIC_DIR)/p_sight.c \
	$(DOOMGENERIC_DIR)/p_spec.c \
	$(DOOMGENERIC_DIR)/p_switch.c \
	$(DOOMGENERIC_DIR)/p_telept.c \
	$(DOOMGENERIC_DIR)/p_tick.c \
	$(DOOMGENERIC_DIR)/p_user.c \
	$(DOOMGENERIC_DIR)/r_bsp.c \
	$(DOOMGENERIC_DIR)/r_data.c \
	$(DOOMGENERIC_DIR)/r_draw.c \
	$(DOOMGENERIC_DIR)/r_main.c \
	$(DOOMGENERIC_DIR)/r_plane.c \
	$(DOOMGENERIC_DIR)/r_segs.c \
	$(DOOMGENERIC_DIR)/r_sky.c \
	$(DOOMGENERIC_DIR)/r_things.c \
	$(DOOMGENERIC_DIR)/sha1.c \
	$(DOOMGENERIC_DIR)/sounds.c \
	$(DOOMGENERIC_DIR)/statdump.c \
	$(DOOMGENERIC_DIR)/st_lib.c \
	$(DOOMGENERIC_DIR)/st_stuff.c \
	$(DOOMGENERIC_DIR)/s_sound.c \
	$(DOOMGENERIC_DIR)/tables.c \
	$(DOOMGENERIC_DIR)/v_video.c \
	$(DOOMGENERIC_DIR)/wi_stuff.c \
	$(DOOMGENERIC_DIR)/w_checksum.c \
	$(DOOMGENERIC_DIR)/w_main.c \
	$(DOOMGENERIC_DIR)/w_wad.c \
	$(DOOMGENERIC_DIR)/z_zone.c \
	$(DOOMGENERIC_DIR)/i_input.c \
	$(DOOMGENERIC_DIR)/i_video.c \
	$(DOOMGENERIC_DIR)/doomgeneric.c

PLATFORM_SRCS = doomgeneric_doomv.c w_file_doomv.c libc_shim.c
ASM_SRCS      = start.S

# WAD target: which real IWAD this build's WAD_LENGTH matches. The guest
# binary doesn't embed the WAD itself -- the host loader places the real
# file's bytes at WAD_BASE at runtime -- so WAD_FILE here is just the
# reference for which one to actually hand the loader for a given build.
WAD ?= free

ifeq ($(WAD),free)
	WAD_FILE   = DOOM1.WAD
	WAD_LENGTH = 4196020
else ifeq ($(WAD),doom)
	WAD_FILE   = DOOM.WAD
	WAD_LENGTH = 12408292
else ifeq ($(WAD),doom2)
	WAD_FILE   = DOOM2.WAD
	WAD_LENGTH = 14604584
else ifeq ($(WAD),finaldoom)
	WAD_FILE   = TNT.WAD
	WAD_LENGTH = 18195736
else
	$(error Unknown WAD target '$(WAD)', expected free, doom, doom2, or finaldoom)
endif

OUT = doomv-$(WAD).elf

all: free doom doom2 finaldoom

free doom doom2 finaldoom:
	$(MAKE) $(OUT) WAD=$@

$(OUT): $(ASM_SRCS) $(PLATFORM_SRCS) $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -DWAD_LENGTH=$(WAD_LENGTH) $(LDFLAGS) -o $@ $^

clean:
	rm -f doomv-free.elf doomv-doom.elf doomv-doom2.elf doomv-finaldoom.elf

.PHONY: all free doom doom2 finaldoom clean
