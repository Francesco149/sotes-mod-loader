# Testing

Two layers: host-side native tests (run anywhere via `nix develop` + WSLInterop) and an
in-game smoke test against the real unpacked SE.

## Host-side (no game)

`nix develop --command make -C core tests` builds + runs the harness exes below via
WSLInterop (statically linked — no DLL side-load, so they run straight from the repo path).
The DLL and DLL-loading tests still run from **NTFS** (Windows blocks app-dir side-load from
`\\wsl$`):

| test | proves |
|---|---|
| `build/lj_smoke.exe` | the cross-built LuaJIT runs on-target (JIT off, FFI call into kernel32) |
| `core/test/host_stub.c` | full loader path: proxy load → `mods\` scan → LuaJIT → `init.lua` → `mod.log` |
| `examples/probe/init.lua` | `mod.mem` (guarded, unmapped→nil) + `mod.game` list/toggle |
| `core/test/exec_test.c` | executor: deferred init on the safepoint, `on_frame` cadence, `mod.main`, `ti_mgr` capture |
| `core/test/hook_typed_test.c` | **Tier-2 typed hooks:** an FFI-closure detour modifies args / blocks / modifies the return across **cdecl/stdcall/thiscall** (callee-cleanup + ecx capture), the off-thread gate skips the chain, and cross-tier exclusion holds — all on local targets, `>> TYPED_HOOK_OK` |
| `core/test/native_mod_test.c` | the native ABI: `OssModInit` on the safepoint, `mem_*`/`scan`, `hook_entry`, `on_frame`, `main_enqueue`, `hook_remove`, `>> NATIVE_ABI_OK` |
| `core/test/ui_smoke.c` | **P5 UI:** drives the full decoupled pipeline — the smoke's main thread plays the engine, calling `ui_build()` (runs the `mod.ui` callbacks, publishes a snapshot) while the UI thread fetches + replays it. Stands up the real companion window + DX11 device, renders for a few seconds, tears down — `>> UI_SMOKE_OK`. Not in the auto-run `tests` (it opens a window); run explicitly: `make -C core ../build/ui_smoke.exe && (cd build && ./ui_smoke.exe 3)`. Proves device creation (HW/WARP), the lock-free triple buffer, and the replay path. |

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

## In-game UI test (P5) — VALIDATED ✅

Stage `examples/ui_demo/` into `stock\mods\ui_demo\` alongside the loader (same steps as the
probe above; a hands-free boot needs `oss_loader.cfg` with `skip_launcher=1` + `windowed=1`),
launch, and:

1. **Companion window** — a "SotES Mod Loader" window appears. The built-in header shows
   `executor armed` + the panel/window counts; the "UI Demo" section shows a live frame counter and
   a working button / checkbox / slider; the party roster fills in once a save is loaded. **F10**
   toggles the window.
2. Confirm the game stays smooth with the UI up — the UI thread is fully decoupled (a lock-free
   snapshot; no lock between it and the game). Check `oss_modloader.log` for `[ui] companion window
   up`, the armed safepoint (so `ui_build` runs every frame), and no `[ui] '<name>' draw error`.

Confirmed on the real EN-SE (2026-07-19): `[ui] companion window up` on the armed `0x437c70`, the
demo registered its panel + window, no crash.

## In-game overlay test (Phase C) — VALIDATED ✅

Same `examples/ui_demo/` staging, but add `ddraw_takeover=1` to `oss_loader.cfg`.  Now the panels draw
as an **overlay on top of the game frame** (no separate window), and `ui_start` does NOT create the
companion window (log: `[ui] in-game overlay mode (takeover) — companion window disabled`).

1. The "SotES Mod Loader" overlay draws over the borderless-fullscreen game; the UI Demo panel + the
   floating "UI Demo — About" window replay the same snapshot.  **F10** toggles it (F8 stays the
   game's own menu key).
2. **Mouse drives the overlay, keyboard drives the game** — you keep full character control with the
   overlay up.  Clicking "log a line" writes `[mod] ui_demo: ... button pressed at frame N` (proves
   mouse → WndProc → ImGui → atomic slot → engine → the Lua callback).
3. Log: `[ddraw] TAKEOVER: ... borderless`, then `[ui] in-game overlay up (engine-thread ImGui ...)`,
   no `draw error`, no fault.

Confirmed on the real EN-SE (2026-07-19): overlay up on the engine-thread ImGui over the game frame,
gameplay reached via autoload, F10 toggles, button clicks logged, keyboard still moved the character —
no crash.
