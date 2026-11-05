#!/bin/sh
# Fill game/ from the player's own arm64 APK. Ships no game data.
#
#   sh tools/setup-from-apk.sh /path/to/VampireSurvivors.apk [GAMEDIR]
#
# GAMEDIR defaults to ../game relative to this script (i.e. <port>/game).
set -eu

APK=${1:-}
[ -n "$APK" ] || { echo "usage: $0 <path/to/game.apk> [GAMEDIR]" >&2; exit 2; }
[ -f "$APK" ] || { echo "no such file: $APK" >&2; exit 2; }

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
GAMEDIR=${2:-$(dirname -- "$HERE")/game}
mkdir -p "$GAMEDIR"
GAMEDIR=$(cd -- "$GAMEDIR" && pwd)
echo "APK     : $APK"
echo "GAMEDIR : $GAMEDIR"

command -v unzip >/dev/null 2>&1 || { echo "unzip is required" >&2; exit 3; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required (store-zip step)" >&2; exit 3; }

LIST=$(unzip -l "$APK")
echo "$LIST" | grep -q 'lib/arm64-v8a/libil2cpp\.so' || {
  echo "APK has no arm64-v8a/libil2cpp.so — this port needs the arm64 Android build" >&2
  exit 4
}
echo "$LIST" | grep -q 'assets/bin/Data/' || {
  echo "APK has no assets/bin/Data — not a Unity IL2CPP build?" >&2
  exit 4
}

echo "== assets =="
unzip -o -q "$APK" 'assets/*' -d "$GAMEDIR"

echo "== native libs =="
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$APK" 'lib/arm64-v8a/*' -d "$TMP"
mkdir -p "$GAMEDIR/lib"
mv -f "$TMP"/lib/arm64-v8a/*.so "$GAMEDIR/lib/"
rm -rf "$GAMEDIR/lib/arm64-v8a"

echo "== store-zip game.apk =="
# The shim prefers a zip over thousands of small reads when it streams
# assets/bin/Data, so pack that subtree uncompressed.
GAMEDIR="$GAMEDIR" python3 - <<'PY'
import os, sys, zipfile
root = os.path.join(os.environ["GAMEDIR"], "assets", "bin", "Data")
out = os.path.join(os.environ["GAMEDIR"], "game.apk")
if not os.path.isdir(root):
    sys.exit("missing " + root)
n = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
    for dirpath, _dirs, files in os.walk(root):
        for f in sorted(files):
            full = os.path.join(dirpath, f)
            z.write(full, os.path.relpath(full, os.environ["GAMEDIR"]))
            n += 1
print("game.apk: %d files, %d bytes" % (n, os.path.getsize(out)))
PY

echo "== summary =="
du -sm "$GAMEDIR"/assets "$GAMEDIR"/lib "$GAMEDIR"/game.apk 2>/dev/null || true
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$GAMEDIR/lib/libil2cpp.so"
fi
echo
echo "Next: start the port from the Ports menu."
echo "All 8 DLC need a patched libil2cpp.so — see docs/DLC.md (your own DLC only)."
