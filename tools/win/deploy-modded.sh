#!/usr/bin/env bash
# tools/win/deploy-modded.sh — build the loader and refresh the isolated modded game copy on the Windows box
# (C:\sotes-modded, desktop shortcut "SotES with mods"; tools/win/deploy.ps1 does the Windows side).
#
#   tools/win/deploy-modded.sh                 # build + deploy loader + $MODS
#   MODS="trainer dashboard autoload" ...      # which ../sotes-mods mods (default: trainer dashboard testroom)
#   UNPACKED=0 ...                             # keep the SteamStub exe (default: the unpacked EN exe runs as
#                                              # sotes_en.exe — no Steam needed, e.g. Steam busy on another machine)
#
# Run by the post-commit hooks of this repo and ../sotes-mods (tools/git-hooks/post-commit, in the background, log
# .git/deploy-modded.log of the committing repo).  One deploy at a time (flock).  ssh rules for that box: NO client
# timeouts, never kill an ssh mid-command (runaway sshd — see OpenSummoners' memory); host pick = TCP banner only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODS_REPO="${MODS_REPO:-$ROOT/../sotes-mods}"
MODS="${MODS:-trainer dashboard testroom}"
UNPACKED="${UNPACKED:-1}"
UNPACKED_EXE="${UNPACKED_EXE:-/mnt/ephemeral/opensummoners/game/ense/sotes_en.unpacked.exe}"
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/sotes-deploy-modded.lock"
flock 9

banner() { timeout 5 bash -c "exec 3<>/dev/tcp/$1/2222 && head -c 4 <&3" 2>/dev/null | grep -q SSH; }
HOST="${WIN_HOST:-}"
if [ -z "$HOST" ]; then
  if banner 10.0.10.157; then HOST=win; elif banner 10.0.10.137; then HOST=win-wifi; else echo "!! windows box unreachable"; exit 2; fi
fi

echo ">> $(date -Is) build loader"
nix develop "$ROOT" --command make -C "$ROOT/core" >/dev/null

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/payload/mods"
cp "$ROOT/build/version.dll" "$STAGE/payload/"
cp "$ROOT/tools/win/modded.oss_loader.cfg" "$STAGE/payload/oss_loader.cfg"
for m in $MODS; do
  [ -d "$MODS_REPO/mods/$m" ] && cp -r "$MODS_REPO/mods/$m" "$STAGE/payload/mods/" || echo ">> skip $m (not in $MODS_REPO/mods)"
done
cp "$ROOT/tools/win/deploy.ps1" "$STAGE/"
if [ "$UNPACKED" = 1 ] && [ -f "$UNPACKED_EXE" ]; then
  # 72 MB: upload only when the copy's sotes_en.exe differs (deploy.ps1 swaps it in, the SteamStub one kept aside)
  want="$(sha256sum "$UNPACKED_EXE" | cut -d' ' -f1)"
  have="$(ssh "$HOST" '(Get-FileHash C:\sotes-modded\sotes_en.exe -ea 0).Hash' 2>/dev/null | tr -d '\r' | tr A-F a-f || true)"
  if [ "$have" != "$want" ]; then cp "$UNPACKED_EXE" "$STAGE/payload/sotes_en.exe"; echo ">> + unpacked sotes_en.exe"; fi
fi

echo ">> upload to $HOST (mods: $MODS)"
ssh "$HOST" 'New-Item -ItemType Directory -Force C:\sml-win | Out-Null; Remove-Item -Recurse -Force C:\sml-win\payload -ea 0' 2>&1 | grep -v '^\*\*' || true
scp -q -r "$STAGE/payload" "$STAGE/deploy.ps1" "$HOST:C:/sml-win/" 2>&1 | grep -v '^\*\*' || true
ssh "$HOST" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\sml-win\deploy.ps1' 2>&1 | grep -v '^\*\*'
