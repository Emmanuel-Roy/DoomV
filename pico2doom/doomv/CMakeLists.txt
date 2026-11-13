# The DoomV guest: the Pico port's code as a bare-metal RV32 program for
# DoomV, which replays a recording from a real Pico 2 in lock-step.
#
#   cmake -S doomv -B build/doomv -G Ninja -DCMAKE_TOOLCHAIN_FILE=doomv/riscv-none-elf.cmake
#   cmake --build build/doomv        ->  build/doomv/pico2doom-doomv.elf (+ doom1.wad)
#
# Run it with DoomV (docs/LOCKSTEP.md):
#   riscv_doom build/doomv/doom1.wad build/doomv/pico2doom-doomv.elf \
#       -march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -picolink=run.p2dl
cmake_minimum_required(VERSION 3.20)
project(pico2doom_doomv C ASM)

include(${CMAKE_CURRENT_LIST_DIR}/../port/port.cmake)

add_executable(pico2doom-doomv.elf start.S doomv_main.c)
port_add_doom(pico2doom-doomv.elf)
port_riscv_lockstep_flags(pico2doom-doomv.elf)
target_include_directories(pico2doom-doomv.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR})
# The guest's own platform code, optimised like the firmware's (a Release
# build): it never runs inside a counted window, but at -O0 presenting a
# frame cost DoomV as many instructions as DOOM's whole frame.
set_source_files_properties(doomv_main.c PROPERTIES COMPILE_OPTIONS "-O2")
target_link_options(pico2doom-doomv.elf PRIVATE
  -nostartfiles -specs=nosys.specs -T ${CMAKE_CURRENT_LIST_DIR}/doomv.ld)
set_target_properties(pico2doom-doomv.elf PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/doomv.ld)

# The same packed WAD the Pico has in flash; DoomV places it in guest memory.
port_pack_wad(DOOMV_WAD)
add_custom_target(doomv_wad ALL DEPENDS ${DOOMV_WAD})
