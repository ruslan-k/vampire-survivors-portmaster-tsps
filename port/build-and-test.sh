#!/bin/bash
set -euo pipefail
CC=aarch64-linux-gnu-gcc
# host test first using native cc in container
echo '=== unit tests ==='
cc -std=gnu11 -I src -O0 -Wall -Werror \
  tests/test_display_contract.c src/display_contract.c \
  -o /tmp/test_display_contract
/tmp/test_display_contract

echo '=== rebuild loader ==='
bash /src/build-inner.sh
