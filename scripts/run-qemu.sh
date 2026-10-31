#!/usr/bin/env bash
set -euo pipefail
ISO="${1:-build/condr.iso}"

exec qemu-system-x86_64 \
    -M q35 \
    -m 512M \
    -smp 2 \
    -cdrom "$ISO" \
    -boot d \
    -serial vc \
    -no-reboot -no-shutdown \
    -d guest_errors \
    "${@:2}"