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
| `tests/actors.lua` | actors.list (one leader; party == roster); codes() = char_init's dispatch (122 enold / 134 SE), live codes inside it; catalog() English names (51110 Org Archer, 51012 Yellow Slime), unknown refused; spawn by the leader (centre x = leader + 60 px, facing) + absolute (feet exact) via summon_spawn; remove via char_unload; party / empty refused | PASS (looked: Org Archer 51110 + slime + NPC drawn — none placed by the room; the slime fights) | PASS | PASS (looked: slime hits Arche 179→168, 180 frames on) |
| `tests/voice.lua` (title is enough; `--cfg voice=0` too) | the monster-SFX fix: SE + voice on → `sound_registrar`'s deluxe-skip je redirected, every no-variant row (monsters) holds its normal id, every deluxe row its deluxe id from the bank (⇒ bank loaded before the registrar); voice=0 → the shipped registrar drops them all; enold off | PASS (58/58, deluxe 200/200, 15 rows shadowed by a later same-group row; voice=0: 0/58) | PASS (same counts; voice=0 0/58) | PASS (off) |
| `tests/banks.sh R` (shell; needs no mod) | api 1.10: the bank tables relocate (every site verified), 40 recolours of the leader's sheet → 40 banks past the retail count, each decodes a frame on the leader (both facings), a retail unused bank still works, a room change and back | PASS (925 retail; banks 928..967) | PASS (929; 932..971) | PASS (909; 912..951) |
| `tests/recettear.sh R` (shell; `--mods recettear --cfg setup_trust=recettear`, `OSS_STEAM=<library with Recettear>`) | api 1.11: the first-time setup ran (Steam lookup, archive, sprites / portraits / music written to mod_data); the storeroom's new door by keys → the branch; the intro once; Recette + Tear in converted banks on the floor line; a portrait bank; the shop music playing; a wholesale order (stock + gold); a customer walks in on the floor line, buys at 110 %, leaves; the closing scene; back out, music off | PASS (alone, and with testroom + trainer loaded; `tests/hall.sh` PASS with both; looked: consent dialog + progress window with `sml shot --x`, portraits, cellar slimes) | not run | not run |
| `tests/roster.lua` | world/room/view via `g_world` + layout; party slots → chars → stat block; one leader; hp/mp ≤ max; level = combat + adventurer; positions inside the map | PASS (HUD-checked: 179/197, 43/43, Lv8, 3/5 stars; Lv15 save: 293/293 …) | PASS (Steam saves) | PASS (port drive save; coords checked vs the frame) |

`continue` caveats: SE's studio intro drops input until ~frame 1000 (one `ent` then skips it); enold has no
intro.  The title + picker cursors start where `config.dat` left them, so `up` copies no `config.dat` unless
`--keep-config`.  Parallel `up`s are safe (display / port / prefix slot claimed under one lock).
Saves for enold: OpenSummoners `tests/scenarios/*/_saves/*/` (copy the `.sdt` files; read-only there).

### For agents: `tools/sml_mcp.py` (MCP, stdio, stdlib only)

The same runs as typed tools: `sml` (passthrough: up / continue / down / play / wait / test …), `lua`, `state`,
`rooms`, `catalog`, `warp` (waits for the arrival), `teleport`, `give`, `heal`, `set_stats`, `gold`, `flags`,
`set_flag`, `actors`, `spawn`, `despawn`, `keys`, `shot` (returns the PNG).  Register: `"sml": {"type": "stdio", "command": "python3", "args":
["/opt/src/sotes-mod-loader/tools/sml_mcp.py"]}`; CLI: `tools/sml_mcp.py state run=R`, `--selftest`.  Verified over
JSON-RPC (initialize → tools/list → warp 440220 → 210110 → give refusal → shot) on ense-en.

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

## Windows (real Steam exes) — `tools/win/`

The packed SteamStub exes on the real install (`steamapps\common\sotes`, app 1381770), on the desktop via the
interactive task **`sml-win-launch`** (`install-task.ps1`; NOT `oss-rhost-runner`).  Copy `tools/win/*` + the tests to
`C:\sml-win\`, back up the install's `version.dll` to `C:\sml-win\backup\`, drop `build/version.dll` + an
`oss_loader.cfg` (`skip_launcher=1`, `windowed=1`, `voice=1`, `devserver=47811`) in, then over ssh (`win-wifi`; never
`timeout`/kill an ssh to that box): game dir + exe → `C:\sml-win\launch.txt`; `Start-ScheduledTask sml-win-launch`;
`continue.ps1 47811 -1`; `lua.ps1 47811 C:\sml-win\<suite>.lua`.  Kill by the exact pid.  Launching `sotes.exe` (JP) makes
SteamStub restart through Steam, which starts the app's EN exe — put `steam_appid.txt` (1381770) in the dir for the run.
Restore `version.dll`, remove `oss_loader.cfg` + `steam_appid.txt` after.

2026-11-24: voice PASS (EN 58/58 wait 40–50 ms, JP 58/58 43 ms, EN voice=0 0/58); roster / world / sandbox / actors PASS
on both (Steam save: Lv15 party, area 410210).

## The modded copy for hand play — `C:\sotes-modded` ("SotES with mods" on the desktop)

An isolated copy of the SE install (own saves, `steam_appid.txt` so SteamStub runs it in place; the Steam install and
rhost's `C:\oss-rhost` are never touched) with the loader + `../sotes-mods` trainer + dashboard.  `tools/win/deploy-modded.sh`
builds the loader and refreshes it (`tools/win/deploy.ps1` on the box: first run copies the game + makes the shortcut;
a running game's `version.dll` is renamed aside, the new one applies next launch).  **Auto:** the post-commit hook
(`tools/git-hooks`, `core.hooksPath`; `../sotes-mods/.git/hooks/post-commit` execs the same, local only) runs it in the
background when a commit touches `core/` / `mods/` / `loader.toml` / the deploy scripts; log `.git/deploy-modded.log`;
opt out `DEPLOY_MODDED=0`.  **Exe = the UNPACKED EN exe** (`sotes_en.exe`; the SteamStub one kept as `sotes_en.steam.exe`) — runs
without Steam (Steam refuses while the account plays on another machine); uploaded only when its hash differs; `UNPACKED=0` keeps the packed one.  `MODS="trainer dashboard autoload"` to ship more.  It ships `tools/win/modded.oss_loader.cfg`
(only when absent): windowed takeover = ONE window (the game's own, drawn by the loader; mod panels overlay it, F10),
starting at 1x, resizes snapping to whole multiples, integer-scaled present.  Looking at it: task **`sml-win-shot`**
(`shot.ps1`, PrintWindow of each sotes window → `C:\sml-win\shot-N.png` + sizes in `shot.txt`; a screen grab would
capture whatever covers the game).  Snap checked from inside the game via LuaJIT ffi (`SendMessageA(WM_SIZING)` with
the dev server on): +300/+250, +700/+540, bottom +400 → 2x (the monitor caps 3x); a 1084x721 client → a centred 1x image.

**Driving the overlay** (a panel, a dropdown) on the modded copy: add `devserver=47811` to its `oss_loader.cfg`, then
`ui.ps1 47811 "x=387,y=154"` / `"click=true"` / `"text=[[slime]]"` / `"key=[[enter]]"` [`SHOTNAME`] (`mod.ui.synth`;
client px = PrintWindow px − (8, 31)).  Posted WM_MOUSE* don't work: the Win32 backend re-reads the real cursor while
the game is foreground.  2026-11-24 combo_search: "org arc" → 1/134 Org Archer; "slime" → 8/134, Enter → 51013 Red
Slime, Spawn → slot 4; `mod.input.stats().captured` true while the search box has focus, false after.

**Display options (2026-11-24, modded copy, 1920x1080 box):** panel checkboxes "Dock beside the game" (window +380 px
sidebar, game left at its scale; F10 hides it and shrinks the window) and "Pixel-perfect scaling" (off = free resize,
aspect-fit smooth scaling), persisted as `loader.*` in oss_mods.cfg.  Checked from screen grabs (`shot.screen` flag +
`top_on.lua`/`top_off.lua`: PrintWindow AND screen copies both show only the ON-SCREEN part — an "overlay clipped" scare
was the test window hanging off a 1080p monitor): 2x docked 1660x960 slid back on screen after WM_EXITSIZEMOVE; free
1484x960 fill; F10 → 1104x960.  Keyboard re-acquire: not reproducible on demand — `mod.input.stats().reacquired` / the
log line `[input] keyboard was not acquired ... re-acquired` show it firing.
