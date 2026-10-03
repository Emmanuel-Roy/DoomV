#include "rtc.hpp"
#include "aplic.hpp"

uint32_t Rtc::read32(uint64_t offset, uint64_t mtime)
{
	switch (offset) {
	case TIME_LOW: {
		const uint64_t t = now(mtime);
		time_high = (uint32_t)(t >> 32);
		return (uint32_t)t;
	}
	case TIME_HIGH:    return time_high;
	case ALARM_LOW:    return (uint32_t)alarm_ns;
	case ALARM_HIGH:   return (uint32_t)(alarm_ns >> 32);
	case IRQ_ENABLED:  return irq_enabled;
	case ALARM_STATUS: return alarm_running;
	default:           return 0;
	}
}

void Rtc::write32(uint64_t offset, uint32_t val, uint64_t mtime, Aplic &aplic)
{
	switch (offset) {
	case TIME_HIGH:
		set_high = val;
		break;
	case TIME_LOW: {
		const uint64_t want = ((uint64_t)set_high << 32) | val;
		offset_ns = want - (epoch_ns + mtime * NS_PER_TICK);
		break;
	}
	case ALARM_HIGH:
		alarm_high = val;
		break;
	case ALARM_LOW:
		alarm_ns = ((uint64_t)alarm_high << 32) | val;
		alarm_running = 1;
		poll(mtime, aplic);   // one already past fires now
		break;
	case IRQ_ENABLED:
		irq_enabled = val & 1;
		if (irq_enabled && irq_pending) aplic.assert_source(irq);
		break;
	case CLEAR_ALARM:
		alarm_running = 0;
		break;
	case CLEAR_INTERRUPT:
		irq_pending = 0;
		break;
	default:
		break;
	}
}

void Rtc::poll(uint64_t mtime, Aplic &aplic)
{
	if (!alarm_running || now(mtime) < alarm_ns) return;
	alarm_running = 0;
	irq_pending = 1;
	if (irq_enabled) aplic.assert_source(irq);
}
