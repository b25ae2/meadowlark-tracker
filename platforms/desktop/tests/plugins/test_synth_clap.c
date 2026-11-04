// A CLAP instrument for plugin_test: one sine voice. A note plays at velocity/127 of the
// level; CC 7 sets the level (value/127, saved as the plugin's state). Takes MIDI and CLAP
// note events. Output: one stereo port, the same sine on both sides.

#include <clap/clap.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    double sampleRate;
    double phase;
    double frequency;
    float amplitude;
    float level;
} Synth;

static const char* const kFeatures[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, NULL };

static const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "org.meadowlark.test-synth",
    "Meadowlark Test Synth",
    "Meadowlark",
    "",
    "",
    "",
    "1.0",
    "A sine for plugin_test",
    kFeatures,
};

static void noteOn(Synth* synth, int key, double velocity)
{
    synth->frequency = 440.0 * pow(2.0, (key - 69) / 12.0);
    synth->amplitude = (float)(synth->level * velocity);
}

static void handleEvent(Synth* synth, const clap_event_header_t* header)
{
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    if (header->type == CLAP_EVENT_NOTE_ON)
    {
        const clap_event_note_t* note = (const clap_event_note_t*)header;
        noteOn(synth, note->key, note->velocity);
    }
    else if (header->type == CLAP_EVENT_NOTE_OFF)
        synth->amplitude = 0.0f;
    else if (header->type == CLAP_EVENT_MIDI)
    {
        const clap_event_midi_t* midi = (const clap_event_midi_t*)header;
        const int type = midi->data[0] & 0xF0;
        if (type == 0x90 && midi->data[2] > 0)
            noteOn(synth, midi->data[1], midi->data[2] / 127.0);
        else if (type == 0x80 || type == 0x90)
            synth->amplitude = 0.0f;
        else if (type == 0xB0 && midi->data[1] == 7)
            synth->level = midi->data[2] / 127.0f;
    }
}

static bool init(const clap_plugin_t* plugin)
{
    (void)plugin;
    return true;
}

static void destroy(const clap_plugin_t* plugin)
{
    free(plugin->plugin_data);
}

static bool activate(const clap_plugin_t* plugin, double sampleRate, uint32_t minFrames, uint32_t maxFrames)
{
    (void)minFrames, (void)maxFrames;
    ((Synth*)plugin->plugin_data)->sampleRate = sampleRate;
    return true;
}

static void deactivate(const clap_plugin_t* plugin)
{
    (void)plugin;
}

static bool startProcessing(const clap_plugin_t* plugin)
{
    (void)plugin;
    return true;
}

static void stopProcessing(const clap_plugin_t* plugin)
{
    (void)plugin;
}

static void reset(const clap_plugin_t* plugin)
{
    ((Synth*)plugin->plugin_data)->amplitude = 0.0f;
}

static clap_process_status process(const clap_plugin_t* plugin, const clap_process_t* process)
{
    Synth* synth = plugin->plugin_data;
    const uint32_t eventCount = process->in_events->size(process->in_events);
    uint32_t nextEvent = 0;
    float* left = process->audio_outputs[0].data32[0];
    float* right = process->audio_outputs[0].data32[1];
    for (uint32_t frame = 0; frame < process->frames_count; frame++)
    {
        while (nextEvent < eventCount)
        {
            const clap_event_header_t* header = process->in_events->get(process->in_events, nextEvent);
            if (header->time > frame)
                break;
            handleEvent(synth, header);
            nextEvent++;
        }
        const float sample = synth->amplitude * (float)sin(synth->phase);
        synth->phase += 2.0 * M_PI * synth->frequency / synth->sampleRate;
        if (synth->phase > 2.0 * M_PI)
            synth->phase -= 2.0 * M_PI;
        left[frame] = sample;
        right[frame] = sample;
    }
    for (; nextEvent < eventCount; nextEvent++)
        handleEvent(synth, process->in_events->get(process->in_events, nextEvent));
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t notePortCount(const clap_plugin_t* plugin, bool isInput)
{
    (void)plugin;
    return isInput ? 1 : 0;
}

static bool notePortGet(const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_note_port_info_t* info)
{
    (void)plugin;
    if (!isInput || index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    strcpy(info->name, "notes");
    return true;
}

static const clap_plugin_note_ports_t kNotePorts = { notePortCount, notePortGet };

static uint32_t audioPortCount(const clap_plugin_t* plugin, bool isInput)
{
    (void)plugin;
    return isInput ? 0 : 1;
}

static bool audioPortGet(const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
    (void)plugin;
    if (isInput || index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    strcpy(info->name, "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t kAudioPorts = { audioPortCount, audioPortGet };

static bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream)
{
    const float level = ((Synth*)plugin->plugin_data)->level;
    return stream->write(stream, &level, sizeof(level)) == (int64_t)sizeof(level);
}

static bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream)
{
    float level;
    if (stream->read(stream, &level, sizeof(level)) != (int64_t)sizeof(level))
        return false;
    ((Synth*)plugin->plugin_data)->level = level;
    return true;
}

static const clap_plugin_state_t kState = { stateSave, stateLoad };

static const void* getExtension(const clap_plugin_t* plugin, const char* id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &kNotePorts;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kAudioPorts;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &kState;
    return NULL;
}

static void onMainThread(const clap_plugin_t* plugin)
{
    (void)plugin;
}

static uint32_t factoryCount(const clap_plugin_factory_t* factory)
{
    (void)factory;
    return 1;
}

static const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t* factory, uint32_t index)
{
    (void)factory;
    return index == 0 ? &kDescriptor : NULL;
}

static const clap_plugin_t* factoryCreate(const clap_plugin_factory_t* factory, const clap_host_t* host, const char* id)
{
    (void)factory;
    if (strcmp(id, kDescriptor.id) != 0)
        return NULL;
    Synth* synth = calloc(1, sizeof(Synth));
    synth->host = host;
    synth->sampleRate = 44100.0;
    synth->frequency = 440.0;
    synth->level = 0.5f;
    synth->plugin.desc = &kDescriptor;
    synth->plugin.plugin_data = synth;
    synth->plugin.init = init;
    synth->plugin.destroy = destroy;
    synth->plugin.activate = activate;
    synth->plugin.deactivate = deactivate;
    synth->plugin.start_processing = startProcessing;
    synth->plugin.stop_processing = stopProcessing;
    synth->plugin.reset = reset;
    synth->plugin.process = process;
    synth->plugin.get_extension = getExtension;
    synth->plugin.on_main_thread = onMainThread;
    return &synth->plugin;
}

static const clap_plugin_factory_t kFactory = { factoryCount, factoryDescriptor, factoryCreate };

static bool entryInit(const char* path)
{
    (void)path;
    return true;
}

static void entryDeinit(void) {}

static const void* entryGetFactory(const char* id)
{
    return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory };
