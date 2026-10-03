#include "virtio_snd.hpp"
#include "memory.hpp"
#include <algorithm>
#include <cstring>

namespace {

// Request codes and status values (include/uapi/linux/virtio_snd.h).
constexpr uint32_t R_PCM_INFO = 0x0100, R_PCM_SET_PARAMS = 0x0101, R_PCM_PREPARE = 0x0102,
                   R_PCM_RELEASE = 0x0103, R_PCM_START = 0x0104, R_PCM_STOP = 0x0105;
constexpr uint32_t S_OK = 0x8000, S_BAD_MSG = 0x8001, S_NOT_SUPP = 0x8002;
constexpr unsigned CONTROLQ = 0, EVENTQ = 1, TXQ = 2, RXQ = 3;
constexpr uint32_t STATUS_SIZE = 8;   // virtio_snd_pcm_status: status, latency_bytes
constexpr uint32_t PCM_INFO_SIZE = 32;

uint32_t le32(const std::vector<uint8_t> &b, size_t at)
{
	if (at + 4 > b.size()) return 0;
	return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
}

void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
	if (b.size() < at + 4) b.resize(at + 4);
	for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (8 * i));
}

void put64(std::vector<uint8_t> &b, size_t at, uint64_t v)
{
	put32(b, at, (uint32_t)v);
	put32(b, at + 4, (uint32_t)(v >> 32));
}

} // namespace

void VirtioSnd::notify(unsigned q, Memory &mem, Aplic &aplic)
{
	if (q == CONTROLQ) control(mem, aplic);
	else if (q == TXQ || q == RXQ) take_io(q, mem);
	// eventq: the driver's buffers wait there for events this device never
	// sends -- no jacks to plug, no periods reported by event.
	(void)EVENTQ;
}

void VirtioSnd::reset()
{
	for (Stream &s : streams) s = Stream{};
	tx.clear();
	rx.clear();
}

// Each request is answered at once: the readable part is the request, the
// writable part takes the response.
void VirtioSnd::control(Memory &mem, Aplic &aplic)
{
	bool completed = false;
	uint16_t head;
	while (next_chain(mem, CONTROLQ, head)) {
		std::vector<uint8_t> in, out;
		std::vector<std::pair<uint64_t, uint32_t>> writable;
		uint16_t d = head;
		for (uint32_t guard = 0; guard <= queue_size(queues[CONTROLQ]); guard++) {
			const Desc desc = descriptor(mem, CONTROLQ, d);
			if (desc.flags & DESC_F_WRITE) {
				writable.push_back({desc.addr, desc.len});
			} else if (in.size() + desc.len <= 4096) {
				const size_t at = in.size();
				in.resize(at + desc.len);
				mem.read_bytes(desc.addr, in.data() + at, desc.len);
			}
			if (!(desc.flags & DESC_F_NEXT)) break;
			d = desc.next;
		}
		put32(out, 0, request(in, out, mem, aplic));
		uint32_t written = 0;
		for (const auto &[addr, len] : writable) {
			const uint32_t n = (uint32_t)std::min<size_t>(len, out.size() - written);
			if (n) mem.write_bytes(addr, out.data() + written, n);
			written += n;
		}
		complete(mem, CONTROLQ, head, written);
		completed = true;
	}
	if (completed) interrupt(aplic);
}

// Returns the status; anything the response carries after it goes into `out`
// from byte 4.
uint32_t VirtioSnd::request(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, Memory &mem, Aplic &aplic)
{
	const uint32_t code = le32(in, 0);
	if (code == R_PCM_INFO) {
		// virtio_snd_query_info: code, start_id, count, size.
		const uint32_t start = le32(in, 4), count = le32(in, 8), size = le32(in, 12);
		if (start >= STREAMS || count > STREAMS - start || size < PCM_INFO_SIZE) return S_BAD_MSG;
		for (uint32_t i = 0; i < count; i++) {
			const size_t at = 4 + (size_t)i * size;
			out.resize(at + size, 0);
			put32(out, at, 0);                                        // hda_fn_nid: one PCM device
			put32(out, at + 4, 0);                                    // no stream features
			put64(out, at + 8, (1ull << FMT_U8) | (1ull << FMT_S16)); // formats
			put64(out, at + 16, 0xFF);                                // 5512 Hz to 48 kHz
			out[at + 24] = (uint8_t)(start + i == OUTPUT ? 0 : 1);   // VIRTIO_SND_D_OUTPUT / INPUT
			out[at + 25] = 1;                                        // channels_min
			out[at + 26] = 2;                                        // channels_max
		}
		return S_OK;
	}
	const uint32_t id = le32(in, 4);
	if (code < R_PCM_SET_PARAMS || code > R_PCM_STOP) return S_NOT_SUPP;
	if (id >= STREAMS) return S_BAD_MSG;
	Stream &s = streams[id];
	switch (code) {
	case R_PCM_SET_PARAMS: {
		// virtio_snd_pcm_set_params: hdr (8), buffer_bytes, period_bytes,
		// features, channels, format, rate.
		if (in.size() < 24 || s.state == State::Running) return S_BAD_MSG;
		const uint8_t channels = in[20], format = in[21], rate = in[22];
		if (channels < 1 || channels > 2 || (format != FMT_U8 && format != FMT_S16)
		    || rate >= sizeof RATES / sizeof RATES[0] || le32(in, 16) != 0)
			return S_NOT_SUPP;
		s.buffer_bytes = le32(in, 8);
		s.period_bytes = le32(in, 12);
		s.channels = channels;
		s.format = format;
		s.rate = rate;
		s.state = State::Ready;
		return S_OK;
	}
	case R_PCM_PREPARE:
		if (s.state == State::Idle || s.state == State::Running) return S_BAD_MSG;
		s.state = State::Prepared;
		return S_OK;
	case R_PCM_START:
		if (s.state != State::Prepared && s.state != State::Stopped) return S_BAD_MSG;
		s.state = State::Running;
		return S_OK;
	case R_PCM_STOP:
		if (s.state != State::Running) return S_BAD_MSG;
		s.state = State::Stopped;
		return S_OK;
	case R_PCM_RELEASE:
		if (s.state == State::Idle || s.state == State::Running) return S_BAD_MSG;
		flush(id, mem, aplic);
		s.state = State::Ready;
		return S_OK;
	}
	return S_NOT_SUPP;
}

// A period posted to txq or rxq: virtio_snd_pcm_xfer (the stream id), then
// the samples or room for them, then the status the device writes back.
// Taken now, at the notify; returned when the host has played it or has
// recorded one, at an input point.
void VirtioSnd::take_io(unsigned q, Memory &mem)
{
	uint16_t head;
	while (next_chain(mem, q, head)) {
		Msg msg;
		msg.head = head;
		std::vector<std::pair<uint64_t, uint32_t>> writable;
		uint32_t readable = 0, stream_id = ~0u;
		uint8_t xfer[4] = {};
		uint16_t d = head;
		for (uint32_t guard = 0; guard <= queue_size(queues[q]); guard++) {
			const Desc desc = descriptor(mem, q, d);
			if (desc.flags & DESC_F_WRITE) {
				writable.push_back({desc.addr, desc.len});
			} else {
				// The first four readable bytes are the header; the rest,
				// on txq, are samples.
				uint32_t skip = 0;
				while (readable < 4 && skip < desc.len) {
					xfer[readable++] = mem.read8(desc.addr + skip);
					skip++;
				}
				if (q == TXQ && desc.len > skip && msg.data.size() + desc.len <= (1u << 20)) {
					const size_t at = msg.data.size();
					msg.data.resize(at + desc.len - skip);
					mem.read_bytes(desc.addr + skip, msg.data.data() + at, desc.len - skip);
				}
			}
			if (!(desc.flags & DESC_F_NEXT)) break;
			d = desc.next;
		}
		if (readable == 4) stream_id = (uint32_t)xfer[0] | xfer[1] << 8 | xfer[2] << 16 | (uint32_t)xfer[3] << 24;
		// The status is the last eight writable bytes; the writable bytes
		// before it, on rxq, are where samples go.
		uint32_t room = 0;
		for (const auto &w : writable) room += w.second;
		if (room < STATUS_SIZE) {
			complete(mem, q, head, 0);   // malformed: nowhere to answer
			continue;
		}
		uint32_t data_room = room - STATUS_SIZE, left = data_room;
		for (const auto &[addr, len] : writable) {
			const uint32_t n = std::min(len, left);
			if (n) msg.buffers.push_back({addr, n});
			left -= n;
			if (n < len) {
				msg.status_addr = addr + n;   // status starts inside this buffer
				break;
			}
		}
		msg.rx_bytes = data_room;
		const bool right_stream = stream_id == (q == TXQ ? OUTPUT : INPUT);
		if (!right_stream) {
			uint8_t status[STATUS_SIZE] = {};
			status[0] = (uint8_t)S_BAD_MSG; status[1] = (uint8_t)(S_BAD_MSG >> 8);
			mem.write_bytes(msg.status_addr, status, STATUS_SIZE);
			complete(mem, q, head, STATUS_SIZE);
			continue;
		}
		(q == TXQ ? tx : rx).push_back(std::move(msg));
	}
}

void VirtioSnd::finish(unsigned q, Msg &msg, uint32_t data_written, Memory &mem)
{
	uint8_t status[STATUS_SIZE] = {};
	status[0] = (uint8_t)S_OK; status[1] = (uint8_t)(S_OK >> 8);   // latency_bytes: 0
	mem.write_bytes(msg.status_addr, status, STATUS_SIZE);
	complete(mem, q, msg.head, data_written + STATUS_SIZE);
}

void VirtioSnd::finish_tx(Memory &mem, Aplic &aplic)
{
	if (tx.empty()) return;
	finish(TXQ, tx.front(), 0, mem);
	tx.pop_front();
	interrupt(aplic);
}

uint32_t VirtioSnd::next_rx_size() const
{
	return running(INPUT) && !rx.empty() ? rx.front().rx_bytes : 0;
}

void VirtioSnd::finish_rx(const uint8_t *data, Memory &mem, Aplic &aplic)
{
	if (rx.empty()) return;
	Msg &msg = rx.front();
	uint32_t at = 0;
	for (const auto &[addr, len] : msg.buffers) {
		mem.write_bytes(addr, data + at, len);
		at += len;
	}
	finish(RXQ, msg, msg.rx_bytes, mem);
	rx.pop_front();
	interrupt(aplic);
}

// RELEASE: whatever the stream still holds goes back, played or not. Input
// buffers return empty -- the driver counts only what was written.
void VirtioSnd::flush(unsigned id, Memory &mem, Aplic &aplic)
{
	std::deque<Msg> &queue = id == OUTPUT ? tx : rx;
	if (queue.empty()) return;
	for (Msg &msg : queue) finish(id == OUTPUT ? TXQ : RXQ, msg, 0, mem);
	queue.clear();
	interrupt(aplic);
}
