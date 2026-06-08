#!/usr/bin/env bash
# Build the Vue frontend into the directory passed as $1.
# Used by the top-level ESP-IDF CMakeLists.txt to keep `idf.py build`
# producing an up-to-date web image.
#
# $2 (optional) is the generated-version dir. When present, the project version
# from tools/gen-version.mjs (app_version.txt there) is handed to Vite via
# APP_VERSION, so the footer matches the firmware exactly.

set -euo pipefail
cd "$(dirname "$0")"

OUT_DIR="${1:-dist}"
GEN_DIR="${2:-}"

if [ -n "$GEN_DIR" ] && [ -f "$GEN_DIR/app_version.txt" ]; then
  export APP_VERSION="$(cat "$GEN_DIR/app_version.txt")"
fi

# Reinstall when lockfile is newer than the install marker.
if [ ! -d node_modules ] \
   || [ ! -f node_modules/.package-lock.json ] \
   || [ package-lock.json -nt node_modules/.package-lock.json ]; then
  echo "[web] installing dependencies"
  npm install --no-audit --no-fund
fi

echo "[web] building → $OUT_DIR"
OUT_DIR="$OUT_DIR" npm run build
