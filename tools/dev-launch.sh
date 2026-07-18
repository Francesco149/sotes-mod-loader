#!/usr/bin/env bash
# tools/dev-launch.sh — launch a Windows game exe DETACHED from WSL (no hang).
#
# `cmd.exe /c start "" <exe>` launches the game fine, but the WSL side stays BLOCKED
# until the whole process tree exits — WSLInterop waits on the job `start` keeps the
# child in, so redirecting stdio doesn't help.  PowerShell `Start-Process` creates the
# process outside that job and returns immediately.
#
# Usage:
#   tools/dev-launch.sh 'C:\oss-ennse-voice-repro\stock' sotes-trainer-oss.exe
#
# (cwd = the game dir so the exe finds sotesd.dll etc.)
set -euo pipefail
DIR="${1:?usage: dev-launch.sh <windows-game-dir> <exe-name>}"
EXE="${2:?usage: dev-launch.sh <windows-game-dir> <exe-name>}"
powershell.exe -NoProfile -Command \
  "Start-Process -FilePath '$DIR\\$EXE' -WorkingDirectory '$DIR'" >/dev/null 2>&1
echo "launched $EXE detached (cwd $DIR)"
