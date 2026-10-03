#include "host_audio.hpp"
#include <SDL2/SDL.h>
#include <iostream>

HostAudio::~HostAudio()
{
	close(output);
	close(input);
	if (initialised) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool HostAudio::open(Device &dev, bool capture, uint32_t hz, uint8_t channels, uint8_t format)
{
	if (dev.id && dev.hz == hz && dev.channels == channels && dev.format == format) return true;
	if (!dev.id && dev.failed && dev.hz == hz && dev.channels == channels && dev.format == format) return false;
	close(dev);
	dev.hz = hz;
	dev.channels = channels;
	dev.format = format;
	dev.failed = false;
	if (!initialised) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
			std::cout << "sound: no host audio (" << SDL_GetError() << ")" << std::endl;
			dev.failed = true;
			return false;
		}
		initialised = true;
	}
	SDL_AudioSpec want{};
	want.freq = (int)hz;
	want.format = format == 4 ? AUDIO_U8 : AUDIO_S16LSB;
	want.channels = channels;
	want.samples = 1024;
	// No callback: samples are queued (play) or dequeued (record).
	SDL_AudioSpec have{};
	dev.id = SDL_OpenAudioDevice(nullptr, capture ? 1 : 0, &want, &have, 0);
	if (!dev.id) {
		std::cout << "sound: cannot open the host's default " << (capture ? "recording" : "playback")
		          << " device (" << SDL_GetError() << ")" << std::endl;
		dev.failed = true;
		return false;
	}
	SDL_PauseAudioDevice(dev.id, 0);
	std::cout << "sound: " << (capture ? "recording" : "playing") << " at " << hz << " Hz, " << (int)channels
	          << (channels == 1 ? " channel" : " channels") << ", through the host's default device" << std::endl;
	return true;
}

void HostAudio::close(Device &dev)
{
	if (dev.id) SDL_CloseAudioDevice(dev.id);
	dev.id = 0;
}

bool HostAudio::open_output(uint32_t hz, uint8_t channels, uint8_t format)
{
	return open(output, false, hz, channels, format);
}

uint64_t HostAudio::play(const uint8_t *data, size_t len)
{
	if (output.id && len && SDL_QueueAudio(output.id, data, (Uint32)len) == 0) handed += len;
	return handed;
}

void HostAudio::stop_output()
{
	if (output.id) SDL_ClearQueuedAudio(output.id);
}

uint32_t HostAudio::queued() const
{
	return output.id ? SDL_GetQueuedAudioSize(output.id) : 0;
}

bool HostAudio::open_input(uint32_t hz, uint8_t channels, uint8_t format)
{
	return open(input, true, hz, channels, format);
}

void HostAudio::close_input()
{
	close(input);
	input.failed = false;
}

uint32_t HostAudio::available() const
{
	return input.id ? SDL_GetQueuedAudioSize(input.id) : 0;
}

size_t HostAudio::record(uint8_t *out, size_t len)
{
	return input.id ? SDL_DequeueAudio(input.id, out, (Uint32)len) : 0;
}
