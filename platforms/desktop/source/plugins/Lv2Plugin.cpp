// LV2 hosting through lilv: one instance at the engine's rate with the features synths
// commonly need (URID map, options, bounded block length, worker, log), MIDI into the
// first atom input that takes MIDI, and state as a Turtle string through lilv_state.
//
// Editors are native UIs loaded directly: X11UI on Linux, CocoaUI on macOS, embedded in
// the host's window (UIs for GTK or Qt would need suil and are not offered). Control values
// and atom messages travel between the plugin and its UI through the main loop.

#include "plugins/Plugin.h"

#include "AudioLock.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>

namespace plugins
{
namespace
{
// --- URIDs, shared by all instances -----------------------------------------------------
class UridMap
{
  public:
    LV2_URID map(const char* uri)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = ids.find(uri);
        if (it != ids.end())
            return it->second;
        uris.push_back(uri);
        const LV2_URID id = static_cast<LV2_URID>(uris.size());
        ids[uris.back()] = id;
        return id;
    }
    const char* unmap(LV2_URID id)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return id >= 1 && id <= uris.size() ? uris[id - 1].c_str() : nullptr;
    }

    LV2_URID_Map mapFeature{ this, [](LV2_URID_Map_Handle h, const char* uri) { return static_cast<UridMap*>(h)->map(uri); } };
    LV2_URID_Unmap unmapFeature{ this, [](LV2_URID_Unmap_Handle h, LV2_URID id) { return static_cast<UridMap*>(h)->unmap(id); } };

  private:
    std::mutex mutex;
    std::map<std::string, LV2_URID> ids;
    std::vector<std::string> uris;
};

UridMap& urids()
{
    static UridMap map;
    return map;
}

// The lilv world, loaded once.
LilvWorld* world()
{
    static LilvWorld* instance = [] {
        LilvWorld* w = lilv_world_new();
        lilv_world_load_all(w);
        return w;
    }();
    return instance;
}

LilvNode* node(const char* uri)
{
    return lilv_new_uri(world(), uri);
}

int logPrintf(LV2_Log_Handle, LV2_URID type, const char* format, va_list args)
{
    if (type == urids().map(LV2_LOG__Error) || type == urids().map(LV2_LOG__Warning))
    {
        std::fputs("meadowlark: lv2 plugin: ", stderr);
        return std::vfprintf(stderr, format, args);
    }
    return 0;
}
int logPrint(LV2_Log_Handle handle, LV2_URID type, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = logPrintf(handle, type, format, args);
    va_end(args);
    return result;
}
LV2_Log_Log logFeature{ nullptr, logPrint, logPrintf };

// A byte queue of length-prefixed messages between threads. The audio side never waits
// for the lock: it tries, and leaves the messages for the next block when it can't.
class MessageQueue
{
  public:
    void push(uint32_t tag, const void* data, uint32_t size)
    {
        std::lock_guard<std::mutex> lock(mutex);
        append(tag, data, size);
    }
    bool tryPush(uint32_t tag, const void* data, uint32_t size)
    {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || bytes.size() > kLimit)
            return false;
        append(tag, data, size);
        return true;
    }
    // Moves everything out; tryTake gives up instead of waiting.
    bool take(std::vector<uint8_t>& out, bool wait)
    {
        std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
        if (wait)
            lock.lock();
        else if (!lock.try_lock())
            return false;
        out.swap(bytes);
        bytes.clear();
        return !out.empty();
    }
    // Walks messages taken with take().
    template <typename F>
    static void each(const std::vector<uint8_t>& data, F&& f)
    {
        for (size_t at = 0; at + 8 <= data.size();)
        {
            uint32_t tag;
            uint32_t size;
            std::memcpy(&tag, &data[at], 4);
            std::memcpy(&size, &data[at + 4], 4);
            f(tag, &data[at + 8], size);
            at += 8 + ((size + 7) & ~7u);
        }
    }

  private:
    static constexpr size_t kLimit = 1 << 20;
    void append(uint32_t tag, const void* data, uint32_t size)
    {
        const size_t at = bytes.size();
        bytes.resize(at + 8 + ((size + 7) & ~7u));
        std::memcpy(&bytes[at], &tag, 4);
        std::memcpy(&bytes[at + 4], &size, 4);
        std::memcpy(&bytes[at + 8], data, size);
    }
    std::mutex mutex;
    std::vector<uint8_t> bytes;
};

class Lv2Plugin : public Plugin
{
  public:
    ~Lv2Plugin() override
    {
        closeEditor();
        if (instance)
        {
            lilv_instance_deactivate(instance);
            lilv_instance_free(instance);
        }
    }

    bool create(const std::string& uri, std::string& error)
    {
        LilvNode* uriNode = lilv_new_uri(world(), uri.c_str());
        plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world()), uriNode);
        lilv_node_free(uriNode);
        if (!plugin)
        {
            error = "no LV2 plugin " + uri + " (is it in LV2_PATH?)";
            return false;
        }
        pluginUri = uri;
        LilvNode* nameNode = lilv_plugin_get_name(plugin);
        pluginName = nameNode ? lilv_node_as_string(nameNode) : uri;
        lilv_node_free(nameNode);

        setUpFeatures();
        if (!checkRequiredFeatures(error))
            return false;
        if (!setUpPorts(error))
            return false;

        instance = lilv_plugin_instantiate(plugin, kSampleRate, features.data());
        if (!instance)
        {
            error = "cannot instantiate " + pluginName;
            return false;
        }
        const LV2_Descriptor* descriptor = lilv_instance_get_descriptor(instance);
        if (descriptor->extension_data)
            worker = static_cast<const LV2_Worker_Interface*>(descriptor->extension_data(LV2_WORKER__interface));
        for (size_t i = 0; i < ports.size(); i++)
            lilv_instance_connect_port(instance, static_cast<uint32_t>(i), ports[i].buffer());
        lilv_instance_activate(instance);
        return true;
    }

    std::string name() const override { return pluginName; }
    std::string id() const override { return pluginUri; }

    void process(const MidiEvent* events, size_t eventCount, float* left, float* right, uint32_t frames) override
    {
        // Values from the UI and state restores.
        for (Port& port : ports)
        {
            if (port.kind == Port::kControl && port.input)
                port.control = port.hostValue.load(std::memory_order_relaxed);
        }

        // Atom inputs: MIDI into the MIDI port, the UI's messages into theirs.
        std::vector<uint8_t>& fromUi = audioScratch;
        const bool haveUi = uiToPlugin.take(fromUi, false);
        for (size_t i = 0; i < ports.size(); i++)
        {
            Port& port = ports[i];
            if (port.kind != Port::kAtom)
                continue;
            if (!port.input)
            {
                port.sequence()->atom.size = static_cast<uint32_t>(port.atom.size() - sizeof(LV2_Atom));
                port.sequence()->atom.type = urid(LV2_ATOM__Chunk);
                continue;
            }
            LV2_Atom_Sequence* sequence = port.sequence();
            lv2_atom_sequence_clear(sequence);
            sequence->atom.type = urid(LV2_ATOM__Sequence);
            if (static_cast<int>(i) == midiPort)
            {
                for (size_t e = 0; e < eventCount; e++)
                    appendEvent(port, events[e].frame, urid(LV2_MIDI__MidiEvent), events[e].data, events[e].size);
            }
            if (haveUi)
            {
                MessageQueue::each(fromUi, [&](uint32_t tag, const uint8_t* data, uint32_t size) {
                    if (tag == i && size >= sizeof(LV2_Atom))
                    {
                        const auto* atom = reinterpret_cast<const LV2_Atom*>(data);
                        appendEvent(port, 0, atom->type, data + sizeof(LV2_Atom), atom->size);
                    }
                });
            }
        }

        // Worker responses, then the block.
        if (worker)
        {
            std::vector<uint8_t>& responses = workerScratch;
            if (workerToPlugin.take(responses, false))
            {
                MessageQueue::each(responses, [&](uint32_t, const uint8_t* data, uint32_t size) {
                    worker->work_response(lilv_instance_get_handle(instance), size, data);
                });
            }
        }

        for (int c = 0; c < 2; c++)
        {
            if (audioOut[c] >= 0)
                std::fill(ports[static_cast<size_t>(audioOut[c])].audio.begin(), ports[static_cast<size_t>(audioOut[c])].audio.end(), 0.0f);
        }
        lilv_instance_run(instance, frames);
        if (worker && worker->end_run)
            worker->end_run(lilv_instance_get_handle(instance));

        const float* outLeft = audioOut[0] >= 0 ? ports[static_cast<size_t>(audioOut[0])].audio.data() : nullptr;
        const float* outRight = audioOut[1] >= 0 ? ports[static_cast<size_t>(audioOut[1])].audio.data() : outLeft;
        for (uint32_t f = 0; f < frames; f++)
        {
            left[f] = outLeft ? outLeft[f] : 0.0f;
            right[f] = outRight ? outRight[f] : 0.0f;
        }

        // What the plugin says, for its UI.
        for (size_t i = 0; i < ports.size(); i++)
        {
            Port& port = ports[i];
            if (port.kind == Port::kControl && !port.input)
                port.hostValue.store(port.control, std::memory_order_relaxed);
            if (port.kind == Port::kAtom && !port.input && editorOpen.load(std::memory_order_relaxed))
            {
                LV2_ATOM_SEQUENCE_FOREACH(port.sequence(), event)
                {
                    pluginToUi.tryPush(static_cast<uint32_t>(i), &event->body, static_cast<uint32_t>(sizeof(LV2_Atom) + event->body.size));
                }
            }
        }
    }

    bool saveState(std::vector<uint8_t>& out) override
    {
        LilvState* state = lilv_state_new_from_instance(plugin,
                                                        instance,
                                                        &urids().mapFeature,
                                                        nullptr,
                                                        nullptr,
                                                        nullptr,
                                                        nullptr,
                                                        getPortValue,
                                                        this,
                                                        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                                                        features.data());
        if (!state)
            return false;
        char* text = lilv_state_to_string(world(), &urids().mapFeature, &urids().unmapFeature, state, "urn:meadowlark:state", nullptr);
        lilv_state_free(state);
        if (!text)
            return false;
        out.assign(text, text + std::strlen(text));
        lilv_free(text);
        clearStateDirty();
        return true;
    }

    bool loadState(const std::vector<uint8_t>& data) override
    {
        const std::string text(data.begin(), data.end());
        LilvState* state = lilv_state_new_from_string(world(), &urids().mapFeature, text.c_str());
        if (!state)
            return false;
        // Restoring isn't real-time safe for most plugins: keep the engine out meanwhile.
        AudioLock::acquire();
        lilv_state_restore(state, instance, setPortValue, this, 0, features.data());
        AudioLock::release();
        lilv_state_free(state);
        return true;
    }

    bool hasEditor(EditorParent::Api api) override { return findUi(api, nullptr); }

    bool openEditor(const EditorParent& parent, uint32_t& width, uint32_t& height, std::string& error) override
    {
        if (ui)
            return true;
        const LilvUI* chosen = nullptr;
        if (!findUi(parent.api, &chosen))
        {
            error = pluginName + " has no " + (parent.api == EditorParent::Api::kX11 ? "X11" : "Cocoa") +
                    " editor (GTK and Qt editors are not supported)";
            return false;
        }

        char* binary = lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_binary_uri(chosen)), nullptr);
        char* bundle = lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_bundle_uri(chosen)), nullptr);
        uiLibrary = binary ? dlopen(binary, RTLD_NOW | RTLD_LOCAL) : nullptr;
        if (!uiLibrary)
        {
            error = std::string("cannot load the editor: ") + (binary ? dlerror() : "no binary");
            lilv_free(binary);
            lilv_free(bundle);
            return false;
        }
        auto descriptorFn = reinterpret_cast<LV2UI_DescriptorFunction>(dlsym(uiLibrary, "lv2ui_descriptor"));
        const char* uiUri = lilv_node_as_uri(lilv_ui_get_uri(chosen));
        for (uint32_t i = 0; descriptorFn; i++)
        {
            const LV2UI_Descriptor* d = descriptorFn(i);
            if (!d)
                break;
            if (!std::strcmp(d->URI, uiUri))
            {
                uiDescriptor = d;
                break;
            }
        }
        if (!uiDescriptor)
        {
            error = "the editor library has no " + std::string(uiUri);
            dlclose(uiLibrary);
            uiLibrary = nullptr;
            lilv_free(binary);
            lilv_free(bundle);
            return false;
        }

        requestedWidth = 0;
        requestedHeight = 0;
        uiResize = { this, [](LV2UI_Feature_Handle h, int w, int hgt) {
                        auto* self = static_cast<Lv2Plugin*>(h);
                        self->requestedWidth = static_cast<uint32_t>(w);
                        self->requestedHeight = static_cast<uint32_t>(hgt);
                        self->requestEditorSize(self->requestedWidth, self->requestedHeight);
                        return 0;
                    } };
        uiPortMap = { this, [](LV2UI_Feature_Handle h, const char* symbol) -> uint32_t {
                         auto* self = static_cast<Lv2Plugin*>(h);
                         for (size_t i = 0; i < self->ports.size(); i++)
                             if (self->ports[i].symbol == symbol)
                                 return static_cast<uint32_t>(i);
                         return LV2UI_INVALID_PORT_INDEX;
                     } };
        extensionData = { lilv_instance_get_descriptor(instance)->extension_data };
        uiFeatureStorage = {
            { LV2_UI__parent, reinterpret_cast<void*>(static_cast<uintptr_t>(parent.handle)) },
            { LV2_UI__resize, &uiResize },
            { LV2_UI__portMap, &uiPortMap },
            { LV2_UI__idleInterface, nullptr },
            { LV2_INSTANCE_ACCESS_URI, lilv_instance_get_handle(instance) },
            { LV2_DATA_ACCESS_URI, &extensionData },
            { LV2_URID__map, &urids().mapFeature },
            { LV2_URID__unmap, &urids().unmapFeature },
            { LV2_OPTIONS__options, options.data() },
            { LV2_LOG__log, &logFeature },
        };
        uiFeatures.clear();
        for (LV2_Feature& feature : uiFeatureStorage)
            uiFeatures.push_back(&feature);
        uiFeatures.push_back(nullptr);

        LV2UI_Widget widget = nullptr;
        editorOpen = true;
        ui = uiDescriptor->instantiate(uiDescriptor,
                                       pluginUri.c_str(),
                                       bundle,
                                       uiWrite,
                                       this,
                                       &widget,
                                       uiFeatures.data());
        lilv_free(binary);
        lilv_free(bundle);
        if (!ui)
        {
            editorOpen = false;
            dlclose(uiLibrary);
            uiLibrary = nullptr;
            uiDescriptor = nullptr;
            error = pluginName + "'s editor did not start";
            return false;
        }
        uiIdle = uiDescriptor->extension_data
                   ? static_cast<const LV2UI_Idle_Interface*>(uiDescriptor->extension_data(LV2_UI__idleInterface))
                   : nullptr;
        uiResizeInterface = uiDescriptor->extension_data
                              ? static_cast<const LV2UI_Resize*>(uiDescriptor->extension_data(LV2_UI__resize))
                              : nullptr;

        // Every control's value to start with.
        for (size_t i = 0; i < ports.size(); i++)
        {
            if (ports[i].kind == Port::kControl)
            {
                ports[i].sentToUi = ports[i].hostValue.load();
                uiDescriptor->port_event(ui, static_cast<uint32_t>(i), sizeof(float), 0, &ports[i].sentToUi);
            }
        }
        markStateDirty();
        width = requestedWidth ? requestedWidth : 640;
        height = requestedHeight ? requestedHeight : 480;
        return true;
    }

    void closeEditor() override
    {
        if (!ui)
            return;
        editorOpen = false;
        uiDescriptor->cleanup(ui);
        ui = nullptr;
        uiDescriptor = nullptr;
        uiIdle = nullptr;
        uiResizeInterface = nullptr;
        dlclose(uiLibrary);
        uiLibrary = nullptr;
    }

    bool isEditorOpen() const override { return ui != nullptr; }
    bool canResizeEditor() override { return ui && uiResizeInterface; }

    void resizeEditor(uint32_t& width, uint32_t& height) override
    {
        if (canResizeEditor())
            uiResizeInterface->ui_resize(uiResizeInterface->handle, static_cast<int>(width), static_cast<int>(height));
    }

    void idle() override
    {
        // Worker jobs run here, off the audio thread.
        if (worker)
        {
            std::vector<uint8_t> jobs;
            if (pluginToWorker.take(jobs, true))
            {
                MessageQueue::each(jobs, [&](uint32_t, const uint8_t* data, uint32_t size) {
                    worker->work(lilv_instance_get_handle(instance),
                                 [](LV2_Worker_Respond_Handle h, uint32_t s, const void* d) {
                                     static_cast<Lv2Plugin*>(h)->workerToPlugin.push(0, d, s);
                                     return LV2_WORKER_SUCCESS;
                                 },
                                 this,
                                 size,
                                 data);
                });
            }
        }

        if (!ui)
            return;
        for (size_t i = 0; i < ports.size(); i++)
        {
            Port& port = ports[i];
            if (port.kind != Port::kControl)
                continue;
            const float value = port.hostValue.load(std::memory_order_relaxed);
            if (value != port.sentToUi)
            {
                port.sentToUi = value;
                uiDescriptor->port_event(ui, static_cast<uint32_t>(i), sizeof(float), 0, &value);
            }
        }
        std::vector<uint8_t> messages;
        if (pluginToUi.take(messages, true))
        {
            MessageQueue::each(messages, [&](uint32_t port, const uint8_t* data, uint32_t size) {
                uiDescriptor->port_event(ui, port, size, urid(LV2_ATOM__eventTransfer), data);
            });
        }
        if (uiIdle && uiIdle->idle(ui) != 0)
            editorClosedItself();
    }

  private:
    struct Port
    {
        enum Kind
        {
            kAudio,
            kControl,
            kAtom,
            kCv,
            kOther
        };
        Kind kind = kOther;
        bool input = false;
        std::string symbol;
        float control = 0.0f; // the buffer for control ports
        std::atomic<float> hostValue{ 0.0f };
        float sentToUi = 0.0f;
        std::vector<float> audio;
        std::vector<uint64_t> atom; // 8-byte aligned

        void* buffer()
        {
            switch (kind)
            {
                case kControl: return &control;
                case kAudio:
                case kCv: return audio.data();
                case kAtom: return atom.data();
                default: return nullptr;
            }
        }
        LV2_Atom_Sequence* sequence() { return reinterpret_cast<LV2_Atom_Sequence*>(atom.data()); }
    };

    LV2_URID urid(const char* uri) { return urids().map(uri); }

    void appendEvent(Port& port, uint32_t frame, LV2_URID type, const void* body, uint32_t size)
    {
        // An LV2_Atom_Event followed by its body.
        alignas(8) uint8_t buffer[sizeof(LV2_Atom_Event) + 512];
        if (size > 512)
            return;
        auto* event = reinterpret_cast<LV2_Atom_Event*>(buffer);
        event->time.frames = frame;
        event->body.type = type;
        event->body.size = size;
        std::memcpy(buffer + sizeof(LV2_Atom_Event), body, size);
        lv2_atom_sequence_append_event(port.sequence(), static_cast<uint32_t>(port.atom.size() * 8 - sizeof(LV2_Atom)), event);
    }

    void setUpFeatures()
    {
        static const int32_t blockLength = kMaxFrames;
        static const int32_t minBlockLength = 1;
        static const float sampleRate = static_cast<float>(kSampleRate);
        const LV2_URID intType = urid(LV2_ATOM__Int);
        options = {
            { LV2_OPTIONS_INSTANCE, 0, urid(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), intType, &blockLength },
            { LV2_OPTIONS_INSTANCE, 0, urid(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), intType, &minBlockLength },
            { LV2_OPTIONS_INSTANCE, 0, urid(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), intType, &blockLength },
            { LV2_OPTIONS_INSTANCE, 0, urid(LV2_PARAMETERS__sampleRate), sizeof(float), urid(LV2_ATOM__Float), &sampleRate },
            { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr },
        };
        schedule = { this, [](LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
                        return static_cast<Lv2Plugin*>(h)->pluginToWorker.tryPush(0, data, size) ? LV2_WORKER_SUCCESS
                                                                                                    : LV2_WORKER_ERR_NO_SPACE;
                    } };
        featureStorage = {
            { LV2_URID__map, &urids().mapFeature },
            { LV2_URID__unmap, &urids().unmapFeature },
            { LV2_OPTIONS__options, options.data() },
            { LV2_BUF_SIZE__boundedBlockLength, nullptr },
            { LV2_WORKER__schedule, &schedule },
            { LV2_LOG__log, &logFeature },
        };
        features.clear();
        for (LV2_Feature& feature : featureStorage)
            features.push_back(&feature);
        features.push_back(nullptr);
    }

    bool checkRequiredFeatures(std::string& error)
    {
        LilvNodes* required = lilv_plugin_get_required_features(plugin);
        bool ok = true;
        LILV_FOREACH(nodes, it, required)
        {
            const char* uri = lilv_node_as_uri(lilv_nodes_get(required, it));
            const bool have = std::any_of(featureStorage.begin(), featureStorage.end(), [&](const LV2_Feature& f) {
                return !std::strcmp(f.URI, uri);
            });
            if (!have)
            {
                error = pluginName + " needs " + uri + ", which Meadowlark doesn't offer";
                ok = false;
            }
        }
        lilv_nodes_free(required);
        return ok;
    }

    bool setUpPorts(std::string& error)
    {
        LilvNode* audioClass = node(LV2_CORE__AudioPort);
        LilvNode* controlClass = node(LV2_CORE__ControlPort);
        LilvNode* cvClass = node(LV2_CORE__CVPort);
        LilvNode* atomClass = node(LV2_ATOM__AtomPort);
        LilvNode* inputClass = node(LV2_CORE__InputPort);
        LilvNode* midiEvent = node(LV2_MIDI__MidiEvent);
        LilvNode* minimumSize = node(LV2_RESIZE_PORT__minimumSize);
        LilvNode* optional = node(LV2_CORE__connectionOptional);

        const uint32_t count = lilv_plugin_get_num_ports(plugin);
        std::vector<float> defaults(count, 0.0f);
        lilv_plugin_get_port_ranges_float(plugin, nullptr, nullptr, defaults.data());
        ports = std::vector<Port>(count);
        bool ok = true;
        for (uint32_t i = 0; i < count; i++)
        {
            const LilvPort* lilvPort = lilv_plugin_get_port_by_index(plugin, i);
            Port& port = ports[i];
            port.symbol = lilv_node_as_string(lilv_port_get_symbol(plugin, lilvPort));
            port.input = lilv_port_is_a(plugin, lilvPort, inputClass);
            if (lilv_port_is_a(plugin, lilvPort, audioClass) || lilv_port_is_a(plugin, lilvPort, cvClass))
            {
                port.kind = lilv_port_is_a(plugin, lilvPort, audioClass) ? Port::kAudio : Port::kCv;
                port.audio.assign(kMaxFrames, 0.0f);
                if (port.kind == Port::kAudio && !port.input)
                {
                    if (audioOut[0] < 0)
                        audioOut[0] = static_cast<int>(i);
                    else if (audioOut[1] < 0)
                        audioOut[1] = static_cast<int>(i);
                }
            }
            else if (lilv_port_is_a(plugin, lilvPort, controlClass))
            {
                port.kind = Port::kControl;
                const float value = std::isnan(defaults[i]) ? 0.0f : defaults[i];
                port.control = value;
                port.hostValue = value;
            }
            else if (lilv_port_is_a(plugin, lilvPort, atomClass))
            {
                port.kind = Port::kAtom;
                size_t bytes = 8192;
                if (LilvNodes* sizes = lilv_port_get_value(plugin, lilvPort, minimumSize))
                {
                    if (lilv_nodes_size(sizes) > 0)
                        bytes = std::max<size_t>(bytes, static_cast<size_t>(lilv_node_as_int(lilv_nodes_get_first(sizes))));
                    lilv_nodes_free(sizes);
                }
                port.atom.assign((bytes + 7) / 8, 0);
                if (port.input && midiPort < 0 && lilv_port_supports_event(plugin, lilvPort, midiEvent))
                    midiPort = static_cast<int>(i);
            }
            else if (!lilv_port_has_property(plugin, lilvPort, optional))
            {
                error = pluginName + " has a port of a kind Meadowlark can't connect (" + port.symbol + ")";
                ok = false;
            }
        }
        if (ok && audioOut[0] < 0)
        {
            error = pluginName + " has no audio output";
            ok = false;
        }
        for (LilvNode* n : { audioClass, controlClass, cvClass, atomClass, inputClass, midiEvent, minimumSize, optional })
            lilv_node_free(n);
        return ok;
    }

    bool findUi(EditorParent::Api api, const LilvUI** out)
    {
        LilvUIs* uis = lilv_plugin_get_uis(plugin);
        LilvNode* wanted = node(api == EditorParent::Api::kX11 ? LV2_UI__X11UI : LV2_UI__CocoaUI);
        bool found = false;
        LILV_FOREACH(uis, it, uis)
        {
            const LilvUI* candidate = lilv_uis_get(uis, it);
            if (lilv_ui_is_a(candidate, wanted))
            {
                found = true;
                if (out)
                    *out = candidate;
                break;
            }
        }
        lilv_node_free(wanted);
        // The LilvUIs list belongs to the world's data, so the chosen UI stays valid; the
        // list itself is freed.
        lilv_uis_free(uis);
        return found;
    }

    static void uiWrite(LV2UI_Controller controller, uint32_t port, uint32_t size, uint32_t protocol, const void* buffer)
    {
        auto* self = static_cast<Lv2Plugin*>(controller);
        if (port >= self->ports.size())
            return;
        if (protocol == 0 && size == sizeof(float))
        {
            float value;
            std::memcpy(&value, buffer, sizeof(float));
            self->ports[port].hostValue = value;
            self->ports[port].sentToUi = value; // the UI knows
        }
        else if (protocol == self->urid(LV2_ATOM__eventTransfer))
            self->uiToPlugin.push(port, buffer, size);
    }

    static const void* getPortValue(const char* symbol, void* data, uint32_t* size, uint32_t* type)
    {
        auto* self = static_cast<Lv2Plugin*>(data);
        for (Port& port : self->ports)
        {
            if (port.kind == Port::kControl && port.input && port.symbol == symbol)
            {
                port.sentToUi = port.hostValue.load();
                *size = sizeof(float);
                *type = self->urid(LV2_ATOM__Float);
                static thread_local float value;
                value = port.hostValue.load();
                return &value;
            }
        }
        *size = 0;
        *type = 0;
        return nullptr;
    }

    static void setPortValue(const char* symbol, void* data, const void* value, uint32_t size, uint32_t type)
    {
        auto* self = static_cast<Lv2Plugin*>(data);
        if (size != sizeof(float) || type != self->urid(LV2_ATOM__Float))
            return;
        for (Port& port : self->ports)
        {
            if (port.kind == Port::kControl && port.symbol == symbol)
            {
                float v;
                std::memcpy(&v, value, sizeof(float));
                port.hostValue = v;
            }
        }
    }

    const LilvPlugin* plugin = nullptr;
    LilvInstance* instance = nullptr;
    std::string pluginUri;
    std::string pluginName;

    std::vector<LV2_Options_Option> options;
    LV2_Worker_Schedule schedule{};
    std::vector<LV2_Feature> featureStorage;
    std::vector<const LV2_Feature*> features;
    const LV2_Worker_Interface* worker = nullptr;

    std::vector<Port> ports;
    int audioOut[2] = { -1, -1 };
    int midiPort = -1;

    MessageQueue uiToPlugin;
    MessageQueue pluginToUi;
    MessageQueue pluginToWorker;
    MessageQueue workerToPlugin;
    std::vector<uint8_t> audioScratch;
    std::vector<uint8_t> workerScratch;

    void* uiLibrary = nullptr;
    const LV2UI_Descriptor* uiDescriptor = nullptr;
    LV2UI_Handle ui = nullptr;
    const LV2UI_Idle_Interface* uiIdle = nullptr;
    const LV2UI_Resize* uiResizeInterface = nullptr;
    LV2UI_Resize uiResize{};
    LV2UI_Port_Map uiPortMap{};
    LV2_Extension_Data_Feature extensionData{};
    std::vector<LV2_Feature> uiFeatureStorage;
    std::vector<const LV2_Feature*> uiFeatures;
    std::atomic<bool> editorOpen{ false };
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
};
} // namespace

std::unique_ptr<Plugin> loadLv2Plugin(const std::string& uri, std::string& error)
{
    auto plugin = std::make_unique<Lv2Plugin>();
    if (!plugin->create(uri, error))
        return nullptr;
    return plugin;
}

// Instruments: plugins of the instrument class, or anything with a MIDI input and sound out
// (many synths declare no class).
std::vector<Found> findLv2Plugins()
{
    std::vector<Found> found;
    const LilvPlugins* all = lilv_world_get_all_plugins(world());
    LilvNode* rdfType = node("http://www.w3.org/1999/02/22-rdf-syntax-ns#type");
    LilvNode* instrumentClass = node(LV2_CORE__InstrumentPlugin);
    LilvNode* inputClass = node(LV2_CORE__InputPort);
    LilvNode* outputClass = node(LV2_CORE__OutputPort);
    LilvNode* atomClass = node(LV2_ATOM__AtomPort);
    LilvNode* audioClass = node(LV2_CORE__AudioPort);
    LilvNode* midiEvent = node(LV2_MIDI__MidiEvent);
    LILV_FOREACH(plugins, it, all)
    {
        const LilvPlugin* plugin = lilv_plugins_get(all, it);
        bool instrument = lilv_node_equals(lilv_plugin_class_get_uri(lilv_plugin_get_class(plugin)), instrumentClass);
        if (!instrument)
        {
            LilvNodes* types = lilv_plugin_get_value(plugin, rdfType);
            instrument = types && lilv_nodes_contains(types, instrumentClass);
            lilv_nodes_free(types);
        }
        if (!instrument && lilv_plugin_get_num_ports_of_class(plugin, outputClass, audioClass, nullptr) > 0)
        {
            for (uint32_t i = 0; i < lilv_plugin_get_num_ports(plugin) && !instrument; i++)
            {
                const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
                instrument = lilv_port_is_a(plugin, port, inputClass) && lilv_port_is_a(plugin, port, atomClass) &&
                             lilv_port_supports_event(plugin, port, midiEvent);
            }
        }
        if (!instrument)
            continue;
        LilvNode* name = lilv_plugin_get_name(plugin);
        found.push_back({ "lv2", name ? lilv_node_as_string(name) : "?", lilv_node_as_uri(lilv_plugin_get_uri(plugin)), "" });
        lilv_node_free(name);
    }
    for (LilvNode* n : { rdfType, instrumentClass, inputClass, outputClass, atomClass, audioClass, midiEvent })
        lilv_node_free(n);
    return found;
}
} // namespace plugins
