#!/bin/sh
# Build a PortMaster release zip: launcher + loader, no game data.
#
#   sh package.sh            -> dist/vampiresurvivors.zip
#
# The zip contains:
#   Vampire Survivors.sh
#   vampiresurvivors/vampiresurvivors-nextos
#   vampiresurvivors/tools/  (setup + DLC patch helpers)
#   vampiresurvivors/docs/   (porting notes, DLC, memory)
set -eu

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
BIN="$HERE/port/vampiresurvivors-nextos"
[ -f "$BIN" ] || { echo "build first: cd port && bash build.sh" >&2; exit 1; }

OUT="$HERE/dist"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$OUT" "$STAGE/vampiresurvivors/tools" "$STAGE/vampiresurvivors/docs"
cp "$HERE/port/Vampire Survivors.sh" "$STAGE/"
cp "$BIN" "$STAGE/vampiresurvivors/"
chmod +x "$STAGE/vampiresurvivors/vampiresurvivors-nextos"
cp "$HERE/tools/setup-from-apk.sh" "$HERE/tools/patch-dlc.py" "$STAGE/vampiresurvivors/tools/"
cp "$HERE/docs/"*.md "$STAGE/vampiresurvivors/docs/"
cp "$HERE/README.md" "$STAGE/vampiresurvivors/"

rm -f "$OUT/vampiresurvivors.zip"
( cd "$STAGE" && zip -qr "$OUT/vampiresurvivors.zip" . )
echo "wrote $OUT/vampiresurvivors.zip ($(wc -c < "$OUT/vampiresurvivors.zip") bytes)"
