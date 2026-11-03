#!/usr/bin/env bash
# Install build dependencies for the Meadowlark desktop build on Debian/Ubuntu.
# Primary target: Debian trixie (uConsole).
#
#   tools/install-deps-linux.sh            # what the current build needs
#   tools/install-deps-linux.sh --probe    # + SDL3, for tools/sdl_input_probe.c
#   tools/install-deps-linux.sh --planned  # + libraries for the later port phases
#
# Extra apt-get options can be passed after "--", e.g. "-- --no-install-recommends".

set -euo pipefail

# Headless spike: compiler, make, git (version.mk reads the commit hash).
BASE_PKGS=(build-essential clang git make)

PROBE_PKGS=(pkg-config libsdl3-dev)

# Phase 2-9: display, audio, input, MIDI, fonts, config, plugin hosting.
PLANNED_PKGS=(
  pkg-config
  libsdl3-dev           # window, input
  libpipewire-0.3-dev   # audio backend
  librtmidi-dev         # MIDI
  libfreetype-dev       # configurable TTF fonts
  libstb-dev            # stb_image
  libtomlplusplus-dev   # input.toml / display.toml / plugins.toml
  liblilv-dev           # LV2 hosting
)

usage() {
  sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
}

pkgs=("${BASE_PKGS[@]}")
apt_opts=()

while [ $# -gt 0 ]; do
  case "$1" in
    --probe)   pkgs+=("${PROBE_PKGS[@]}") ;;
    --planned) pkgs+=("${PLANNED_PKGS[@]}") ;;
    -h|--help) usage; exit 0 ;;
    --)        shift; apt_opts=("$@"); break ;;
    *)         echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

# Drop duplicates while keeping order.
unique=()
for p in "${pkgs[@]}"; do
  case " ${unique[*]-} " in
    *" $p "*) ;;
    *) unique+=("$p") ;;
  esac
done

if ! command -v apt-get >/dev/null 2>&1; then
  echo "apt-get not found: this script supports Debian-based systems only." >&2
  echo "Packages needed: ${unique[*]}" >&2
  exit 1
fi

if [ -r /etc/os-release ]; then
  . /etc/os-release
  if [ "${VERSION_CODENAME:-}" != "trixie" ]; then
    echo "note: tested on Debian trixie, this is ${PRETTY_NAME:-unknown}; package names may differ." >&2
  fi
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  SUDO="sudo"
fi

echo "Installing: ${unique[*]}"
$SUDO apt-get update
$SUDO apt-get install -y ${apt_opts[@]+"${apt_opts[@]}"} "${unique[@]}"

echo
echo "Done. Build with:"
echo "  make -C platforms/desktop -j\$(nproc) CXX=clang++ CC=clang"
