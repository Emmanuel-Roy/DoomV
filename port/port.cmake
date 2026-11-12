# The engine and the shared port layer, as one list every target uses
# (firmware/, sim/, doomv/). Keeping it in one place is what makes the three
# builds run the same code -- which lock-step depends on.

set(PORT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(DOOMGENERIC_PRISTINE ${PORT_DIR}/../external/doomgeneric/doomgeneric)

if(NOT EXISTS ${DOOMGENERIC_PRISTINE}/d_main.c)
  message(FATAL_ERROR "doomgeneric is missing: run  git submodule update --init")
endif()

# The engine is compiled from a patched copy in the build tree; the submodule
# itself is never modified. patches/*.patch (see patches/README.md) are
# applied in name order, each one a small, separately explained change --
# mostly to make DOOM fit in the Pico 2's 520 KB.
find_package(Git REQUIRED)
set(DOOMGENERIC_DIR ${CMAKE_BINARY_DIR}/doomgeneric-patched)
option(PORT_APPLY_PATCHES "Apply patches/*.patch (OFF: stock engine, for A/B checks)" ON)
file(GLOB PORT_PATCHES ${PORT_DIR}/../patches/*.patch)
list(SORT PORT_PATCHES)
if(NOT PORT_APPLY_PATCHES)
  set(PORT_PATCHES)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PORT_PATCHES})
file(REMOVE_RECURSE ${DOOMGENERIC_DIR})
file(GLOB _dg_files ${DOOMGENERIC_PRISTINE}/*.c ${DOOMGENERIC_PRISTINE}/*.h)
file(COPY ${_dg_files} DESTINATION ${DOOMGENERIC_DIR})
foreach(p ${PORT_PATCHES})
  get_filename_component(pname ${p} NAME)
  execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn -p1 ${p}
    WORKING_DIRECTORY ${DOOMGENERIC_DIR} RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "patch ${pname} does not apply:\n${err}")
  endif()
  message(STATUS "doomgeneric: applied ${pname}")
endforeach()

# doomgeneric's engine, minus the platform files this port replaces:
#   doomgeneric.c  -> port_main.c   (no 256 KB true-colour screen copy)
#   i_video.c      -> port_video.c  (8-bit frame straight to the backend)
#   f_wipe.c       -> port_wipe.c   (the melt in 64 KB instead of 192 KB)
#   w_file*.c      -> port_wad.c    (the WAD is memory-mapped, never copied)
#   s_sound.c, sounds.c, i_sound.c -> port_sound.c  (no audio hardware)
set(DOOM_ENGINE_FILES
  am_map d_event d_items d_iwad d_loop d_main d_mode d_net doomdef doomstat
  dstrings dummy f_finale g_game hu_lib hu_stuff i_cdmus i_endoom i_input
  i_joystick i_system i_timer info m_argv m_bbox m_cheat m_config
  m_controls m_fixed m_menu m_misc m_random memio p_ceilng p_doors p_enemy
  p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg
  p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw
  r_main r_plane r_segs r_sky r_things sha1 statdump st_lib st_stuff
  tables v_video wi_stuff w_checksum w_main w_wad z_zone)

set(DOOM_ENGINE_SOURCES)
foreach(f ${DOOM_ENGINE_FILES})
  list(APPEND DOOM_ENGINE_SOURCES ${DOOMGENERIC_DIR}/${f}.c)
endforeach()

set(PORT_SOURCES
  ${PORT_DIR}/port_main.c
  ${PORT_DIR}/port_video.c
  ${PORT_DIR}/port_wipe.c
  ${PORT_DIR}/port_wad.c
  ${PORT_DIR}/port_link.c
  ${PORT_DIR}/port_perf.c
  ${PORT_DIR}/port_system.c
  ${PORT_DIR}/port_sound.c)

# Engine functions the port replaces or times (port_system.c, port_perf.c),
# as SYMBOL:file-that-defines-it. This does what the linker's --wrap does,
# but with the preprocessor, so it works with any compiler and linker
# (zig's has no --wrap) and survives LTO:
#   * the defining file is compiled with  -DSYMBOL=__real_SYMBOL
#   * every other engine file with        -DSYMBOL=__wrap_SYMBOL
# The port supplies __wrap_SYMBOL and may call __real_SYMBOL.
set(PORT_WRAPS
  I_ZoneBase:i_system I_Error:i_system I_Quit:i_system M_FileExists:m_misc
  G_Ticker:g_game R_RenderBSPNode:r_bsp R_DrawPlanes:r_plane
  R_DrawMasked:r_things ST_Drawer:st_stuff G_DeferedPlayDemo:g_game)

# Adds the engine + port layer to `target`. A target may set
# PORT_EXTRA_WRAPS (same SYMBOL:file form) first, e.g. the sim's Z_Malloc.
function(port_add_doom target)
  target_sources(${target} PRIVATE ${DOOM_ENGINE_SOURCES} ${PORT_SOURCES})
  target_include_directories(${target} PRIVATE ${DOOMGENERIC_DIR} ${PORT_DIR})
  target_compile_definitions(${target} PRIVATE
    DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)
  target_compile_options(${target} PRIVATE -fwrapv -fno-strict-aliasing -fsigned-char)
  # The engine is 1990s C. gnu11 keeps `boolean`'s enum {false, true} legal
  # (C23 made those keywords). -fwrapv and -fno-strict-aliasing give signed
  # overflow and type-punning the meaning DOOM assumes (fixed-point math
  # wraps everywhere); without them clang -O2 turns R_Init into an endless
  # loop. -fsigned-char: DOOM was written for x86, where plain char is
  # signed; on RISC-V it is unsigned, and a DoomV replay of an x86 sim run
  # drifted apart at frame 38 until this was added. All three together make
  # the result the same on every compiler and CPU, which lock-step needs. The warnings silenced are the engine's, not ours.
  set_source_files_properties(${DOOM_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-parameter;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-implicit-fallthrough;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-pointer-sign;-Wno-format-truncation;-Wno-dangling-pointer;-Wno-unknown-warning-option;-Wno-array-bounds;-Wno-stringop-overflow")
  set_property(TARGET ${target} PROPERTY C_STANDARD 11)
  set_property(TARGET ${target} PROPERTY C_EXTENSIONS ON)
  foreach(f ${DOOM_ENGINE_FILES})
    set(defs)
    foreach(w ${PORT_WRAPS} ${PORT_EXTRA_WRAPS})
      string(REPLACE ":" ";" pair ${w})
      list(GET pair 0 sym)
      list(GET pair 1 home)
      if(f STREQUAL home)
        list(APPEND defs "${sym}=__real_${sym}")
      else()
        list(APPEND defs "${sym}=__wrap_${sym}")
      endif()
    endforeach()
    set_property(SOURCE ${DOOMGENERIC_DIR}/${f}.c APPEND PROPERTY COMPILE_DEFINITIONS ${defs})
  endforeach()
endfunction()

# The packed WAD: tools/wadpack.py turns DOOM1.WAD into build/.../doom1.wad.
set(PORT_WAD_INPUT ${PORT_DIR}/../wad/DOOM1.WAD CACHE FILEPATH "Shareware DOOM1.WAD")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(port_pack_wad out_var)
  set(out ${CMAKE_BINARY_DIR}/doom1.wad)
  if(NOT EXISTS ${PORT_WAD_INPUT})
    message(FATAL_ERROR "Put the shareware DOOM1.WAD at ${PORT_WAD_INPUT} (see wad/README.md)")
  endif()
  add_custom_command(OUTPUT ${out}
    COMMAND Python3::Interpreter ${PORT_DIR}/../tools/wadpack.py ${PORT_WAD_INPUT} ${out}
    DEPENDS ${PORT_WAD_INPUT} ${PORT_DIR}/../tools/wadpack.py
    COMMENT "Packing DOOM1.WAD for flash")
  set(${out_var} ${out} PARENT_SCOPE)
endfunction()

# RISC-V targets that take part in lock-step (firmware/ and doomv/) call this,
# so the Pico and DoomV retire the same instructions for the same work:
#   * the same optimisation level for the shared code;
#   * no linker relaxation. Relaxation turns two-instruction address
#     sequences into one gp-relative instruction, but only for data that
#     happens to land near gp -- which differs between the two images, since
#     their backends differ. Without it, both execute identical sequences
#     and their minstret counts per frame can be compared exactly.
function(port_riscv_lockstep_flags target)
  set_property(SOURCE ${DOOM_ENGINE_SOURCES} ${PORT_SOURCES} APPEND PROPERTY
    COMPILE_OPTIONS -O2 -mno-relax -ffunction-sections -fdata-sections)
  target_link_options(${target} PRIVATE -Wl,--no-relax)
endfunction()
