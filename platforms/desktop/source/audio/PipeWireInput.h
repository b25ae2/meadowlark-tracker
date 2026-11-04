#pragma once

// Native PipeWire capture stream: 44.1 kHz stereo S16 from the default source (PipeWire
// converts the rate and upmixes mono microphones). With captureSink it records the default
// output's monitor instead, which audio_test uses to loop the output back.

#include "AudioCapture.h"

struct pw_thread_loop;
struct pw_stream;

class PipeWireInput : public AudioInput
{
  public:
    explicit PipeWireInput(bool captureSink = false) : captureSink(captureSink) {}
    ~PipeWireInput() override { stop(); }
    const char* name() const override { return "pipewire"; }
    bool start(AudioCaptureBuffer& buffer) override;
    void stop() override;

  private:
    static void onProcess(void* data);

    bool captureSink;
    AudioCaptureBuffer* buffer = nullptr;
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    bool initialized = false;
};
