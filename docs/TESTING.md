# Testing

Two layers: host-side native tests (run anywhere via `nix develop` + WSLInterop) and an
in-game smoke test against the real unpacked SE.

## Host-side (no game)

Built beside `version.dll`; run from **NTFS** (Windows blocks app-dir DLL side-load from
`\\wsl$` paths):

| test | proves |
|---|---|
| `build/lj_smoke.exe` | the cross-built LuaJIT runs on-target (JIT off, FFI call into kernel32) |
| `core/test/host_stub.c` | full loader path: proxy load → `mods\` scan → LuaJIT → `init.lua` → `mod.log` |
| `examples/probe/init.lua` | `mod.mem` (guarded, unmapped→nil) + `mod.game` list/toggle |
| `core/test/exec_test.c` | executor: deferred init on the safepoint, `on_frame` cadence, `mod.main`, `ti_mgr` capture |

`host_stub.exe <ms>` takes an optional lifetime so the executor's ~5 s window-search
fallback can complete (pass e.g. `7000`).

## In-game smoke test (real unpacked SE) — P0–P2 VALIDATED ✅

Validated 2026-07-19 against `sotes-trainer-oss.exe` (unpacked EN-SE, standalone, no Steam
DRM). `examples/probe_live/init.lua` read, live, off the running game:

```
[exec] safepoint hook armed @ 00437c70 (profile sotes_en va 0x00437c70)   <- MinHook on the real fn, game kept running
[mod] probe_live:   exe base = 0x00400000  MZ = 0x5a4d  scan('4d 5a') = 0x00400000
[mod] probe_live: [frame 88200] roster = 3 member(s)                       <- on_frame fires every frame
   Arche   Lv5  pos=(326400,25599)  hp=366/366  active=true                <- controlled-member detect (ti_mgr) works
   Sana    Lv5  pos=(317976,25599)  hp=286/286  active=false
   Stella  Lv5  pos=(322776,25599)  hp=297/297  active=false
   controlled member @ (326400,25599)
```

### Reproduce (careful — the stock dir is the trainer's dev env)

The unpacked exe + assets + saves live in `C:\oss-ennse-voice-repro\stock\`. Its `mods\`
has `sotes_trainer.dll`, which **also hooks `0x437c70`** — move it aside first or it fights
the executor.

1. Back up `stock\version.dll`; move `stock\mods\{sotes_trainer,ennse_voice}.dll` aside.
2. Install `build/version.dll` → `stock\version.dll`; add `stock\mods\probe_live\init.lua`.
3. Clear `stock\oss_modloader.log`, then launch **detached** (see below):
   `tools/dev-launch.sh 'C:\oss-ennse-voice-repro\stock' sotes-trainer-oss.exe`
4. Load a save in-game; read `stock\oss_modloader.log`.
5. Kill by **exact pid** (`tasklist.exe`/`taskkill.exe`, never `pkill`), then restore
   `version.dll` + the moved mods (wait for the file lock to release after the kill).

### Launch hang (fixed)

`cmd.exe /c start "" <exe>` launches the game but leaves WSL blocked until the whole
process tree exits (WSLInterop waits on `start`'s job — stdio redirection does not help).
Use `tools/dev-launch.sh` (PowerShell `Start-Process`), which detaches and returns in ~0.3 s.
