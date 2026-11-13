# Pico2Doom firmware: DOOM on the RP2350's Hazard3 RISC-V cores.
#
#   cmake -S firmware -B build/firmware -G Ninja
#   cmake --build build/firmware        ->  build/firmware/pico2doom.uf2
#
# Needs: riscv-none-elf-gcc (xPack, the same compiler DoomV's guests use),
# CMake, Ninja, Python 3, a host C++ compiler (for picotool), and the
# submodules (git submodule update --init --recursive).
cmake_minimum_required(VERSION 3.20)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# --- the SDK, from the submodule, for the RISC-V cores --------------------
set(PICO_SDK_PATH ${ROOT}/external/pico-sdk CACHE PATH "pico-sdk (submodule)")
set(PICO_PLATFORM rp2350-riscv CACHE STRING "Hazard3 cores: same ISA as DoomV")
set(PICO_BOARD pico2 CACHE STRING "Raspberry Pi Pico 2")
set(PICO_GCC_TRIPLE riscv-none-elf CACHE STRING "xPack RISC-V GCC")
# picotool (turns the ELF into a UF2) builds from our submodule, not a download.
set(FETCHCONTENT_SOURCE_DIR_PICOTOOL ${ROOT}/external/picotool CACHE PATH "")
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(pico2doom C CXX ASM)

# Identical builds (docs/LOCKSTEP.md): pico-sdk's runtime --wraps malloc,
# calloc, realloc and free (pico_malloc, adding a mutex) and printf, vprintf,
# puts, putchar and getchar (pico_stdio, through its stdio drivers). The
# --wrap reaches into newlib too (sscanf's mallocs, assert's printf), so the
# Pico and DoomV ran different code under DOOM. Define the two libraries
# first -- the SDK skips its own when they exist -- without the wraps:
# pico_stdio keeps its source and headers, pico_malloc only its headers
# (malloc.c is nothing but the wrappers). newlib's functions are then the
# ones called, as on DoomV.
set(SDK_RP2 ${PICO_SDK_PATH}/src/rp2_common)
add_library(pico_stdio_headers INTERFACE)
target_include_directories(pico_stdio_headers SYSTEM INTERFACE ${SDK_RP2}/pico_stdio/include)
add_library(pico_stdio INTERFACE)
target_sources(pico_stdio INTERFACE ${SDK_RP2}/pico_stdio/stdio.c)
target_link_libraries(pico_stdio INTERFACE pico_stdio_headers)
add_library(pico_malloc_headers INTERFACE)
target_include_directories(pico_malloc_headers SYSTEM INTERFACE ${SDK_RP2}/pico_malloc/include)
add_library(pico_malloc INTERFACE)
target_link_libraries(pico_malloc INTERFACE pico_malloc_headers)

pico_sdk_init()

include(${ROOT}/port/port.cmake)

# --- options ---------------------------------------------------------------
set(DOOM_SYS_MHZ 250 CACHE STRING "System clock in MHz (150 = stock; 250 = default overclock; 300 hangs at boot)")
set(DOOM_LCD_SPI_HZ 0 CACHE STRING "ILI9341 SPI clock in Hz (0 = fastest at or under 42 MHz)")
set(DOOM_FPS_CAP 30 CACHE STRING "Frame pacing: frames per second shown (0 = unpaced)")
option(DOOM_LCD_TEAR "Never wait for the LCD: send a frame only when it is idle (can tear; keeps 30 fps)" ON)

add_executable(pico2doom
  main.c
  lcd_ili9341.c
  buttons.c
  usb_link.c
  core1_io.c
  usb_descriptors.c
  hud_font.c
  wad_blob.S)
port_add_doom(pico2doom)

target_include_directories(pico2doom PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(pico2doom PRIVATE
  DOOM_SYS_MHZ=${DOOM_SYS_MHZ}
  DOOM_LCD_SPI_HZ=${DOOM_LCD_SPI_HZ}
  DOOM_FPS_CAP=${DOOM_FPS_CAP}
  DOOM_LCD_TEAR=$<BOOL:${DOOM_LCD_TEAR}>
  # Core 0's stack: the linker reserves 4 KB at the top of SCRATCH_Y, and
  # SCRATCH_X (4 KB) sits directly below it, unused -- core 1 runs on a
  # stack in main RAM (usb_link.c) -- so DOOM really has 8 KB.
  PICO_STACK_SIZE=0x1000
  PICO_HEAP_SIZE=0
  # Only the two USB CDC interfaces this firmware defines; no stdio on USB.
  PICO_STDIO_USB=0
  CFG_TUSB_CONFIG_FILE="tusb_config.h")

# DOOM and the port layer are built exactly as the DoomV guest builds them
# (-O2, no linker relaxation: see port_riscv_lockstep_flags). The SDK's own
# -march (rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb) is kept: DoomV runs every
# one of those extensions, so the engine's instructions are the same on both
# machines.
port_riscv_lockstep_flags(pico2doom)

# The packed WAD goes into flash inside the firmware image (one UF2 to copy).
port_pack_wad(PACKED_WAD)
set_source_files_properties(wad_blob.S PROPERTIES
  COMPILE_DEFINITIONS "WAD_PATH=\"${PACKED_WAD}\""
  OBJECT_DEPENDS ${PACKED_WAD})

target_link_libraries(pico2doom PRIVATE
  pico_stdlib
  pico_multicore
  hardware_spi
  hardware_dma
  hardware_clocks
  hardware_vreg
  hardware_watchdog
  tinyusb_device
  tinyusb_board)

# newlib's printf, not the SDK's own: DoomV's guest build links newlib from
# the same toolchain, so formatting code (DOOM's HUD messages, say) retires
# the same instructions on both machines.
pico_set_printf_implementation(pico2doom compiler)
# Likewise libgcc's soft-float, not pico-sdk's replacement (DOOM uses a little
# float); and port/port_libc.c replaces the SDK's malloc and stdio wrappers.
pico_set_float_implementation(pico2doom compiler)
pico_set_double_implementation(pico2doom compiler)

pico_set_program_name(pico2doom "Pico2Doom")
pico_set_program_description(pico2doom "DOOM (shareware) on the RP2350's RISC-V cores")
pico_add_extra_outputs(pico2doom)

# Print the RAM/flash budget after every build.
add_custom_command(TARGET pico2doom POST_BUILD
  COMMAND Python3::Interpreter ${ROOT}/tools/membudget.py $<TARGET_FILE:pico2doom>
  VERBATIM)

# --- identical builds ------------------------------------------------------
# Lock-step with DoomV holds the Pico to DoomV instruction by instruction, so
# every firmware build also builds the DoomV guest (doomv/, same compiler and
# flags) and checks that everything DOOM can execute inside a counted window
# is the same machine code in both, down through newlib and libgcc
# (tools/compare_isa.py). If the builds ever drift apart, this build fails.
include(ExternalProject)
ExternalProject_Add(doomv_guest
  SOURCE_DIR ${ROOT}/doomv
  BINARY_DIR ${CMAKE_BINARY_DIR}/doomv-guest
  CMAKE_ARGS -G ${CMAKE_GENERATOR} -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
             -DCMAKE_TOOLCHAIN_FILE=${ROOT}/doomv/riscv-none-elf.cmake
             -DPORT_WAD_INPUT=${PORT_WAD_INPUT}
             -DPORT_FUNCPROF=${PORT_FUNCPROF}
             -DPORT_FUNCPROF_BITS=${PORT_FUNCPROF_BITS}
  BUILD_ALWAYS 1
  INSTALL_COMMAND "")
add_custom_target(identical_builds ALL
  COMMAND Python3::Interpreter ${ROOT}/tools/compare_isa.py
          $<TARGET_FILE:pico2doom> ${CMAKE_BINARY_DIR}/doomv-guest/pico2doom-doomv.elf
  DEPENDS pico2doom doomv_guest
  COMMENT "Checking the Pico and DoomV builds are identical where DOOM is counted"
  VERBATIM)
