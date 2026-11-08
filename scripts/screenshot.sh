#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PAGE="${1:-0}"
OUT="${2:-docs/ui-current.png}"
python3 host/tfreader_host.py --ui-page "$PAGE" --screenshot "$OUT"
