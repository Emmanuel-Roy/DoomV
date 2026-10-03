#pragma once
#include <cstdint>

class Aplic;

// A Goldfish real-time clock, the one QEMU's virt board has at the same
// address, so the guest learns the date at boot -- Linux's driver
// (drivers/rtc/rtc-goldfish.c) is in the riscv defconfig, and RTC_HCTOSYS
// sets the system time from it. Without one the guest starts in whatever
// year systemd was built, and HTTPS and apt both refuse dates that far
// off.
//
// The time is never read from the host while the machine runs. It is
//
//     epoch + offset + mtime * NS_PER_TICK
//
// where mtime is the CLINT's, which advances with the instruction stream,
// and epoch is fixed before the first instruction: a constant unless -rtc
// says otherwise. -rtc=host takes the host's clock once, at start, the way
// a key press is an input -- -record logs it, -replay brings it back, and a
// snapshot keeps it -- so the same inputs still give the same machine.
// offset is what the guest wrote (hwclock --systohc), relative to that.
//
// Registers are the Goldfish layout: 32-bit, nanoseconds since the Unix
// epoch. Reading TIME_LOW latches the high half for the TIME_HIGH read that
// follows; writing TIME_HIGH then TIME_LOW sets the time, and ALARM_HIGH
// then ALARM_LOW arms the alarm.
class Rtc {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr uint64_t TIME_LOW        = 0x00;
	static constexpr uint64_t TIME_HIGH       = 0x04;
	static constexpr uint64_t ALARM_LOW       = 0x08;
	static constexpr uint64_t ALARM_HIGH      = 0x0c;
	static constexpr uint64_t IRQ_ENABLED     = 0x10;
	static constexpr uint64_t CLEAR_ALARM     = 0x14;
	static constexpr uint64_t ALARM_STATUS    = 0x18;
	static constexpr uint64_t CLEAR_INTERRUPT = 0x1c;

	// timebase-frequency in the device tree is 500 MHz.
	static constexpr uint64_t NS_PER_TICK = 2;
	// 2026-01-01 00:00:00 UTC: the clock's start when nothing names one.
	// Any constant keeps runs repeatable; a recent one keeps the guest's
	// certificate and package checks from failing on it.
	static constexpr uint64_t DEFAULT_EPOCH = 1767225600;

	explicit Rtc(uint32_t irq) : irq(irq) {}

	// Seconds since the Unix epoch at mtime 0. Before anything runs.
	void set_epoch(uint64_t seconds) { epoch_ns = seconds * 1000000000ull; }
	uint64_t epoch_seconds() const { return epoch_ns / 1000000000ull; }

	uint32_t read32(uint64_t offset, uint64_t mtime);
	void write32(uint64_t offset, uint32_t val, uint64_t mtime, Aplic &aplic);
	// Fire the alarm if its time has come. Called at the input points, which
	// fall at the same instruction every run.
	void poll(uint64_t mtime, Aplic &aplic);

private:
	uint64_t now(uint64_t mtime) const { return epoch_ns + offset_ns + mtime * NS_PER_TICK; }

	uint32_t irq;
	uint64_t epoch_ns = DEFAULT_EPOCH * 1000000000ull;
	uint64_t offset_ns = 0;        // guest-set time minus epoch-based time; wraps
	uint32_t time_high = 0;        // latched by a TIME_LOW read
	uint32_t set_high = 0;         // a TIME_HIGH write, waiting for TIME_LOW
	uint32_t alarm_high = 0;       // an ALARM_HIGH write, waiting for ALARM_LOW
	uint64_t alarm_ns = 0;
	uint8_t alarm_running = 0;
	uint8_t irq_enabled = 0;
	uint8_t irq_pending = 0;
};
