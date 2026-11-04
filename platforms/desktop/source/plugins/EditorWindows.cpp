#include "plugins/EditorWindows.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#else
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace plugins
{
namespace
{
constexpr uint32_t kDefaultWidth = 640;
constexpr uint32_t kDefaultHeight = 480;
} // namespace

#ifdef __APPLE__
struct EditorWindows::Impl
{
    struct Editor
    {
        Plugin* plugin;
        SDL_Window* window;
        uint32_t width;
        uint32_t height;
    };
    std::vector<Editor> editors;

    bool open(Plugin& plugin)
    {
        if (!plugin.hasEditor(EditorParent::Api::kCocoa))
        {
            std::fprintf(stderr, "meadowlark: %s has no editor\n", plugin.name().c_str());
            return false;
        }
        SDL_Window* window = SDL_CreateWindow(plugin.name().c_str(), kDefaultWidth, kDefaultHeight, SDL_WINDOW_HIDDEN);
        if (!window)
        {
            std::fprintf(stderr, "meadowlark: cannot create a window for %s: %s\n", plugin.name().c_str(), SDL_GetError());
            return false;
        }
        void* nsWindow = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        using GetView = void* (*)(void*, SEL);
        void* view = nsWindow ? reinterpret_cast<GetView>(objc_msgSend)(nsWindow, sel_registerName("contentView")) : nullptr;
        EditorParent parent{ EditorParent::Api::kCocoa, reinterpret_cast<uintptr_t>(view) };
        uint32_t width = kDefaultWidth;
        uint32_t height = kDefaultHeight;
        std::string error;
        if (!view || !plugin.openEditor(parent, width, height, error))
        {
            std::fprintf(stderr, "meadowlark: %s\n", view ? error.c_str() : "no content view for the editor");
            SDL_DestroyWindow(window);
            return false;
        }
        SDL_SetWindowResizable(window, plugin.canResizeEditor());
        SDL_SetWindowSize(window, static_cast<int>(width), static_cast<int>(height));
        SDL_ShowWindow(window);
        editors.push_back({ &plugin, window, width, height });
        return true;
    }

    void close(size_t index)
    {
        editors[index].plugin->closeEditor();
        SDL_DestroyWindow(editors[index].window);
        editors.erase(editors.begin() + static_cast<std::ptrdiff_t>(index));
    }

    bool handleEvent(const SDL_Event& event)
    {
        if (event.type < SDL_EVENT_WINDOW_FIRST || event.type > SDL_EVENT_WINDOW_LAST)
            return find(event.window.windowID) >= 0 && isWindowInput(event);
        const int index = find(event.window.windowID);
        if (index < 0)
            return false;
        Editor& editor = editors[static_cast<size_t>(index)];
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            close(static_cast<size_t>(index));
        else if (event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            uint32_t width = static_cast<uint32_t>(event.window.data1);
            uint32_t height = static_cast<uint32_t>(event.window.data2);
            if (width == editor.width && height == editor.height)
                return true;
            editor.plugin->resizeEditor(width, height);
            editor.width = width;
            editor.height = height;
            if (width != static_cast<uint32_t>(event.window.data1) || height != static_cast<uint32_t>(event.window.data2))
                SDL_SetWindowSize(editor.window, static_cast<int>(width), static_cast<int>(height));
        }
        return true;
    }

    // Keys and the mouse in an editor window are the editor's, not the tracker's.
    static bool isWindowInput(const SDL_Event& event)
    {
        switch (event.type)
        {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_TEXT_INPUT:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL: return true;
            default: return false;
        }
    }

    int find(SDL_WindowID id) const
    {
        for (size_t i = 0; i < editors.size(); i++)
        {
            if (SDL_GetWindowID(editors[i].window) == id)
                return static_cast<int>(i);
        }
        return -1;
    }

    void resize(Editor& editor, uint32_t width, uint32_t height)
    {
        editor.width = width;
        editor.height = height;
        SDL_SetWindowSize(editor.window, static_cast<int>(width), static_cast<int>(height));
    }

    void pump() {}
};
#else
struct EditorWindows::Impl
{
    struct Editor
    {
        Plugin* plugin;
        Window window;
        uint32_t width;
        uint32_t height;
    };
    std::vector<Editor> editors;
    Display* display = nullptr;
    Atom deleteWindow = 0;

    ~Impl()
    {
        if (display)
            XCloseDisplay(display);
    }

    bool open(Plugin& plugin)
    {
        if (!plugin.hasEditor(EditorParent::Api::kX11))
        {
            std::fprintf(stderr, "meadowlark: %s has no X11 editor\n", plugin.name().c_str());
            return false;
        }
        if (!display)
        {
            // Plugins use X from their own threads too.
            XInitThreads();
            display = XOpenDisplay(nullptr);
            if (!display)
            {
                std::fprintf(stderr, "meadowlark: plugin editors need X11 (or XWayland): cannot open the display\n");
                return false;
            }
            deleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
        }

        const int screen = DefaultScreen(display);
        Window window = XCreateSimpleWindow(display,
                                            RootWindow(display, screen),
                                            0,
                                            0,
                                            kDefaultWidth,
                                            kDefaultHeight,
                                            0,
                                            BlackPixel(display, screen),
                                            BlackPixel(display, screen));
        XStoreName(display, window, plugin.name().c_str());
        XClassHint* classHint = XAllocClassHint();
        if (classHint)
        {
            char name[] = "meadowlark-plugin";
            char className[] = "Meadowlark";
            classHint->res_name = name;
            classHint->res_class = className;
            XSetClassHint(display, window, classHint);
            XFree(classHint);
        }
        XSetWMProtocols(display, window, &deleteWindow, 1);
        XSelectInput(display, window, StructureNotifyMask);
        XFlush(display);

        EditorParent parent{ EditorParent::Api::kX11, static_cast<uint64_t>(window) };
        uint32_t width = kDefaultWidth;
        uint32_t height = kDefaultHeight;
        std::string error;
        if (!plugin.openEditor(parent, width, height, error))
        {
            std::fprintf(stderr, "meadowlark: %s\n", error.c_str());
            XDestroyWindow(display, window);
            XFlush(display);
            return false;
        }
        editors.push_back({ &plugin, window, width, height });
        setSize(editors.back(), width, height);
        XMapRaised(display, window);
        XFlush(display);
        return true;
    }

    // A fixed size unless the editor can be resized.
    void setSize(Editor& editor, uint32_t width, uint32_t height)
    {
        editor.width = std::max<uint32_t>(width, 1);
        editor.height = std::max<uint32_t>(height, 1);
        XSizeHints* hints = XAllocSizeHints();
        if (hints && !editor.plugin->canResizeEditor())
        {
            hints->flags = PMinSize | PMaxSize;
            hints->min_width = hints->max_width = static_cast<int>(editor.width);
            hints->min_height = hints->max_height = static_cast<int>(editor.height);
            XSetWMNormalHints(display, editor.window, hints);
        }
        if (hints)
            XFree(hints);
        XResizeWindow(display, editor.window, editor.width, editor.height);
    }

    void resize(Editor& editor, uint32_t width, uint32_t height)
    {
        setSize(editor, width, height);
        XFlush(display);
    }

    void close(size_t index)
    {
        editors[index].plugin->closeEditor();
        XDestroyWindow(display, editors[index].window);
        XFlush(display);
        editors.erase(editors.begin() + static_cast<std::ptrdiff_t>(index));
    }

    bool handleEvent(const SDL_Event&) { return false; }

    int find(Window window) const
    {
        for (size_t i = 0; i < editors.size(); i++)
        {
            if (editors[i].window == window)
                return static_cast<int>(i);
        }
        return -1;
    }

    void pump()
    {
        if (!display)
            return;
        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);
            const int index = find(event.xany.window);
            if (index < 0)
                continue;
            Editor& editor = editors[static_cast<size_t>(index)];
            if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == deleteWindow)
                close(static_cast<size_t>(index));
            else if (event.type == ConfigureNotify && editor.plugin->canResizeEditor())
            {
                uint32_t width = static_cast<uint32_t>(event.xconfigure.width);
                uint32_t height = static_cast<uint32_t>(event.xconfigure.height);
                if (width == editor.width && height == editor.height)
                    continue;
                editor.plugin->resizeEditor(width, height);
                editor.width = width;
                editor.height = height;
                if (width != static_cast<uint32_t>(event.xconfigure.width) ||
                    height != static_cast<uint32_t>(event.xconfigure.height))
                    resize(editor, width, height);
            }
        }
    }
};
#endif

EditorWindows::EditorWindows() : impl(std::make_unique<Impl>()) {}

EditorWindows::~EditorWindows()
{
    closeAll();
}

void EditorWindows::toggle(Plugin& plugin)
{
    for (size_t i = 0; i < impl->editors.size(); i++)
    {
        if (impl->editors[i].plugin == &plugin)
        {
            impl->close(i);
            return;
        }
    }
    impl->open(plugin);
}

bool EditorWindows::handleEvent(const SDL_Event& event)
{
    return impl->handleEvent(event);
}

void EditorWindows::idle()
{
    impl->pump();
    for (size_t i = 0; i < impl->editors.size();)
    {
        auto& editor = impl->editors[i];
        if (editor.plugin->takeEditorClosed())
        {
            impl->close(i);
            continue;
        }
        uint32_t width;
        uint32_t height;
        if (editor.plugin->takeEditorResizeRequest(width, height) && (width != editor.width || height != editor.height))
            impl->resize(editor, width, height);
        i++;
    }
}

void EditorWindows::closeAll()
{
    while (!impl->editors.empty())
        impl->close(impl->editors.size() - 1);
}

size_t EditorWindows::openCount() const
{
    return impl->editors.size();
}
} // namespace plugins
