# Compiler settings
#
# Clang by default, because it is much faster on this code: 1.25-1.31x with
# PGO and ThinLTO over the best the installed GCC can build, and ahead even
# plain. performance/README.md has the measurements. GCC remains fully
# supported and is the automatic fallback -- both compilers pass the whole
# gate, and the two builds stop both workloads with the same crash.log.
#
# Which one is picked, in order:
#   1. CXX= on the command line, honoured as ever (this is what pgo.py uses)
#   2. the llvm-mingw unpacked under build/toolchains/, if it is there
#   3. a clang++ on PATH
#   4. g++
#
# `scripts/get_clang.py` fetches (2) for a checkout that has no clang, and
# nothing here fails without it -- a machine with only GCC still just builds.
CLANG_BUNDLED := $(firstword $(wildcard build/toolchains/llvm-mingw-*/bin/clang++.exe))
ifneq ($(CLANG_BUNDLED),)
  CXX = $(CLANG_BUNDLED)
  CC = $(CLANG_BUNDLED:clang++.exe=clang.exe)
else ifneq ($(shell command -v clang++ 2>/dev/null),)
  CXX = clang++
  CC = clang
else
  CXX = g++
  CC = gcc
endif

# -frounding-math is for GCC; Clang accepts and ignores it, which is harmless
# here because nothing depends on the compiler preserving the FP environment
# across a call -- the flags are collected before the rounding mode is
# restored, deliberately, see ext_fp_common.hpp's collect_fflags.
#
# No -flto in the default build. With GCC 8.1 it is unusable: the LTO plugin
# warns "No symbol for section 'Extensions'" -- the C++17 inline variable in
# extensions.hpp -- and the linked emulator dies of heap corruption before
# printing anything. Clang's ThinLTO does work and is worth a further ~2% on
# its own (much more with a profile), but it is left to pgo.py --lto rather
# than made the default, so that `make` means the same build on either
# compiler.
CXXFLAGS = -std=c++2a -O3 -pthread -frounding-math -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic

# Include and Library paths
#
# Berkeley SoftFloat comes from the spike submodule rather than a separate
# checkout -- it is the same library spike itself uses for F/D, and vendoring
# a second copy would invite the two drifting apart.
SOFTFLOAT_DIR = tools/verification/simulators/spike/src/softfloat
# Per-compiler, because this rule is timestamp-only: without it, switching
# compilers leaves the previous one's objects in place and silently links
# them, since the .c files have not changed.
SOFTFLOAT_OBJDIR = build/softfloat/$(notdir $(CC))
SOFTFLOAT_SRCS = $(wildcard $(SOFTFLOAT_DIR)/*.c)
SOFTFLOAT_OBJS = $(patsubst $(SOFTFLOAT_DIR)/%.c,$(SOFTFLOAT_OBJDIR)/%.o,$(SOFTFLOAT_SRCS))

INCLUDES = -Isrc/include -Isrc -Isrc/softfloat -I$(SOFTFLOAT_DIR)
LIBS = -Lsrc/lib -lmingw32 -lSDL2main -lSDL2

# Source files
SRCS = src/main.cpp src/doom_system.cpp src/memory.cpp src/registers.cpp \
       src/riscv_decoder.cpp src/mmu.cpp src/pmp.cpp src/timer.cpp src/imsic.cpp src/aplic.cpp src/uart.cpp src/virtio_blk.cpp src/virtio_input.cpp src/virtio_9p.cpp src/lockstep.cpp \
       src/debugger.cpp src/gui.cpp \
       src/controls.cpp src/extensions.cpp \
       src/extensions/ext_i.cpp src/extensions/ext_m.cpp src/extensions/ext_a.cpp \
       src/extensions/ext_c.cpp src/extensions/ext_zba.cpp src/extensions/ext_zbb.cpp src/extensions/ext_zbkb.cpp src/extensions/ext_zfh.cpp \
       src/extensions/ext_zbs.cpp src/extensions/ext_zicond.cpp src/extensions/ext_zcb.cpp \
       src/extensions/ext_zihintpause.cpp src/extensions/ext_zihintntl.cpp \
       src/extensions/ext_zimop.cpp src/extensions/ext_zcmop.cpp \
       src/extensions/ext_zicbom.cpp src/extensions/ext_zicbop.cpp \
       src/extensions/ext_zicboz.cpp src/extensions/ext_zawrs.cpp \
       src/extensions/ext_zicntr.cpp \
       src/extensions/ext_zfa.cpp \
       src/extensions/ext_zfhmin.cpp \
       src/extensions/ext_svinval.cpp \
       src/extensions/ext_h.cpp \
       src/extensions/ext_h_ldst.cpp \
       src/extensions/ext_sscofpmf.cpp src/extensions/ext_ssstateen.cpp \
       src/extensions/ext_zifencei.cpp src/extensions/ext_zvbb.cpp src/extensions/ext_zvkned.cpp src/extensions/ext_zvknh.cpp src/extensions/ext_zvksm.cpp src/extensions/ext_zicsr.cpp src/extensions/ext_f.cpp src/extensions/ext_d.cpp \
       src/extensions/ext_v.cpp src/extensions/ext_v_config.cpp src/extensions/ext_v_ldst.cpp \
       src/extensions/ext_v_int.cpp src/extensions/ext_v_muldiv.cpp src/extensions/ext_v_mask.cpp \
       src/extensions/ext_v_perm.cpp src/extensions/ext_v_reduce.cpp src/extensions/ext_v_fp.cpp
OUT = riscv_doom.exe

all: $(OUT)

# SoftFloat is C, not C++, and is compiled as such: building it with g++
# is not merely stylistic, several of its files are not valid C++.
$(SOFTFLOAT_OBJDIR)/%.o: $(SOFTFLOAT_DIR)/%.c
	@mkdir -p $(SOFTFLOAT_OBJDIR)
	$(CC) -O2 -Isrc/softfloat -I$(SOFTFLOAT_DIR) -c $< -o $@

# The C++ sources and every header are prerequisites, not just the
# SoftFloat objects. Listing only the objects made this target look
# up-to-date after a source edit, so `make` reported success and left the
# previous binary in place -- which then gets tested and blamed for a bug
# that was already fixed. Before SoftFloat, `all` was phony and always
# relinked, so nothing depended on this being right.
HEADERS = $(wildcard src/*.hpp src/extensions/*.hpp src/include/*.h)
$(OUT): $(SOFTFLOAT_OBJS) $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(OUT) $(SRCS) $(SOFTFLOAT_OBJS) $(LIBS)

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