# Compiler settings
CXX = g++
CXXFLAGS = -std=c++2a -O3

# Include and Library paths
INCLUDES = -Isrc/include
LIBS = -Lsrc/lib -lmingw32 -lSDL2main -lSDL2

# Source files
SRCS = src/main.cpp src/Doom_System.cpp src/RV32IMAC_Core.cpp
OUT = riscv_doom.exe

all:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(OUT) $(SRCS) $(LIBS)

clean:
	del $(OUT)