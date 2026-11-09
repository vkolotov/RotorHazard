#!/usr/bin/env bash
# Build the C5 node firmware and check it: ./tools/build.sh [c5zero|xiaoc5] [extra idf.py args]
set -euo pipefail
board=${1:-c5zero}
shift || true
app=node
for a in "$@"; do case $a in -DRHNODE_C5_APP=*) app=${a#-DRHNODE_C5_APP=};; esac; done
here=$(cd "$(dirname "$0")/.." && pwd)
: "${IDF_PATH:=$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null
cd "$here"
build="build-$board-$app"
[ -f "$build/CMakeCache.txt" ] || idf.py -B "$build" set-target esp32c5
idf.py -B "$build" -DRHNODE_C5_BOARD="$board" "$@" build
lib="$IDF_PATH/components/esp_phy/lib/esp32c5"
python tools/check_phy.py --objdump riscv32-esp-elf-objdump --nm riscv32-esp-elf-nm \
    --libphy "$lib/libphy.a" --librftest "$lib/librftest.a" --elf "$build/rhnode_esp32c5.elf"
ls -l "$build/rhnode_esp32c5.bin"
