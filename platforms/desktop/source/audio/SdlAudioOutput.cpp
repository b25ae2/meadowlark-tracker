#include "SdlAudioOutput.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
constexpr int kStride = 2 * sizeof(int16_t);
constexpr int kChunkFrames = 1024;
} // namespace

bool SdlAudioOutput::start(AudioRenderer& audioRenderer)
{
    stop();
    renderer = &audioRenderer;
    scratch.assign(kChunkFrames * 2, 0);

    // Ask the device for periods of about the renderer's buffer.
    const std::string frames = std::to_string(renderer->bufferFrames());
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames.c_str());

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        std::fprintf(stderr, "meadowlark: sdl audio: %s\n", SDL_GetError());
        return false;
    }
    subsystemStarted = true;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = AudioRenderer::kSampleRate;
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, onMoreData, this);
    if (!stream)
    {
        std::fprintf(stderr, "meadowlark: sdl audio: cannot open the output device: %s\n", SDL_GetError());
        stop();
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream);
    std::printf("meadowlark: sdl audio driver %s\n", SDL_GetCurrentAudioDriver());
    return true;
}

void SdlAudioOutput::stop()
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

void SDLCALL SdlAudioOutput::onMoreData(void* userdata, SDL_AudioStream* stream, int additional, int)
{
    auto* self = static_cast<SdlAudioOutput*>(userdata);
    int frames = additional / kStride;
    while (frames > 0)
    {
        const int chunk = std::min(frames, kChunkFrames);
        self->renderer->read(self->scratch.data(), static_cast<size_t>(chunk));
        SDL_PutAudioStreamData(stream, self->scratch.data(), chunk * kStride);
        frames -= chunk;
    }
}
