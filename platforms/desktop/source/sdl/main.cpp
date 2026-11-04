// Meadowlark with an SDL3 window: the software FT812's frames scaled into the window, and
// keyboard, gamepad, joystick and mouse input mapped to the Tracker Mini's buttons through
// input.toml. Sound goes to PipeWire on Linux and through SDL elsewhere; MIDI to the ALSA
// sequencer or CoreMIDI, connected as midi.toml says. Instrument plugins (CLAP, LV2) from
// plugins.toml play the MIDI instruments, their editors in windows of their own.
//
//   meadowlark [--sd-root dir | --no-sd] [--input-config file] [--trace-input]
//              [--scale n] [--scale-mode sharp|nearest|smooth|integer] [--fullscreen]
//              [--print-input-config]
//              [--audio pipewire|sdl|none] [--audio-buffer frames] [--no-host-folders]
//              [--audio-input auto|always|off]
//              [--midi-config file] [--no-midi] [--list-midi] [--print-midi-config]
//              [--font-config file] [--print-font-config]
//              [--plugin-config file] [--no-plugins] [--list-plugins] [--print-plugin-config]
//
// Closing the window holds the power button, so core saves the project to the workspace and
// powers off, as on the Mini. A second close quits at once.

#include "AudioLock.h"
#include "CrashHandler.h"
#include "PlatformDesktop.h"
#include "PngWriter.h"
#include "Tracker.h"
#include "audio/AudioCapture.h"
#include "audio/AudioRenderer.h"
#include "audio/SdlAudioInput.h"
#include "audio/SdlAudioOutput.h"
#include "ConfigPaths.h"
#include "fonts/FontConfig.h"
#include "input/Actions.h"
#include "input/InputConfig.h"
#include "input/InputResolver.h"
#include "input/SdlInput.h"
#include "midi/MidiBackend.h"
#include "midi/MidiConfig.h"
#include "midi/MidiScheduler.h"
#include "mtStructs.h"
#include "plugins/EditorWindows.h"
#include "plugins/PluginConfig.h"
#include "plugins/PluginHost.h"

#ifdef MEADOWLARK_HAVE_PIPEWIRE
#include "audio/PipeWireInput.h"
#include "audio/PipeWireOutput.h"
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// hardware.cpp: the firmware's own output mute (startup, power-off).
uint8_t isAudioOutMuted();
// mtSequencer.cpp: an instrument's MIDI channel, kNoChannel (-1) for a sample instrument.
int instrumentIdxToMidiChannel(int instrument);

namespace
{
constexpr int kAudioBlockSamples = 128;
constexpr double kSampleRate = 44100.0;
// Core gets this long to finish the power-off sequence after the window is closed.
constexpr uint64_t kQuitTimeoutMs = 6000;
// How often plugins get their main-thread work (timers, editors).
constexpr uint64_t kPluginIdleMs = 10;

PlatformDesktop platform;

#ifdef MEADOWLARK_HAVE_PIPEWIRE
constexpr const char* kDefaultAudio = "pipewire";
#else
constexpr const char* kDefaultAudio = "sdl";
#endif

// How the 800x480 screen fills the window (App::layoutScreen).
enum class ScaleMode
{
    kSharp,   // whole-number scale with pixels copied, then smoothly to the window's size
    kNearest, // pixels copied at any scale: no blur, uneven at fractional scales
    kSmooth,  // filtered: even, soft at fractional scales
    kInteger, // the largest whole-number scale that fits, black borders around it
};

bool parseScaleMode(const char* name, ScaleMode& mode)
{
    static const struct
    {
        const char* name;
        ScaleMode mode;
    } kModes[] = { { "sharp", ScaleMode::kSharp },
                   { "nearest", ScaleMode::kNearest },
                   { "smooth", ScaleMode::kSmooth },
                   { "integer", ScaleMode::kInteger } };
    for (const auto& entry : kModes)
    {
        if (std::strcmp(name, entry.name) == 0)
        {
            mode = entry.mode;
            return true;
        }
    }
    return false;
}

struct Options
{
    std::string sdRoot = DesktopFileSystem::defaultRoot();
    std::string inputConfig = input::defaultInputConfigPath();
    bool traceInput = false;
    bool fullscreen = false;
    int scale = 1;
    ScaleMode scaleMode = ScaleMode::kSharp;
    std::string audio = kDefaultAudio;
    int audioBuffer = 512; // frames, about 12 ms
    // auto: open the capture device when the Sample Recorder first configures its input.
    std::string audioInput = "auto";
    bool hostFolders = true;
    std::string midiConfig = midi::defaultMidiConfigPath();
    bool midi = true;
    std::string fontConfig = fonts::defaultFontConfigPath();
    std::string pluginConfig = plugins::defaultPluginConfigPath();
    bool plugins = true;
};

void printUsage()
{
    std::printf("usage: meadowlark [--sd-root dir | --no-sd] [--input-config file] [--trace-input]\n"
                "                  [--scale n] [--scale-mode sharp|nearest|smooth|integer] [--fullscreen]\n"
                "                  [--print-input-config]\n"
                "                  [--audio pipewire|sdl|none] [--audio-buffer frames] [--no-host-folders]\n"
                "                  [--audio-input auto|always|off]\n"
                "                  [--midi-config file] [--no-midi] [--list-midi] [--print-midi-config]\n"
                "                  [--font-config file] [--print-font-config]\n"
                "                  [--plugin-config file] [--no-plugins] [--list-plugins] [--print-plugin-config]\n");
}

class App
{
  public:
    App(const Options& options) : options(options), sdlInput(resolver) {}
    ~App();

    bool init();
    void run();

  private:
    void handleAction(input::ActionId action, bool pressed);
    void handleEvent(const SDL_Event& event);
    void requestQuit();
    void toggleFullscreen();
    void saveScreenshot();
    void layoutScreen();
    void present();
    void startAudio();
    void stopAudio();
    void startCapture();
    void startMidi();
    void stopMidi();
    void renderAudioBlock(int16_t* left, int16_t* right, int frames);
    void renderAudioInline();
    void togglePluginEditor();

    Options options;
    input::InputResolver resolver;
    input::SdlInput sdlInput;
    Tracker* tracker = nullptr;

    AudioRenderer audioRenderer;
    std::unique_ptr<AudioOutput> audioOutput;
    AudioCaptureBuffer captureBuffer;
    std::unique_ptr<AudioInput> audioInput;
    std::atomic<bool> captureActive{ false };
    // Render thread only: the block core reads as its I2S input, and the USB input's.
    int16_t inputBlock[kAudioBlockSamples * 2] = {};
    int16_t usbInputBlock[kAudioBlockSamples * 2] = {};

    std::unique_ptr<midi::MidiBackend> midiBackend;
    std::unique_ptr<midi::MidiConnector> midiConnector;
    midi::MidiScheduler midiScheduler;
    uint64_t nextMidiScan = 0;

    plugins::PluginHost pluginHost;
    plugins::EditorWindows editorWindows;
    uint64_t nextPluginIdle = 0;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    // Sharp scaling's intermediate: the screen at a whole-number scale, nearest-neighbour.
    SDL_Texture* upscaled = nullptr;
    int upscale = 0;
    SDL_FRect screenRect{};
    uint64_t presentedFrame = 0;
    bool needsPresent = true;

    bool running = true;
    bool quitting = false;
    uint64_t quitDeadline = 0;

    std::chrono::steady_clock::time_point nextAudioBlock;
    int16_t left[kAudioBlockSamples];
    int16_t right[kAudioBlockSamples];
};

App::~App()
{
    // Audio first: the render thread sends MIDI.
    stopAudio();
    stopMidi();
    // Editors before their plugins, which go with the host.
    editorWindows.closeAll();
    if (upscaled)
        SDL_DestroyTexture(upscaled);
    if (texture)
        SDL_DestroyTexture(texture);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
}

bool App::init()
{
    input::InputConfig inputConfig;
    input::loadInputConfig(options.inputConfig, inputConfig);
    sdlInput.setOptions(inputConfig.options);
    sdlInput.setTrace(options.traceInput);
    resolver.setBindings(std::move(inputConfig.bindings));
    resolver.setSink([this](input::ActionId action, bool pressed) { handleAction(action, pressed); });

    SDL_SetAppMetadata("Meadowlark", nullptr, "org.meadowlark.tracker");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        std::fprintf(stderr, "meadowlark: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    // Before the first events, which announce the joysticks.
    for (const std::string& mapping : inputConfig.gamepadMappings)
    {
        if (SDL_AddGamepadMapping(mapping.c_str()) < 0)
            std::fprintf(stderr, "meadowlark: bad gamepad mapping \"%s\": %s\n", mapping.c_str(), SDL_GetError());
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    const int scale = options.scale > 0 ? options.scale : 1;
    window = SDL_CreateWindow("Meadowlark", eve::kScreenWidth * scale, eve::kScreenHeight * scale, flags);
    if (!window)
    {
        std::fprintf(stderr, "meadowlark: cannot create window: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(window, eve::kScreenWidth / 2, eve::kScreenHeight / 2);

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        std::fprintf(stderr, "meadowlark: cannot create renderer: %s\n", SDL_GetError());
        return false;
    }
    texture = SDL_CreateTexture(renderer,
                                SDL_PIXELFORMAT_XRGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                eve::kScreenWidth,
                                eve::kScreenHeight);
    if (!texture)
    {
        std::fprintf(stderr, "meadowlark: cannot create texture: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    layoutScreen();

    if (inputConfig.captureMouse)
        SDL_SetWindowRelativeMouseMode(window, true);

    std::printf("meadowlark: SDL %d.%d.%d, video %s, renderer %s\n",
                SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
                SDL_GetCurrentVideoDriver(),
                SDL_GetRendererName(renderer));

    if (options.hostFolders)
        platform.desktopFileSystem().addDefaultHostMounts();
    if (!platform.desktopFileSystem().setRoot(options.sdRoot))
    {
        std::fprintf(stderr, "meadowlark: cannot use %s as the SD card\n", options.sdRoot.c_str());
        return false;
    }
    std::printf("meadowlark: SD card %s\n", options.sdRoot.empty() ? "(none)" : options.sdRoot.c_str());

    // Before core starts: it lists /Fonts in its start tasks.
    fonts::FontConfig fontConfig;
    fonts::loadFontConfig(options.fontConfig, fontConfig);
    fonts::installFonts(fontConfig, userCachePath("fonts"), platform.desktopFileSystem());

    // Before core starts: its audio engine takes the instruments when it starts.
    if (options.plugins)
    {
        plugins::PluginConfig pluginConfig;
        plugins::loadPluginConfig(options.pluginConfig, pluginConfig);
        if (pluginHost.load(pluginConfig) > 0)
        {
            pluginHost.setCardRoot(platform.desktopFileSystem().root());
            platform.setExternalInstruments(&pluginHost);
        }
    }

    tracker = Tracker::getInstance();
    tracker->getStartDisplayFunc()(&platform);
    tracker->init(&platform);
    nextAudioBlock = std::chrono::steady_clock::now();
    startMidi();
    startAudio();
    if (options.audioInput == "always")
        startCapture();
    else if (options.audioInput == "auto")
        platform.desktopCodec().armInputRequests();
    return true;
}

// Tries the chosen backend, then SDL. Without one, blocks render on the main loop and are
// dropped, so the engine still runs in time.
void App::startAudio()
{
    if (options.audio == "none")
    {
        std::printf("meadowlark: audio off\n");
        return;
    }

    audioRenderer.start([this](int16_t* left, int16_t* right, int frames) { renderAudioBlock(left, right, frames); },
                        options.audioBuffer);

    std::vector<std::string> backends = { options.audio };
    if (options.audio != "sdl")
        backends.push_back("sdl");
    for (const std::string& backend : backends)
    {
        std::unique_ptr<AudioOutput> output;
#ifdef MEADOWLARK_HAVE_PIPEWIRE
        if (backend == "pipewire")
            output = std::make_unique<PipeWireOutput>();
#endif
        if (backend == "sdl")
            output = std::make_unique<SdlAudioOutput>();
        if (!output)
        {
            std::fprintf(stderr, "meadowlark: audio backend \"%s\" is not available in this build\n", backend.c_str());
            continue;
        }
        if (output->start(audioRenderer))
        {
            audioOutput = std::move(output);
            std::printf("meadowlark: audio %s, %d frames buffered (%.1f ms)\n",
                        audioOutput->name(),
                        audioRenderer.bufferFrames(),
                        audioRenderer.bufferFrames() * 1000.0 / AudioRenderer::kSampleRate);
            return;
        }
    }

    std::fprintf(stderr, "meadowlark: no audio output, running silently\n");
    audioRenderer.stop();
}

// Same backend family as the output: PipeWire's capture with PipeWire output, else SDL's.
void App::startCapture()
{
    if (audioInput || !audioRenderer.isRunning())
        return;

    std::vector<std::string> backends;
    if (audioOutput && std::strcmp(audioOutput->name(), "pipewire") == 0)
        backends.push_back("pipewire");
    backends.push_back("sdl");
    for (const std::string& backend : backends)
    {
        std::unique_ptr<AudioInput> input;
#ifdef MEADOWLARK_HAVE_PIPEWIRE
        if (backend == "pipewire")
            input = std::make_unique<PipeWireInput>();
#endif
        if (backend == "sdl")
            input = std::make_unique<SdlAudioInput>();
        if (input && input->start(captureBuffer))
        {
            audioInput = std::move(input);
            captureActive = true;
            std::printf("meadowlark: audio input %s\n", audioInput->name());
            return;
        }
    }
    std::fprintf(stderr, "meadowlark: no audio input, recording gets silence\n");
}

// The firmware's two ports appear in the system as "Meadowlark USB" and "Meadowlark Jack";
// the connector links them to the devices midi.toml names, checking for new ones every
// second from the main loop.
void App::startMidi()
{
    if (!options.midi)
    {
        std::printf("meadowlark: midi off\n");
        return;
    }
    midi::MidiConfig config;
    midi::loadMidiConfig(options.midiConfig, config);
    if (!config.enabled)
    {
        std::printf("meadowlark: midi off (midi.toml)\n");
        return;
    }

    std::unique_ptr<midi::MidiBackend> backend = midi::createMidiBackend();
    if (!backend || !backend->start([](int port, const midi::Message& message) {
            platform.midiPort(port).input().push(message);
        }))
    {
        std::fprintf(stderr, "meadowlark: no MIDI\n");
        return;
    }
    midiBackend = std::move(backend);
    std::printf("meadowlark: midi %s\n", midiBackend->name());

    // Messages follow the audio: due when the block being rendered is heard. Without a sound
    // device the renderer never reads, so there is no play time and they go out at once.
    midiScheduler.start([this](int port, const midi::Message& message) { midiBackend->send(port, message); },
                        [this] { return audioRenderer.playTime(audioRenderer.framesRendered()); },
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double, std::milli>(config.outputOffsetMs)));
    for (int port = 0; port < midi::kPortCount; port++)
        platform.midiPort(port).setSink(&midiScheduler);

    midiConnector = std::make_unique<midi::MidiConnector>(*midiBackend, config);
    midiConnector->update();
    nextMidiScan = SDL_GetTicks() + 1000;
}

void App::stopMidi()
{
    if (!midiBackend)
        return;
    for (int port = 0; port < midi::kPortCount; port++)
        platform.midiPort(port).setSink(nullptr);
    midiScheduler.stop();
    midiConnector.reset();
    midiBackend->stop();
    midiBackend.reset();
    std::printf("meadowlark: midi %llu messages sent, worst %.1f ms late\n",
                static_cast<unsigned long long>(midiScheduler.messagesSent()),
                midiScheduler.worstLatenessUs() / 1000.0);
}

void App::stopAudio()
{
    if (audioInput)
    {
        captureActive = false;
        audioInput->stop();
        audioInput.reset();
        std::printf("meadowlark: audio input %llu frames, %llu underruns, %llu frames dropped\n",
                    static_cast<unsigned long long>(captureBuffer.framesCaptured()),
                    static_cast<unsigned long long>(captureBuffer.underruns()),
                    static_cast<unsigned long long>(captureBuffer.framesDropped()));
    }
    if (audioOutput)
    {
        audioOutput->stop();
        audioOutput.reset();
    }
    if (audioRenderer.isRunning())
    {
        audioRenderer.stop();
        std::printf("meadowlark: audio %llu blocks, %llu underruns, buffer %d frames (device asked for up to %zu)\n",
                    static_cast<unsigned long long>(audioRenderer.blocksRendered()),
                    static_cast<unsigned long long>(audioRenderer.underruns()),
                    audioRenderer.bufferFrames(),
                    audioRenderer.largestRead());
    }
}

// The audio interrupt's work: core renders a block with the main loop locked out, then the
// codec's volume and the firmware's output mute apply.
void App::renderAudioBlock(int16_t* left, int16_t* right, int frames)
{
    // The codec's input: one captured block, with the ADC gain, which core reads while it
    // renders this block (Input.cpp keeps the pointer, as with the I2S DMA buffer).
    const bool withInput = captureActive.load(std::memory_order_acquire);
    if (withInput)
    {
        captureBuffer.readBlock(inputBlock, static_cast<size_t>(frames));
        const float inputGain = platform.desktopCodec().inputGain();
        if (inputGain != 1.0f)
        {
            for (int i = 0; i < frames * 2; i++)
            {
                const long sample = std::lrintf(inputBlock[i] * inputGain);
                inputBlock[i] = static_cast<int16_t>(sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample));
            }
        }
    }

    platform.desktopTimer().markAudioBlockStart();
    AudioLock::acquire();
    if (withInput)
        tracker->updateInputAudioBuffers(inputBlock, usbInputBlock, frames);
    tracker->updateAudioBuffers(left, right, frames);
    AudioLock::release();

    const float gain = isAudioOutMuted() ? 0.0f : platform.desktopCodec().outputGain();
    if (gain == 1.0f)
        return;
    for (int i = 0; i < frames; i++)
    {
        left[i] = static_cast<int16_t>(std::lrintf(left[i] * gain));
        right[i] = static_cast<int16_t>(std::lrintf(right[i] * gain));
    }
}

void App::run()
{
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            handleEvent(event);

        if (!audioRenderer.isRunning())
            renderAudioInline();
        if (platform.desktopCodec().takeInputRequest())
            startCapture();
        if (midiConnector && SDL_GetTicks() >= nextMidiScan)
        {
            midiConnector->update();
            nextMidiScan = SDL_GetTicks() + 1000;
        }
        if (pluginHost.size() > 0 && SDL_GetTicks() >= nextPluginIdle)
        {
            pluginHost.idle();
            editorWindows.idle();
            nextPluginIdle = SDL_GetTicks() + kPluginIdleMs;
        }
        platform.desktopButtons().update();
        tracker->update();

        if (platform.powerOffRequested())
        {
            std::printf("meadowlark: powered off\n");
            break;
        }
        if (quitting && SDL_GetTicks() > quitDeadline)
        {
            std::fprintf(stderr, "meadowlark: core did not power off, quitting without saving\n");
            break;
        }

        if (platform.desktopDisplay().eve().frameCount() != presentedFrame || needsPresent)
            present();

        SDL_DelayNS(500 * 1000);
    }
}

void App::handleEvent(const SDL_Event& event)
{
    if (editorWindows.handleEvent(event))
        return;
    switch (event.type)
    {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            requestQuit();
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            sdlInput.releaseAll();
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            layoutScreen();
            needsPresent = true;
            break;

        case SDL_EVENT_WINDOW_EXPOSED:
            needsPresent = true;
            break;

        default:
            sdlInput.handleEvent(event);
            break;
    }
}

void App::handleAction(input::ActionId action, bool pressed)
{
    if (options.traceInput)
        std::printf("action: %s %s\n", input::actionName(action).c_str(), pressed ? "press" : "release");

    if (input::isButtonAction(action))
    {
        DesktopButtons& buttons = platform.desktopButtons();
        pressed ? buttons.press(action) : buttons.release(action);
        return;
    }

    switch (action)
    {
        case input::kActionPaste:
        {
            // The Mini pastes with Shift + Copy; Shift goes down first, as a player's would.
            static input::ActionId shift = 0;
            static input::ActionId copy = 0;
            if (!shift)
            {
                input::parseAction("shift", shift);
                input::parseAction("copy", copy);
            }
            DesktopButtons& buttons = platform.desktopButtons();
            if (pressed)
            {
                buttons.press(shift);
                buttons.press(copy);
            }
            else
            {
                buttons.release(copy);
                buttons.release(shift);
            }
            break;
        }
        case input::kActionPower:
            // While quitting, the power button stays held until core powers off.
            if (!quitting)
                platform.setPowerButton(pressed);
            break;
        case input::kActionFullscreen:
            if (pressed)
                toggleFullscreen();
            break;
        case input::kActionScreenshot:
            if (pressed)
                saveScreenshot();
            break;
        case input::kActionQuit:
            if (pressed)
                requestQuit();
            break;
        case input::kActionPluginEditor:
            if (pressed)
                togglePluginEditor();
            break;
        default:
            break;
    }
}

void App::requestQuit()
{
    if (quitting)
    {
        running = false;
        return;
    }
    quitting = true;
    quitDeadline = SDL_GetTicks() + kQuitTimeoutMs;
    std::printf("meadowlark: saving and powering off (close again to quit at once)\n");
    platform.setPowerButton(true);
}

// The editor of the plugin that the selected instrument plays.
void App::togglePluginEditor()
{
    const int channel = instrumentIdxToMidiChannel(mtProject.values.lastUsedInstrument);
    plugins::Plugin* plugin = channel > 0 ? pluginHost.pluginOnChannel(channel) : nullptr;
    if (!plugin)
    {
        if (channel > 0)
            std::printf("meadowlark: no plugin on MIDI channel %d (plugins.toml)\n", channel);
        else
            std::printf("meadowlark: select a MIDI instrument for its plugin editor\n");
        return;
    }
    editorWindows.toggle(*plugin);
}

void App::toggleFullscreen()
{
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window, !fullscreen);
}

void App::saveScreenshot()
{
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "meadowlark-%Y%m%d-%H%M%S.png", std::localtime(&now));
    const eve::Chip& chip = platform.desktopDisplay().eve();
    if (writePng(name, chip.frame(), eve::kScreenWidth, eve::kScreenHeight))
        std::printf("meadowlark: screenshot %s\n", name);
    else
        std::fprintf(stderr, "meadowlark: could not write %s\n", name);
}

// Where the screen goes in the window, centred at the largest scale that fits, and how it
// gets there. Sharp scaling copies pixels up to the next whole-number scale, then filters
// down to the window's size, so only the edges between pixels are blended and every pixel
// stays the same size; at whole-number scales it is a plain copy.
void App::layoutScreen()
{
    int width = 0;
    int height = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &width, &height);
    float scale = std::min(static_cast<float>(width) / eve::kScreenWidth, static_cast<float>(height) / eve::kScreenHeight);
    if (options.scaleMode == ScaleMode::kInteger && scale >= 1.0f)
        scale = std::floor(scale);
    const float w = std::round(eve::kScreenWidth * scale);
    const float h = std::round(eve::kScreenHeight * scale);
    screenRect = { std::floor((width - w) / 2), std::floor((height - h) / 2), w, h };

    const bool whole = scale == std::floor(scale);
    const bool filtered = options.scaleMode == ScaleMode::kSmooth ||
                          ((options.scaleMode == ScaleMode::kSharp || options.scaleMode == ScaleMode::kInteger) && !whole);
    SDL_SetTextureScaleMode(texture, filtered ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);

    // Sharp at a fractional scale above 1: the intermediate at the next whole scale.
    const int wanted = options.scaleMode == ScaleMode::kSharp && !whole && scale > 1.0f ? static_cast<int>(std::ceil(scale)) : 0;
    if (wanted != upscale)
    {
        if (upscaled)
            SDL_DestroyTexture(upscaled);
        upscaled = nullptr;
        upscale = 0;
        if (wanted > 0)
        {
            upscaled = SDL_CreateTexture(renderer,
                                         SDL_PIXELFORMAT_XRGB8888,
                                         SDL_TEXTUREACCESS_TARGET,
                                         eve::kScreenWidth * wanted,
                                         eve::kScreenHeight * wanted);
            if (upscaled)
            {
                SDL_SetTextureScaleMode(upscaled, SDL_SCALEMODE_LINEAR);
                upscale = wanted;
            }
            else
                std::fprintf(stderr, "meadowlark: sharp scaling unavailable (%s), filtering instead\n", SDL_GetError());
        }
    }
    if (upscaled)
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
}

void App::present()
{
    const eve::Chip& chip = platform.desktopDisplay().eve();
    SDL_UpdateTexture(texture, nullptr, chip.frame(), eve::kScreenWidth * sizeof(uint32_t));
    presentedFrame = chip.frameCount();
    needsPresent = false;

    if (upscaled)
    {
        SDL_SetRenderTarget(renderer, upscaled);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_SetRenderTarget(renderer, nullptr);
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, upscaled ? upscaled : texture, nullptr, &screenRect);
    SDL_RenderPresent(renderer);
}

// Without an output, blocks render on the main loop at the audio rate. After a stall
// (window drag, debugger) it skips ahead instead of rendering the backlog.
void App::renderAudioInline()
{
    using clock = std::chrono::steady_clock;
    const auto blockPeriod = std::chrono::duration_cast<clock::duration>(
      std::chrono::duration<double>(kAudioBlockSamples / kSampleRate));
    const auto now = clock::now();
    if (now - nextAudioBlock > std::chrono::milliseconds(100))
        nextAudioBlock = now;

    while (now >= nextAudioBlock)
    {
        renderAudioBlock(left, right, kAudioBlockSamples);
        nextAudioBlock += blockPeriod;
    }
}
} // namespace

int main(int argc, char** argv)
{
    installCrashHandler();
    // Line by line even into a pipe, so --trace-input can be watched through tee.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Options options;
    for (int i = 1; i < argc; i++)
    {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--sd-root") == 0 && i + 1 < argc)
            options.sdRoot = argv[++i];
        else if (std::strcmp(arg, "--no-sd") == 0)
            options.sdRoot.clear();
        else if (std::strcmp(arg, "--input-config") == 0 && i + 1 < argc)
            options.inputConfig = argv[++i];
        else if (std::strcmp(arg, "--trace-input") == 0)
            options.traceInput = true;
        else if (std::strcmp(arg, "--fullscreen") == 0)
            options.fullscreen = true;
        else if (std::strcmp(arg, "--scale") == 0 && i + 1 < argc)
            options.scale = std::atoi(argv[++i]);
        else if (std::strcmp(arg, "--scale-mode") == 0 && i + 1 < argc)
        {
            if (!parseScaleMode(argv[++i], options.scaleMode))
            {
                printUsage();
                return 2;
            }
        }
        else if (std::strcmp(arg, "--audio") == 0 && i + 1 < argc)
            options.audio = argv[++i];
        else if (std::strcmp(arg, "--audio-buffer") == 0 && i + 1 < argc)
            options.audioBuffer = std::atoi(argv[++i]);
        else if (std::strcmp(arg, "--audio-input") == 0 && i + 1 < argc)
            options.audioInput = argv[++i];
        else if (std::strcmp(arg, "--no-host-folders") == 0)
            options.hostFolders = false;
        else if (std::strcmp(arg, "--midi-config") == 0 && i + 1 < argc)
            options.midiConfig = argv[++i];
        else if (std::strcmp(arg, "--no-midi") == 0)
            options.midi = false;
        else if (std::strcmp(arg, "--font-config") == 0 && i + 1 < argc)
            options.fontConfig = argv[++i];
        else if (std::strcmp(arg, "--plugin-config") == 0 && i + 1 < argc)
            options.pluginConfig = argv[++i];
        else if (std::strcmp(arg, "--no-plugins") == 0)
            options.plugins = false;
        else if (std::strcmp(arg, "--print-plugin-config") == 0)
        {
            std::fputs(plugins::defaultPluginConfig(), stdout);
            return 0;
        }
        else if (std::strcmp(arg, "--list-plugins") == 0)
        {
            std::vector<plugins::Found> found = plugins::findClapPlugins();
            for (const plugins::Found& lv2 : plugins::findLv2Plugins())
                found.push_back(lv2);
            if (found.empty())
                std::printf("no instrument plugins found\n");
            for (const plugins::Found& plugin : found)
            {
                if (plugin.format == "clap")
                    std::printf("%s\n  clap = \"%s\"\n  id = \"%s\"\n", plugin.name.c_str(), plugin.location.c_str(), plugin.id.c_str());
                else
                    std::printf("%s\n  lv2 = \"%s\"\n", plugin.name.c_str(), plugin.location.c_str());
            }
            return 0;
        }
        else if (std::strcmp(arg, "--print-font-config") == 0)
        {
            std::fputs(fonts::defaultFontConfig(), stdout);
            return 0;
        }
        else if (std::strcmp(arg, "--print-midi-config") == 0)
        {
            std::fputs(midi::defaultMidiConfig(), stdout);
            return 0;
        }
        else if (std::strcmp(arg, "--list-midi") == 0)
        {
            midi::MidiConfig config;
            midi::loadMidiConfig(options.midiConfig, config);
            std::unique_ptr<midi::MidiBackend> backend = midi::createMidiBackend();
            if (!backend || !backend->start([](int, const midi::Message&) {}))
                return 1;
            midi::printMidiPorts(*backend, config);
            return 0;
        }
        else if (std::strcmp(arg, "--print-input-config") == 0)
        {
            std::fputs(input::defaultInputConfig(), stdout);
            return 0;
        }
        else
        {
            printUsage();
            return std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0 ? 0 : 2;
        }
    }

    int status = 0;
    {
        App app(options);
        if (app.init())
            app.run();
        else
            status = 1;
    }
    SDL_Quit();
    return status;
}
