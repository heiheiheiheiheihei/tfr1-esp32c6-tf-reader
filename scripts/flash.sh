#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-/dev/ttyACM0}"
if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    source "${IDF_PATH:-$HOME/esp/esp-idf-5.5.3}/export.sh"
fi
idf.py -p "$PORT" flash
