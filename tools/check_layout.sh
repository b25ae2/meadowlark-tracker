#!/usr/bin/env bash
# Check that the desktop build lays out on-disk structs exactly like the Tracker Mini firmware.
#
#   tools/check_layout.sh <host layout_probe binary> [toolchain prefix]
#
# Compiles platforms/desktop/tests/layout_probe.cpp with the firmware compiler and the
# Mini's flags (object file only), reads the values out of its .layout_probe section, and
# diffs them against what the host probe prints. The toolchain defaults to agt/10.3, the
# same one the firmware Makefile uses.

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
HOST_PROBE=${1:?usage: $0 <host layout_probe binary> [toolchain prefix]}
TOOLCHAIN=${2:-$ROOT_DIR/agt/10.3/bin/arm-none-eabi-}

if [ ! -x "${TOOLCHAIN}c++" ]; then
  echo "firmware compiler not found: ${TOOLCHAIN}c++" >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Same defines and code-generation flags as the Mini build of core/ (platforms/imx, PRODUCT=MINI).
CORE=$ROOT_DIR/core
"${TOOLCHAIN}c++" \
  -DAUDIO_PERFORMANCE_INFO_ENABLED -DMEMORY_USAGE_INFO_ENABLED -D__NEWLIB__ -DKINETISK \
  -D__MK66FX1M0__ -DF_CPU=180000000 -DUSB_MIDI -DTEENSYDUINO=142 -DARDUINO=10805 \
  -DLAYOUT_US_ENGLISH -DCPU_MK66FX1M0VMD18 -DPLATFORM_IMX -DFEATURE_SYNTH -DTRACKER_MINI \
  -DLAYOUT_PROBE_OBJECT_ONLY \
  -I"$CORE/include" -I"$CORE/config" -I"$CORE/source" -I"$CORE/synth" -I"$CORE/libs" \
  -I"$CORE/libs/Audio" -I"$CORE/libs/ElapsedMillis" -I"$CORE/libs/Audio/utility" \
  -I"$CORE/libs/FT812" -I"$CORE/libs/fonts" -I"$CORE/libs/NewDisplay" -I"$CORE/interface" \
  -I"$CORE/effector" -I"$CORE/FileManager" -I"$CORE/Exporter" -I"$CORE/AudioEngine" \
  -I"$CORE/libs/Audio/Polyverb" -I"$CORE/AudioEngine/ConstantValues" \
  -I"$CORE/AudioEngine/MasterEnhancer" -I"$ROOT_DIR/util/include" \
  -I"$ROOT_DIR/util/3rdparty" -I"$ROOT_DIR/synth/include" \
  -std=gnu++20 -include cstdint -fno-rtti -fno-exceptions -fno-common \
  -mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -mfp16-format=ieee \
  -ffreestanding -fno-builtin -O2 -w \
  -c -o "$WORK/probe.o" "$ROOT_DIR/platforms/desktop/tests/layout_probe.cpp"

"${TOOLCHAIN}objcopy" -O binary --only-section=.layout_probe "$WORK/probe.o" "$WORK/values.bin"

"$HOST_PROBE" > "$WORK/host.txt"
cut -d' ' -f1- "$WORK/host.txt" | sed 's/ [0-9]*$//' > "$WORK/names.txt"
od -An -v -t u4 -w4 "$WORK/values.bin" | tr -d ' ' > "$WORK/arm_values.txt"

if [ "$(wc -l < "$WORK/names.txt")" -ne "$(wc -l < "$WORK/arm_values.txt")" ]; then
  echo "probe item count differs between host and firmware builds" >&2
  exit 1
fi
paste -d' ' "$WORK/names.txt" "$WORK/arm_values.txt" > "$WORK/arm.txt"

if diff -u --label firmware "$WORK/arm.txt" --label host "$WORK/host.txt"; then
  echo "layout OK: $(wc -l < "$WORK/host.txt") sizes and offsets match the firmware"
else
  exit 1
fi
