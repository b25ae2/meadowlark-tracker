#include "SdlAudioInput.h"

#include "AudioRenderer.h"

#include <cstdio>

namespace
{
constexpr int kStride = 2 * sizeof(int16_t);
constexpr int kChunkFrames = 1024;
} // namespace

bool SdlAudioInput::start(AudioCaptureBuffer& captureBuffer)
{
    stop();
    buffer = &captureBuffer;
    scratch.assign(kChunkFrames * 2, 0);

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        std::fprintf(stderr, "meadowlark: sdl capture: %s\n", SDL_GetError());
        return false;
    }
    subsystemStarted = true;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = AudioRenderer::kSampleRate;
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, onData, this);
    if (!stream)
    {
        std::fprintf(stderr, "meadowlark: sdl capture: cannot open the recording device: %s\n", SDL_GetError());
        stop();
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return true;
}

void SdlAudioInput::stop()
{
    if (stream)
    {
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
    if (subsystemStarted)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        subsystemStarted = false;
    }
}

void SDLCALL SdlAudioInput::onData(void* userdata, SDL_AudioStream* stream, int, int)
{
    auto* self = static_cast<SdlAudioInput*>(userdata);
    for (;;)
    {
        const int bytes = SDL_GetAudioStreamData(stream, self->scratch.data(), kChunkFrames * kStride);
        if (bytes <= 0)
            break;
        self->buffer->write(self->scratch.data(), static_cast<size_t>(bytes / kStride));
    }
}
