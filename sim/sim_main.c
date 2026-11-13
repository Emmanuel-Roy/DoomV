// The PC simulator: the Pico port's code with a PC underneath.
//
// It exists to answer "does this fit, and does it work?" without hardware:
//
//   * the zone is capped at what a Pico 2 has left (-zone KB), so a level
//     that will not fit fails here with the same Z_Malloc error;
//   * the WAD is mapped read-only, as flash is, so an engine write into a
//     lump crashes here instead of silently misbehaving on the Pico;
//   * built 32-bit (see sim/README.md) its structures are the size they
//     are on the RP2350, so the memory it reports is the Pico's;
//   * frames can be dumped as PPM to look at.
//
// Usage: doom_sim [-zone KB] [-frames N] [-dump N,N,...] [-look | -explore] [-zonereport]
//                 [-hashes FILE] [-record FILE]
//                 [-fastclock] [-- any DOOM arguments, e.g. -warp 1 6]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define boolean win_boolean      // windows.h has its own; DOOM's is an enum
#include <windows.h>
#include <dbghelp.h>
#undef boolean
#else
#include <sys/mman.h>
#endif

#include "doomkeys.h"
#include "doomstat.h"
#include "p_local.h"
#include "r_state.h"
#include "port.h"
#include "port_perf.h"

void port_run(char **extra);
int port_zone_used(void);

static const char *wad_path = "build/sim/doom1.wad";
static int zone_kb = 0;            // 0: measure mode, a generous zone
static long max_frames = -1;
static int dump_frames[64], n_dump;
static int look_around;            // turn in place, to render every direction
static int explore;                // god mode, visit every sector and spin: worst-case renderer load
static int zone_report;            // -zonereport: list the zone's owners at exit
void sim_zone_report(void);
static int fast_clock;             // each clock read advances 1 ms (deterministic)
static long rows_private;     // rows presented from port_video.c's borrowed back buffer
static volatile long progress;

// ---- memory accounting for malloc (lumpinfo and friends live here) -----

static long heap_now, heap_peak;
// The engine's calls are renamed to these by sim/CMakeLists.txt; this file
// is not renamed, so malloc/free here are the C library's.
#define __real_malloc malloc
#define __real_free free

typedef union { size_t n; double align; } hdr_t;

static long frame_no;
void *__wrap_malloc(size_t n)
{
	// SIM_TRACE_MALLOC=1: list the engine's malloc calls once DOOM is running
	// (on a Pico they reach the C library's _sbrk -- platform code).
	static int trace = -1;
	if (trace < 0) trace = getenv("SIM_TRACE_MALLOC") != NULL;
	if (trace && frame_no > 0) printf("sim: malloc(%u) at frame %ld from %p\n", (unsigned)n, frame_no, __builtin_return_address(0));
	hdr_t *h = __real_malloc(n + sizeof(hdr_t));
	if (!h) return NULL;
	h->n = n;
	heap_now += n;
	if (heap_now > heap_peak) heap_peak = heap_now;
	return h + 1;
}
void *__wrap_calloc(size_t a, size_t b)
{
	void *p = __wrap_malloc(a * b);
	if (p) memset(p, 0, a * b);
	return p;
}
void __wrap_free(void *p)
{
	if (!p) return;
	hdr_t *h = (hdr_t *)p - 1;
	heap_now -= h->n;
	__real_free(h);
}
void *__wrap_realloc(void *p, size_t n)
{
	if (!p) return __wrap_malloc(n);
	hdr_t *h = (hdr_t *)p - 1;
	void *q = __wrap_malloc(n);
	if (q) memcpy(q, p, h->n < n ? h->n : n);
	__wrap_free(p);
	return q;
}

// ---- backend --------------------------------------------------------------

static uint8_t *wad;
static uint32_t wad_size;
static uint32_t clock_ms;

void backend_init(void) {}

uint32_t backend_ticks_ms(void)
{
	if (fast_clock) return clock_ms++;
	return (uint32_t)(clock() * 1000 / CLOCKS_PER_SEC);
}

void backend_sleep_ms(uint32_t ms) { if (fast_clock) clock_ms += ms; }

uint64_t backend_cycles(void)
{
#ifdef _WIN32
	LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart;
#else
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
#endif
}

uint32_t backend_cycles_per_us(void)
{
#ifdef _WIN32
	LARGE_INTEGER f; QueryPerformanceFrequency(&f);
	uint32_t r = (uint32_t)(f.QuadPart / 1000000);
	return r ? r : 1;
#else
	return 1000;
#endif
}

// -look: hold "turn right" for the whole run (with a pause every so often),
// so the renderer draws the level from every direction at the start spot.
static FILE *record_file;

int backend_get_key(int *pressed, unsigned char *key)
{
	static int held;
	if (!look_around || frame_no < 40) return 0;
	if (!held) {
		held = 1; *pressed = 1; *key = KEY_RIGHTARROW;
		if (record_file) {   // mirror it to the "PC", as the firmware does (LINK_TAG_INPUT)
			uint8_t rec[4] = { 0x0B, 0, 1, KEY_RIGHTARROW };
			fwrite(rec, 1, sizeof rec, record_file);
		}
		return 1;
	}
	return 0;
}

static void write_ppm(const uint8_t *frame, const uint8_t *pal, long n)
{
	char name[64];
	snprintf(name, sizeof name, "frame%05ld.ppm", n);
	FILE *f = fopen(name, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", PORT_SCREEN_W, PORT_SCREEN_H);
	for (int i = 0; i < PORT_SCREEN_W * PORT_SCREEN_H; i++) fwrite(&pal[3 * frame[i]], 1, 3, f);
	fclose(f);
	printf("sim: wrote %s\n", name);
}

static void finish(int code, const char *why);

// -explore: every 6 frames, jump to the middle of the next sector (when the
// player fits there) and keep turning, so over a run the renderer draws the
// level from everywhere. Invulnerable, so the monsters can't end it early.
static void explore_step(void)
{
	static int next_sector;
	if (gamestate != GS_LEVEL || !players[consoleplayer].mo) return;
	player_t *p = &players[consoleplayer];
	p->cheats |= CF_GODMODE;
	p->mo->angle += ANG90 / 6;
	if (frame_no % 6) return;
	for (int tries = 0; tries < numsectors; tries++) {
		sector_t *sec = &sectors[next_sector++ % numsectors];
		if (P_TeleportMove(p->mo, sec->soundorg.x, sec->soundorg.y)) {
			p->mo->z = p->mo->floorz;
			break;
		}
	}
}

void backend_present_wait(void) {}

void backend_present(const uint8_t *frame, const uint16_t *pal565, const uint8_t *pal_rgb,
                     const uint8_t *tail, int tail_rows)
{
	(void)tail;
	rows_private += tail_rows;
	(void)pal565;
	frame_no++;
	progress++;
	if (explore) explore_step();
	for (int i = 0; i < n_dump; i++)
		if (dump_frames[i] == frame_no) write_ppm(frame, pal_rgb, frame_no);
	if (max_frames >= 0 && frame_no >= max_frames) finish(0, "frame limit reached");
}

const uint8_t *backend_wad(uint32_t *size) { *size = wad_size; return wad; }

uint8_t *backend_zone(int *size)
{
	int kb = zone_kb ? zone_kb : 4096;
	*size = kb * 1024;
	uint8_t *z = __real_malloc(*size);
	if (!z) { fprintf(stderr, "sim: no memory for zone\n"); exit(2); }
	return z;
}

void backend_log(const char *line) { printf("%s\n", line); fflush(stdout); }
void backend_hud(const char *text) { (void)text; }

// -hashes FILE: log every frame's framebuffer and game-state hashes -- the
// same ones lock-step compares -- to check that a change to the engine
// changed nothing (compare the files from a patched and a stock build).
static FILE *hash_log;
// -record FILE: write the lock-step recording a Pico would send, so DoomV can
// replay a sim run (tests the link without hardware; no retired counts).
// (record_file is declared above backend_get_key.)

static void finish(int code, const char *why)
{
	const perf_window_t *t = perf_totals();
	if (zone_report) sim_zone_report();
	if (hash_log) fclose(hash_log);
	if (record_file) fclose(record_file);
	printf("sim: %s after %ld frames (%ld%% of rows sent from the back buffer: the Pico's core 0 skips that much of the LCD wait)\n",
	       why, frame_no, frame_no ? rows_private * 100 / (frame_no * PORT_SCREEN_H) : 0);
	printf("sim: zone peak %d KB live data (of %d KB), malloc peak %ld KB, pointer size %d\n",
	       t->zone_peak / 1024, t->zone_size / 1024, heap_peak / 1024, (int)sizeof(void *));
	const perf_limits_t *l = perf_limits();
	printf("sim: renderer peaks: %d visplanes, %d openings, %d drawsegs, %d vissprites\n",
	       l->visplanes, l->openings, l->drawsegs, l->vissprites);
	printf("SIMRESULT zone_peak=%d malloc_peak=%ld frames=%ld code=%d visplanes=%d openings=%d drawsegs=%d vissprites=%d\n",
	       t->zone_peak, heap_peak, frame_no, code, l->visplanes, l->openings, l->drawsegs, l->vissprites);
	fflush(stdout);
	exit(code);
}

void backend_fatal(const char *msg)
{
	printf("DOOM: %s\n", msg);
	// "timed N gametics in M realtics" is how -timedemo reports, not a failure.
	finish(strncmp(msg, "timed ", 6) == 0 || strncmp(msg, "Thanks", 6) == 0 ? 0 : 1, "stopped");
}

enum port_link_mode backend_link_mode(void) { return (hash_log || record_file) ? LINK_RECORD : LINK_NONE; }
void backend_link_write(const void *d, size_t n) { if (record_file) fwrite(d, 1, n, record_file); }
uint32_t backend_replay_ticks(void) { return 0; }
int backend_replay_key(int *p, unsigned char *k) { (void)p; (void)k; return 0; }
void backend_link_check(uint32_t f, uint32_t a, uint32_t b, uint32_t it, uint32_t ir)
{
	// it/ir (retired instructions) are 0 here: x86 has no minstret.
	if (hash_log) fprintf(hash_log, "%u %08x %08x %u %u\n", f, a, b, it, ir);
}

static void load_wad(void)
{
	FILE *f = fopen(wad_path, "rb");
	if (!f) { fprintf(stderr, "sim: cannot open %s (build the sim first)\n", wad_path); exit(2); }
	fseek(f, 0, SEEK_END);
	wad_size = (uint32_t)ftell(f);
	fseek(f, 0, SEEK_SET);
#ifdef _WIN32
	wad = VirtualAlloc(NULL, wad_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	wad = mmap(NULL, wad_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
	if (fread(wad, 1, wad_size, f) != wad_size) { fprintf(stderr, "sim: short read\n"); exit(2); }
	fclose(f);
	// Read-only from here on, like flash.
#ifdef _WIN32
	DWORD old;
	VirtualProtect(wad, wad_size, PAGE_READONLY, &old);
#else
	mprotect(wad, wad_size, PROT_READ);
#endif
}

// Stall watchdog: if nothing is drawn or logged for 5 seconds, print where
// the main thread is -- function and line, read from the PDB by DbgHelp.
#ifdef _WIN32
static HANDLE main_thread;
static void where(uint32_t addr)
{
	static union { SYMBOL_INFO info; char bytes[sizeof(SYMBOL_INFO) + 256]; } buf;
	SYMBOL_INFO *sym = &buf.info;
	sym->SizeOfStruct = sizeof(SYMBOL_INFO);
	sym->MaxNameLen = 255;
	DWORD64 off = 0;
	DWORD loff = 0;
	IMAGEHLP_LINE64 line = { .SizeOfStruct = sizeof line };
	if (SymFromAddr(GetCurrentProcess(), addr, &off, sym)) printf("  %s+0x%llx", sym->Name, (unsigned long long)off);
	else printf("  %08x", addr);
	if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &loff, &line)) printf("  (%s:%lu)", line.FileName, line.LineNumber);
	printf("\n");
}
static void backtrace_from(const CONTEXT *c)
{
	SymInitialize(GetCurrentProcess(), NULL, TRUE);
	where((uint32_t)c->Eip);
	uint32_t *fp = (uint32_t *)c->Ebp;
	for (int i = 0; i < 12 && fp && !IsBadReadPtr(fp, 8); i++, fp = (uint32_t *)fp[0])
		where(fp[1]);
}

// Crashes (including zig's UBSan traps, and writes to the read-only WAD).
// Symbolising in the crashed thread is not safe, so it hands its context to
// the watchdog thread and waits there.
static CONTEXT crash_ctx;
static volatile LONG crashed;
static LONG WINAPI on_crash(EXCEPTION_POINTERS *e)
{
	DWORD code = e->ExceptionRecord->ExceptionCode;
	printf("sim: CRASH %s (0x%08lx)\n",
	       code == EXCEPTION_ILLEGAL_INSTRUCTION ? "undefined behaviour trap" :
	       code == EXCEPTION_ACCESS_VIOLATION ? "access violation" : "exception",
	       (unsigned long)code);
	if (code == EXCEPTION_ACCESS_VIOLATION) {
		uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
		printf("  %s address %08lx%s\n", e->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read of",
		       (unsigned long)a, (a >= (uintptr_t)wad && a < (uintptr_t)wad + wad_size) ? " -- inside the WAD (flash!)" : "");
	}
	crash_ctx = *e->ContextRecord;
	crashed = 1;
	for (;;) Sleep(1000);
	return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI watchdog(LPVOID arg)
{
	(void)arg;
	long seen = -1;
	for (int ms = 0;; ms += 50) {
		Sleep(50);
		if (crashed) { printf("  at:\n"); backtrace_from(&crash_ctx); ExitProcess(4); }
		if (ms < 5000) continue;
		ms = 0;
		if (progress != seen) { seen = progress; continue; }
		SuspendThread(main_thread);
		CONTEXT c = { .ContextFlags = CONTEXT_FULL };
		GetThreadContext(main_thread, &c);
		printf("sim: STALLED, main thread is at:\n");
		backtrace_from(&c);
		ExitProcess(3);
	}
}
#endif

int main(int argc, char **argv)
{
	char *extra[16] = { 0 };
	int n_extra = 0;
	setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
	CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
	SetUnhandledExceptionFilter(on_crash);
#endif
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-zone") && i + 1 < argc) zone_kb = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-frames") && i + 1 < argc) max_frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "-wad") && i + 1 < argc) wad_path = argv[++i];
		else if (!strcmp(argv[i], "-look")) look_around = 1;
		else if (!strcmp(argv[i], "-explore")) explore = 1;
		else if (!strcmp(argv[i], "-fastclock")) fast_clock = 1;
		else if (!strcmp(argv[i], "-zonereport")) zone_report = 1;
		else if (!strcmp(argv[i], "-hashes") && i + 1 < argc) hash_log = fopen(argv[++i], "w");
		else if (!strcmp(argv[i], "-record") && i + 1 < argc) record_file = fopen(argv[++i], "wb");
		else if (!strcmp(argv[i], "-dump") && i + 1 < argc) {
			for (char *s = strtok(argv[++i], ","); s && n_dump < 64; s = strtok(NULL, ","))
				dump_frames[n_dump++] = atoi(s);
		} else if (!strcmp(argv[i], "--")) {
			while (++i < argc && n_extra < 15) extra[n_extra++] = argv[i];
		}
	}
	load_wad();
	backend_init();
	port_run(extra);
	return 0;
}
