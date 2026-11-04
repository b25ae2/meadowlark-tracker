// Meadowlark with an SDL3 window: the software FT812's frames scaled into the window, and
// keyboard, gamepad, joystick and mouse input mapped to the Tracker Mini's buttons through
// input.toml. Sound goes to PipeWire on Linux and through SDL elsewhere.
//
//   meadowlark [--sd-root dir | --no-sd] [--input-config file] [--trace-input]
//              [--scale n] [--fullscreen] [--print-input-config]
//              [--audio pipewire|sdl|none] [--audio-buffer frames] [--no-host-folders]
//
// Closing the window holds the power button, so core saves the project to the workspace and
// powers off, as on the Mini. A second close quits at once.

#include "AudioLock.h"
#include "CrashHandler.h"
#include "PlatformDesktop.h"
#include "PngWriter.h"
#include "Tracker.h"
#include "audio/AudioRenderer.h"
#include "audio/SdlAudioOutput.h"
#include "input/Actions.h"
#include "input/InputConfig.h"
#include "input/InputResolver.h"
#include "input/SdlInput.h"

#ifdef MEADOWLARK_HAVE_PIPEWIRE
#include "audio/PipeWireOutput.h"
#endif

#include <SDL3/SDL.h>

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

namespace
{
constexpr int kAudioBlockSamples = 128;
constexpr double kSampleRate = 44100.0;
// Core gets this long to finish the power-off sequence after the window is closed.
constexpr uint64_t kQuitTimeoutMs = 6000;

PlatformDesktop platform;

#ifdef MEADOWLARK_HAVE_PIPEWIRE
constexpr const char* kDefaultAudio = "pipewire";
#else
constexpr const char* kDefaultAudio = "sdl";
#endif

struct Options
{
    std::string sdRoot = DesktopFileSystem::defaultRoot();
    std::string inputConfig = input::defaultInputConfigPath();
    bool traceInput = false;
    bool fullscreen = false;
    int scale = 1;
    std::string audio = kDefaultAudio;
    int audioBuffer = 512; // frames, about 12 ms
    bool hostFolders = true;
};

void printUsage()
{
    std::printf("usage: meadowlark [--sd-root dir | --no-sd] [--input-config file] [--trace-input]\n"
                "                  [--scale n] [--fullscreen] [--print-input-config]\n"
                "                  [--audio pipewire|sdl|none] [--audio-buffer frames] [--no-host-folders]\n");
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
    void updateScaleMode();
    void present();
    void startAudio();
    void stopAudio();
    void renderAudioBlock(int16_t* left, int16_t* right, int frames);
    void renderAudioInline();

    Options options;
    input::InputResolver resolver;
    input::SdlInput sdlInput;
    Tracker* tracker = nullptr;

    AudioRenderer audioRenderer;
    std::unique_ptr<AudioOutput> audioOutput;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
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
    stopAudio();
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
    SDL_SetRenderLogicalPresentation(renderer, eve::kScreenWidth, eve::kScreenHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);

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
    updateScaleMode();

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

    tracker = Tracker::getInstance();
    tracker->getStartDisplayFunc()(&platform);
    tracker->init(&platform);
    nextAudioBlock = std::chrono::steady_clock::now();
    startAudio();
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

void App::stopAudio()
{
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
    AudioLock::acquire();
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
            updateScaleMode();
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

// Whole-number scales stay sharp with nearest-neighbour; anything else is filtered so the
// pixel rows don't come out uneven.
void App::updateScaleMode()
{
    int width = 0;
    int height = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &width, &height);
    const int scaleX = width / eve::kScreenWidth;
    const int scaleY = height / eve::kScreenHeight;
    const int scale = scaleX < scaleY ? scaleX : scaleY;
    const bool exact = scale > 0 && (width == scale * eve::kScreenWidth || height == scale * eve::kScreenHeight);
    SDL_SetTextureScaleMode(texture, exact ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
}

void App::present()
{
    const eve::Chip& chip = platform.desktopDisplay().eve();
    SDL_UpdateTexture(texture, nullptr, chip.frame(), eve::kScreenWidth * sizeof(uint32_t));
    presentedFrame = chip.frameCount();
    needsPresent = false;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
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
        else if (std::strcmp(arg, "--audio") == 0 && i + 1 < argc)
            options.audio = argv[++i];
        else if (std::strcmp(arg, "--audio-buffer") == 0 && i + 1 < argc)
            options.audioBuffer = std::atoi(argv[++i]);
        else if (std::strcmp(arg, "--no-host-folders") == 0)
            options.hostFolders = false;
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
