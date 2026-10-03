#pragma once
// virtio-snd over the MMIO transport: a sound card with one output stream
// and one input stream, which DoomSystem connects to the host's default
// playback and recording devices.
//
// Four queues: controlq (0) carries the driver's requests -- what the streams
// can do, set their format, prepare, start, stop, release -- and each is
// answered inside the notify that posted it. eventq (1) is for jack and
// period events, of which there are none. txq (2) carries PCM to play and
// rxq (3) empty buffers to record into, one period each.
//
// Those last two are the part that is not the guest's own. When a period has
// been played, and when a recorded one is ready, is decided by the host's
// sound hardware -- outside the machine, as typing and network frames are. So
// a period finishes only through finish_tx() and finish_rx(), which
// DoomSystem calls at the input points, where -record logs it and -replay
// brings it back, and the guest sees the same thing on every replay.
//
// The device offers what Linux's ALSA plug layer can convert anything to:
// 16-bit signed samples or 8-bit unsigned, one or two channels, at the
// common rates from 8 to 48 kHz. Jacks, channel maps and controls are not
// offered, so the driver asks nothing about them.
#include "virtio_mmio.hpp"
#include <cstdint>
#include <deque>
#include <vector>

class VirtioSnd final : public VirtioMmio {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr uint32_t DEVICE_ID = 25;   // sound card
	static constexpr unsigned STREAMS = 2;
	static constexpr unsigned OUTPUT = 0, INPUT = 1;   // stream ids
	// Formats and rates, as the spec numbers them.
	static constexpr uint8_t FMT_U8 = 4, FMT_S16 = 5;
	static constexpr uint32_t RATES[] = {5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000};

	enum class State : uint8_t { Idle, Ready, Prepared, Running, Stopped };
	struct Stream {
		State state = State::Idle;
		uint8_t channels = 2, format = FMT_S16, rate = 7;   // 7: 48 kHz
		uint32_t buffer_bytes = 0, period_bytes = 0;
		uint32_t hz() const { return rate < sizeof RATES / sizeof RATES[0] ? RATES[rate] : 48000; }
		uint32_t frame_bytes() const { return channels * (format == FMT_S16 ? 2u : 1u); }
	};

	explicit VirtioSnd(uint32_t irq) : VirtioMmio(DEVICE_ID, VENDOR_DOOM, irq, 4) {}

	// A card is only there when the machine is given sound (-snd).
	void set_enabled(bool on) { enabled = on; }
	bool is_enabled() const { return enabled; }

	const Stream &stream(unsigned id) const { return streams[id]; }
	bool running(unsigned id) const { return streams[id].state == State::Running; }

	// The periods waiting to be played, oldest first, while the output
	// runs. Each carries a mark for the host's side: where in the host's
	// stream of played bytes the period ends, 0 until it is handed over.
	// The mark is not machine state -- the guest never sees it, and a
	// snapshot does not keep it.
	size_t tx_count() const { return running(OUTPUT) ? tx.size() : 0; }
	const std::vector<uint8_t> &tx_data(size_t i) const { return tx[i].data; }
	uint64_t &tx_mark(size_t i) { return tx[i].host_end; }
	// The oldest has been played: return it to the guest.
	void finish_tx(Memory &mem, Aplic &aplic);
	// How many bytes the oldest waiting record buffer wants, if the input is
	// running; 0 when there is none.
	uint32_t next_rx_size() const;
	// Fill it -- `data` is exactly next_rx_size() bytes -- and return it.
	void finish_rx(const uint8_t *data, Memory &mem, Aplic &aplic);

private:
	struct Msg {
		uint16_t head;
		uint64_t status_addr;                 // virtio_snd_pcm_status, 8 bytes
		std::vector<uint8_t> data;            // tx: the samples
		std::vector<std::pair<uint64_t, uint32_t>> buffers;   // rx: where samples go
		uint32_t rx_bytes = 0;
		uint64_t host_end = 0;                // see tx_mark
	};

	bool present() const override { return enabled; }
	// struct virtio_snd_config: jacks, streams, chmaps.
	uint8_t config_read8(uint64_t offset) const override
	{
		return offset >= 4 && offset < 8 ? (uint8_t)(STREAMS >> (8 * (offset - 4))) : 0;
	}
	void notify(unsigned q, Memory &mem, Aplic &aplic) override;
	void reset() override;

	void control(Memory &mem, Aplic &aplic);
	uint32_t request(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, Memory &mem, Aplic &aplic);
	void take_io(unsigned q, Memory &mem);
	// Every outstanding period of `id`, returned: RELEASE requires it.
	void flush(unsigned id, Memory &mem, Aplic &aplic);
	void finish(unsigned q, Msg &msg, uint32_t data_written, Memory &mem);

	bool enabled = false;
	Stream streams[STREAMS];
	std::deque<Msg> tx, rx;
};
