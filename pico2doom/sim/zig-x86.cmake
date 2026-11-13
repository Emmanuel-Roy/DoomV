# Toolchain file: build the simulator as a 32-bit Windows program with
# `zig cc` (pip install ziglang). 32-bit matters -- DOOM's map structures are
# full of pointers, and at 4 bytes each they are the size they are on the
# RP2350, so the memory the sim reports is what the Pico needs.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

execute_process(
  COMMAND python -c "import ziglang, os; print(os.path.dirname(ziglang.__file__))"
  OUTPUT_VARIABLE ZIG_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT EXISTS "${ZIG_DIR}/zig.exe")
  message(FATAL_ERROR "zig not found: pip install ziglang")
endif()
file(TO_NATIVE_PATH "${ZIG_DIR}/zig.exe" ZIG_EXE)

set(_wrap ${CMAKE_CURRENT_BINARY_DIR}/zigwrap)
file(MAKE_DIRECTORY ${_wrap})
file(WRITE ${_wrap}/zigcc.cmd "@\"${ZIG_EXE}\" cc -target x86-windows-gnu %*\r\n")
file(WRITE ${_wrap}/zigar.cmd "@\"${ZIG_EXE}\" ar %*\r\n")
file(WRITE ${_wrap}/zigranlib.cmd "@\"${ZIG_EXE}\" ranlib %*\r\n")

set(CMAKE_C_COMPILER ${_wrap}/zigcc.cmd)
# zig 0.13's MinGW headers define _strnicmp as a gnu_inline function that
# calls itself; clang compiles every strncasecmp into an endless loop (DOOM
# hung in R_TextureNumForName). __CRT__NO_INLINE drops those inline bodies.
# -fno-sanitize=undefined: zig turns on UBSan traps in unoptimised builds,
# and vanilla DOOM deliberately indexes past arrays into padding (visplanes).
set(CMAKE_C_FLAGS_INIT "-D__CRT__NO_INLINE -fno-sanitize=undefined")
set(CMAKE_AR ${_wrap}/zigar.cmd CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB ${_wrap}/zigranlib.cmd CACHE FILEPATH "" FORCE)
