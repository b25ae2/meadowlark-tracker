#pragma once

// SDL3 capture from the default recording device, converted to 44.1 kHz S16 stereo by the
// SDL audio stream. Used on macOS (Core Audio; the first use asks for microphone
// permission) and as the fallback elsewhere.

#include "AudioCapture.h"

#include <SDL3/SDL.h>

#include <vector>

class SdlAudioInput : public AudioInput
{
  public:
    ~SdlAudioInput() override { stop(); }
    const char* name() const override { return "sdl"; }
    bool start(AudioCaptureBuffer& buffer) override;
    void stop() override;

  private:
    static void SDLCALL onData(void* userdata, SDL_AudioStream* stream, int additional, int total);

    AudioCaptureBuffer* buffer = nullptr;
    SDL_AudioStream* stream = nullptr;
    bool subsystemStarted = false;
    std::vector<int16_t> scratch;
};
