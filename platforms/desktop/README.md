# Meadowlark desktop build

Builds `core/` for Linux and macOS as a Tracker Mini without the proprietary synth
engines. Primary target is Debian trixie on the uConsole; Apple Silicon macOS is
secondary. The firmware build (`make` at the repository root) is separate and unchanged.

## Dependencies

- Debian/Ubuntu: `tools/install-deps-linux.sh`.
- macOS: `brew install cmake ninja sdl3 tomlplusplus freetype lilv`.

The CLAP headers are in `util/3rdparty/clap`.

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
                                   [--audio-input auto|always|off] [--no-host-folders]
                                   [--midi-config file] [--no-midi] [--list-midi]
                                   [--print-midi-config] [--font-config file] [--print-font-config]
                                   [--plugin-config file] [--no-plugins] [--list-plugins]
                                   [--print-plugin-config]
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

Audio input (the Sample Recorder's line-in and mic sources, and input monitoring) comes
from the system's default input: a PipeWire capture stream when the output is PipeWire,
SDL otherwise. With `--audio-input auto` (the default) the device opens the first time the
firmware selects an input, so macOS asks for microphone permission only then; `always`
opens it at start, `off` records silence. The Sample Recorder's gain applies digitally as
the codec's ADC stage (line-in 0–22.5 dB in 1.5 dB steps; for mic, only the part above the
Mini's analog preamp). The uConsole has no built-in input; it needs a USB microphone or
audio interface set as the default source.

Closing the window holds the Mini's power button, so the firmware saves the project to the
workspace and powers off (about 1.5 s); closing it again quits at once.

```sh
platforms/desktop/build/meadowlark-headless [seconds] [--screenshot out.png] [--sd-root dir | --no-sd]
                                            [--press button@seconds[:hold] ...] [--midi-trace]
                                            [--ptfont file] [--plugin clap:file[#id]@channel ...]
                                            [--plugin lv2:uri@channel ...] [--note channel@seconds[:hold] ...]
```

Runs the core for the given time (default 3 s) without a window or sound device, prints
how many audio blocks and display lists it produced, and can save the last frame as PNG.
`--press down@2.5` presses a Mini button (an `input.toml` action name) at that time for
100 ms, `--press menu@2:1.5` holds it for 1.5 s; presses go through the same path as the
window's input, for scripted checks. `--midi-trace` turns on MIDI clock, transport and
note output and prints every message core sends, with its audio block and offset.
`--ptfont` adds a font file to the card's `/Fonts` and switches the display to it.
`--plugin` loads an instrument plugin on a MIDI channel and `--note 1@2` sends core's
MIDI-instrument note (middle C, 0.5 s) on channel 1 at 2 s; the peak shows it played.

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

The built-in bindings are generic keyboard ones; a uConsole layout comes later. 1-8 are
the eight buttons under the screen (`screen1`-`screen8`, left to right like the labels along
its bottom edge), F1-F5 Note, Instr, FX1, FX2 and Pattern, F6-F9 Fn1-Fn4, arrows, Shift,
Space for Play, R for Rec, Return for the middle button and Escape for Menu.

## MIDI

The firmware's two MIDI ports, USB and the MIDI jack, become system MIDI ports: "Meadowlark
USB" and "Meadowlark Jack" in CoreMIDI, client "Meadowlark" with ports "USB" and "Jack" in
the ALSA sequencer (which PipeWire bridges). Each has an input and an output that other
programs can connect to. Config > MIDI on the tracker chooses what uses which port, as on
the Mini; everything there starts off.

`midi.toml`, next to `input.toml` (or `--midi-config <file>`, `MEADOWLARK_MIDI_CONFIG`),
lists system ports to connect automatically, including devices plugged in later. By
default every hardware MIDI input plays into the USB port:

```toml
[usb]
inputs = ["*"]          # "*": every hardware port; otherwise part of a name, any case
outputs = []

[jack]
inputs = []
outputs = ["MicroFreak"]
```

`meadowlark --list-midi` prints the system's ports and which ones the config matches;
`--print-midi-config` prints the defaults with their documentation. Outgoing messages are
timed to the audio: a note or clock leaves when the sound rendered with it is heard, at its
place within the audio block, so external synths stay in time with the tracker's own sound.
`[timing] output_offset_ms` shifts that if a device responds late.

## Fonts

The tracker draws text with five bitmap fonts (14 and 17 px regular, 14 and 17 px bold,
12 px regular); Config > General > Display font picks the built-in ones or a `.ptfont`
file from the card's `/Fonts`. `fonts.toml`, next to `input.toml` (or `--font-config
<file>`, `MEADOWLARK_FONT_CONFIG`), adds system fonts to that list: at startup each one is
rendered from its TTF, OTF or TTC file with FreeType into `~/.cache/meadowlark/fonts`
(`~/Library/Caches/Meadowlark/fonts` on macOS) and shown in `/Fonts` without being
written to the card.

```toml
[[font]]
name = "DejaVu Mono"
regular = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
bold = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"  # optional: else emboldened

[[font]]
name = "Iosevka"
regular = { file = "~/.local/share/fonts/iosevka/Iosevka-Regular.ttc", family = "Iosevka Term" }
sizes = { regular14 = 13, bold14 = 13 }  # pixel sizes, only the ones to change
```

`meadowlark --print-font-config` prints the format. Cells follow the font's own glyphs, so a
font with larger letters than the built-in Roboto Mono may not fit everywhere; smaller
sizes help, and very large ones are refused (the firmware's limit is about 22 KB per size).
The bold 17 size keeps the built-in transport icons.

## Plugins

CLAP and LV2 instruments can be played by the tracker's MIDI instruments. `plugins.toml`,
next to `input.toml` (or `--plugin-config <file>`, `MEADOWLARK_PLUGIN_CONFIG`), lists them,
each on a MIDI channel:

```toml
[[plugin]]
clap = "~/.clap/Dexed.clap"
channel = 1
reverb_send = 0.2     # also volume_db, pan, delay_send

[[plugin]]
lv2 = "http://drobilla.net/plugins/mda/EPiano"
channel = 2
```

A plugin hears everything the tracker sends to the MIDI instrument on its channel (notes,
CC, program change, pitch bend, aftertouch), whatever Config's MIDI out is set to, and also
while a song or pattern is exported. Its sound joins the tracks in the master mix, with
optional sends into the delay and reverb. `meadowlark --list-plugins` prints the installed
instruments in the file's format: CLAP from `$CLAP_PATH`, `~/.clap` and `/usr/lib/clap`
(`~/Library/Audio/Plug-Ins/CLAP` and `/Library/Audio/Plug-Ins/CLAP` on macOS), LV2 from
`$LV2_PATH` or the standard places. `--print-plugin-config` prints the format.

The `plugin_editor` action (F10) opens or closes the editor of the plugin on the selected
instrument's channel in a window of its own. On Linux these are X11 windows (shown through
XWayland under a Wayland session); LV2 editors must be X11 (macOS: Cocoa) ones, as GTK or
Qt editors would need suil. Saving the project to the workspace saves each plugin's state in
`Workspace/Plugins/channel-NN.state`, and the `Plugins` folder goes with the project when it
is saved or opened. A project without a saved state for a plugin leaves the plugin as it
is.

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

The card's root also shows two folders from the computer: **Home** (your home directory)
and **Computer** (`/`), so the sample loader can browse and load from anywhere, going up
with its `/..` entry. They are read-only: renaming, deleting or saving into them fails, and
emptying the card never reaches them (symlinks on the card are removed, not followed). A
real card folder with the same name hides the virtual one. `--no-host-folders` turns them
off.

## Tests

```sh
platforms/desktop/build/fs_test /tmp/fs_test     # file system semantics; recreates the directory
platforms/desktop/build/input_test /tmp/input    # button timing, bindings, input.toml
platforms/desktop/build/eve_image_test           # JPEG/PNG decoding in the FT812 emulator
platforms/desktop/build/audio_test [pipewire|sdl] # sound output runs in real time without
                                                  # underruns; silent unless --tone
platforms/desktop/build/midi_test /tmp/midi      # MIDI ports, output timing, midi.toml
platforms/desktop/build/font_test /tmp/fonts some.ttf # .ptfont generation, fonts.toml, /Fonts
platforms/desktop/build/plugin_test /tmp/plugins # plugin host with the test synths in the build
platforms/desktop/build/midi_test /tmp/midi --loopback # also through the system's MIDI
platforms/desktop/build/audio_test pipewire --capture # also records the output's monitor and
                                                      # checks a quiet tone comes back
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
