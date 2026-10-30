#!/usr/bin/env bash
# RE4 Modern Controls installer for Linux and Steam Deck (Proton).
#   ./install.sh               install or update, then choose the settings
#   ./install.sh --defaults    install with the recommended settings, no questions
#   ./install.sh --uninstall   remove the mod
#   ./install.sh --game-dir "/path/to/Resident Evil 4"   use this game folder instead of searching Steam
set -euo pipefail

APPID=254700
SUPPORTED_SHA256=19aed4af0ab06a748ff8744d45ac5580fcd6be6b6b7e944b1ab8822a00c8ee4a
HERE="$(cd "$(dirname "$0")" && pwd)"
DLL_SRC="$HERE/files/xinput1_3.dll"
INI_TEMPLATE="$HERE/files/default.ini"
REG_SECTION='[Software\\Wine\\AppDefaults\\bio4.exe\\DllOverrides]'  # as written in user.reg
REG_MATCH='AppDefaults\\bio4.exe\\DllOverrides]'

mode=install
defaults=0
game_dir=""
while [ $# -gt 0 ]; do
  case "$1" in
    --uninstall) mode=uninstall ;;
    --defaults) defaults=1 ;;
    --game-dir) game_dir="${2:-}"; shift ;;
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
  shift
done

say() { printf '%s\n' "$*"; }
die() { printf '\nError: %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------------------------------- find Steam and RE4
steam_roots=()
for r in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
         "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
  [ -f "$r/steamapps/libraryfolders.vdf" ] && steam_roots+=("$(cd "$r" && pwd -P)")
done

libraries=()
for r in "${steam_roots[@]}"; do
  libraries+=("$r")
  while IFS= read -r p; do libraries+=("$p"); done < <(
    sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$r/steamapps/libraryfolders.vdf")
done

if [ -z "$game_dir" ]; then
  for lib in "${libraries[@]}"; do
    acf="$lib/steamapps/appmanifest_$APPID.acf"
    [ -f "$acf" ] || continue
    name=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$acf")
    [ -f "$lib/steamapps/common/$name/Bin32/bio4.exe" ] && { game_dir="$lib/steamapps/common/$name"; break; }
  done
fi
[ -n "$game_dir" ] || die "Resident Evil 4 (2005) was not found in your Steam libraries.
Run the installer again with: ./install.sh --game-dir \"/path/to/Resident Evil 4\""
bin="$game_dir/Bin32"
[ -f "$bin/bio4.exe" ] || die "$bin/bio4.exe not found. Is this the Resident Evil 4 folder?"

prefix_reg=""
for lib in "${libraries[@]}"; do
  [ -f "$lib/steamapps/compatdata/$APPID/pfx/user.reg" ] && { prefix_reg="$lib/steamapps/compatdata/$APPID/pfx/user.reg"; break; }
done

if ps -eo comm= | grep -qx bio4.exe; then die "Resident Evil 4 is running. Quit the game first."; fi

say "RE4 Modern Controls"
say "Game folder: $game_dir"

# ------------------------------------------------------------------------------------------- Proton DLL override
# Proton prefers its own xinput1_3; this tells it to load the mod's copy for bio4.exe only (no launch option needed).
reg_add() {
  [ -n "$prefix_reg" ] || return 1
  grep -qF "$REG_MATCH" "$prefix_reg" && return 0
  cp "$prefix_reg" "$prefix_reg.re4mc-backup"
  printf '\n%s %s\n"xinput1_3"="native,builtin"\n' "$REG_SECTION" "$(date +%s)" >> "$prefix_reg"
}
reg_remove() {
  [ -n "$prefix_reg" ] || return 0
  grep -qF "$REG_MATCH" "$prefix_reg" || return 0
  # Drop the section header and its entries up to the next blank line.
  REG_MATCH="$REG_MATCH" awk 'BEGIN {m = ENVIRON["REG_MATCH"]} substr($0, 1, 1) == "[" && index($0, m) {skip=1; next}
       skip && /^$/ {skip=0; next}
       !skip' "$prefix_reg" > "$prefix_reg.tmp" && mv "$prefix_reg.tmp" "$prefix_reg"
}

is_ours() { [ -f "$1" ] && grep -qa "RE4 Modern Controls" "$1"; }

# ------------------------------------------------------------------------------------------- uninstall
if [ "$mode" = uninstall ]; then
  if is_ours "$bin/xinput1_3.dll"; then rm -f "$bin/xinput1_3.dll"; say "Removed the mod."; else say "The mod is not installed."; fi
  [ -f "$bin/xinput1_3.dll.re4mc-backup" ] && mv "$bin/xinput1_3.dll.re4mc-backup" "$bin/xinput1_3.dll" && say "Restored your previous xinput1_3.dll."
  rm -f "$bin/RE4ModernControls.ini" "$bin/RE4ModernControls.log"
  reg_remove
  say "Done. If you added a WINEDLLOVERRIDES launch option for this mod yourself, you can remove it now."
  exit 0
fi

# ------------------------------------------------------------------------------------------- install
[ -f "$DLL_SRC" ] || die "files/xinput1_3.dll is missing next to this script. Extract the whole zip first."
if command -v sha256sum >/dev/null; then
  sum=$(sha256sum "$bin/bio4.exe" | cut -d' ' -f1)
  if [ "$sum" != "$SUPPORTED_SHA256" ]; then
    say ""
    say "Warning: this bio4.exe is not the version the mod supports (Steam, update 1.1.0)."
    say "The mod will load but stay inactive, and the game plays as usual."
    if [ $defaults = 0 ]; then read -r -p "Install anyway? [y/N] " a; [[ "$a" =~ ^[Yy] ]] || exit 1; fi
  fi
fi

if [ -f "$bin/xinput1_3.dll" ] && ! is_ours "$bin/xinput1_3.dll"; then
  mv "$bin/xinput1_3.dll" "$bin/xinput1_3.dll.re4mc-backup"
  say "Kept your previous xinput1_3.dll as xinput1_3.dll.re4mc-backup."
fi
cp "$DLL_SRC" "$bin/xinput1_3.dll.new" && mv "$bin/xinput1_3.dll.new" "$bin/xinput1_3.dll"
say "Installed the mod."

# ------------------------------------------------------------------------------------------- settings
ini="$bin/RE4ModernControls.ini"
current() { [ -f "$ini" ] || return 0; sed -n "s/^$1=\\(.*\\)\$/\\1/p" "$ini" | tr -d '\r' | head -1; }
run=$(current Run); walk=$(current Walk); cross=$(current Crosshair); ashley=$(current ProtectAshley); height=$(current Height); invy=$(current InvertY)
run=${run:-Always}; walk=${walk:-1}; cross=${cross:-0}; ashley=${ashley:-1}; height=${height:-0}; invy=${invy:-0}

ask_choice() {  # ask_choice "question" default option...
  local q="$1" def="$2" i=1 a; shift 2
  say "" >&2; say "$q" >&2
  for o in "$@"; do say "  $i) $o" >&2; i=$((i + 1)); done
  read -r -p "Choose [${def}]: " a; echo "${a:-$def}"
}
ask_yn() {  # ask_yn "question" 1|0
  local d="Y/n" a; [ "$2" = 0 ] && d="y/N"
  read -r -p "$1 [$d] " a
  case "$a" in [Yy]*) echo 1 ;; [Nn]*) echo 0 ;; *) echo "$2" ;; esac
}

if [ $defaults = 0 ]; then
  say ""
  say "Settings (press Enter to keep the value in brackets; run the installer again to change them later)"
  case "$run" in OuterRing) d=2 ;; Button) d=3 ;; *) d=1 ;; esac
  case "$(ask_choice "How should Leon run?" $d \
      "Always run, hold the run button to walk (recommended)" \
      "Run when the stick is pushed all the way, walk below that" \
      "Classic: hold the run button to run")" in
    2) run=OuterRing ;; 3) run=Button ;; *) run=Always ;;
  esac
  say ""
  walk=$(ask_yn "Walk with the left stick while aiming?" "$walk")
  cross=$(ask_yn "Show a crosshair where the laser points?" "$cross")
  ashley=$(ask_yn "Protect Ashley from your own guns and knife?" "$ashley")
  [ "$height" = 0 ] && d=1 || d=2
  case "$(ask_choice "Camera height" $d "Same height as the original game (recommended)" "A bit higher")" in
    2) height=250 ;; *) height=0 ;;
  esac
  say ""
  invy=$(ask_yn "Invert camera up/down?" "$invy")
fi

# Start from the template (every option commented = recommended default) and set only what differs.
tmp=$(mktemp)
tr -d '\r' < "$INI_TEMPLATE" > "$tmp"
set_key() { sed -i "s/^;\\?$1=.*\$/$1=$2/" "$tmp"; }
[ "$run" != Always ] && set_key Run "$run"
[ "$walk" != 1 ] && set_key Walk "$walk"
[ "$cross" != 0 ] && set_key Crosshair "$cross"
[ "$ashley" != 1 ] && set_key ProtectAshley "$ashley"
[ "$height" != 0 ] && set_key Height "$height"
[ "$invy" != 0 ] && set_key InvertY "$invy"
sed 's/$/\r/' "$tmp" > "$ini" && rm -f "$tmp"
say ""
say "Saved settings to $ini"

# ------------------------------------------------------------------------------------------- Proton override
say ""
if reg_add; then
  say "Proton is set up to load the mod. Start Resident Evil 4 from Steam as usual."
else
  say "One more step, needed once: Resident Evil 4 has not been started on this PC yet."
  say "Either start the game once from Steam, quit it, and run this installer again,"
  say "or open Steam > Resident Evil 4 > Properties > Launch Options and paste:"
  say ""
  say '    WINEDLLOVERRIDES="xinput1_3=n,b" %command%'
fi
