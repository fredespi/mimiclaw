#!/usr/bin/env bash
# MimiClaw — build, flash (fast), and monitor
set -e

PORT="${1:-/dev/cu.usbserial-A5069RR4}"
FLASH_BAUD="${2:-921600}"

source "$HOME/.espressif/v5.5.2/esp-idf/export.sh" 2>/dev/null

idf.py build
idf.py -p "$PORT" -b "$FLASH_BAUD" flash
idf.py -p "$PORT" monitor
