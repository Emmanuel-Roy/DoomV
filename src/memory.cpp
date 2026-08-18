#include "memory.hpp"
#include <cstring>

Memory::Memory()
	: ram(RAM_SIZE + WAD_SIZE, 0), fb(FB_SIZE, 0),
	  key_queue_head(0), key_queue_tail(0), tick_counter(0), fb_write_count(0)
{
	for (int i = 0; i < 16; i++) key_queue[i] = 0;
}

uint8_t Memory::read8(uint32_t addr)
{
	if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE + WAD_SIZE)
		return ram[addr - RAM_BASE];
	if (addr >= MMIO_FB && addr < MMIO_FB + FB_SIZE)
		return fb[addr - MMIO_FB];
	return 0;
}

uint16_t Memory::read16(uint32_t addr)
{
	return (uint16_t)read8(addr) | ((uint16_t)read8(addr + 1) << 8);
}

uint32_t Memory::read32(uint32_t addr)
{
	if (addr == MMIO_INPUT) {
		if (key_queue_head == key_queue_tail) return 0;
		uint32_t val = key_queue[key_queue_head];
		key_queue_head = (key_queue_head + 1) % 16;
		return val;
	}
	if (addr == MMIO_TICK) {
		return tick_counter;
	}
	return (uint32_t)read16(addr) | ((uint32_t)read16(addr + 2) << 16);
}

void Memory::write8(uint32_t addr, uint8_t val)
{
	if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE + WAD_SIZE) {
		ram[addr - RAM_BASE] = val;
	} else if (addr >= MMIO_FB && addr < MMIO_FB + FB_SIZE) {
		fb[addr - MMIO_FB] = val;
		fb_write_count++;
	}
}

uint32_t Memory::take_fb_write_count()
{
	uint32_t count = fb_write_count;
	fb_write_count = 0;
	return count;
}

void Memory::write32(uint32_t addr, uint32_t val)
{
	write8(addr + 0, (val >> 0) & 0xFF);
	write8(addr + 1, (val >> 8) & 0xFF);
	write8(addr + 2, (val >> 16) & 0xFF);
	write8(addr + 3, (val >> 24) & 0xFF);
}

bool Memory::load_elf(const char *path)
{
	(void)path;
	// TODO: parse ELF program headers, copy each PT_LOAD segment to its
	// p_vaddr within the RAM region. See PLAN.md.
	return false;
}

bool Memory::load_wad(const uint8_t *wad_bytes, size_t len)
{
	if (len > WAD_SIZE) return false;
	std::memcpy(&ram[RAM_SIZE], wad_bytes, len);
	return true;
}

void Memory::push_key_event(bool pressed, uint8_t doom_keycode)
{
	int next = (key_queue_tail + 1) % 16;
	if (next == key_queue_head) return; // queue full, drop the event

	key_queue[key_queue_tail] = ((uint32_t)pressed << 8) | doom_keycode;
	key_queue_tail = next;
}

void Memory::step_instructions(uint32_t count)
{
	tick_counter += count;
}
