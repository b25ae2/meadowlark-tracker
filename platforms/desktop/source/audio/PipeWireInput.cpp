#include "PipeWireInput.h"

#include "AudioRenderer.h"

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
        std::fprintf(stderr, "meadowlark: pipewire capture error: %s\n", error ? error : "?");
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

bool PipeWireInput::start(AudioCaptureBuffer& captureBuffer)
{
    stop();
    buffer = &captureBuffer;

    pw_init(nullptr, nullptr);
    initialized = true;

    loop = pw_thread_loop_new("meadowlark-capture", nullptr);
    if (!loop)
    {
        std::fprintf(stderr, "meadowlark: pipewire: cannot create capture thread loop\n");
        stop();
        return false;
    }

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                             PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "Music",
                                             PW_KEY_APP_NAME, "Meadowlark",
                                             PW_KEY_NODE_NAME, "meadowlark-input",
                                             PW_KEY_NODE_DESCRIPTION, "Meadowlark input",
                                             nullptr);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", 256, AudioRenderer::kSampleRate);
    if (captureSink)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    static const pw_stream_events events = makeEvents(onProcess);
    stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "Meadowlark input", props, &events, this);
    if (!stream)
    {
        std::fprintf(stderr, "meadowlark: pipewire: cannot create capture stream\n");
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
    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0 ||
        pw_thread_loop_start(loop) < 0)
    {
        std::fprintf(stderr, "meadowlark: pipewire: cannot start capture (is the PipeWire server running?)\n");
        stop();
        return false;
    }

    for (int i = 0; i < 100; i++)
    {
        pw_thread_loop_lock(loop);
        const char* error = nullptr;
        const pw_stream_state state = pw_stream_get_state(stream, &error);
        pw_thread_loop_unlock(loop);
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
        {
            std::fprintf(stderr, "meadowlark: pipewire capture: %s\n", error ? error : "stream failed");
            stop();
            return false;
        }
        if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
            return true;
        struct timespec delay = { 0, 20 * 1000 * 1000 };
        nanosleep(&delay, nullptr);
    }
    std::fprintf(stderr, "meadowlark: pipewire capture: no answer from the server\n");
    stop();
    return false;
}

void PipeWireInput::stop()
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

void PipeWireInput::onProcess(void* data)
{
    auto* self = static_cast<PipeWireInput*>(data);
    pw_buffer* pwBuffer = pw_stream_dequeue_buffer(self->stream);
    if (!pwBuffer)
        return;

    const spa_data& in = pwBuffer->buffer->datas[0];
    if (in.data && in.chunk)
    {
        const uint32_t offset = std::min(in.chunk->offset, in.maxsize);
        const uint32_t size = std::min(in.chunk->size, in.maxsize - offset);
        self->buffer->write(reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(in.data) + offset),
                            size / kStride);
    }
    pw_stream_queue_buffer(self->stream, pwBuffer);
}
