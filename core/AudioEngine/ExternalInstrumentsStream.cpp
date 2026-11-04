#include "ExternalInstrumentsStream.h"

void ExternalInstrumentsStream::update(void)
{
    if (instruments == nullptr)
    {
        return;
    }

    audio_block_t* blocks[ExternalInstruments::kBusCount] = {};
    int16_t* buses[ExternalInstruments::kBusCount];
    for (int bus = 0; bus < ExternalInstruments::kBusCount; bus++)
    {
        blocks[bus] = allocate();
        if (blocks[bus] == nullptr)
        {
            for (int i = 0; i < bus; i++)
            {
                release(blocks[i]);
            }
            return;
        }
        buses[bus] = blocks[bus]->data;
    }

    instruments->render(buses, AUDIO_BLOCK_SAMPLES);

    for (int bus = 0; bus < ExternalInstruments::kBusCount; bus++)
    {
        transmit(blocks[bus], bus);
        release(blocks[bus]);
    }
}
