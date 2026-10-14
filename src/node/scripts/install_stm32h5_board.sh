#!/bin/bash
# Add the RotorHazard Black Pill H523CE board entry and linker script to an
#  installed STM32 Arduino core.  Usage: install_stm32h5_board.sh <core dir>
#  e.g. ~/.arduino15/packages/STMicroelectronics/hardware/stm32/2.12.0
set -euo pipefail
CORE_DIR="${1:?usage: $0 <STM32 core dir>}"
SRC_DIR="$(cd "$(dirname "$0")/stm32h5" && pwd)"
VARIANT_DIR="$CORE_DIR/variants/STM32H5xx/H523C(C-E)(T-U)_H533CE(T-U)"

[ -f "$CORE_DIR/boards.txt" ] || { echo "No boards.txt in $CORE_DIR" >&2; exit 1; }
[ -d "$VARIANT_DIR" ] || { echo "Core has no H523C variant: $VARIANT_DIR" >&2; exit 1; }

cp "$SRC_DIR/boards.local.txt" "$CORE_DIR/boards.local.txt"
cp "$SRC_DIR/ldscript.ld" "$VARIANT_DIR/ldscript.ld"
echo "Installed RH_BLACKPILL_H523CE into $CORE_DIR"
