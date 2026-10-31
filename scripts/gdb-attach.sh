#!/usr/bin/env bash
set -euo pipefail
gdb -ex "target remote :1234" \
    -ex "symbol-file build/condr.elf" \
    -ex "break kernel_main" \
    -ex "continue"