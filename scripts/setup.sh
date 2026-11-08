#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== TFR-1 setup check =="
if ! command -v idf.py >/dev/null 2>&1 && [ -f "${IDF_PATH:-$HOME/esp/esp-idf-5.5.3}/export.sh" ]; then
    # shellcheck disable=SC1090
    source "${IDF_PATH:-$HOME/esp/esp-idf-5.5.3}/export.sh" >/dev/null 2>&1 || true
fi
if command -v idf.py >/dev/null 2>&1; then
    echo "ESP-IDF : $(idf.py --version 2>/dev/null | tail -1)"
else
    echo "ESP-IDF : NOT in PATH"
    echo "          source ~/esp/esp-idf-5.5.3/export.sh"
fi

if python3 -c 'import serial' 2>/dev/null; then
    echo "pyserial: OK"
else
    echo "pyserial: MISSING"
    echo "          python3 -m pip install --user -r host/requirements.txt"
fi

if [ -e /dev/nbd0 ]; then
    echo "nbd     : OK (/dev/nbd0)"
else
    echo "nbd     : loading module"
    sudo modprobe nbd max_part=8
    echo "nbd     : $(ls /dev/nbd0 2>/dev/null || echo FAILED)"
fi

echo "serial  : $(ls /dev/ttyACM* 2>/dev/null || echo 'no /dev/ttyACM*')"
echo "done."
