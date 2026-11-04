#ifndef AUDIOENGINE_EXTERNALINSTRUMENTSSTREAM_H_
#define AUDIOENGINE_EXTERNALINSTRUMENTSSTREAM_H_

#include "AudioStream.h"
#include "Platform.h"

// The audio of the platform's ExternalInstruments as an audio stream with one output per
// bus (dry, delay send and reverb send, each left and right). Silent without instruments.
class ExternalInstrumentsStream : public AudioStream
{
  public:
    ExternalInstrumentsStream() : AudioStream(0, nullptr) {}
    void setInstruments(ExternalInstruments* instruments) { this->instruments = instruments; }
    virtual void update(void) override;

  private:
    ExternalInstruments* instruments = nullptr;
};

#endif /* AUDIOENGINE_EXTERNALINSTRUMENTSSTREAM_H_ */
