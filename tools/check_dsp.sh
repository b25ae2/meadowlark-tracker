#!/usr/bin/env bash
# Check that the plain-C DSP fallbacks give bit-identical results to the ARM asm they replace.
#
#   tools/check_dsp.sh <host dsp_vectors binary> [toolchain prefix]
#
# Builds platforms/desktop/tests/dsp_vectors.cpp as a freestanding 32-bit ARM program with
# the firmware toolchain (asm paths on, CMSIS arm_sin_q15 from the firmware libraries) and
# runs it next to the host build. Needs an ARM Linux kernel that can run 32-bit binaries,
# e.g. the uConsole's AArch64 kernel (CONFIG_COMPAT=y).

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
HOST_BIN=${1:?usage: $0 <host dsp_vectors binary> [toolchain prefix]}
TOOLCHAIN=${2:-$ROOT_DIR/agt/10.3/bin/arm-none-eabi-}

if [ ! -x "${TOOLCHAIN}c++" ]; then
  echo "firmware compiler not found: ${TOOLCHAIN}c++" >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Cortex-M4 hard-float, matching libarm_cortexM4lf_math.a. The T32 code also runs in
# AArch32 user mode on Cortex-A cores. Linked above vm.mmap_min_addr (64 KiB on Debian).
"${TOOLCHAIN}c++" \
  -DDSP_VECTORS_ARM -DKINETISK \
  -I"$ROOT_DIR/core/libs/Audio" \
  -std=gnu++20 -fno-rtti -fno-exceptions -ffreestanding -fno-builtin -O2 \
  -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -nostdlib -static -Wl,-Ttext=0x10000 -Wl,--entry=_start \
  -o "$WORK/dsp_vectors_arm" "$ROOT_DIR/platforms/desktop/tests/dsp_vectors.cpp" \
  "$ROOT_DIR/platforms/imx/3rdparty/math/libarm_cortexM4lf_math.a" -lgcc

"$WORK/dsp_vectors_arm" > "$WORK/arm.txt"
"$HOST_BIN" > "$WORK/host.txt"

if diff -u --label arm "$WORK/arm.txt" --label host "$WORK/host.txt"; then
  echo "dsp OK: $(wc -l < "$WORK/host.txt") functions bit-identical to the ARM instructions"
else
  exit 1
fi
