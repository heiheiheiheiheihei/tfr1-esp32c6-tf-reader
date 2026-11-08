#!/usr/bin/env bash
set -euo pipefail
MNT="${1:-/mnt/tfreader}"
if [ "$(id -u)" -ne 0 ]; then
    sudo umount "$MNT"
else
    umount "$MNT"
fi
echo "unmounted $MNT (now press Ctrl-C in the bridge terminal)"
