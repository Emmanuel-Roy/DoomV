# The PC simulator. Build it 32-bit to get the Pico's structure sizes:
#   cmake -S sim -B build/sim -G Ninja -DCMAKE_TOOLCHAIN_FILE=sim/zig-x86.cmake
#   cmake --build build/sim
cmake_minimum_required(VERSION 3.20)
project(doom_sim C)

include(${CMAKE_CURRENT_LIST_DIR}/../port/port.cmake)

set(PORT_EXTRA_WRAPS Z_Malloc:z_zone)   # for -zonereport
add_executable(doom_sim sim_main.c sim_zonereport.c)
port_add_doom(doom_sim)
set(SIM_OPT "-O2" CACHE STRING "Optimisation flag for the sim")
target_compile_options(doom_sim PRIVATE ${SIM_OPT} -g -fno-omit-frame-pointer)
target_link_libraries(doom_sim PRIVATE dbghelp)  # the stall watchdog names functions
# Count the engine's malloc use (lumpinfo lives there, not in the zone).
set_property(SOURCE ${DOOM_ENGINE_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS
  malloc=__wrap_malloc calloc=__wrap_calloc realloc=__wrap_realloc free=__wrap_free)

port_pack_wad(SIM_WAD)
add_custom_target(sim_wad ALL DEPENDS ${SIM_WAD})

# Debugging aid: engine files to build at -O0 regardless of SIM_OPT, e.g.
#   -DSIM_O0_FILES="r_data;p_switch"   (bisecting an optimiser-sensitive bug)
set(SIM_O0_FILES "" CACHE STRING "Engine files (no .c) to compile at -O0")
foreach(f ${SIM_O0_FILES})
  set_property(SOURCE ${DOOMGENERIC_DIR}/${f}.c APPEND PROPERTY COMPILE_OPTIONS -O0)
endforeach()
