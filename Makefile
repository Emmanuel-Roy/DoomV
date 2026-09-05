# Compiler settings
CXX = g++
CXXFLAGS = -std=c++2a -O3 -pthread -frounding-math -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic

# Include and Library paths
INCLUDES = -Isrc/include -Isrc
LIBS = -Lsrc/lib -lmingw32 -lSDL2main -lSDL2

# Source files
SRCS = src/main.cpp src/doom_system.cpp src/memory.cpp src/registers.cpp \
       src/riscv_decoder.cpp src/mmu.cpp src/timer.cpp src/imsic.cpp src/aplic.cpp src/uart.cpp \
       src/debugger.cpp src/gui.cpp \
       src/controls.cpp src/extensions.cpp \
       src/extensions/ext_i.cpp src/extensions/ext_m.cpp src/extensions/ext_a.cpp \
       src/extensions/ext_c.cpp src/extensions/ext_zb.cpp src/extensions/ext_zicsr.cpp src/extensions/ext_fd.cpp \
       src/extensions/ext_v.cpp src/extensions/ext_v_config.cpp src/extensions/ext_v_ldst.cpp \
       src/extensions/ext_v_int.cpp src/extensions/ext_v_muldiv.cpp src/extensions/ext_v_mask.cpp \
       src/extensions/ext_v_perm.cpp src/extensions/ext_v_reduce.cpp src/extensions/ext_v_fp.cpp
OUT = riscv_doom.exe

all:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(OUT) $(SRCS) $(LIBS)

# Portable file deletion. GNU Make's built-in $(RM) is hardcoded to `rm -f`,
# which cmd does not have -- so `make clean` used to fail depending on which
# shell you launched it from. Make picks sh.exe when one is on PATH (Git Bash,
# MSYS) and silently falls back to cmd otherwise, so probe for the same sh it
# would have selected and pick the matching deleter.
ifeq ($(OS),Windows_NT)
  ifeq ($(findstring ok,$(shell sh -c "echo ok" 2>&1)),ok)
    RMF = rm -f
  else
    RMF = del /Q /F
  endif
else
  RMF = rm -f
endif

clean:
	-$(RMF) $(OUT)