#!/bin/bash
# Flash a fresh build + wipe every NVS partition so the device boots
# as if it had never been configured. After this, the firmware will:
#   - find configured=false in app_config → first-boot Setup flow
#   - have no stored Wi-Fi station info (default `nvs` is empty)
#
# Usage:
#   ./tools/flash-fresh.sh                       # auto-detect port
#   ./tools/flash-fresh.sh -p /dev/cu.usbserial-XXXX
#   ./tools/flash-fresh.sh -p /dev/cu.usbserial-XXXX monitor
#
# Anything passed on the command line is forwarded to idf.py — append
# `monitor` to drop straight into the serial monitor after flashing.

set -euo pipefail

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: idf.py not on PATH — source the ESP-IDF environment first." >&2
    echo "       e.g. source ~/.espressif/tools/activate_idf_v6.0.1.sh"      >&2
    exit 1
fi

# Run from the project root regardless of where the caller cd'd to.
cd "$(dirname "$0")/.."

idf.py "$@" \
    erase-partition --partition-name storage \
    erase-partition --partition-name nvs \
    flash
