#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(id -u)" -ne 0 ]; then
    exec sudo python3 -u host/tfreader_host.py --attach "$@"
else
    exec python3 -u host/tfreader_host.py --attach "$@"
fi
