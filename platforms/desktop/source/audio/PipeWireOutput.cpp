#include "PipeWireOutput.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace
{
constexpr uint32_t kChannels = 2;
constexpr uint32_t kStride = kChannels * sizeof(int16_t);

void onStateChanged(void*, pw_stream_state, pw_stream_state state, const char* error)
{
    if (state == PW_STREAM_STATE_ERROR)
        std::fprintf(stderr, "meadowlark: pipewire stream error: %s\n", error ? error : "?");
}

pw_stream_events makeEvents(void (*process)(void*))
{
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.process = process;
    events.state_changed = onStateChanged;
    return events;
}
} // namespace

bool PipeWireOutput::start(AudioRenderer& audioRenderer)
{
    stop();
    renderer = &audioRenderer;

    pw_init(nullptr, nullptr);
    initialized = true;

    loop = pw_thread_loop_new("meadowlark-pipewire", nullptr);
    if (!loop)
    {
        std::fprintf(stderr, "meadowlark: pipewire: cannot create thread loop\n");
        stop();
        return false;
    }

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                             PW_KEY_MEDIA_CATEGORY, "Playback",
                                             PW_KEY_MEDIA_ROLE, "Music",
                                             PW_KEY_APP_NAME, "Meadowlark",
                                             PW_KEY_NODE_NAME, "meadowlark",
                                             PW_KEY_NODE_DESCRIPTION, "Meadowlark",
                                             nullptr);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", renderer->bufferFrames(), AudioRenderer::kSampleRate);

    static const pw_stream_events events = makeEvents(onProcess);
    stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "Meadowlark", props, &events, this);
    if (!stream)
    {
        std::fprintf(stderr, "meadowlark: pipewire: cannot create stream\n");
        stop();
        return false;
    }

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = AudioRenderer::kSampleRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;

    uint8_t podBuffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    const spa_pod* params[1] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };

    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                    PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0)
    {
        std::fprintf(stderr, "meadowlark: pipewire: cannot connect stream (is the PipeWire server running?)\n");
        stop();
        return false;
    }

    if (pw_thread_loop_start(loop) < 0)
    {
        std::fprintf(stderr, "meadowlark: pipewire: cannot start thread loop\n");
        stop();
        return false;
    }

    // Wait for the server to accept or refuse the stream, so a missing server can fall back
    // to another backend instead of playing into nothing.
    for (int i = 0; i < 100; i++)
    {
        pw_thread_loop_lock(loop);
        const char* error = nullptr;
        const pw_stream_state state = pw_stream_get_state(stream, &error);
        pw_thread_loop_unlock(loop);
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
        {
            std::fprintf(stderr, "meadowlark: pipewire: %s\n", error ? error : "stream failed");
            stop();
            return false;
        }
        if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
            return true;
        struct timespec delay = { 0, 20 * 1000 * 1000 };
        nanosleep(&delay, nullptr);
    }
    std::fprintf(stderr, "meadowlark: pipewire: no answer from the server\n");
    stop();
    return false;
}

void PipeWireOutput::stop()
{
    if (loop)
        pw_thread_loop_stop(loop);
    if (stream)
    {
        pw_stream_destroy(stream);
        stream = nullptr;
    }
    if (loop)
    {
        pw_thread_loop_destroy(loop);
        loop = nullptr;
    }
    if (initialized)
    {
        pw_deinit();
        initialized = false;
    }
}

void PipeWireOutput::onProcess(void* data)
{
    auto* self = static_cast<PipeWireOutput*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
    if (!buffer)
        return;

    spa_data& out = buffer->buffer->datas[0];
    if (out.data)
    {
        uint32_t frames = out.maxsize / kStride;
        if (buffer->requested)
            frames = std::min<uint32_t>(frames, static_cast<uint32_t>(buffer->requested));
        self->renderer->read(static_cast<int16_t*>(out.data), frames);
        out.chunk->offset = 0;
        out.chunk->stride = kStride;
        out.chunk->size = frames * kStride;
    }
    pw_stream_queue_buffer(self->stream, buffer);
}
