// A stand-in for a core's retirement port, so that Vitis HLS's C simulation
// (software emulation) and co-simulation (hardware emulation, in XSim) can be
// shown handing what comes out of it to DoomV in the same process: records
// go in as 64-bit words and come out of the synthesised RTL unchanged.
// See retire_tb.cpp and tools/verification/lockstep_lib.py --vitis.
#pragma once
#include "ap_int.h"
#include "hls_stream.h"

typedef ap_uint<64> retire_word;

void retire_port(hls::stream<retire_word> &in, hls::stream<retire_word> &out, int words);
