#pragma once

// A hosted instrument plugin (CLAP or LV2), as the plugin host sees it.
//
// Threads, as both formats define them: process() runs on the audio thread (the engine's
// render); everything else on the main loop, where idle() also delivers the plugin's
// main-thread work (CLAP callbacks, timers and file descriptors; LV2 worker jobs and UI
// updates). The host keeps the plugin activated at kSampleRate with blocks of up to
// kMaxFrames from load until it is destroyed.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plugins
{
constexpr double kSampleRate = 44100.0;
constexpr uint32_t kMaxFrames = 128;

// A MIDI 1.0 channel message at a frame of the block.
struct MidiEvent
{
    uint32_t frame = 0;
    uint8_t data[3] = {};
    uint8_t size = 0;
};

// Where a plugin's editor goes: a window the host made for it.
struct EditorParent
{
    enum class Api
    {
        kX11,   // handle: an X11 Window id
        kCocoa, // handle: an NSView*
    };
    Api api;
    uint64_t handle = 0;
};

class Plugin
{
  public:
    virtual ~Plugin() = default;

    virtual std::string name() const = 0;
    // A stable identity for saved state: the CLAP id or the LV2 URI.
    virtual std::string id() const = 0;

    // Audio thread. Adds nothing to left and right; they are overwritten with the output
    // (a mono plugin's into both).
    virtual void process(const MidiEvent* events, size_t eventCount, float* left, float* right, uint32_t frames) = 0;

    virtual bool saveState(std::vector<uint8_t>& out) = 0;
    virtual bool loadState(const std::vector<uint8_t>& data) = 0;

    // The editor, embedded in the host's window.
    virtual bool hasEditor(EditorParent::Api api) = 0;
    // Returns false with error set if it can't open; width and height are its size.
    virtual bool openEditor(const EditorParent& parent, uint32_t& width, uint32_t& height, std::string& error) = 0;
    virtual void closeEditor() = 0;
    virtual bool isEditorOpen() const = 0;
    // The state may differ from the last saveState(): the plugin said so (CLAP's
    // state.mark_dirty), or its editor was open since. saveState() clears it.
    bool isStateDirty() const { return stateDirty || isEditorOpen(); }
    // The window was resized to width x height; the editor may round the size, which it
    // returns.
    virtual void resizeEditor(uint32_t& width, uint32_t& height) { (void)width, (void)height; }
    virtual bool canResizeEditor() { return false; }
    // The editor asked for a size (it applies on the next idle); false when it hasn't.
    bool takeEditorResizeRequest(uint32_t& width, uint32_t& height)
    {
        if (!resizeRequested)
            return false;
        resizeRequested = false;
        width = requestedWidth;
        height = requestedHeight;
        return true;
    }
    // The editor closed itself.
    bool takeEditorClosed()
    {
        const bool closed = editorClosed;
        editorClosed = false;
        return closed;
    }

    virtual void idle() = 0;

  protected:
    void requestEditorSize(uint32_t width, uint32_t height)
    {
        requestedWidth = width;
        requestedHeight = height;
        resizeRequested = true;
    }
    void editorClosedItself() { editorClosed = true; }
    void markStateDirty() { stateDirty = true; }
    void clearStateDirty() { stateDirty = false; }

  private:
    bool resizeRequested = false;
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
    bool editorClosed = false;
    bool stateDirty = false;
};

// path: a .clap file (Linux) or bundle (macOS); id: the plugin in it, empty for the first.
std::unique_ptr<Plugin> loadClapPlugin(const std::string& path, const std::string& id, std::string& error);
// uri: the plugin's URI, found in LV2_PATH (or the standard places).
std::unique_ptr<Plugin> loadLv2Plugin(const std::string& uri, std::string& error);

// What --list-plugins prints: instruments on this system.
struct Found
{
    std::string format; // "clap" or "lv2"
    std::string name;
    std::string location; // the .clap path or the LV2 URI
    std::string id;       // CLAP id (empty for LV2)
};
std::vector<Found> findClapPlugins();
std::vector<Found> findLv2Plugins();
} // namespace plugins
