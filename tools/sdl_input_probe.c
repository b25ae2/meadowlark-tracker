// sdl_input_probe: print how SDL3 sees joysticks, keyboard and mouse/trackball.
//
// Build: cc -O2 -o sdl_input_probe sdl_input_probe.c $(pkg-config --cflags --libs sdl3)
// Run from a desktop session (keyboard needs the probe window focused):
//   ./sdl_input_probe | tee probe.log
// F12 toggles relative mouse mode (trackball capture). Close the window or Ctrl+C to quit.

#include <SDL3/SDL.h>
#include <stdio.h>

static void printJoystickInfo(SDL_JoystickID id)
{
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    if (!joy)
    {
        printf("joystick %u: open failed: %s\n", (unsigned)id, SDL_GetError());
        return;
    }

    char guid[64];
    SDL_GUIDToString(SDL_GetJoystickGUID(joy), guid, sizeof(guid));

    printf("joystick %u added: \"%s\"\n", (unsigned)id, SDL_GetJoystickName(joy));
    printf("  path     : %s\n", SDL_GetJoystickPath(joy) ? SDL_GetJoystickPath(joy) : "?");
    printf("  vendor   : 0x%04x  product: 0x%04x\n",
           SDL_GetJoystickVendor(joy),
           SDL_GetJoystickProduct(joy));
    printf("  guid     : %s\n", guid);
    printf("  buttons  : %d  axes: %d  hats: %d\n",
           SDL_GetNumJoystickButtons(joy),
           SDL_GetNumJoystickAxes(joy),
           SDL_GetNumJoystickHats(joy));

    if (SDL_IsGamepad(id))
    {
        char* mapping = SDL_GetGamepadMappingForID(id);
        printf("  gamepad  : yes, mapping: %s\n", mapping ? mapping : "(none)");
        SDL_free(mapping);
    }
    else
    {
        printf("  gamepad  : no (raw joystick only)\n");
    }
    fflush(stdout);
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    printf("SDL %d.%d.%d, video driver: %s\n",
           SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
           SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
           SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
           SDL_GetCurrentVideoDriver());

    SDL_Window* window = SDL_CreateWindow("sdl_input_probe (focus me)", 640, 240, 0);
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    bool relativeMouse = false;
    bool running = true;

    while (running)
    {
        SDL_Event e;
        if (!SDL_WaitEvent(&e))
            continue;

        switch (e.type)
        {
            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_JOYSTICK_ADDED:
                printJoystickInfo(e.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                printf("joystick %u removed\n", (unsigned)e.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                printf("joy %u  btn%-2d %s\n",
                       (unsigned)e.jbutton.which,
                       e.jbutton.button,
                       e.jbutton.down ? "down" : "up");
                break;
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                printf("joy %u  axis%d %6d\n",
                       (unsigned)e.jaxis.which,
                       e.jaxis.axis,
                       e.jaxis.value);
                break;
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                printf("joy %u  hat%d 0x%02x\n", (unsigned)e.jhat.which, e.jhat.hat, e.jhat.value);
                break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (e.key.repeat)
                    break;
                printf("key  scancode %3d %-14s keycode %-14s mod 0x%04x %s\n",
                       e.key.scancode,
                       SDL_GetScancodeName(e.key.scancode),
                       SDL_GetKeyName(e.key.key),
                       e.key.mod,
                       e.key.down ? "down" : "up");
                if (e.key.down && e.key.scancode == SDL_SCANCODE_F12)
                {
                    relativeMouse = !relativeMouse;
                    SDL_SetWindowRelativeMouseMode(window, relativeMouse);
                    printf("relative mouse mode: %s\n", relativeMouse ? "on" : "off");
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                printf("mouse motion  xrel %6.1f yrel %6.1f\n", e.motion.xrel, e.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                printf("mouse wheel   x %5.2f y %5.2f\n", e.wheel.x, e.wheel.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                printf("mouse button %d %s\n", e.button.button, e.button.down ? "down" : "up");
                break;

            default:
                break;
        }
        fflush(stdout);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
