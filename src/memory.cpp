#include "memory.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>

// Minimal ELF32 structures -- just enough to read PT_LOAD segments,
// not a general-purpose ELF library.
namespace {

struct Elf32_Ehdr {
	uint8_t  e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct Elf32_Phdr {
	uint32_t p_type;
	uint32_t p_offset;
	uint32_t p_vaddr;
	uint32_t p_paddr;
	uint32_t p_filesz;
	uint32_t p_memsz;
	uint32_t p_flags;
	uint32_t p_align;
};

constexpr uint32_t PT_LOAD = 1;

} // namespace

Memory::Memory()
	: ram(RAM_SIZE + WAD_SIZE, 0), fb(FB_SIZE, 0),
	  key_queue_head(0), key_queue_tail(0), instr_count(0), tick_counter(0), fb_write_count(0)
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
		std::lock_guard<std::mutex> lock(key_mutex);
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
	} else if (addr == MMIO_DEBUG) {
		std::putchar(val);
		std::fflush(stdout);
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
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return false;

	Elf32_Ehdr ehdr;
	file.read((char *)&ehdr, sizeof(ehdr));
	if (!file) return false;
	if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
	    ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
		return false;
	}

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf32_Phdr phdr;
		file.seekg(ehdr.e_phoff + (uint32_t)i * ehdr.e_phentsize);
		file.read((char *)&phdr, sizeof(phdr));
		if (!file) return false;

		if (phdr.p_type != PT_LOAD) continue;
		if (phdr.p_vaddr < RAM_BASE || phdr.p_vaddr + phdr.p_memsz > RAM_BASE + RAM_SIZE + WAD_SIZE) {
			return false;
		}

		uint32_t ram_off = phdr.p_vaddr - RAM_BASE;

		if (phdr.p_filesz > 0) {
			file.seekg(phdr.p_offset);
			file.read((char *)&ram[ram_off], phdr.p_filesz);
			if (!file) return false;
		}

		if (phdr.p_memsz > phdr.p_filesz) {
			std::memset(&ram[ram_off + phdr.p_filesz], 0, phdr.p_memsz - phdr.p_filesz);
		}
	}

	return true;
}

bool Memory::load_wad(const uint8_t *wad_bytes, size_t len)
{
	if (len > WAD_SIZE) return false;
	std::memcpy(&ram[RAM_SIZE], wad_bytes, len);
	return true;
}

void Memory::push_key_event(bool pressed, uint8_t doom_keycode)
{
	std::lock_guard<std::mutex> lock(key_mutex);
	int next = (key_queue_tail + 1) % 16;
	if (next == key_queue_head) return; // queue full, drop the event

	key_queue[key_queue_tail] = ((uint32_t)pressed << 8) | doom_keycode;
	key_queue_tail = next;
}

void Memory::step_instructions(uint32_t count)
{
	instr_count += count;
	tick_counter = instr_count / INSTR_PER_MS;
}
