// Sound output check: runs AudioRenderer into a real backend for a few seconds and reports
// whether the device consumed audio at the engine's rate, and how many underruns there were.
// Plays silence unless --tone is given (a quiet 440 Hz sine).
//
//   audio_test [pipewire|sdl] [--seconds n] [--buffer frames] [--tone]

#include "audio/AudioRenderer.h"
#include "audio/SdlAudioOutput.h"
#ifdef MEADOWLARK_HAVE_PIPEWIRE
#include "audio/PipeWireOutput.h"
#endif

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
#ifdef MEADOWLARK_HAVE_PIPEWIRE
    std::string backend = "pipewire";
#else
    std::string backend = "sdl";
#endif
    double seconds = 3.0;
    int buffer = 512;
    bool tone = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--buffer") == 0 && i + 1 < argc)
            buffer = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--tone") == 0)
            tone = true;
        else
            backend = argv[i];
    }

    std::unique_ptr<AudioOutput> output;
#ifdef MEADOWLARK_HAVE_PIPEWIRE
    if (backend == "pipewire")
        output = std::make_unique<PipeWireOutput>();
#endif
    if (backend == "sdl")
        output = std::make_unique<SdlAudioOutput>();
    if (!output)
    {
        std::fprintf(stderr, "audio_test: unknown or unavailable backend %s\n", backend.c_str());
        return 2;
    }

    double phase = 0.0;
    AudioRenderer renderer;
    renderer.start(
      [&](int16_t* left, int16_t* right, int frames) {
          for (int i = 0; i < frames; i++)
          {
              const int16_t value = tone ? static_cast<int16_t>(3000.0 * std::sin(phase)) : 0;
              phase += 2.0 * M_PI * 440.0 / AudioRenderer::kSampleRate;
              left[i] = right[i] = value;
          }
      },
      buffer);
    const uint64_t prefilled = renderer.blocksRendered();

    if (!output->start(renderer))
        return 1;

    // Let the stream settle before measuring.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uint64_t startBlocks = renderer.blocksRendered();
    const uint64_t startUnderruns = renderer.underruns();
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint64_t blocks = renderer.blocksRendered() - startBlocks;
    const uint64_t underruns = renderer.underruns() - startUnderruns;

    output->stop();
    renderer.stop();
    SDL_Quit();

    const double rate = blocks * AudioRenderer::kBlockFrames / elapsed;
    std::printf("audio_test: %s, buffer %d frames (asked for %d, %llu blocks prefilled)\n",
                output->name(),
                renderer.bufferFrames(),
                buffer,
                static_cast<unsigned long long>(prefilled));
    std::printf("  largest device request: %zu frames\n", renderer.largestRead());
    std::printf("  consumed %.0f frames/s over %.1f s (expected %d)\n", rate, elapsed, AudioRenderer::kSampleRate);
    std::printf("  underruns after start-up: %llu\n", static_cast<unsigned long long>(underruns));

    // Within 2 %: the device clock and the measurement window don't line up exactly.
    const bool rateOk = std::fabs(rate - AudioRenderer::kSampleRate) < AudioRenderer::kSampleRate * 0.02;
    if (!rateOk || underruns != 0)
    {
        std::printf("audio_test: FAILED\n");
        return 1;
    }
    std::printf("audio_test: ok\n");
    return 0;
}
