# Cross-compiles doomgeneric for DoomV's RV32IMA target.
# Needs riscv-none-elf-gcc on PATH (xPack GNU RISC-V Embedded GCC).
#
# Usage: make doom1   (shareware, default)
#        make doom2
#        make final   (TNT.WAD)
#        make all     (all three)

CC = riscv-none-elf-gcc

# -nostartfiles (not -nostdlib) + -specs=nosys.specs: drop the toolchain's
# own crt0 (we have our own _start in start.S) but keep newlib's real
# libc.a/libnosys.a -- gets memcpy/strlen/printf/malloc/etc "for free"
# instead of hand-writing ~50 functions, at the cost of only needing to
# stub the small syscall surface nosys.specs doesn't already cover
# (currently just mkdir, see libc_shim.c). _sbrk works via the `end`
# symbol PROVIDE'd in riscv.lds.
MARCH   = -march=rv32ima -mabi=ilp32
CFLAGS  = $(MARCH) -nostartfiles -specs=nosys.specs -O2 -I. -I$(DOOMGENERIC_DIR)
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

# WAD target: WAD=doom1 (default) | doom2 | final
WAD ?= doom1

ifeq ($(WAD),doom1)
	WAD_LENGTH = 4196020
else ifeq ($(WAD),doom2)
	WAD_LENGTH = 14604584
else ifeq ($(WAD),final)
	WAD_LENGTH = 18195736
else
	$(error Unknown WAD target '$(WAD)', expected doom1, doom2, or final)
endif

OUT = doomv-$(WAD).elf

all: doom1 doom2 final

doom1 doom2 final:
	$(MAKE) $(OUT) WAD=$@

$(OUT): $(ASM_SRCS) $(PLATFORM_SRCS) $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -DWAD_LENGTH=$(WAD_LENGTH) $(LDFLAGS) -o $@ $^

clean:
	rm -f doomv-doom1.elf doomv-doom2.elf doomv-final.elf

.PHONY: all doom1 doom2 final clean
