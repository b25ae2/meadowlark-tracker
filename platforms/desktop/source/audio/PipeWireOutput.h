#pragma once

// Native PipeWire playback stream: 44.1 kHz stereo S16, the engine's own format. PipeWire
// resamples to the graph rate. The node asks for a quantum of about its buffer size, so the
// sound server's latency matches the renderer's.

#include "AudioRenderer.h"

struct pw_thread_loop;
struct pw_stream;

class PipeWireOutput : public AudioOutput
{
  public:
    ~PipeWireOutput() override { stop(); }
    const char* name() const override { return "pipewire"; }
    bool start(AudioRenderer& renderer) override;
    void stop() override;

  private:
    static void onProcess(void* data);

    AudioRenderer* renderer = nullptr;
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    bool initialized = false;
};
