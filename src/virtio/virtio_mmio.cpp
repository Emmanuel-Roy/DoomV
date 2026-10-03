#include "virtio_mmio.hpp"
#include "memory.hpp"
#include "aplic.hpp"

uint8_t VirtioMmio::read8(uint64_t offset) const
{
	if (offset >= REG_CONFIG) return config_read8(offset - REG_CONFIG);
	return (uint8_t)(read32(offset & ~3ull) >> (8 * (offset & 3)));
}

uint32_t VirtioMmio::read32(uint64_t offset) const
{
	const bool selected = queue_sel < num_queues;
	switch (offset) {
	case REG_MAGIC:          return MAGIC;
	case REG_VERSION:        return VERSION;
	case REG_DEVICE_ID:      return present() ? device_id : 0;
	case REG_VENDOR_ID:      return vendor;
	case REG_DEVICE_FEAT:    return features(device_feat_sel);
	case REG_QUEUE_NUM_MAX:  return selected ? QUEUE_MAX : 0;
	case REG_QUEUE_READY:    return selected ? queues[queue_sel].ready : 0;
	case REG_INTERRUPT_STAT: return interrupt_status;
	case REG_STATUS:         return status;
	case REG_CONFIG_GEN:     return 0;
	// No device here has shared memory regions. "None" is a length of all
	// ones; zero would be a region of no size at address 0, and the GPU
	// driver fails its probe trying to reserve it.
	case REG_SHM_LEN_LO: case REG_SHM_LEN_HI:
	case REG_SHM_BASE_LO: case REG_SHM_BASE_HI:
		return 0xFFFFFFFFu;
	default:
		if (offset >= REG_CONFIG) {
			uint32_t v = 0;
			for (int i = 0; i < 4; i++) v |= (uint32_t)config_read8(offset - REG_CONFIG + i) << (8 * i);
			return v;
		}
		return 0;
	}
}

void VirtioMmio::write8(uint64_t offset, uint8_t value)
{
	if (offset >= REG_CONFIG) config_write8(offset - REG_CONFIG, value);
}

void VirtioMmio::write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic)
{
	Queue *q = queue_sel < num_queues ? &queues[queue_sel] : nullptr;
	const auto low = [&](uint64_t &a) { a = (a & ~0xFFFFFFFFull) | value; };
	const auto high = [&](uint64_t &a) { a = (a & 0xFFFFFFFFull) | ((uint64_t)value << 32); };
	switch (offset) {
	case REG_DEVICE_FEAT_SEL: device_feat_sel = value; return;
	case REG_DRIVER_FEAT_SEL: driver_feat_sel = value; return;
	case REG_DRIVER_FEAT:     if (driver_feat_sel < 2) driver_feat[driver_feat_sel] = value; return;
	case REG_QUEUE_SEL:       queue_sel = value; return;
	case REG_QUEUE_NUM:       if (q) q->num = value; return;
	case REG_QUEUE_READY:     if (q) q->ready = value; return;
	case REG_QUEUE_DESC_LO:   if (q) low(q->desc); return;
	case REG_QUEUE_DESC_HI:   if (q) high(q->desc); return;
	case REG_QUEUE_AVAIL_LO:  if (q) low(q->avail); return;
	case REG_QUEUE_AVAIL_HI:  if (q) high(q->avail); return;
	case REG_QUEUE_USED_LO:   if (q) low(q->used); return;
	case REG_QUEUE_USED_HI:   if (q) high(q->used); return;
	case REG_INTERRUPT_ACK:   interrupt_status &= ~value; return;
	case REG_QUEUE_NOTIFY:
		if (value < num_queues && queues[value].ready && present()) notify(value, mem, aplic);
		return;
	case REG_STATUS:
		status = value;
		// Status 0 is a reset. Everything the driver set up is stale,
		// including how far the avail rings were read: carrying that
		// across makes a re-probed device skip its first request.
		if (value == 0) {
			for (Queue &each : queues) each = Queue{};
			interrupt_status = 0;
			reset();
		}
		return;
	default:
		if (offset >= REG_CONFIG)
			for (int i = 0; i < 4; i++) config_write8(offset - REG_CONFIG + i, (uint8_t)(value >> (8 * i)));
		return;
	}
}

bool VirtioMmio::has_chain(Memory &mem, unsigned q)
{
	const Queue &queue = queues[q];
	if (!queue.ready || !queue.avail || !queue.used) return false;
	return queue.last_avail != mem.read16(queue.avail + 2);
}

bool VirtioMmio::next_chain(Memory &mem, unsigned q, uint16_t &head)
{
	if (!has_chain(mem, q)) return false;
	Queue &queue = queues[q];
	const uint16_t slot = (uint16_t)(queue.last_avail % queue_size(queue));
	head = mem.read16(queue.avail + 4 + (uint64_t)slot * 2);
	queue.last_avail++;
	return true;
}

VirtioMmio::Desc VirtioMmio::descriptor(Memory &mem, unsigned q, uint16_t index)
{
	const uint64_t at = queues[q].desc + (uint64_t)index * 16;
	return Desc{mem.read64(at), mem.read32(at + 8), mem.read16(at + 12), mem.read16(at + 14)};
}

void VirtioMmio::complete(Memory &mem, unsigned q, uint16_t head, uint32_t written)
{
	const Queue &queue = queues[q];
	const uint16_t used_idx = mem.read16(queue.used + 2);
	const uint64_t slot = queue.used + 4 + (uint64_t)(used_idx % queue_size(queue)) * 8;
	mem.write32(slot, head);
	mem.write32(slot + 4, written);
	// The index after the entry it covers, so a driver that sees the new
	// index always sees a whole entry. Memory has no write16.
	const uint16_t next = (uint16_t)(used_idx + 1);
	mem.write8(queue.used + 2, (uint8_t)(next & 0xFF));
	mem.write8(queue.used + 3, (uint8_t)(next >> 8));
}

void VirtioMmio::interrupt(Aplic &aplic)
{
	interrupt_status |= 1;   // bit 0: the used ring moved
	aplic.assert_source(irq);
}
