# Meadowlark desktop build

Builds `core/` for Linux and macOS as a Tracker Mini without the proprietary synth
engines. Primary target is Debian trixie on the uConsole; Apple Silicon macOS is
secondary. The firmware build (`make` at the repository root) is separate and unchanged.

## Dependencies

- Debian/Ubuntu: `tools/install-deps-linux.sh` (add `--planned` for the libraries later
  phases will need).
- macOS: `brew install cmake ninja sdl3`.

## Build

```sh
cmake -S platforms/desktop -B platforms/desktop/build -G Ninja
cmake --build platforms/desktop/build
```

Options:

| Option | Default | Effect |
|---|---|---|
| `-DMEADOWLARK_SDL=OFF` | `ON` | Skip everything that needs SDL3 |
| `-DMEADOWLARK_SANITIZE=undefined` | empty | Build with the given `-fsanitize=` list |
| `-DCMAKE_BUILD_TYPE=Debug` | `RelWithDebInfo` | Usual CMake build types |

`platforms/desktop/build*/` is ignored by git, so several build directories (for example
one per compiler or sanitizer) can live side by side.

AddressSanitizer does not work on the uConsole kernel (`CONFIG_ARM64_VA_BITS=39`) or with
Apple clang 15 on macOS 26; UBSan works on both.

## Run

```sh
platforms/desktop/build/meadowlark-headless [seconds]
```

Runs the core for the given time (default 3 s) without a window or sound device and prints
how many audio blocks and display lists it produced.

`platforms/desktop/build/sdl_input_probe` prints the SDL3 numbering of joystick buttons,
axes and keys, for writing input bindings.

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
