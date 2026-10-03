#pragma once
#include <cstddef>
#include <cstdint>

// Rewrites the root-level memory@... node's `reg` to <base, size>, in place.
// Returns false if the blob is not a device tree this understands, or has no
// such node -- in which case nothing has been written to it.
bool fdt_set_memory_size(uint8_t *blob, size_t len, uint64_t base, uint64_t size);

// Removes the root-level node called `name` (e.g. "framebuffer@50000000"),
// in place, by turning it into FDT_NOP tokens. Returns false if there is no
// such node.
bool fdt_remove_node(uint8_t *blob, size_t len, const char *name);
