#pragma once

// SDL3 audio: the default output device through an SDL audio stream, which converts the
// engine's 44.1 kHz S16 stereo to whatever the device runs at. Used on macOS (Core Audio)
// and as the fallback elsewhere.

#include "AudioRenderer.h"

#include <SDL3/SDL.h>

#include <vector>

class SdlAudioOutput : public AudioOutput
{
  public:
    ~SdlAudioOutput() override { stop(); }
    const char* name() const override { return "sdl"; }
    bool start(AudioRenderer& renderer) override;
    void stop() override;

  private:
    static void SDLCALL onMoreData(void* userdata, SDL_AudioStream* stream, int additional, int total);

    AudioRenderer* renderer = nullptr;
    SDL_AudioStream* stream = nullptr;
    bool subsystemStarted = false;
    std::vector<int16_t> scratch;
};
