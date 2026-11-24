# Testing

Two layers: host-side native tests (run anywhere via `nix develop` + WSLInterop) and an
in-game smoke test against the real unpacked SE.

## Real exe under wine — `tools/sml` (v2, any edition, no human)

```
tools/sml up R --edition enold|ense-en|ense-jp --saves steam|DIR   # own prefix slot + Xvfb; copies *.sdt only
tools/sml continue R [--slot N]   # title → save N via mod.game.save.load (no key timing); waits for a party
tools/sml test R tests/*.lua  # each suite returns {pass, fails, info} → PASS/FAIL, exit 1 on a FAIL
tools/sml shot R out.png      # LOOK at it: the test's numbers vs the HUD
```

| suite | proves | ense-en | ense-jp | enold |
|---|---|---|---|---|
| `sml continue --slot N` (mod.game.save.load) | title hooks → the requested slot loads (retail's own out-params log `kind 10040 slot N`); empty slot → `failed` + back to the title; a post-game save's Quest Selection → Main Quest | PASS (slots 2 / 7 / default → Lv11 / Lv17 / Lv8 saves; slot 42 refused) | PASS (slot 7) | PASS (slots 22, 3 = post-game) |
| `tests/world.lua` | camera view inside the map; area id → area table → map; story flags (list = get, set / clear round trip on a free id); area flags; inventory named; gold ≤ cap + set/restore | PASS (HUD ×630 = 630 CP; set_gold 4321 → HUD ×4321) | PASS (names Shift-JIS) | PASS (day-10b port save: area 210140 map 210) |
| `tests/sandbox.lua` | map.rooms / room (loadable, exits resolve); warp refusals (unknown room, map-less room); items.catalog names == inventory names; items.give through room_item_add (+1 record, unknown id refused); roster.set hp round trip; coordinates.set ±1 px | PASS (428 rooms / 408 loadable; catalog 306) | PASS | PASS (417 / 397; catalog 215) |
| `tests/warp.sh R` (shell; spans frames) | Enter through a load-time event (map.event()), forced warp to a neighbour → arrival (room, party, idle), back via retail's warp where allowed else forced | PASS (+ retail path by hand: 980110 → 210165, sparkle-out + dissolve, status requested → warping → idle; town refused) | PASS | PASS (the save loads into a recap + a school event) |
| `tests/roster.lua` | world/room/view via `g_world` + layout; party slots → chars → stat block; one leader; hp/mp ≤ max; level = combat + adventurer; positions inside the map | PASS (HUD-checked: 179/197, 43/43, Lv8, 3/5 stars; Lv15 save: 293/293 …) | PASS (Steam saves) | PASS (port drive save; coords checked vs the frame) |

`continue` caveats: SE's studio intro drops input until ~frame 1000 (one `ent` then skips it); enold has no
intro.  The title + picker cursors start where `config.dat` left them, so `up` copies no `config.dat` unless
`--keep-config`.  Parallel `up`s are safe (display / port / prefix slot claimed under one lock).
Saves for enold: OpenSummoners `tests/scenarios/*/_saves/*/` (copy the `.sdt` files; read-only there).

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
