// CLAP hosting: one plugin instance from a .clap file, activated at the engine's rate.
//
// The host offers log, thread-check, state, params, note-ports, audio-ports, gui,
// timer-support and posix-fd-support (Linux editors run their event loop through the last
// two). Notes go in as MIDI when the plugin's note port takes MIDI, else as CLAP note
// events; output events are dropped.

#include "plugins/Plugin.h"

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/stat.h>
#include <thread>

namespace plugins
{
namespace
{
using Clock = std::chrono::steady_clock;

std::thread::id mainThread = std::this_thread::get_id();
thread_local bool isAudioThread = false;

// The shared library inside a .clap: the file itself on Linux, Contents/MacOS/* in a
// macOS bundle.
std::string clapBinary(const std::string& path)
{
#ifdef __APPLE__
    const std::string dir = path + "/Contents/MacOS";
    if (DIR* d = opendir(dir.c_str()))
    {
        std::string found;
        while (dirent* entry = readdir(d))
        {
            if (entry->d_name[0] != '.')
            {
                found = dir + "/" + entry->d_name;
                break;
            }
        }
        closedir(d);
        return found;
    }
#endif
    return path;
}

// A loaded .clap: dlopen handle and initialised entry, shared by its instances.
class Library
{
  public:
    ~Library()
    {
        if (entry)
            entry->deinit();
        if (handle)
            dlclose(handle);
    }

    bool open(const std::string& path, std::string& error)
    {
        const std::string binary = clapBinary(path);
        handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            error = std::string("cannot load ") + binary + ": " + dlerror();
            return false;
        }
        auto* found = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
        if (!found || !clap_version_is_compatible(found->clap_version) || !found->init(path.c_str()))
        {
            error = path + " is not a usable CLAP plugin";
            return false;
        }
        entry = found;
        factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory)
        {
            error = path + " has no plugin factory";
            return false;
        }
        return true;
    }

    void* handle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
};

class ClapPlugin : public Plugin
{
  public:
    explicit ClapPlugin(std::unique_ptr<Library> library) : library(std::move(library))
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "Meadowlark";
        host.vendor = "Meadowlark";
        host.url = "";
        host.version = "1";
        host.get_extension = getExtension;
        host.request_restart = [](const clap_host_t* h) { self(h)->restartRequested = true; };
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t* h) { self(h)->callbackRequested = true; };
    }

    ~ClapPlugin() override
    {
        if (!plugin)
            return;
        closeEditor();
        if (active)
            plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }

    bool create(const std::string& wantedId, const std::string& path, std::string& error)
    {
        const clap_plugin_factory_t* factory = library->factory;
        const clap_plugin_descriptor_t* chosen = nullptr;
        std::string ids;
        for (uint32_t i = 0, n = factory->get_plugin_count(factory); i < n; i++)
        {
            const clap_plugin_descriptor_t* descriptor = factory->get_plugin_descriptor(factory, i);
            if (!descriptor || !clap_version_is_compatible(descriptor->clap_version))
                continue;
            ids += std::string(ids.empty() ? "" : ", ") + descriptor->id;
            if (!chosen && (wantedId.empty() || wantedId == descriptor->id))
                chosen = descriptor;
        }
        if (!chosen)
        {
            error = path + " has no plugin " + wantedId + " (it has: " + ids + ")";
            return false;
        }
        pluginId = chosen->id;
        pluginName = chosen->name ? chosen->name : chosen->id;

        plugin = factory->create_plugin(factory, &host, chosen->id);
        if (!plugin || !plugin->init(plugin))
        {
            if (plugin)
                plugin->destroy(plugin);
            plugin = nullptr;
            error = "cannot create " + pluginName;
            return false;
        }

        gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        timerSupport = static_cast<const clap_plugin_timer_support_t*>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
        fdSupport = static_cast<const clap_plugin_posix_fd_support_t*>(plugin->get_extension(plugin, CLAP_EXT_POSIX_FD_SUPPORT));
        readPorts();

        if (!plugin->activate(plugin, kSampleRate, 1, kMaxFrames))
        {
            error = "cannot activate " + pluginName;
            return false;
        }
        active = true;
        return true;
    }

    std::string name() const override { return pluginName; }
    std::string id() const override { return pluginId; }

    void process(const MidiEvent* events, size_t eventCount, float* left, float* right, uint32_t frames) override
    {
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
        if (!active || restarting.load(std::memory_order_acquire))
            return;
        isAudioThread = true;
        if (!processing)
        {
            processing = plugin->start_processing(plugin);
            if (!processing)
                return;
        }

        // Events, in the dialect the note port takes.
        eventStorage.clear();
        for (size_t i = 0; i < eventCount; i++)
            addEvent(events[i]);

        clap_input_events_t in{ this, inSize, inGet };
        clap_output_events_t out{ this, [](const clap_output_events_t*, const clap_event_header_t*) { return true; } };

        // Inputs get silence; the first output port's channels are the result.
        float* outputs[2] = { left, right };
        clap_audio_buffer_t inputBuffers[kMaxPorts]{};
        for (uint32_t i = 0; i < inputPortCount; i++)
        {
            inputBuffers[i].data32 = silentChannels;
            inputBuffers[i].channel_count = std::min<uint32_t>(inputChannels[i], kMaxChannels);
        }
        clap_audio_buffer_t outputBuffers[kMaxPorts]{};
        for (uint32_t i = 0; i < outputPortCount; i++)
        {
            outputBuffers[i].channel_count = std::min<uint32_t>(outputChannels[i], kMaxChannels);
            outputBuffers[i].data32 = i == 0 && outputChannels[0] <= 2 ? outputs : scratchChannels;
        }

        clap_process_t process{};
        process.steady_time = static_cast<int64_t>(steadyTime);
        process.frames_count = frames;
        process.audio_inputs = inputBuffers;
        process.audio_inputs_count = inputPortCount;
        process.audio_outputs = outputBuffers;
        process.audio_outputs_count = outputPortCount;
        process.in_events = &in;
        process.out_events = &out;
        const clap_process_status status = plugin->process(plugin, &process);
        steadyTime += frames;
        if (status == CLAP_PROCESS_ERROR)
        {
            std::fill(left, left + frames, 0.0f);
            std::fill(right, right + frames, 0.0f);
            return;
        }
        if (outputPortCount > 0 && outputChannels[0] == 1)
            std::copy(left, left + frames, right);
    }

    bool saveState(std::vector<uint8_t>& out) override
    {
        out.clear();
        if (!state)
            return false;
        clap_ostream_t stream{ &out, [](const clap_ostream_t* s, const void* buffer, uint64_t size) -> int64_t {
                                  auto* bytes = static_cast<std::vector<uint8_t>*>(s->ctx);
                                  const auto* data = static_cast<const uint8_t*>(buffer);
                                  bytes->insert(bytes->end(), data, data + size);
                                  return static_cast<int64_t>(size);
                              } };
        if (!state->save(plugin, &stream))
            return false;
        clearStateDirty();
        return true;
    }

    bool loadState(const std::vector<uint8_t>& data) override
    {
        if (!state)
            return false;
        struct Reader
        {
            const std::vector<uint8_t>* data;
            size_t position;
        } reader{ &data, 0 };
        clap_istream_t stream{ &reader, [](const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t {
                                  auto* r = static_cast<Reader*>(s->ctx);
                                  const size_t count = std::min<size_t>(size, r->data->size() - r->position);
                                  std::memcpy(buffer, r->data->data() + r->position, count);
                                  r->position += count;
                                  return static_cast<int64_t>(count);
                              } };
        return state->load(plugin, &stream);
    }

    bool hasEditor(EditorParent::Api api) override
    {
        return gui && gui->is_api_supported(plugin, apiName(api), false);
    }

    bool openEditor(const EditorParent& parent, uint32_t& width, uint32_t& height, std::string& error) override
    {
        if (editorOpen)
            return true;
        const char* api = apiName(parent.api);
        if (!hasEditor(parent.api) || !gui->create(plugin, api, false))
        {
            error = pluginName + " has no editor that can go in a window here";
            return false;
        }
        clap_window_t window{};
        window.api = api;
        if (parent.api == EditorParent::Api::kX11)
            window.x11 = static_cast<clap_xwnd>(parent.handle);
        else
            window.cocoa = reinterpret_cast<clap_nsview>(static_cast<uintptr_t>(parent.handle));
        width = 640;
        height = 480;
        gui->get_size(plugin, &width, &height);
        if (!gui->set_parent(plugin, &window))
        {
            gui->destroy(plugin);
            error = pluginName + "'s editor did not accept the window";
            return false;
        }
        gui->show(plugin);
        editorOpen = true;
        markStateDirty();
        return true;
    }

    void closeEditor() override
    {
        if (!editorOpen)
            return;
        gui->hide(plugin);
        gui->destroy(plugin);
        editorOpen = false;
    }

    bool isEditorOpen() const override { return editorOpen; }
    bool canResizeEditor() override { return editorOpen && gui->can_resize(plugin); }

    void resizeEditor(uint32_t& width, uint32_t& height) override
    {
        if (!canResizeEditor())
            return;
        gui->adjust_size(plugin, &width, &height);
        gui->set_size(plugin, width, height);
    }

    void idle() override
    {
        if (callbackRequested.exchange(false))
            plugin->on_main_thread(plugin);
        if (restartRequested.exchange(false))
            restart();

        const Clock::time_point now = Clock::now();
        for (Timer& timer : timers)
        {
            if (now >= timer.next && timerSupport)
            {
                timer.next = now + std::chrono::milliseconds(timer.periodMs);
                timerSupport->on_timer(plugin, timer.id);
            }
        }

        if (!fds.empty() && fdSupport)
        {
            std::vector<pollfd> polled;
            for (const Fd& fd : fds)
                polled.push_back(pollfd{ fd.fd, static_cast<short>((fd.flags & CLAP_POSIX_FD_READ ? POLLIN : 0) |
                                                                    (fd.flags & CLAP_POSIX_FD_WRITE ? POLLOUT : 0)),
                                         0 });
            if (poll(polled.data(), polled.size(), 0) > 0)
            {
                for (const pollfd& p : polled)
                {
                    clap_posix_fd_flags_t flags = 0;
                    if (p.revents & POLLIN)
                        flags |= CLAP_POSIX_FD_READ;
                    if (p.revents & POLLOUT)
                        flags |= CLAP_POSIX_FD_WRITE;
                    if (p.revents & (POLLERR | POLLHUP))
                        flags |= CLAP_POSIX_FD_ERROR;
                    if (flags)
                        fdSupport->on_fd(plugin, p.fd, flags);
                }
            }
        }
    }

  private:
    static constexpr uint32_t kMaxPorts = 8;
    static constexpr uint32_t kMaxChannels = 8;

    struct Timer
    {
        clap_id id;
        uint32_t periodMs;
        Clock::time_point next;
    };
    struct Fd
    {
        int fd;
        clap_posix_fd_flags_t flags;
    };
    union Event
    {
        clap_event_header_t header;
        clap_event_note_t note;
        clap_event_midi_t midi;
    };

    static ClapPlugin* self(const clap_host_t* h) { return static_cast<ClapPlugin*>(h->host_data); }

    static const char* apiName(EditorParent::Api api)
    {
        return api == EditorParent::Api::kX11 ? CLAP_WINDOW_API_X11 : CLAP_WINDOW_API_COCOA;
    }

    void readPorts()
    {
        if (auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS)))
        {
            clap_note_port_info_t info{};
            if (notePorts->count(plugin, true) > 0 && notePorts->get(plugin, 0, true, &info))
                useMidi = (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
        }
        if (auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS)))
        {
            inputPortCount = std::min(audioPorts->count(plugin, true), kMaxPorts);
            outputPortCount = std::min(audioPorts->count(plugin, false), kMaxPorts);
            for (uint32_t i = 0; i < inputPortCount; i++)
            {
                clap_audio_port_info_t info{};
                inputChannels[i] = audioPorts->get(plugin, i, true, &info) ? info.channel_count : 2;
            }
            for (uint32_t i = 0; i < outputPortCount; i++)
            {
                clap_audio_port_info_t info{};
                outputChannels[i] = audioPorts->get(plugin, i, false, &info) ? info.channel_count : 2;
            }
        }
        for (uint32_t c = 0; c < kMaxChannels; c++)
        {
            silentChannels[c] = silent[c];
            scratchChannels[c] = scratch[c];
        }
    }

    void addEvent(const MidiEvent& event)
    {
        Event e{};
        const uint8_t type = event.data[0] & 0xF0;
        const int16_t channel = event.data[0] & 0x0F;
        if (useMidi)
        {
            e.midi.header = { sizeof(clap_event_midi_t), event.frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
            std::memcpy(e.midi.data, event.data, 3);
        }
        else if (type == 0x90 || type == 0x80)
        {
            const bool on = type == 0x90 && event.data[2] > 0;
            e.note.header = { sizeof(clap_event_note_t), event.frame, CLAP_CORE_EVENT_SPACE_ID,
                              static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
            e.note.note_id = -1;
            e.note.port_index = 0;
            e.note.channel = channel;
            e.note.key = event.data[1];
            e.note.velocity = event.data[2] / 127.0;
        }
        else
            return; // CC and the rest need the MIDI dialect
        eventStorage.push_back(e);
    }

    static uint32_t inSize(const clap_input_events_t* list)
    {
        return static_cast<uint32_t>(static_cast<ClapPlugin*>(list->ctx)->eventStorage.size());
    }
    static const clap_event_header_t* inGet(const clap_input_events_t* list, uint32_t index)
    {
        return &static_cast<ClapPlugin*>(list->ctx)->eventStorage[index].header;
    }

    // The plugin asked for a restart: deactivate and activate again with the audio thread
    // kept out (process() skips while restarting).
    void restart()
    {
        restarting.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // a block or two
        if (active)
            plugin->deactivate(plugin);
        processing = false;
        readPorts();
        active = plugin->activate(plugin, kSampleRate, 1, kMaxFrames);
        restarting.store(false, std::memory_order_release);
    }

    static const void* getExtension(const clap_host_t*, const char* id);

    std::unique_ptr<Library> library;
    clap_host_t host{};
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_timer_support_t* timerSupport = nullptr;
    const clap_plugin_posix_fd_support_t* fdSupport = nullptr;
    std::string pluginId;
    std::string pluginName;

    bool active = false;
    bool processing = false; // audio thread
    std::atomic<bool> restarting{ false };
    std::atomic<bool> restartRequested{ false };
    std::atomic<bool> callbackRequested{ false };
    bool editorOpen = false;
    bool useMidi = false;
    uint64_t steadyTime = 0;

    uint32_t inputPortCount = 0;
    uint32_t outputPortCount = 1;
    uint32_t inputChannels[kMaxPorts] = {};
    uint32_t outputChannels[kMaxPorts] = { 2 };
    float silent[kMaxChannels][kMaxFrames] = {};
    float scratch[kMaxChannels][kMaxFrames] = {};
    float* silentChannels[kMaxChannels] = {};
    float* scratchChannels[kMaxChannels] = {};
    std::vector<Event> eventStorage;

    std::vector<Timer> timers;
    std::vector<Fd> fds;
    clap_id nextTimerId = 1;

    friend struct HostExtensions;
};

struct HostExtensions
{
    static ClapPlugin* self(const clap_host_t* h) { return static_cast<ClapPlugin*>(h->host_data); }

    static constexpr clap_host_log_t log{ [](const clap_host_t* h, clap_log_severity severity, const char* message) {
        if (severity >= CLAP_LOG_WARNING)
            std::fprintf(stderr, "meadowlark: plugin %s: %s\n", self(h)->pluginName.c_str(), message);
    } };
    static constexpr clap_host_thread_check_t threadCheck{
        [](const clap_host_t*) { return std::this_thread::get_id() == mainThread; },
        [](const clap_host_t*) { return isAudioThread; },
    };
    static constexpr clap_host_state_t state{ [](const clap_host_t* h) { self(h)->markStateDirty(); } };
    static constexpr clap_host_params_t params{
        [](const clap_host_t*, clap_param_rescan_flags) {},
        [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
        [](const clap_host_t*) {}, // flush: the plugin is processed all the time
    };
    static constexpr clap_host_note_ports_t notePorts{
        [](const clap_host_t*) -> uint32_t { return CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP; },
        [](const clap_host_t*, uint32_t) {},
    };
    static constexpr clap_host_audio_ports_t audioPorts{
        [](const clap_host_t*, uint32_t) { return false; },
        [](const clap_host_t* h, uint32_t) { self(h)->restartRequested = true; },
    };
    static constexpr clap_host_gui_t gui{
        [](const clap_host_t*) {},
        [](const clap_host_t* h, uint32_t width, uint32_t height) {
            self(h)->requestEditorSize(width, height);
            return true;
        },
        [](const clap_host_t*) { return false; },
        [](const clap_host_t*) { return false; },
        [](const clap_host_t* h, bool wasDestroyed) {
            // The host closes the window, and destroys the editor unless it already is.
            if (wasDestroyed)
                self(h)->editorOpen = false;
            self(h)->editorClosedItself();
        },
    };
    static constexpr clap_host_timer_support_t timers{
        [](const clap_host_t* h, uint32_t periodMs, clap_id* id) {
            ClapPlugin* plugin = self(h);
            *id = plugin->nextTimerId++;
            periodMs = std::max<uint32_t>(periodMs, 1);
            plugin->timers.push_back({ *id, periodMs, Clock::now() + std::chrono::milliseconds(periodMs) });
            return true;
        },
        [](const clap_host_t* h, clap_id id) {
            auto& timers = self(h)->timers;
            timers.erase(std::remove_if(timers.begin(), timers.end(), [&](const ClapPlugin::Timer& t) { return t.id == id; }),
                         timers.end());
            return true;
        },
    };
    static constexpr clap_host_posix_fd_support_t fds{
        [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
            self(h)->fds.push_back({ fd, flags });
            return true;
        },
        [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
            for (ClapPlugin::Fd& entry : self(h)->fds)
                if (entry.fd == fd)
                    entry.flags = flags;
            return true;
        },
        [](const clap_host_t* h, int fd) {
            auto& fds = self(h)->fds;
            fds.erase(std::remove_if(fds.begin(), fds.end(), [&](const ClapPlugin::Fd& f) { return f.fd == fd; }), fds.end());
            return true;
        },
    };
};

const void* ClapPlugin::getExtension(const clap_host_t*, const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_LOG))
        return &HostExtensions::log;
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK))
        return &HostExtensions::threadCheck;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &HostExtensions::state;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &HostExtensions::params;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &HostExtensions::notePorts;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &HostExtensions::audioPorts;
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &HostExtensions::gui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &HostExtensions::timers;
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &HostExtensions::fds;
    return nullptr;
}

bool isClap(const std::string& path)
{
    return path.size() > 5 && path.compare(path.size() - 5, 5, ".clap") == 0;
}

// The standard CLAP locations, plus $CLAP_PATH.
std::vector<std::string> clapDirectories()
{
    std::vector<std::string> dirs;
    if (const char* extra = std::getenv("CLAP_PATH"))
    {
        std::string list = extra;
        for (size_t start = 0, colon; start <= list.size(); start = colon + 1)
        {
            colon = list.find(':', start);
            if (colon == std::string::npos)
                colon = list.size();
            if (colon > start)
                dirs.push_back(list.substr(start, colon - start));
        }
    }
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    if (home)
        dirs.push_back(std::string(home) + "/Library/Audio/Plug-Ins/CLAP");
    dirs.push_back("/Library/Audio/Plug-Ins/CLAP");
#else
    if (home)
        dirs.push_back(std::string(home) + "/.clap");
    dirs.push_back("/usr/lib/clap");
    dirs.push_back("/usr/local/lib/clap");
#endif
    return dirs;
}
} // namespace

std::unique_ptr<Plugin> loadClapPlugin(const std::string& path, const std::string& id, std::string& error)
{
    auto library = std::make_unique<Library>();
    if (!library->open(path, error))
        return nullptr;
    auto plugin = std::make_unique<ClapPlugin>(std::move(library));
    if (!plugin->create(id, path, error))
        return nullptr;
    return plugin;
}

std::vector<Found> findClapPlugins()
{
    std::vector<Found> found;
    for (const std::string& dir : clapDirectories())
    {
        DIR* d = opendir(dir.c_str());
        if (!d)
            continue;
        while (dirent* entry = readdir(d))
        {
            const std::string path = dir + "/" + entry->d_name;
            if (!isClap(path))
                continue;
            Library library;
            std::string error;
            if (!library.open(path, error))
            {
                std::fprintf(stderr, "meadowlark: %s\n", error.c_str());
                continue;
            }
            for (uint32_t i = 0, n = library.factory->get_plugin_count(library.factory); i < n; i++)
            {
                const clap_plugin_descriptor_t* descriptor = library.factory->get_plugin_descriptor(library.factory, i);
                if (!descriptor)
                    continue;
                bool instrument = false;
                for (const char* const* feature = descriptor->features; feature && *feature; feature++)
                    instrument = instrument || !std::strcmp(*feature, CLAP_PLUGIN_FEATURE_INSTRUMENT);
                if (instrument)
                    found.push_back({ "clap", descriptor->name ? descriptor->name : descriptor->id, path, descriptor->id });
            }
        }
        closedir(d);
    }
    return found;
}
} // namespace plugins
