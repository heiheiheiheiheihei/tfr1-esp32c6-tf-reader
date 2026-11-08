#!/usr/bin/env bash
set -euo pipefail
DEV="${1:-/dev/nbd0}"
MNT="${2:-/mnt/tfreader}"
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

$SUDO partprobe "$DEV" >/dev/null 2>&1 || true
sleep 0.4

PART="${DEV}p1"
[ -b "$PART" ] || PART="$DEV"
[ -b "$PART" ] || { echo "no block device: $PART (is the bridge attached?)" >&2; exit 1; }

FSTYPE="$(lsblk -no FSTYPE "$PART" 2>/dev/null | head -1 || true)"
$SUDO mkdir -p "$MNT"
if [ "$FSTYPE" = "vfat" ] || [ "$FSTYPE" = "exfat" ]; then
    $SUDO mount -o "uid=$(id -u),gid=$(id -g),umask=022" "$PART" "$MNT"
else
    $SUDO mount "$PART" "$MNT"
fi
findmnt "$MNT" -o TARGET,SOURCE,FSTYPE,OPTIONS
