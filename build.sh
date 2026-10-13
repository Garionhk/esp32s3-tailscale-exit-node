#!/usr/bin/env bash
# Builds the firmware and writes a single flashable image to dist/.
#
#   ./build.sh              build + images in dist/
#   ./build.sh flash PORT   build, ERASE everything, flash the full image (new board)
#   ./build.sh upgrade PORT build, write only the app at 0x20000 (keeps settings
#                           and the Tailscale identity)
#
# The full image starts at 0x0 and pads the gap up to otadata with 0xFF, which
# covers the NVS partition at 0x9000 -- writing it always wipes settings,
# erase or not. Upgrades must use the app-only image.
#
# Uses the ESP-IDF at $IDF_PATH (default ~/esp/esp-idf). If that install has
# no Xtensa toolchain (only the RISC-V one for P4/C-series was installed),
# the identical GCC shipped with the Arduino ESP32 core is used instead.
set -euo pipefail
cd "$(dirname "$0")"

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1 || true

if ! command -v xtensa-esp32s3-elf-gcc >/dev/null 2>&1; then
  for d in "$HOME"/Library/Arduino15/packages/esp32/tools/esp-x32/*/bin \
           "$HOME"/.arduino15/packages/esp32/tools/esp-x32/*/bin; do
    [ -x "$d/xtensa-esp32s3-elf-gcc" ] && PATH="$d:$PATH" && break
  done
fi
command -v xtensa-esp32s3-elf-gcc >/dev/null 2>&1 || {
  echo "No Xtensa toolchain. Run: $IDF_PATH/install.sh esp32s3" >&2; exit 1; }
command -v idf.py >/dev/null 2>&1 || { echo "idf.py not found under $IDF_PATH" >&2; exit 1; }

[ -f sdkconfig ] || idf.py set-target esp32s3
idf.py build

VER=$(python3 -c "import json;print(json.load(open('build/project_description.json'))['project_version'])")
OUT="ts-exit-node-v${VER}-esp32s3-n16r8.bin"
APP="ts-exit-node-v${VER}-esp32s3-n16r8-app.bin"
idf.py merge-bin -o "$OUT" >/dev/null
mkdir -p dist
cp "build/$OUT" "dist/$OUT"
cp build/ts-exit-node.bin "dist/$APP"
(cd dist && shasum -a 256 "$OUT" "$APP" > "ts-exit-node-v${VER}.sha256")
echo
echo "dist/$OUT  ($(wc -c < "dist/$OUT" | tr -d ' ') bytes) -- new board: write at 0x0, wipes settings"
echo "dist/$APP  ($(wc -c < "dist/$APP" | tr -d ' ') bytes) -- upgrade: write at 0x20000, keeps settings"

case "${1:-}" in
  flash)
    PORT="${2:?usage: ./build.sh flash <port>}"
    esptool --port "$PORT" erase-flash
    esptool --port "$PORT" --baud 460800 write-flash 0x0 "dist/$OUT"
    ;;
  upgrade)
    PORT="${2:?usage: ./build.sh upgrade <port>}"
    esptool --port "$PORT" --baud 460800 write-flash 0x20000 "dist/$APP"
    ;;
esac
