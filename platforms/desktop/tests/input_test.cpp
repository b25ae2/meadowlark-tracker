// Input layer checks: DesktopButtons against the Keys driver's timing, the resolver's
// chord and counting rules, input names, and input.toml loading.
//
//   input_test <scratch dir>

#include "DesktopButtons.h"
#include "input/Actions.h"
#include "input/InputConfig.h"
#include "input/InputResolver.h"
#include "input/SdlInput.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

class FakeTimer : public Timer
{
  public:
    uint32_t now = 1000;
    uint32_t millis() override { return now; }
    uint32_t micros() override { return now * 1000; }
    void delay(int ms) override { now += ms; }
    uint32_t clockCycles() override { return now; }
    uint32_t clockCyclesPerUs() override { return 1; }
};

// --- DesktopButtons --------------------------------------------------------------------
DesktopButtons* buttonsUnderTest = nullptr;
std::vector<std::string> buttonEvents;

void onPush(uint8_t n)
{
    // Like the Keys driver, the button reads as not pressed during its own push callback.
    buttonEvents.push_back("push" + std::to_string(n) + (buttonsUnderTest->isButtonPressed(n) ? "!" : ""));
}

void onRelease(uint8_t n)
{
    buttonEvents.push_back("release" + std::to_string(n) + (buttonsUnderTest->isButtonPressed(n) ? "" : "!"));
}

void onHold(uint8_t n)
{
    buttonEvents.push_back("hold" + std::to_string(n));
}

void testButtons()
{
    FakeTimer timer;
    DesktopButtons buttons(&timer);
    buttonsUnderTest = &buttons;
    buttons.setActionCallback(SwitchGroup::kPress, onPush);
    buttons.setActionCallback(SwitchGroup::kRelease, onRelease);
    buttons.setActionCallback(SwitchGroup::kHold, onHold);

    buttons.press(23);
    CHECK(buttons.isButtonPressed(23));
    buttons.press(23); // a second input bound to the same button
    CHECK(buttonEvents.size() == 1 && buttonEvents[0] == "push23");

    // Hold timing: first after > 333 ms, then 233, 143, 98, 78, 63, 63 ... ms.
    std::vector<uint32_t> holdTimes;
    for (uint32_t t = 0; t < 1200; t++)
    {
        timer.now = 1000 + t;
        size_t before = buttonEvents.size();
        buttons.update();
        if (buttonEvents.size() != before)
            holdTimes.push_back(t);
    }
    std::vector<uint32_t> expected = { 334 };
    const uint32_t steps[] = { 233, 143, 98, 78, 63 };
    for (uint32_t step : steps)
        expected.push_back(expected.back() + step + 1);
    while (expected.back() + 64 < 1200)
        expected.push_back(expected.back() + 64);
    CHECK(holdTimes == expected);

    buttonEvents.clear();
    buttons.release(23);
    CHECK(buttonEvents.empty());
    buttons.release(23);
    CHECK(buttonEvents.size() == 1 && buttonEvents[0] == "release23");
    CHECK(!buttons.isButtonPressed(23));
    buttons.release(23); // unbalanced release is ignored
    CHECK(buttonEvents.size() == 1);

    buttonEvents.clear();
    buttons.setHoldTime(1200);
    buttons.press(29);
    timer.now += 1200;
    buttons.update();
    CHECK(buttonEvents.size() == 1);
    timer.now += 1;
    buttons.update();
    CHECK(buttonEvents.size() == 2 && buttonEvents[1] == "hold29");

    buttons.press(30);
    buttonEvents.clear();
    buttons.releaseAll();
    CHECK(buttonEvents.size() == 2);
    CHECK(!buttons.isButtonPressed(29) && !buttons.isButtonPressed(30));
    CHECK(!buttons.isButtonPressed(-1) && !buttons.isButtonPressed(99));
}

// --- InputResolver ---------------------------------------------------------------------
void testResolver()
{
    using namespace input;
    std::vector<std::string> events;
    InputResolver resolver;
    resolver.setSink([&](ActionId action, bool pressed) {
        events.push_back(actionName(action) + (pressed ? "+" : "-"));
    });

    constexpr InputId kA = 1, kB = 2, kSelect = 3, kStart = 4;
    ActionId play = 0, rec = 0, menu = 0, quit = 0;
    CHECK(parseAction("play", play) && parseAction("REC", rec) && parseAction("menu", menu) && parseAction("quit", quit));
    resolver.setBindings({
      { { kA }, play },
      { { kB }, play },
      { { kSelect }, rec },
      { { kSelect, kA }, menu },
      { { kSelect, kStart }, quit },
    });

    // Two bindings for one action: one press, one release.
    resolver.inputDown(kA);
    resolver.inputDown(kB);
    resolver.inputUp(kA);
    CHECK(resolver.isActionHeld(play));
    resolver.inputUp(kB);
    CHECK((events == std::vector<std::string>{ "play+", "play-" }));

    // Chord: select fires its own binding, then A completes the chord and is consumed.
    events.clear();
    resolver.inputDown(kSelect);
    resolver.inputDown(kA);
    resolver.inputUp(kSelect);
    resolver.inputUp(kA);
    CHECK((events == std::vector<std::string>{ "rec+", "menu+", "rec-", "menu-" }));

    // Chord in the other order: select completes it, A had already fired.
    events.clear();
    resolver.inputDown(kA);
    resolver.inputDown(kSelect);
    resolver.inputUp(kSelect);
    resolver.inputUp(kA);
    CHECK((events == std::vector<std::string>{ "play+", "menu+", "menu-", "play-" }));

    // Repeated down is ignored; releaseAll releases everything once.
    events.clear();
    resolver.inputDown(kSelect);
    resolver.inputDown(kSelect);
    resolver.inputDown(kStart);
    resolver.releaseAll();
    CHECK((events == std::vector<std::string>{ "rec+", "quit+", "rec-", "quit-" }));
    CHECK(!resolver.isDown(kSelect));
    resolver.inputUp(kSelect);
    CHECK(events.size() == 4);

    ActionId button = 0;
    CHECK(parseAction("button43", button) && button == 43);
    CHECK(!parseAction("button44", button));
    CHECK(!parseAction("button", button));
    CHECK(!parseAction("nope", button));
    CHECK(parseAction("vol", button) && actionName(button) == "fx2");
    CHECK(parseAction("screen1", button) && button == 0 && actionName(0) == "screen1");
    CHECK(parseAction("Screen8", button) && button == 7);
}

// --- input names -----------------------------------------------------------------------
void testNames()
{
    using namespace input;
    const char* roundTrip[] = {
        "key:A",          "key:Left Shift",  "key:Keypad +",  "key:=",           "key:F12",
        "joy:button0",    "joy:button17",    "joy:axis1+",    "joy:axis3-",      "joy:hat0up",
        "joy:hat1right",  "pad:a",           "pad:dpdown",    "pad:leftx-",      "pad:righty+",
        "pad:lefttrigger", "mouse:left",     "mouse:x2",      "mouse:wheelup",   "mouse:moveleft",
    };
    for (const char* name : roundTrip)
    {
        InputId id = 0;
        bool parsed = parseInput(name, id);
        CHECK(parsed);
        if (parsed && inputName(id) != name)
        {
            std::printf("FAIL name round trip: %s -> %s\n", name, inputName(id).c_str());
            failures++;
        }
    }

    InputId a = 0, b = 0;
    CHECK(parseInput("KEY:left shift", a) && parseInput("key:Left Shift", b) && a == b);
    CHECK(parseInput("Pad:A", a) && parseInput("pad:a", b) && a == b);

    const char* invalid[] = {
        "A", "key:", "key:NoSuchKey", "joy:axis1", "joy:button", "joy:hat0", "joy:buttonx",
        "pad:a+", "pad:leftx", "pad:lefttrigger-", "pad:nope", "mouse:top", "mouse:wheel",
        "tablet:pen",
    };
    for (const char* name : invalid)
    {
        InputId id = 0;
        if (parseInput(name, id))
        {
            std::printf("FAIL accepted invalid input name %s\n", name);
            failures++;
        }
    }
}

// --- input.toml ------------------------------------------------------------------------
bool hasBinding(const input::InputConfig& config, const char* action, std::vector<const char*> names)
{
    input::ActionId id = 0;
    if (!input::parseAction(action, id))
        return false;
    std::vector<input::InputId> inputs;
    for (const char* name : names)
    {
        input::InputId input = 0;
        if (!input::parseInput(name, input))
            return false;
        inputs.push_back(input);
    }
    for (const input::Binding& binding : config.bindings)
    {
        if (binding.action == id && binding.inputs == inputs)
            return true;
    }
    return false;
}

size_t bindingCount(const input::InputConfig& config, const char* action)
{
    input::ActionId id = 0;
    input::parseAction(action, id);
    size_t count = 0;
    for (const input::Binding& binding : config.bindings)
        count += binding.action == id;
    return count;
}

bool writeFile(const std::string& path, const char* text)
{
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file)
        return false;
    std::fputs(text, file);
    return std::fclose(file) == 0;
}

void testConfig(const std::string& dir)
{
    // Fails if any built-in binding doesn't parse.
    input::InputConfig config;
    CHECK(input::loadInputConfig("", config));
    CHECK(hasBinding(config, "play", { "key:Space" }));
    CHECK(hasBinding(config, "screen1", { "key:1" }) && hasBinding(config, "screen8", { "key:8" }));
    CHECK(hasBinding(config, "note", { "key:F1" }) && hasBinding(config, "fx2", { "key:F4" }));
    CHECK(hasBinding(config, "note", { "key:[" }) && hasBinding(config, "fx2", { "key:-" }));
    CHECK(hasBinding(config, "fn1", { "key:Tab" }) && hasBinding(config, "fn4", { "key:E" }));
    CHECK(hasBinding(config, "minus10", { "key:9" }) && hasBinding(config, "plus1", { "key:P" }));
    CHECK(hasBinding(config, "paste", { "key:V" }));
    CHECK(hasBinding(config, "middle", { "pad:a" }) && hasBinding(config, "up", { "pad:lefty-" }));
    CHECK(hasBinding(config, "up", { "mouse:wheelup" }) && hasBinding(config, "down", { "mouse:wheeldown" }));
    CHECK(hasBinding(config, "shift", { "key:Right Shift" }) && hasBinding(config, "shift", { "pad:y" }));
    CHECK(config.gamepadMappings.size() == 1);
    CHECK(hasBinding(config, "quit", { "key:Left Ctrl", "key:Q" }));
    CHECK(bindingCount(config, "power") == 0);
    CHECK(config.options.axisPress == 0.5f && !config.captureMouse);

    const std::string path = dir + "/input.toml";
    CHECK(writeFile(path,
                    "[options]\n"
                    "axis_press = 0.6\n"
                    "capture_mouse = true\n"
                    "gamepad_mappings = []\n"
                    "[bindings]\n"
                    "play = [\"pad:a\", \"joy:button3\", [\"pad:back\", \"pad:start\"]]\n"
                    "rec = []\n"
                    "menu = [\"key:Escape\", \"key:NoSuchKey\"]\n"
                    "frobnicate = [\"key:F\"]\n"
                    "button5 = [\"mouse:wheeldown\"]\n"));
    std::printf("(the next two warnings are expected)\n");
    std::fflush(stdout);
    CHECK(input::loadInputConfig(path, config));
    CHECK(config.options.axisPress > 0.59f && config.options.axisPress < 0.61f && config.captureMouse);
    CHECK(config.gamepadMappings.empty());
    CHECK(bindingCount(config, "play") == 3);
    CHECK(hasBinding(config, "play", { "pad:back", "pad:start" }));
    CHECK(!hasBinding(config, "play", { "key:Space" }));
    CHECK(bindingCount(config, "rec") == 0);
    CHECK(bindingCount(config, "menu") == 1);
    CHECK(hasBinding(config, "button5", { "mouse:wheeldown" }));
    CHECK(hasBinding(config, "up", { "key:Up" })); // untouched default

    CHECK(writeFile(path, "[options]\ndefaults = false\n[bindings]\nplay = [\"key:P\"]\n"));
    CHECK(input::loadInputConfig(path, config));
    CHECK(config.bindings.size() == 1 && hasBinding(config, "play", { "key:P" }));

    CHECK(writeFile(path, "[bindings\nplay = 1\n"));
    std::printf("(the next parse error is expected)\n");
    std::fflush(stdout);
    CHECK(!input::loadInputConfig(path, config));
    CHECK(hasBinding(config, "play", { "key:Space" }));
    CHECK(hasBinding(config, "screen1", { "key:1" }) && hasBinding(config, "screen8", { "key:8" }));
    CHECK(hasBinding(config, "note", { "key:F1" }) && hasBinding(config, "fx2", { "key:F4" }));
    CHECK(hasBinding(config, "fn1", { "key:Tab" }));
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: input_test <scratch dir>\n");
        return 2;
    }
    ::mkdir(argv[1], 0755);
    testButtons();
    testResolver();
    testNames();
    testConfig(argv[1]);
    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all input checks passed\n");
    return 0;
}
