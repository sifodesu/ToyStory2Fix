#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_EXE_SRC="$ROOT_DIR/Setup/Toy2/Hd/toy2.exe"
GAME_DATA_SRC="$ROOT_DIR/Setup/Toy2/Hd/data"
GAME_SCRIPTS_SRC="$ROOT_DIR/Setup/Toy2/Hd/scripts"
GAME_DSOUND_SRC="$ROOT_DIR/Setup/Toy2/Hd/dsound.dll"
GAME_DSOUND_INI_SRC="$ROOT_DIR/Setup/Toy2/Hd/dsound.ini"
GAME_CD_VALIDATE="$ROOT_DIR/Setup/Toy2/Cd/validate.tta"
COMPAT_DATA="$ROOT_DIR/.toy2-proton"
PROTON_PATH="${PROTON_PATH:-}"
PFX_DIR="$COMPAT_DATA/pfx"
INSTALL_DIR="$PFX_DIR/drive_c/toy2"
INSTALL_EXE="$INSTALL_DIR/toy2.exe"

find_proton() {
  local candidate
  for candidate in \
    "$HOME/.steam/root/steamapps/common/Proton - Experimental/proton" \
    "$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

if [[ ! -f "$GAME_EXE_SRC" ]]; then
  echo "Missing game executable: $GAME_EXE_SRC" >&2
  exit 1
fi

if [[ ! -d "$GAME_DATA_SRC" ]]; then
  echo "Missing game data folder: $GAME_DATA_SRC" >&2
  exit 1
fi

if [[ ! -d "$GAME_SCRIPTS_SRC" ]]; then
  echo "Missing scripts folder: $GAME_SCRIPTS_SRC" >&2
  exit 1
fi

if [[ ! -f "$GAME_CD_VALIDATE" ]]; then
  echo "Missing CD validation file: $GAME_CD_VALIDATE" >&2
  exit 1
fi

if [[ -z "$PROTON_PATH" ]]; then
  PROTON_PATH="$(find_proton || true)"
fi

if [[ -z "$PROTON_PATH" ]]; then
  echo "Could not find Proton. Install Proton Experimental in Steam first." >&2
  exit 1
fi

if [[ -d "$HOME/.steam/root" ]]; then
  STEAM_CLIENT="$HOME/.steam/root"
elif [[ -d "$HOME/.local/share/Steam" ]]; then
  STEAM_CLIENT="$HOME/.local/share/Steam"
else
  echo "Could not find Steam client directory." >&2
  exit 1
fi

export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_CLIENT"
export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA"
export STEAM_COMPAT_APP_ID=0
export SteamAppId=0
export SteamGameId=0

mkdir -p "$COMPAT_DATA"

if [[ ! -d "$PFX_DIR" ]]; then
  "$PROTON_PATH" run wineboot -u >/dev/null
fi

mkdir -p "$INSTALL_DIR"

# Stage the game in C:\toy2 like a normal installation.
if [[ ! -f "$INSTALL_EXE" ]]; then
  cp -a "$GAME_EXE_SRC" "$INSTALL_EXE"
fi

if [[ ! -d "$INSTALL_DIR/data" ]]; then
  cp -a "$GAME_DATA_SRC" "$INSTALL_DIR/data"
fi

mkdir -p "$INSTALL_DIR/scripts"
cp -af "$GAME_SCRIPTS_SRC"/. "$INSTALL_DIR/scripts"/

# Keep game-specific DirectSound shim files beside the EXE.
if [[ -f "$GAME_DSOUND_SRC" ]]; then
  cp -a "$GAME_DSOUND_SRC" "$INSTALL_DIR/dsound.dll"
fi
if [[ -f "$GAME_DSOUND_INI_SRC" ]]; then
  cp -a "$GAME_DSOUND_INI_SRC" "$INSTALL_DIR/dsound.ini"
fi

# Emulate a real CD drive: D:\setup\toy2\cd\ maps to this rip root.
ln -snf "$ROOT_DIR" "$PFX_DIR/dosdevices/d:"
"$PROTON_PATH" run reg add "HKLM\\Software\\Wine\\Drives" /v "d:" /t REG_SZ /d "cdrom" /f >/dev/null
"$PROTON_PATH" run reg add "HKCU\\Software\\Wine\\Drives" /v "d:" /t REG_SZ /d "cdrom" /f >/dev/null

# The original installer writes these keys.
for hive in \
  "HKCU\\Software\\TravellersTalesToyStory2" \
  "HKLM\\Software\\TravellersTalesToyStory2" \
  "HKLM\\Software\\Wow6432Node\\TravellersTalesToyStory2"; do
  "$PROTON_PATH" run reg add "$hive" /v path /t REG_SZ /d "C:\\toy2\\data\\" /f >/dev/null
  "$PROTON_PATH" run reg add "$hive" /v cdpath /t REG_SZ /d "D:\\setup\\toy2\\cd\\" /f >/dev/null
done

# Use DXVK by default; set TOY2_FORCE_WINED3D=1 as fallback.
if [[ "${TOY2_FORCE_WINED3D:-0}" == "1" ]]; then
  export PROTON_USE_WINED3D=1
else
  unset PROTON_USE_WINED3D || true
fi

# Force old render/audio paths and keep it in a visible desktop.
BASE_OVERRIDES="dsound=n,b;sound=n,b;quartz=n,b;winegstreamer=d"
if [[ -n "${WINEDLLOVERRIDES:-}" ]]; then
  export WINEDLLOVERRIDES="${BASE_OVERRIDES};${WINEDLLOVERRIDES}"
else
  export WINEDLLOVERRIDES="${BASE_OVERRIDES}"
fi

cd "$INSTALL_DIR"
if [[ "${TOY2_VIRTUAL_DESKTOP:-1}" == "1" ]]; then
  DESKTOP_RES="${TOY2_DESKTOP_RES:-}"
  if [[ -z "$DESKTOP_RES" ]] && command -v xrandr >/dev/null 2>&1; then
    DESKTOP_RES="$(xrandr --current 2>/dev/null | awk '/\*/ {print $1; exit}')"
  fi
  DESKTOP_RES="${DESKTOP_RES:-1920x1080}"
  exec "$PROTON_PATH" run explorer /desktop=ToyStory2,"$DESKTOP_RES" "C:\\toy2\\toy2.exe"
else
  exec "$PROTON_PATH" run "$INSTALL_EXE"
fi
