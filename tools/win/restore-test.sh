#!/usr/bin/env bash
# tools/win/restore-test.sh on|off|status — the hand-play copy (C:\sotes-modded) in the game's own exclusive FULLSCREEN
# for the display-restore test, and back.
#   on      the current oss_loader.cfg is kept as oss_loader.cfg.normal; the test config goes in: windowed=0 (the
#           launcher is dismissed in Full Screen mode), no takeover, no capture, no companion window — the game presents
#           by itself, so alt-tab away and back loses and restores its DirectDraw surfaces
#   off     oss_loader.cfg.normal comes back
#   status  which one is in place
# The test: Mod Hall, Bit + the Tester on screen → alt-tab away, back → they must still be drawn, and
# oss_modloader.log must hold "[sprites] DISPLAY RESTORE #n" (no line = no surface was lost: the path did not run).
# ssh rules for that box: no client timeouts, never kill an ssh mid-command.
set -euo pipefail
banner() { timeout 5 bash -c "exec 3<>/dev/tcp/$1/2222 && head -c 4 <&3" 2>/dev/null | grep -q SSH; }
HOST="${WIN_HOST:-}"
if [ -z "$HOST" ]; then
  if banner 10.0.10.157; then HOST=win; elif banner 10.0.10.137; then HOST=win-wifi; else echo "!! windows box unreachable"; exit 2; fi
fi
D='C:\sotes-modded'
case "${1:-status}" in
  on) ssh "$HOST" "if (-not (Test-Path '$D\\oss_loader.cfg.normal')) { Copy-Item '$D\\oss_loader.cfg' '$D\\oss_loader.cfg.normal' }; Set-Content -Encoding ascii '$D\\oss_loader.cfg' @('# TEMPORARY: display-restore test (tools/win/restore-test.sh off puts oss_loader.cfg.normal back)','windowed=0','ddraw_takeover=0','ddraw=0','ui=0'); Get-Content '$D\\oss_loader.cfg'" ;;
  off) ssh "$HOST" "if (Test-Path '$D\\oss_loader.cfg.normal') { Move-Item -Force '$D\\oss_loader.cfg.normal' '$D\\oss_loader.cfg' }; Get-Content '$D\\oss_loader.cfg'" ;;
  status) ssh "$HOST" "Get-Content '$D\\oss_loader.cfg'; Test-Path '$D\\oss_loader.cfg.normal'" ;;
  *) echo "usage: $0 on|off|status"; exit 1 ;;
esac 2>&1 | grep -v '^\*\*' | tr -d '\r'
