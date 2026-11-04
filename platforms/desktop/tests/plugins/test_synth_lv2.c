// An LV2 instrument for plugin_test, the same as test_synth_clap.c: one sine voice, a note at
// velocity/127 of the level, CC 7 sets the level (saved through the state interface). The
// "gain" control port scales the output, so control values and port state are tested too.

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SYNTH_URI "urn:meadowlark:test-synth"
#define LEVEL_URI SYNTH_URI "#level"

enum
{
    kPortMidi,
    kPortGain,
    kPortLeft,
    kPortRight
};

typedef struct
{
    const LV2_Atom_Sequence* midi;
    const float* gain;
    float* left;
    float* right;
    double sampleRate;
    double phase;
    double frequency;
    float amplitude;
    float level;
    LV2_URID midiEvent;
    LV2_URID levelKey;
    LV2_URID floatType;
} Synth;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double rate,
                              const char* bundlePath,
                              const LV2_Feature* const* features)
{
    (void)descriptor, (void)bundlePath;
    const LV2_URID_Map* map = NULL;
    for (int i = 0; features[i]; i++)
    {
        if (!strcmp(features[i]->URI, LV2_URID__map))
            map = features[i]->data;
    }
    if (!map)
        return NULL;
    Synth* synth = calloc(1, sizeof(Synth));
    synth->sampleRate = rate;
    synth->frequency = 440.0;
    synth->level = 0.5f;
    synth->midiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);
    synth->levelKey = map->map(map->handle, LEVEL_URI);
    synth->floatType = map->map(map->handle, LV2_ATOM__Float);
    return synth;
}

static void connectPort(LV2_Handle instance, uint32_t port, void* data)
{
    Synth* synth = instance;
    switch (port)
    {
        case kPortMidi: synth->midi = data; break;
        case kPortGain: synth->gain = data; break;
        case kPortLeft: synth->left = data; break;
        case kPortRight: synth->right = data; break;
    }
}

static void handleMidi(Synth* synth, const uint8_t* data)
{
    const int type = data[0] & 0xF0;
    if (type == 0x90 && data[2] > 0)
    {
        synth->frequency = 440.0 * pow(2.0, (data[1] - 69) / 12.0);
        synth->amplitude = synth->level * data[2] / 127.0f;
    }
    else if (type == 0x80 || type == 0x90)
        synth->amplitude = 0.0f;
    else if (type == 0xB0 && data[1] == 7)
        synth->level = data[2] / 127.0f;
}

static void render(Synth* synth, uint32_t from, uint32_t to)
{
    const float gain = *synth->gain;
    for (uint32_t frame = from; frame < to; frame++)
    {
        const float sample = gain * synth->amplitude * (float)sin(synth->phase);
        synth->phase += 2.0 * M_PI * synth->frequency / synth->sampleRate;
        if (synth->phase > 2.0 * M_PI)
            synth->phase -= 2.0 * M_PI;
        synth->left[frame] = sample;
        synth->right[frame] = sample;
    }
}

static void run(LV2_Handle instance, uint32_t frames)
{
    Synth* synth = instance;
    uint32_t done = 0;
    LV2_ATOM_SEQUENCE_FOREACH(synth->midi, event)
    {
        uint32_t at = (uint32_t)event->time.frames;
        if (at > frames)
            at = frames;
        render(synth, done, at);
        done = at;
        if (event->body.type == synth->midiEvent && event->body.size >= 1)
            handleMidi(synth, (const uint8_t*)(event + 1));
    }
    render(synth, done, frames);
}

static void cleanup(LV2_Handle instance)
{
    free(instance);
}

static LV2_State_Status save(LV2_Handle instance,
                             LV2_State_Store_Function store,
                             LV2_State_Handle handle,
                             uint32_t flags,
                             const LV2_Feature* const* features)
{
    (void)flags, (void)features;
    Synth* synth = instance;
    return store(handle, synth->levelKey, &synth->level, sizeof(float), synth->floatType,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static LV2_State_Status restore(LV2_Handle instance,
                                LV2_State_Retrieve_Function retrieve,
                                LV2_State_Handle handle,
                                uint32_t flags,
                                const LV2_Feature* const* features)
{
    (void)flags, (void)features;
    Synth* synth = instance;
    size_t size = 0;
    uint32_t type = 0;
    uint32_t valueFlags = 0;
    const void* value = retrieve(handle, synth->levelKey, &size, &type, &valueFlags);
    if (value && size == sizeof(float) && type == synth->floatType)
        synth->level = *(const float*)value;
    return LV2_STATE_SUCCESS;
}

static const void* extensionData(const char* uri)
{
    static const LV2_State_Interface state = { save, restore };
    return !strcmp(uri, LV2_STATE__interface) ? &state : NULL;
}

static const LV2_Descriptor kDescriptor = { SYNTH_URI, instantiate, connectPort, NULL, run, NULL, cleanup, extensionData };

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &kDescriptor : NULL;
}
