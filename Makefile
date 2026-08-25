# Compiler settings
CXX = g++
CXXFLAGS = -std=c++2a -O3 -pthread -frounding-math

# Include and Library paths
INCLUDES = -Isrc/include -Isrc
LIBS = -Lsrc/lib -lmingw32 -lSDL2main -lSDL2

# Source files
SRCS = src/main.cpp src/doom_system.cpp src/memory.cpp src/registers.cpp \
       src/riscv_decoder.cpp src/debugger.cpp src/gui.cpp \
       src/controls.cpp src/extensions.cpp \
       src/extensions/ext_i.cpp src/extensions/ext_m.cpp src/extensions/ext_a.cpp \
       src/extensions/ext_c.cpp src/extensions/ext_zicsr.cpp src/extensions/ext_fd.cpp \
       src/extensions/ext_v.cpp src/extensions/ext_v_config.cpp src/extensions/ext_v_ldst.cpp \
       src/extensions/ext_v_int.cpp src/extensions/ext_v_muldiv.cpp src/extensions/ext_v_mask.cpp \
       src/extensions/ext_v_perm.cpp src/extensions/ext_v_reduce.cpp src/extensions/ext_v_fp.cpp
OUT = riscv_doom.exe

all:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(OUT) $(SRCS) $(LIBS)

clean:
	del $(OUT)