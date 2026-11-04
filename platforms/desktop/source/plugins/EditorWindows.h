#pragma once

// Windows for plugin editors, one per open editor, each holding the plugin's own view.
//
// Linux: plain X11 windows on a display connection of their own, since plugin editors are
// X11 views (under Wayland they show through XWayland, whatever SDL's own window uses).
// macOS: SDL windows, the editor's NSView added to the window's content view.

#include "plugins/Plugin.h"

#include <memory>
#include <string>

union SDL_Event;

namespace plugins
{
class EditorWindows
{
  public:
    EditorWindows();
    ~EditorWindows();

    // Opens the plugin's editor, or closes it if it is open. Problems go to stderr.
    void toggle(Plugin& plugin);
    // An SDL event for one of the editor windows (true: it was, and is handled).
    bool handleEvent(const SDL_Event& event);
    // Main loop: the windows' events, and the editors' own size changes and closing.
    void idle();
    void closeAll();
    size_t openCount() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace plugins
