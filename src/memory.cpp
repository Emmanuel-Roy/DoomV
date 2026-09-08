#include "memory.hpp"
#include "extensions.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>

// Minimal ELF32/ELF64 structures -- just enough to read PT_LOAD segments,
// not a general-purpose ELF library. Field widths AND (for Phdr) field
// order differ between the two -- p_flags sits right after p_type in
// Elf64_Phdr, but last in Elf32_Phdr -- so this genuinely needs two
// struct layouts, not one templated on width.
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

struct Elf64_Ehdr {
	uint8_t  e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct Elf64_Phdr {
	uint32_t p_type;
	uint32_t p_flags; // note: right after p_type in Elf64, unlike Elf32 where it's last
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

constexpr uint32_t PT_LOAD = 1;

// Both Ehdr/Phdr pairs expose the same field names despite differing
// widths and (for Phdr) field order, so one templated loader body covers
// both formats -- the only thing that differs is which struct types get
// plugged in.
template <typename Ehdr, typename Phdr>
bool load_elf_generic(std::ifstream &file, std::vector<uint8_t> &ram)
{
	Ehdr ehdr;
	file.read((char *)&ehdr, sizeof(ehdr));
	if (!file) return false;
	if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
	    ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
		return false;
	}

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Phdr phdr;
		file.seekg((uint64_t)ehdr.e_phoff + (uint64_t)i * ehdr.e_phentsize);
		file.read((char *)&phdr, sizeof(phdr));
		if (!file) return false;

		if (phdr.p_type != PT_LOAD) continue;
		uint64_t vaddr = phdr.p_vaddr, memsz = phdr.p_memsz;
		if (vaddr < Memory::RAM_BASE || vaddr + memsz > Memory::RAM_BASE + Memory::RAM_SIZE + Memory::WAD_SIZE) {
			return false;
		}

		uint64_t ram_off = vaddr - Memory::RAM_BASE;

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

} // namespace

Memory::Memory()
	: ram(RAM_SIZE + WAD_SIZE, 0), fb(FB_SIZE, 0),
	  key_queue_head(0), key_queue_tail(0), instr_count(0), tick_counter(0), ms_accum(0), fb_write_count(0),
	  aplic(imsic_s)
{
	for (int i = 0; i < 16; i++) key_queue[i] = 0;
}

uint8_t Memory::read8(uint64_t addr)
{
	if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE + WAD_SIZE)
		return ram[addr - RAM_BASE];
	if (addr >= MMIO_FB && addr < MMIO_FB + FB_SIZE)
		return fb[addr - MMIO_FB];
	if (addr >= UART_BASE && addr < UART_BASE + UART_SIZE)
		return uart.read(addr - UART_BASE);
	return 0;
}

uint16_t Memory::read16(uint64_t addr)
{
	return (uint16_t)read8(addr) | ((uint16_t)read8(addr + 1) << 8);
}

uint32_t Memory::read32(uint64_t addr)
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
	if (addr >= CLINT_BASE && addr < CLINT_BASE + CLINT_SIZE) return timer.read32(addr - CLINT_BASE);
	if (addr >= APLIC_BASE && addr < APLIC_BASE + APLIC_SIZE) return aplic.read32(addr - APLIC_BASE);
	if (addr >= IMSIC_M_BASE && addr < IMSIC_M_BASE + IMSIC_SIZE) return 0; // seteipnum_le reads as zero, per spec
	if (addr >= IMSIC_S_BASE && addr < IMSIC_S_BASE + IMSIC_SIZE) return 0;

	// Fast path: every instruction fetch and almost every load/store lands
	// here. One range check plus a direct 4-byte copy replaces the 10-branch
	// chain of read32 -> 2x read16 -> 4x read8. memcpy (not a pointer cast)
	// avoids strict-aliasing UB; it produces RISC-V's little-endian byte
	// order for free because the host (x86) is little-endian too -- already
	// an implicit assumption everywhere else raw instruction words get
	// manipulated directly (e.g. the decoder's immediate-field shifts).
	if (addr >= RAM_BASE && addr <= RAM_BASE + RAM_SIZE + WAD_SIZE - 4) {
		uint32_t val;
		std::memcpy(&val, &ram[addr - RAM_BASE], sizeof(val));
		return val;
	}

	return (uint32_t)read16(addr) | ((uint32_t)read16(addr + 2) << 16);
}

uint64_t Memory::read64(uint64_t addr)
{
	if (addr >= RAM_BASE && addr <= RAM_BASE + RAM_SIZE + WAD_SIZE - 8) {
		uint64_t val;
		std::memcpy(&val, &ram[addr - RAM_BASE], sizeof(val));
		return val;
	}
	return (uint64_t)read32(addr) | ((uint64_t)read32(addr + 4) << 32);
}

void Memory::write8(uint64_t addr, uint8_t val)
{
	if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE + WAD_SIZE) {
		ram[addr - RAM_BASE] = val;
	} else if (addr >= MMIO_FB && addr < MMIO_FB + FB_SIZE) {
		fb[addr - MMIO_FB] = val;
		fb_write_count++;
	} else if (addr == MMIO_DEBUG) {
		std::putchar(val);
		std::fflush(stdout);
	} else if (addr >= UART_BASE && addr < UART_BASE + UART_SIZE) {
		uart.write(addr - UART_BASE, val);
	}
}

uint32_t Memory::take_fb_write_count()
{
	uint32_t count = fb_write_count;
	fb_write_count = 0;
	return count;
}

void Memory::write32(uint64_t addr, uint32_t val)
{
	// Unlike the byte-addressable devices below (RAM/framebuffer/debug
	// putchar, all handled through write8), these are register-file-style
	// devices whose writes need to land atomically (e.g. setipnum's
	// side effect must see the whole 32-bit value at once, not one byte
	// at a time) -- so they're intercepted here, before any byte
	// decomposition, exactly like read32 already special-cases
	// MMIO_INPUT/MMIO_TICK ahead of its own RAM fast path.
	if (addr >= CLINT_BASE && addr < CLINT_BASE + CLINT_SIZE) { timer.write32(addr - CLINT_BASE, val); return; }
	if (addr >= APLIC_BASE && addr < APLIC_BASE + APLIC_SIZE) { aplic.write32(addr - APLIC_BASE, val); return; }
	if (addr >= IMSIC_M_BASE && addr < IMSIC_M_BASE + IMSIC_SIZE) {
		if (addr - IMSIC_M_BASE == 0) imsic_m.set_pending(val); // seteipnum_le
		return;
	}
	if (addr >= IMSIC_S_BASE && addr < IMSIC_S_BASE + IMSIC_SIZE) {
		if (addr - IMSIC_S_BASE == 0) imsic_s.set_pending(val);
		return;
	}

	write8(addr + 0, (val >> 0) & 0xFF);
	write8(addr + 1, (val >> 8) & 0xFF);
	write8(addr + 2, (val >> 16) & 0xFF);
	write8(addr + 3, (val >> 24) & 0xFF);
}

void Memory::write64(uint64_t addr, uint64_t val)
{
	// tohost is not only the exit channel. It is a full HTIF port, and the
	// word is device(63:56) | command(55:48) | payload(47:0): device 1 is
	// the console, so a test that prints writes here too. Treating the
	// first nonzero store as the verdict reads a character as a result --
	// the damo hypervisor tests print, and every one of them looked like a
	// failure whose reported code was a carriage return.
	//
	// The exit is device 0, command 0, with bit 0 of the payload set; the
	// code is the rest of the payload, so 1 means pass and (n<<1)|1 names
	// failing subtest n.
	if (tohost_addr && addr == tohost_addr && tohost_value == 0
	    && (val >> 48) == 0 && (val & 1))
		tohost_value = val;
	write32(addr + 0, (uint32_t)(val & 0xFFFFFFFFu));
	write32(addr + 4, (uint32_t)(val >> 32));

	// The console half of HTIF. A test that prints sends device 1,
	// command 1, with the character in the payload, and then *waits for the
	// host to clear tohost* before sending the next one. With nothing
	// answering, the guest spins on its first character and never reaches
	// its own exit -- which is exactly how the damo hypervisor tests
	// behaved here: running forever, producing nothing, looking like a hang
	// in the emulator rather than an unimplemented handshake.
	//
	// Acknowledging is the whole protocol: print the byte and zero tohost.
	if (tohost_addr && addr == tohost_addr && ((val >> 56) & 0xFF) == 1
	    && ((val >> 48) & 0xFF) == 1) {
		std::putchar((int)(val & 0xFF));
		std::fflush(stdout);
		write32(addr + 0, 0);
		write32(addr + 4, 0);
	}
}

bool Memory::load_elf(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return false;

	if (Extensions.XLEN64) return load_elf_generic<Elf64_Ehdr, Elf64_Phdr>(file, ram);
	return load_elf_generic<Elf32_Ehdr, Elf32_Phdr>(file, ram);
}

bool Memory::load_blob(const char *path, uint64_t addr)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.is_open()) return false;
	size_t len = (size_t)file.tellg();
	file.seekg(0);

	if (addr < RAM_BASE || addr + len > RAM_BASE + RAM_SIZE + WAD_SIZE) return false;

	file.read((char *)&ram[addr - RAM_BASE], len);
	return (bool)file;
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
	ms_accum += count;
	while (ms_accum >= INSTR_PER_MS) {
		ms_accum -= INSTR_PER_MS;
		tick_counter++;
	}
	timer.tick(count);
}

// See memory.hpp for why this exists. The list mirrors the decode in the
// read/write paths above; a region added there and forgotten here becomes an
// access fault on real hardware DoomV claims to model, so the two belong
// next to each other in any future edit.
bool Memory::is_backed(uint64_t addr, unsigned size) const
{
	if (size == 0) size = 1;
	uint64_t last = addr + size - 1;
	if (last < addr) return false;   // wrapped

	auto in = [&](uint64_t base, uint64_t len) {
		return addr >= base && last < base + len;
	};

	// RAM and the WAD window are contiguous and are treated as one region:
	// they are one allocation, and Doom's WAD really is addressable memory.
	if (in(RAM_BASE, RAM_SIZE + WAD_SIZE)) return true;

	if (in(MMIO_FB, FB_SIZE)) return true;
	if (in(UART_BASE, UART_SIZE)) return true;
	if (in(CLINT_BASE, CLINT_SIZE)) return true;
	if (in(APLIC_BASE, APLIC_SIZE)) return true;
	if (in(IMSIC_M_BASE, IMSIC_SIZE)) return true;
	if (in(IMSIC_S_BASE, IMSIC_SIZE)) return true;

	// The three word-sized Doom control registers. Each is exactly four
	// bytes; an eight-byte access spanning two of them is not a thing the
	// hardware answers.
	if (in(MMIO_INPUT, 4) || in(MMIO_TICK, 4) || in(MMIO_DEBUG, 4)) return true;

	return false;
}
