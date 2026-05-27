#!/usr/bin/env bash
# Build the Vue frontend into the directory passed as $1.
# Used by the top-level ESP-IDF CMakeLists.txt to keep `idf.py build`
# producing an up-to-date web image.

set -euo pipefail
cd "$(dirname "$0")"

OUT_DIR="${1:-dist}"

# Reinstall when lockfile is newer than the install marker.
if [ ! -d node_modules ] \
   || [ ! -f node_modules/.package-lock.json ] \
   || [ package-lock.json -nt node_modules/.package-lock.json ]; then
  echo "[web] installing dependencies"
  npm install --no-audit --no-fund
fi

echo "[web] building → $OUT_DIR"
OUT_DIR="$OUT_DIR" npm run build
