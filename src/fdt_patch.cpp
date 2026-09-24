// Rewrite the memory node's size in a flattened device tree.
//
// The DTB is loaded into guest RAM as an opaque blob, and it carries its own
// idea of how much memory the machine has -- `reg = <0x0 0x80000000 0x0
// 0x40000000>`. With -ram= that number is no longer fixed, and a guest told
// the wrong one does not fail in any way that names memory: too large and
// Linux writes past the end of what exists, too small and it OOM-kills init.
//
// So the blob is patched in place after loading, which is exactly a size
// change -- the property value is four fixed cells, so nothing moves and no
// offset in the header needs recomputing.
#include "fdt_patch.hpp"

#include <cstring>
#include <string>

namespace {

constexpr uint32_t FDT_MAGIC      = 0xd00dfeed;
constexpr uint32_t FDT_BEGIN_NODE = 1;
constexpr uint32_t FDT_END_NODE   = 2;
constexpr uint32_t FDT_PROP       = 3;
constexpr uint32_t FDT_NOP        = 4;
constexpr uint32_t FDT_END        = 9;

uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void put_be64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

} // namespace

bool fdt_set_memory_size(uint8_t *blob, size_t len, uint64_t base, uint64_t size)
{
	if (len < 40 || be32(blob) != FDT_MAGIC) return false;
	const uint32_t total    = be32(blob + 4);
	const uint32_t off_str  = be32(blob + 12);
	const uint32_t off_stru = be32(blob + 8);
	const uint32_t len_stru = be32(blob + 36);
	if (total > len || off_stru + len_stru > total || off_str > total) return false;

	const char *strings = (const char *)blob + off_str;
	uint8_t *p = blob + off_stru;
	uint8_t *end = blob + off_stru + len_stru;

	// The root node is itself a BEGIN_NODE, with an empty name, so it is
	// depth 1 and its children are depth 2 -- which is where memory@... sits.
	// Only that one is touched: a node called memory@ nested deeper belongs
	// to something else (a /reserved-memory child, say).
	int depth = 0;
	bool in_memory = false;
	int memory_depth = -1;

	while (p + 4 <= end) {
		const uint32_t token = be32(p);
		p += 4;
		if (token == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			const size_t n = std::strlen(name);
			p += (n + 4) & ~3u;
			depth++;
			if (!in_memory && depth == 2 && std::strncmp(name, "memory@", 7) == 0) {
				in_memory = true;
				memory_depth = depth;
			}
		} else if (token == FDT_END_NODE) {
			if (in_memory && depth == memory_depth) in_memory = false;
			depth--;
		} else if (token == FDT_PROP) {
			if (p + 8 > end) return false;
			const uint32_t plen = be32(p);
			const uint32_t nameoff = be32(p + 4);
			p += 8;
			if (p + plen > end) return false;
			if (in_memory && off_str + nameoff < total
			    && std::strcmp(strings + nameoff, "reg") == 0) {
				// <#address-cells=2, #size-cells=2>: base then size, both
				// 64-bit. Anything else is a tree this was not written for,
				// and guessing at it would be worse than refusing.
				if (plen != 16) return false;
				put_be64(p, base);
				put_be64(p + 8, size);
				return true;
			}
			p += (plen + 3) & ~3u;
		} else if (token == FDT_NOP) {
			continue;
		} else if (token == FDT_END) {
			break;
		} else {
			return false;
		}
	}
	return false;
}
