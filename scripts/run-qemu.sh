#!/usr/bin/env bash
set -euo pipefail
ISO="${1:-build/condr.iso}"
DISK="${CONDR_DISK:-build/condr-data.img}"

mkdir -p "$(dirname "$DISK")"
if [[ ! -e "$DISK" ]]; then
    truncate -s 4M "$DISK"
fi

exec qemu-system-x86_64 \
    -M q35 \
    -m 512M \
    -smp 1 \
    -cdrom "$ISO" \
    -boot d \
    -drive if=none,id=condr-data,format=raw,file="$DISK" \
    -device virtio-blk-pci,disable-modern=on,drive=condr-data \
    -serial vc \
    -no-reboot -no-shutdown \
    -d guest_errors \
    "${@:2}"