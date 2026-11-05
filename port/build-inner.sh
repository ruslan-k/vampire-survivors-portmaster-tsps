#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Bad 2 Bad Delta - AArch64 loader build (run inside portmaster-builder).
set -euo pipefail

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
OUTPUT=${OUTPUT:-greak-nextos}
OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

OBJS=()
for source in src/*.c \
              vendor/nxinput/src/nxinput_gptk.c \
              vendor/nxinput/src/nxinput_gptk_motion.c \
              vendor/nxcompat/src/nxcompat_settings.c \
              vendor/nxgl/src/nxgl_quality.c \
              vendor/nxgl/adapters/nxgl_frame_proof_adapter.c; do
  object="$OBJDIR/$(basename "${source%.c}").o"
  echo "CC $source"
  $CC -D_GNU_SOURCE -std=gnu11 \
    -I src \
    -I vendor/nxinput/include \
    -I vendor/nxcompat/include \
    -I vendor/nxgl/include \
    -I vendor/nxgl/adapters \
    -O2 -fPIE -fno-strict-aliasing -fno-omit-frame-pointer \
    -Wno-unused-parameter -Wno-unused-result \
    -c "$source" -o "$object"
  OBJS+=("$object")
done

UNDEFINED=$($NM --undefined-only "${OBJS[@]}" 2>/dev/null | awk '{print $NF}' | sort -u)

stub_lib() {
  stub_out=$1; stub_soname=$2; stub_regex=$3
  : > "$STUBDIR/$stub_out.c"
  for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E "$stub_regex" || true); do
    printf 'void %s(void) {}\n' "$symbol" >> "$STUBDIR/$stub_out.c"
  done
  $CC -shared -fPIC -nostdlib -Wl,-soname,"$stub_soname" \
    "$STUBDIR/$stub_out.c" -o "$STUBDIR/lib$stub_out.so"
}
stub_lib SDL2 libSDL2-2.0.so.0 '^SDL_'
stub_lib z    libz.so.1        '^(inflate|deflate|uncompress|compress|crc32|adler32|zlib|zError|gz)'

$CC -fPIE -pie -rdynamic -static-libgcc -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL2 -lz -ldl -lm -lpthread

guard_offset=$($READELF -sW "$OUTPUT" |
  awk '$8 == "g_bionic_guard_pad" && !found { value = $2; found = 1 }
       END { if (found) print value }')
[ "$guard_offset" = "0000000000000000" ] || {
  echo "FAIL: TLS guard not at offset zero: $guard_offset" >&2; exit 1; }

MAX_GLIBC=$($READELF --version-info "$OUTPUT" |
  grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
echo "GLIBC max: $MAX_GLIBC"
ALLOWED='^(libSDL2-2\.0\.so\.0|libz\.so\.1|libdl\.so\.2|libm\.so\.6|libpthread\.so\.0|libc\.so\.6|ld-linux-aarch64\.so\.1)$'
BAD=$($READELF -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); print $NF}' |
  grep -Ev "$ALLOWED" || true)
if [ -n "$BAD" ]; then
  echo "FAIL: DT_NEEDED outside baseline:" >&2
  printf '  %s\n' $BAD >&2
  exit 1
fi
$READELF -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); printf "  NEEDED %s\n", $NF}'
echo "BUILT $OUTPUT"
file "$OUTPUT" || true
ls -la "$OUTPUT"
