#!/bin/bash
# PORTMASTER: vampiresurvivors.zip, Vampire Survivors.sh
# BYO-APK host loader: this port ships no game data, only the Android/Unity
# runtime shim. game/ must be filled from the player's own APK.
#
# SPDX-License-Identifier: GPL-3.0-only

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# PortMaster control folder (first one that actually has control.txt)
controlfolder=""
for d in "$XDG_DATA_HOME/PortMaster" \
         "/mnt/SDCARD/Persistent/portmaster/PortMaster" \
         "/opt/system/Tools/PortMaster" \
         "/opt/tools/PortMaster" \
         "/roms/ports/PortMaster"; do
  if [ -f "$d/control.txt" ]; then controlfolder="$d"; break; fi
done

if [ -n "$controlfolder" ]; then
  # shellcheck disable=SC1091
  source "$controlfolder/control.txt"
  get_controls
fi

# PortMaster exports $directory; a standalone run (SSH, tests) has none, so fall
# back to the launcher's own folder and to the sibling ports/ tree.
SELF=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
GAMEDIR=""
for c in "/${directory:-}/ports/vampiresurvivors" \
         "$SELF/vampiresurvivors" \
         "$SELF/../ports/vampiresurvivors"; do
  if [ -d "$c/game" ]; then GAMEDIR=$(cd -- "$c" && pwd); break; fi
done
if [ -z "$GAMEDIR" ]; then
  echo "vampiresurvivors: cannot find the port folder (expected <PORTS>/vampiresurvivors/game)"
  exit 1
fi

cd "$GAMEDIR" || exit 1

# fresh log per run, everything the shim prints goes there
: > "$GAMEDIR/log.txt"
exec > >(tee "$GAMEDIR/log.txt") 2>&1

# Unity writes its caches next to the game data, never into the read-only rootfs
export HOME="$GAMEDIR/game/home"
export TMPDIR="$GAMEDIR/tmp"
mkdir -p "$TMPDIR" "$HOME"

# PortMaster sets up the pad; the shim also needs a usable mapping on firmwares
# that ship SDL_GAMECONTROLLERCONFIG empty (e.g. SpruceOS on the TrimUI pad).
if [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG="0300a3845e0400008e02000014010000,TRIMUI Player1,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Linux"
fi
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-alsa}
export AUDIODEV=${AUDIODEV:-plughw:0,0}

# 30 fps pacer (SF_FRAME_US microseconds per frame; 33333 = 30 fps, 0 = unlimited).
# The game is far above 30 on this class of hardware; the cap keeps the SoC cool
# and leaves headroom for the SD-card texture streaming.
export SF_FRAME_US=${SF_FRAME_US:-33333}
export SF_FPS=${SF_FPS:-1}

chmod +x "$GAMEDIR/vampiresurvivors-nextos" 2>/dev/null
"$GAMEDIR/vampiresurvivors-nextos" "$GAMEDIR/game"
rc=$?
echo "exit=$rc"
exit $rc
