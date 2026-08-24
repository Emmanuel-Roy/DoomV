# Compiler settings
CXX = g++
CXXFLAGS = -std=c++2a -O3 -pthread

# Include and Library paths
INCLUDES = -Isrc/include
LIBS = -Lsrc/lib -lmingw32 -lSDL2main -lSDL2

# Source files
SRCS = src/main.cpp src/doom_system.cpp src/memory.cpp src/registers.cpp \
       src/riscv_decoder.cpp src/riscv_core.cpp src/debugger.cpp src/gui.cpp \
       src/controls.cpp src/extensions.cpp
OUT = riscv_doom.exe

all:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(OUT) $(SRCS) $(LIBS)

clean:
	del $(OUT)