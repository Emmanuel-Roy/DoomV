#pragma once
// The host's sound: its default playback and recording devices, through SDL's
// queueing audio API, so nothing runs on a callback thread of ours. Behind
// the guest's virtio-snd card (src/virtio/virtio_snd.cpp); DoomSystem moves
// the samples between the two at the input points.
//
// Each direction opens with the format the guest asked for and SDL converts
// to whatever the device takes. Recording opens only while the guest records,
// so the microphone is not held -- or shown as in use -- otherwise.
#include <cstddef>
#include <cstdint>

class HostAudio {
public:
	~HostAudio();

	// `format` is virtio-snd's: 4 for unsigned 8-bit, 5 for signed 16-bit.
	// Reopens when the format changes; false when the host has no device.
	bool open_output(uint32_t hz, uint8_t channels, uint8_t format);
	// Returns where the data ends in the stream of bytes handed over, which
	// counts up across every play() for as long as this lives.
	uint64_t play(const uint8_t *data, size_t len);
	// Bytes handed to the device and not yet played.
	uint32_t queued() const;
	// How far through that stream the device has played. Whatever was
	// queued when the device was closed or reopened counts as played.
	uint64_t played() const { return handed - queued(); }
	// Drop what is queued and not yet played.
	void stop_output();

	bool open_input(uint32_t hz, uint8_t channels, uint8_t format);
	void close_input();
	// Bytes recorded and not yet taken.
	uint32_t available() const;
	size_t record(uint8_t *out, size_t len);

private:
	struct Device {
		uint32_t id = 0;
		uint32_t hz = 0;
		uint8_t channels = 0, format = 0;
		bool failed = false;   // tried with these settings and could not open
	};
	bool open(Device &dev, bool capture, uint32_t hz, uint8_t channels, uint8_t format);
	void close(Device &dev);

	bool initialised = false;
	uint64_t handed = 0;
	Device output, input;
};
