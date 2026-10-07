#include "retire_port.hpp"

void retire_port(hls::stream<retire_word> &in, hls::stream<retire_word> &out, int words)
{
	for (int i = 0; i < words; i++) {
#pragma HLS PIPELINE II=1
		out.write(in.read());
	}
}
