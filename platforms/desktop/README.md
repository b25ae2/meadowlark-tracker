# Meadowlark desktop build

Builds `core/` for Linux and macOS as a Tracker Mini without the proprietary synth
engines. Primary target is Debian trixie on the uConsole; Apple Silicon macOS is
secondary. The firmware build (`make` at the repository root) is separate and unchanged.

## Dependencies

- Debian/Ubuntu: `tools/install-deps-linux.sh` (add `--planned` for the libraries later
  phases will need).
- macOS: `brew install cmake ninja sdl3 tomlplusplus`.

With `-DMEADOWLARK_SDL=OFF` neither SDL3 nor toml++ is needed, and on Linux
`-DMEADOWLARK_PIPEWIRE=OFF` drops the PipeWire dependency (sound then goes through SDL).

## Build

```sh
cmake -S platforms/desktop -B platforms/desktop/build -G Ninja
cmake --build platforms/desktop/build
```

Options:

| Option | Default | Effect |
|---|---|---|
| `-DMEADOWLARK_SDL=OFF` | `ON` | Skip everything that needs SDL3 |
| `-DMEADOWLARK_PIPEWIRE=OFF` | `ON` on Linux | No native PipeWire output |
| `-DMEADOWLARK_SANITIZE=undefined` | empty | Build with the given `-fsanitize=` list |
| `-DCMAKE_BUILD_TYPE=Debug` | `RelWithDebInfo` | Usual CMake build types |

`platforms/desktop/build*/` is ignored by git, so several build directories (for example
one per compiler or sanitizer) can live side by side.

AddressSanitizer does not work on the uConsole kernel (`CONFIG_ARM64_VA_BITS=39`) or with
Apple clang 15 on macOS 26; UBSan works on both.

## Run

```sh
platforms/desktop/build/meadowlark [--sd-root dir | --no-sd] [--input-config file] [--trace-input]
                                   [--scale n] [--fullscreen] [--print-input-config]
                                   [--audio pipewire|sdl|none] [--audio-buffer frames]
```

Opens an 800x480 window (`--scale` multiplies it) that can be resized freely; the screen is
letterboxed, sharp at whole-number scales and filtered otherwise.

Sound goes to PipeWire on Linux and through SDL (Core Audio) on macOS, at the engine's
44.1 kHz; the sound server resamples if it runs at another rate. `--audio sdl` uses SDL on
Linux too, `--audio none` runs silently. The engine renders on its own thread into a buffer
of `--audio-buffer` frames (default 512, about 12 ms); if the device asks for more per
callback, the buffer grows to that plus one block. The exit message reports underruns. The
firmware's master volume applies as on the Mini's headphone output, with 81 % (the default)
at 0 dB; the hardware's analog boost above that is not reproduced, use the system volume.

Closing the window holds the Mini's power button, so the firmware saves the project to the
workspace and powers off (about 1.5 s); closing it again quits at once.

```sh
platforms/desktop/build/meadowlark-headless [seconds] [--screenshot out.png] [--sd-root dir | --no-sd]
                                            [--press button@seconds[:hold] ...]
```

Runs the core for the given time (default 3 s) without a window or sound device, prints
how many audio blocks and display lists it produced, and can save the last frame as PNG.
`--press down@2.5` presses a Mini button (an `input.toml` action name) at that time for
100 ms, `--press menu@2:1.5` holds it for 1.5 s; presses go through the same path as the
window's input, for scripted checks.

## Input

Keyboard, gamepad, joystick and mouse inputs map to the Tracker Mini's buttons through
`input.toml`:

- Linux: `$XDG_CONFIG_HOME/meadowlark/input.toml` (usually `~/.config/meadowlark/input.toml`)
- macOS: `~/Library/Application Support/Meadowlark/input.toml`
- anywhere else: `--input-config <file>` or `MEADOWLARK_INPUT_CONFIG=<file>`

`meadowlark --print-input-config` prints the built-in bindings with a description of the
format; save it to that path as a starting point. A file only needs the actions it
changes. Each action takes any number of bindings, and a binding can be a chord:

```toml
[bindings]
play = ["key:Space", "pad:a", ["pad:back", "pad:start"]]
up   = ["key:Up", "pad:dpup", "pad:lefty-", "joy:hat0up"]
rec  = []   # unbound
```

`--trace-input` prints the name of every input pressed and the actions it triggers.
`platforms/desktop/build/sdl_input_probe` shows how SDL numbers a device's buttons, axes and
hats. Buttons repeat while held with the hardware's timing.

The built-in bindings are generic keyboard ones; a uConsole layout comes later.

## SD card

A host directory stands in for the SD card:

- Linux: `$XDG_DATA_HOME/meadowlark/sdcard` (usually `~/.local/share/meadowlark/sdcard`)
- macOS: `~/Library/Application Support/Meadowlark/sdcard`
- anywhere else: `--sd-root <dir>` or `MEADOWLARK_SD_ROOT=<dir>`

It is created on first start; the firmware then creates its `Workspace`, `Fonts` and
`Themes` folders and `trackerConfig` there, as on a fresh card. The contents of a real
Tracker card can be copied in. Names are matched case-insensitively, like FAT. The FAT
hidden attribute (used for `Workspace`) is kept in `.hidden` files; dot files count as
hidden. `MEADOWLARK_FS_TRACE=1` logs file opens, existence checks and directory changes.

## Tests

```sh
platforms/desktop/build/fs_test /tmp/fs_test     # file system semantics; recreates the directory
platforms/desktop/build/input_test /tmp/input    # button timing, bindings, input.toml
platforms/desktop/build/eve_image_test           # JPEG/PNG decoding in the FT812 emulator
platforms/desktop/build/audio_test [pipewire|sdl] # sound output runs in real time without
                                                  # underruns; silent unless --tone
```

## Checks against the firmware

Both need the firmware toolchain in `agt/10.3` (see the root `Makefile`). Run them after
changing on-disk structs or DSP code.

```sh
# On-disk structs (project, instrument, pattern, config) have the firmware's sizes and offsets.
tools/check_layout.sh platforms/desktop/build/layout_probe

# The C DSP fallbacks and sin_q15 are bit-identical to the ARM instructions and CMSIS.
# Runs a 32-bit ARM binary, so it needs an ARM Linux kernel with 32-bit support (the uConsole).
tools/check_dsp.sh platforms/desktop/build/dsp_vectors
```
