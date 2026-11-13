# Toolchain file: bare-metal RV32 guest for DoomV, with the same compiler the
# Pico firmware uses (xPack riscv-none-elf-gcc).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_C_COMPILER riscv-none-elf-gcc)
set(CMAKE_ASM_COMPILER riscv-none-elf-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# The Pico 2's Hazard3 -march, exactly as pico-sdk builds the firmware, so
# DOOM's instructions are the same on both machines.
set(CMAKE_C_FLAGS_INIT "-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32 -g -DNDEBUG")
set(CMAKE_ASM_FLAGS_INIT "-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32")
