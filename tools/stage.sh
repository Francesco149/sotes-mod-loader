#!/usr/bin/env bash
# tools/stage.sh — build (incremental) + stage the loader, mods, and launcher into the scratch game
# install, so C:\oss-ennse-voice-repro\stock\ is ALWAYS ready to launch for in-game testing.
#
#   tools/stage.sh                              # build loader, stage version.dll + default mods (+ launcher if built)
#   SKIP_BUILD=1 tools/stage.sh                 # stage the existing build/ without rebuilding
#   MODS="autoload dashboard ui_demo" tools/stage.sh
#   STOCK=/mnt/c/other/game tools/stage.sh
#
# Then launch DETACHED (no WSL hang):
#   tools/dev-launch.sh 'C:\oss-ennse-voice-repro\stock' sotes-trainer-oss.exe
#
# Kill by EXACT pid (tasklist.exe/taskkill.exe, never pkill) before re-staging — a running game locks
# version.dll.  realver.dll (the real Windows version.dll the proxy forwards to) must already be there.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STOCK="${STOCK:-/mnt/c/oss-ennse-voice-repro/stock}"
MODS_REPO="${MODS_REPO:-$ROOT/../sotes-mods}"
MODS="${MODS:-autoload dashboard}"

[ -d "$STOCK" ] || { echo "!! scratch install not found: $STOCK" >&2; exit 1; }
[ -f "$STOCK/realver.dll" ] || echo "!! warning: $STOCK/realver.dll missing — the proxy has nothing to forward to"

# 1) loader — build incrementally (unless SKIP_BUILD=1), then copy over version.dll
if [ "${SKIP_BUILD:-0}" != 1 ]; then
  echo ">> building loader (incremental) ..."
  nix develop "$ROOT" --command make -C "$ROOT/core" >/dev/null
fi
[ -f "$ROOT/build/version.dll" ] || { echo "!! build/version.dll missing (run without SKIP_BUILD)" >&2; exit 1; }
cp -f "$ROOT/build/version.dll" "$STOCK/version.dll"
echo ">> staged loader:   version.dll ($(stat -c%s "$STOCK/version.dll") bytes, built $(date -r "$ROOT/build/version.dll" +%H:%M:%S))"

# 2) mods — sync each from ../sotes-mods (falling back to the loader's own examples/)
mkdir -p "$STOCK/mods"
for m in $MODS; do
  src="$MODS_REPO/mods/$m"; [ -d "$src" ] || src="$ROOT/examples/$m"
  [ -d "$src" ] || { echo ">> skip mod:     $m (not in ../sotes-mods or examples/)"; continue; }
  rm -rf "$STOCK/mods/$m"; cp -r "$src" "$STOCK/mods/$m"
  echo ">> staged mod:     $m"
done

# 3) launcher — copy the built .exe if present (rebuild it yourself; it changes rarely)
LX="$ROOT/launcher/target/x86_64-pc-windows-gnu/release/sotes-launcher.exe"
if [ -f "$LX" ]; then
  cp -f "$LX" "$STOCK/sotes-launcher.exe"
  echo ">> staged launcher: sotes-launcher.exe (built $(date -r "$LX" +%Y-%m-%d))"
else
  echo ">> launcher: not built — cargo build --manifest-path launcher/Cargo.toml -p sml-gui --release --target x86_64-pc-windows-gnu"
fi

echo ">> done. launch:    tools/dev-launch.sh 'C:\\oss-ennse-voice-repro\\stock' sotes-trainer-oss.exe"
