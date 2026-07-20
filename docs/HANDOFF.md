# HANDOFF — current checkpoint

Rolling "where we are / what's next" for a fresh session. Orient: this → `DESIGN.md`
(architecture + phase table) → `MOD-FORMAT.md` (the `mod.*` API) → `TESTING.md`.

> **Cross-reference `../OpenSummoners`** — the parity port + the **canonical RE home** for the
> SAME `sotes.exe`. Look there before reversing (decompile `docs/decompiled/`, findings, Frida
> harness, trainer `tools/sotes_trainer/SE_CODE_MAP.md`); and **record any NEW engine RE finding
> THERE** (`SE_CODE_MAP.md` / `engine-quirks.md`), not only in this repo — loader docs cover the
> loader, game facts belong to the port so both projects benefit. (README "Related".)

## State: P0–P5 DONE + **DDraw present (borderless + windowed takeover)** + **in-game overlay** + **`mod.config`** + **semantic `mod.game` ops** + **mod API versioning** + **early-boot phase & the `OssModEarlyInit` ABI** + **built-in JP voice restore** (2026-07-20). Next = **P8 launcher** + P7 trainer as a mod.

### Session 2026-07-20 (early-boot ABI + a live RE pass)
- **`OssModEarlyInit`** — the general early-boot native ABI (`core/oss_mod_api.h` "EARLY-BOOT ABI"):
  a mod patches the game DURING its own boot (before the engine inits) with a small vtable —
  `log`/guarded `mem`/`patch`(RWX code)/`config`/**`when_text_ready`** (the shared decrypt-wait that
  generalizes the voice restore's SteamStub poll).  Loaded in a new early native pass in `loader.c`;
  `examples/early_probe` + a host `EARLY_ABI_OK` test; validated in-game.
- **Live RE (vs save 15)** — recorded in **`../OpenSummoners`**: `docs/findings/save15-live-stats.md`
  (the character **stat block** — name/HP/MP/the 4 combat stats/levels/exp; the **adventure-stats**
  struct; the per-char **save record**; and the negative result that SE has **no fixed-index party
  array**) and `docs/findings/title-menu-state.md` (the title runner + input-ring + injection).
- **`mod.game.roster`** now exposes the full stats (attack/defense/spirit/resist, combat & adventurer
  level, `char_level` = their sum, exp/exp_max); `mod.game.input.press(id)` injects UI buttons for
  **arbitrary menu navigation**.  Auto-load now warns on the Start-default new-game case.
- **Open follow-up:** the title-menu selection **cursor** is not in the input manager (it's in the
  title scene object) — needed to fully fix "auto-load starts a new game when the title defaults to
  Start".  Building block (button injection) is in place.  See `title-menu-state.md`.

The loader now also OWNS THE DISPLAY: it hooks the DirectDraw present, and either mirrors the game in
the companion window (`ddraw=1`, default) or TAKES OVER the game window with a vsync'd, sharp-bilinear
D3D11 swapchain — **`ddraw_takeover=1`** borderless-fullscreen OR **`=2`** a resizable normal window
(both user-confirmed smooth).  In takeover the `mod.ui` host draws as an **in-game overlay** on top of
the game frame (F10 toggles) — retiring the companion window there.  See "DDraw present backend",
"Phase C — the in-game overlay", and "Mod config + semantic ops + versioning".

The loader is a usable modding API — in Lua AND native C: guarded memory (`mod.mem`, incl. `patch` for
code), live RE'd game state + **semantic ops** (`mod.game.*` — roster/coords/mouse, plus `attract`/
`save` that expose RE'd menu-drives so a mod ORCHESTRATES instead of reimplementing), per-frame/one-shot
engine work (`mod.on_frame`/`mod.main`), chained hooks (`mod.hook`), ImGui panels (`mod.ui`), and
**per-mod settings** (`mod.config`, schema in `mod.toml [config]`).  Mods declare the API they need
(`[loader] api`); the loader **refuses an incompatible one**.

The loader is a usable modding API — in Lua AND native C: a mod reads guarded memory
(`mod.mem` / `api->mem_*`), reads live RE'd game state (`mod.game.*`), runs per-frame/one-shot
on the engine thread (`mod.on_frame`/`mod.main` / `api->on_frame`/`main_enqueue`), **hooks
any function alongside other mods** (`mod.hook` — Tier-1 observe + Tier-2 typed; native
`api->hook_entry` shares the same chain), and adds **ImGui panels/windows** to a loader-owned
companion window (`mod.ui`).

| phase | what | status |
|---|---|---|
| P0 | proxy `version.dll` + LuaJIT host + `mods\` scan (native + Lua) | ✅ |
| P1 | `mod.mem` (guarded) + togglable `mod.game.*` bindings (roster, coordinates) | ✅ |
| P2 | main-thread executor: WndProc bootstrap → safepoint hook `0x437c70` → `mod.main`/`on_frame` | ✅ |
| P3 | chained hook registry — **Tier-1 observers** (`mod.hook.entry`) **+ Tier-2 typed** (`mod.hook.typed`) | ✅ |
| P4 | native-mod C ABI: `OssModInit(const OssApi*)`, shared hook + executor registries | ✅ |
| P5 | ImGui UI: loader-owned companion window — `mod.ui` + a lock-free snapshot pipeline | ✅ (host + in-game) |
| P6 | JP voice restore — landed as a **built-in** (early-boot phase, `core/voice.c`), + the general **`OssModEarlyInit`** ABI it generalizes into | ✅ |
| P7 | trainer → a mod; coexistence verified | ⬜ |
| P8 | launcher (package manager: git-repo sources, install/update, Launch) | ⬜ (NEXT) |

## Mod config + semantic ops + versioning (NEW — 2026-07-19, groundwork for P8)

Direction set this session: **known RE work becomes a semantic loader op; a mod collapses to telling
the loader what to do + its config.** Arbitrary `mod.mem.patch`/`mod.hook` stay for the un-upstreamed
frontier.  Landed:

- **`mod.config`** — a mod declares its settings in **`mod.toml [config]`** (`type` bool/int/float/
  string, `default`, `label`, `min`/`max`); `mod.config.get/set/schema`.  Schema parsed by a small
  pure-Lua TOML subset reader in the loader (`CONFIG_LUA` in `lua_host.c`) — the SAME file a Go/Rust/Zig
  launcher will read to render the SAME table.  Values in **`oss_mods.cfg`** (namespaced `<id>.<key>`,
  machine-managed, separate from `oss_loader.cfg`), **debounced** write (a slider drag → one write, via
  `config_mod_flush` on the safepoint).  `examples/config_demo` renders a generic editor from the schema.
- **Semantic `mod.game` ops (SotES)** — `mod.game.attract.set(on)` (attract/demo toggle) and
  `mod.game.save.load(slot)` (drive title→picker→save).  The old `core/sotes_autoload.c` + `autoload=`
  flag are **retired** — the mechanism moved into `sotes_bindings.c`, and auto-loading a save is now the
  **`autoload` mod** (in `../sotes-mods`, the first core→mod extraction): a few lines calling `save.load`.
- **`mod.mem.patch(va, bytes)`** — general code patching (RWX + memcpy + icache flush).
- **Mod API versioning** — `OSS_API_VERSION` (MAJOR.MINOR); a mod's `[loader] api` is checked before it
  runs and an incompatible one is **REFUSED** (major mismatch = breaking; minor-too-new = missing
  features; absent ⇒ "1.0").  `mod.api_version` for runtime checks.  See MOD-FORMAT "Versioning".

All validated in-game (EN-SE): config round-trips + persists, the autoload mod reaches gameplay via
`mod.game.save`, an api-"2.0" mod is refused.

**Next (P8 launcher):** a single-binary package manager (Go/Rust/Zig — decide when starting; ImGui/C++
also on the table) per `REGISTRY.md`, reading `mod.toml [config]` to render the generic settings editor
and writing `oss_mods.cfg`.  Ships `../sotes-mods` as the default source.  The registry lives in
`../sotes-mods` (already scaffolded: `registry.json` + `mods/autoload`).

## DDraw present backend + borderless-fullscreen (NEW — `core/ddraw_present.{h,cpp}`, 2026-07-19)

SotES renders through a DirectDraw7 software blitter and PRESENTS once per frame in **zdd_present
(EN-SE `0x5e1470`, `profile.present_va`)** — thiscall, ECX = the ZDD screen ctx (mode field `+0x164`),
finished back-buffer surface at `*(*(ECX+0x16c)+0x2c)` (640×480, **24bpp** in EN-SE windowed).  The
executor hooks it (asm thunk like the safepoint), and `ddraw_present.cpp` re-presents the frame through
our own **vsync'd D3D11 swapchain**.  Two modes (config):

- **`ddraw=1`** (default): capture the back-buffer into a lock-free triple buffer; the UI thread draws
  it as a game mirror behind the companion UI.  *Proven.*
- **`ddraw_takeover=1`** (opt-in): OWN the game window — the present thunk **`ret 4`'s to SKIP the
  game's own (unsynced ~130fps GDI BitBlt) present** and we drive the window: **borderless-fullscreen**
  (`WS_POPUP` over the monitor), **sharp-bilinear** upscale, aspect-correct pillarbox, `Present(1,0)`
  (smooths + paces).  Makes windowed play look/feel like a smooth exclusive-fullscreen.  Companion
  window auto-hides (F8 to show).  *User-confirmed in-game "feels good".*

Also **`mod.game.mouse.get()`** → `{screen_x, screen_y, over, world_x, world_y}`: the cursor in the
game's 640×480 space (the ddraw layer undoes the pillarbox / client scale) + world centi-px via the
camera (`render_root+0x104c` → view obj; origin `+0x60/+0x5c`, span `+0x64/+0x68`).  *Validated to the px.*

## Phase C — the in-game overlay (DONE, 2026-07-19)

The `mod.ui` panels now draw ON TOP of the game frame, inside the game window, in borderless takeover —
retiring the companion window there.  The P5-anticipated "renderer-hook backend consuming the SAME
snapshot": **`mod.ui` is unchanged.**  Implemented (`ddraw: Phase C` commit):

- **ONE ImGui context, owned by the ENGINE thread** — `ui_overlay_present()` (`ui.cpp`), called from
  `ddp_engine_present()` after the game-frame quad, before `Present(1,0)`.  In takeover `ui_start`
  does NOT spawn the companion UI thread (the ⚠ from the plan — ImGui's global current-context would
  be raced across two threads), so there's exactly one context on one thread.  The engine thread is
  BOTH producer (`ui_build`) and consumer, so the lock-free triple buffer just degenerates to
  same-thread latest-wins — the existing `draw_from_snapshot`/`replay_cmds`/`snap_fetch` are reused
  as-is (all already in `ui.cpp`; no factoring needed).
- **Input** — the executor's `boot_wndproc` forwards to `ui_overlay_wndproc()`; the overlay is
  **MOUSE-ONLY** (no ImGui keyboard-nav), so gameplay keys belong entirely to the game (you keep full
  control with the overlay up).  The game ignores the mouse — no contention.  Toggle is **F10** (F8
  collides with SotES' in-game menu; F10 is a system key so we consume it to avoid menu-mode).
- **Validated in-game** (unpacked EN-SE, `ddraw_takeover=1`): overlay drew on the engine-thread ImGui
  over the game frame, gameplay reached via autoload, F10 toggles, mouse clicks reached the `mod.ui`
  callbacks (button presses logged), keyboard still drove the character — no crash.

**Next:** flip `ddraw_takeover` default-on after edge-testing — multi-monitor (`MonitorFromWindow`
picks the game's monitor — verify placement), alt-tab in/out of the borderless window, and restoring
the `WS_POPUP` style + overlay ImGui teardown on clean unload (`DllMain` `DLL_PROCESS_DETACH`,
reserved==NULL).  Then **P6 voice**.

**In-game validation** (unpacked EN-SE `sotes-trainer-oss.exe`, 2026-07-19): executor armed
on the real `0x437c70` (game ran 24 min, no crash); `mod.game.roster` read all 3 live party
members (correct coords/HP + `active` detect); the hook registry's codegen thunk installed
on `kb_poll`, register capture correct (`ctx.ecx == the real this`), 2 cbs on 1 VA both
fired, `remove` worked. Evidence: `TESTING.md`.

**Tier-2 validation** — host (`make -C core tests` → `>> TYPED_HOOK_OK`): an FFI-closure
detour **modifies args, blocks, and modifies the return** across **cdecl / __stdcall /
__thiscall** (callee-stack-cleanup + ecx-capture — the crashy bits — all clean); the
**off-thread gate** skips the Lua chain; cross-tier exclusion holds both ways. **AND in-game**
(2026-07-19, `examples/hook_typed`): the typed hook installed on the live `kb_poll` thiscall,
the FFI closure marshaled the real `ecx` (keyboard device) as `ctx.args[1]` **every frame**
(`this match=true`, hits 119→599), and `mod.hook.remove` froze it cleanly (game stayed up).

## Build / run

Self-contained flake (no dep on OpenSummoners). Everything in `nix develop`:
```
nix develop --command make -C core            # -> build/version.dll  (LuaJIT cross-built once, cached)
```
Host-side tests: `core/test/{lj_smoke,host_stub,exec_test}.c` + `examples/*` (run from NTFS,
not `\\wsl$`). In-game: see `TESTING.md` (stage into the game dir, launch with
`tools/dev-launch.sh`, read `oss_modloader.log`, restore).

## Core files (`core/`)

- `loader.c` — DllMain, proxy attach, `config_load`, `mods\` scan (defers Lua mods to the
  executor); after arming, starts the UI host (`ui_start`, unless `ui=0`).
- `lua_host.c` — LuaJIT bring-up (JIT compiled OUT, FFI on), per-mod env + `pcall` isolation,
  builds each mod's `mod` table (mem/game/main/on_frame/hook/**ui**).  Lua is single-threaded on
  the engine thread (the UI thread never touches it — no lock).
- `ui.cpp` / `ui.h` — the ImGui/DX11 UI host (P5): the UI thread + companion window, the lock-free
  snapshot **triple buffer** + **atomic input slots**, the `mod.ui` binding + widgets.  `ui_build()`
  (engine thread, from the safepoint) records the snapshot; the UI thread replays it.  Only C++ TU.
- `mem.c` — guarded r/w (VirtualQuery), scan, module/reloc. `mem.h` guards used by bindings.
- `game_bindings.c` — the togglable `mod.game.*` registry (dynamic-resolve metatable + enable/disable).
- `sotes_bindings.c` — SotES roster+coordinates, profile-gated. **PORT-DEBT: heap-scan (task #7).**
- `executor.c` — WndProc bootstrap (positive `profile.window_class` match — see gotchas),
  safepoint hook (register-capture asm thunk), job queue + on_frame, launcher dismiss
  (`skip_launcher`/`windowed`), `exec_main_tid`/`exec_main_tid_ptr`/`exec_ti_mgr`/`exec_game_hwnd`.
  The safepoint also calls `ui_build()` (records the UI snapshot on the engine thread).
- `hooks.c` — the chained hook registry (both tiers): Tier-1 per-VA codegen thunk →
  `hook_dispatch` → observer chain; Tier-2 per-VA main-thread gate → FFI-closure dispatcher
  (the embedded `TIER2_LUA`) → typed chain. `g_hk[]` carries a `tier` (VA is T1 xor T2).
- `oss_mod_api.h` — the STABLE native-mod ABI (the only header a native mod needs): `OssApi`
  vtable, `OssHookCtx`, `OssModInit`.  `native_bridge.c` — fills that vtable from mem/executor/
  hooks and hands it to each native mod (`oss_api()`).
- `profile.c` — the C game profile (safepoint VA, **window class**, launcher class + control ids).
- `config.c` — `oss_loader.cfg` (key=value) reader. `minhook/` — vendored trampoline backend.
- `prof.c` — opt-in per-frame profiler (`profile=1`): times the safepoint / ui_build / roster on the
  engine thread, logs avg/max µs every 5 s.  `sotes_autoload.c` — dev save auto-load (menu-drive) +
  keepactive lives in `executor.c` (`exec_keepactive`, `exec_defer_fn`, `exec_sp_now`).

## P3 Tier-2 — typed hooks (LANDED)

`mod.hook.typed(va, "<ret> [__conv](args)", {pre=,post=})`. How it works (all in `hooks.c`
+ an embedded Lua chunk `TIER2_LUA`):

- **An FFI closure IS the detour** (the locked design). `ffi.cast(ctype, dispatch)` makes the
  Lua chain-dispatcher C-callable; MinHook points the VA at it (via the gate, below). The
  dispatcher marshals typed args into `ctx.args`, runs pre (may mutate args / set
  `ctx.blocked`+`ctx.ret`), calls the original via `ffi.cast(ctype, trampoline)` unless
  blocked, runs post (may rewrite `ctx.ret`). Each cb in `pcall` (fault → disabled + logged).
- **A tiny C main-thread GATE fronts the closure** (`gen_gate_thunk`): `mov eax,fs:[0x24]; cmp
  [&g_main_tid]; jne -> jmp tramp; jmp closure`. This is the keystone fix — a pure FFI-closure
  detour would enter the shared `lua_State` on *whatever* thread calls the target; an
  off-engine-thread call would corrupt every mod. The gate runs Lua only on the engine thread
  and otherwise runs the original untouched. (EAX/EFLAGS are dead at a call boundary; ECX/EDX/
  stack pass through, so thiscall `this` reaches the closure.)
- **Verified fact:** LuaJIT x86 callbacks handle `__cdecl/__stdcall/__thiscall/__fastcall`
  incl. the non-cdecl callee stack cleanup (`lj_ccallback.c:610-663`) — so a typed detour with
  the target's real convention is stack-correct. The sig string **must match the real
  convention + arg count** (wrong count → stack corruption; that's why Tier-1 is the default).
- **One tier per VA** (one MinHook per target): a unified `g_hk[]` with a `tier` field; Tier-1
  and Tier-2 refuse each other on the same VA (+log). Within a tier, mods chain.
- **Handles:** Tier-2 handles are `>= 0x40000000`; `mod.hook.remove` routes by handle. Removal
  marks the chain entry dead (MinHook + the closure stay installed — a later reclaim).

Host-proven exhaustively by `core/test/hook_typed_test.c` (`make -C core tests`). Not yet run
in-game — `examples/hook_typed/init.lua` is the smoke test (typed `kb_poll`, observe-only).

## P4 native-mod C ABI (LANDED)

`mods\<name>.dll` exports `int OssModInit(const OssApi *api)`; the loader resolves it
(`GetProcAddress`) and **defers it to the first safepoint** so it runs on the engine thread
like a Lua mod's init.  `OssApi` (`core/oss_mod_api.h` — the whole contract, versioned by
`abi_version` + `struct_size`, append-only) wires to the internal services via
`core/native_bridge.c`:

- `log`; guarded `mem_read/write/readable/writable/base/reloc/scan` (the AOB scan core was
  lifted out of `l_scan` into `mem_scan_aob`).
- `main_enqueue`/`on_frame` — the C-side of `mod.main`/`mod.on_frame`.  The executor's job
  queue + on_frame list now hold a tagged entry (Lua ref OR C fn+user).
- `hook_entry`/`hook_remove` — a native `OssHookEntryFn` joins the SAME per-VA Tier-1 chain as
  Lua `mod.hook.entry` (the `hcb` slot is tagged; the dispatcher builds the Lua ctx table only
  when a Lua cb is present, so a native-only hook needs no Lua).  Entry handles are now
  `0x10000 | (rec<<8) | slot` (never 0, since the ABI uses 0 = fail).
- A DLL with NO `OssModInit` still loads as a legacy standalone native mod (the trainer/voice
  today) — its DllMain runs, no ABI handoff.

Host-proven by `core/test/native_mod_test.c` (`>> NATIVE_ABI_OK`).  Example: `examples/
native_hello/` (a real `.dll` — `make -C examples/native_hello`).  **Not yet run in-game**
(host-verified; the trainer/voice migrations P6/P7 will exercise it live).

Native gaps (deferred, ABI-compatible to add): **no SEH isolation** for native cbs (a faulting
native mod can fault the game; Lua gets pcall); no native **Tier-2 typed** hooks; no native
`mod.game.*` accessor.

## P5 UI host (LANDED — `core/ui.cpp` + `ui.h`)

A loader-owned **companion window** (ImGui/DX11, its own thread + DX11 device — the trainer's
stock-example helper, HW→WARP, `R8G8B8A8`, DISCARD, `Present(1,0)`), fed by a **lock-free,
decoupled pipeline** to/from the engine thread.  (The first cut ran the mod.ui callbacks on the UI
thread under a shared "Lua Big Lock" that every engine-thread Lua entry point also took; in-game
that coupled the game's main thread to the UI thread + locked the hook hot-path → **visible lag**.
Replaced — the LBL is **gone**; Lua is single-threaded on the engine thread again.)

- **engine thread — `ui_build()`** (called from the executor safepoint, self-throttled ~30 Hz via
  `GetTickCount`): runs the mod.ui callbacks, which **record a plain-data snapshot** (per-surface
  command lists), and publishes it through a **lock-free triple buffer** (`g_ctl` CAS; latest-wins —
  an unconsumed frame is overwritten, never queued).  `SetEvent(g_dirty)` only when the snapshot
  actually changed.  This is the *only* place Lua runs for the UI.
- **UI thread** — **event-driven** (`MsgWaitForMultipleObjects` on input / the dirty event / a 250 ms
  heartbeat; renders only on input, a changed snapshot, or an active drag → **~zero idle cost**).
  Fetches the latest snapshot lock-free and **replays** it into ImGui; **never touches lua_State**.
  Interactions go back through **per-widget atomic slots** (`g_input[]`, id = `surf*MAX_W+ix`; value
  latest-wins, button clicks sticky-OR until the engine drains+clears them at the next build).
- **`mod.ui` unchanged** (immediate-mode on the surface): `panel`/`window` + `text[_disabled/
  _wrapped/_colored]`/`bullet`/`separator`/`spacing`/`same_line`/`button`/`small_button`/`checkbox`/
  `slider_int`/`slider_float`/`progress`.  Widgets are inert unless `g_building` (engine thread,
  inside `ui_build`), so a stray call elsewhere is a no-op not a crash.  A **UI-thread live-value
  cache** (`g_live[]`) keeps sliders/checkboxes smooth between the throttled builds — it re-seeds
  from the snapshot only when the mod changed the value AND the widget isn't `IsItemActive()`, so
  the engine's lagged sample can't fight a live drag.  ~1 build-cycle input latency, imperceptible.
  A faulting callback disables just that surface (`pcall`).  Toggle **F8** (`ui_key`); build rate
  `ui_hz` (default 30); `ui=0` disables.

**Nothing locks between the two threads** (the requirement): the snapshot is a lock-free triple
buffer and input is a fixed atomic-slot array — both latest-wins / drop-old, so a slow UI never
stalls the game and a busy game never stalls the UI, and stale data is dropped, not accumulated.

**In-game overlay: DONE** (Phase C — see that section).  It landed exactly as the P5 note predicted:
an **internal renderer-hook backend** (the engine-thread D3D11 present) consuming the SAME snapshot,
so `mod.ui` didn't change.  In takeover it replaces the companion window (which isn't started there);
the transparent-DWM-window approach stayed retired.

**Build:** `ui.cpp` + the ImGui TUs compile with **g++**; the whole DLL links with **g++ +
`-static -static-libgcc -static-libstdc++`** (the game's DLL path has no `libstdc++-6`/`libgcc_s`),
libs `-lgdi32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32`.  DLL ~1.4 MB, imports only system
DLLs.  Host tests link the UI objects (via g++) and stay green.

**Validated:** `make -C core tests` green (`EXEC_OK` / `TYPED_HOOK_OK` / `NATIVE_ABI_OK` — the LBL
removal + the Tier-2 `make_dispatch` revert didn't regress).  `core/test/ui_smoke.c` drives the full
build→triple-buffer→replay path host-side (`make -C core ../build/ui_smoke.exe && (cd build &&
./ui_smoke.exe 3)` → `>> UI_SMOKE_OK`).  **In-game** (real EN-SE): the companion window came up on
the armed safepoint (`[ui] companion window up`, `ui_build` running every frame), no crash, no draw
faults.

UI gaps (deferred): no `mod.ui` in the native ABI (Lua-
only); no input-text / combo / tree widgets (curated set — extend as needed); no per-surface remove
(`ui.panel`/`window` return a handle for a future `ui.remove`); positional input keys can
misattribute one frame if a mod reorders its widgets (self-correcting).

## Open debts / follow-ups

- **#7 — roster heap-scan** (`PORT-DEBT(sotes-roster-heapscan)`): **mostly retired** (commit
  `perf: roster …`).  `sotes_bindings.c` now (a) does NOTHING at the title/menu (`render_root
  *(0x92dd38)==0` → no party → no scan: killed the ~65 ms menu walk → ~3 µs) and (b) in-scene scans
  the two **SE-verified render bands** (EFFECT `render_root+0x1160`×32, CHARACTER `render_root+0x11e0`
  ×128; ~160 reads) for the party codes; the full-heap walk stays only as an ultimate fallback (never
  hit in testing).  Validated in-game: identical roster (Arche/Sana/Stella), scene-transition cost
  200-600 ms → ~2.4 ms.  **Remaining (deferred):** the base-game "canonical" party array
  `map_obj+0x4030` doesn't reach the SE party via any `render_root+off` single/double indirection I
  could pin live (needs deeper SE memory tracing) — the bands suffice for now.
- **#12 — Tier-2 follow-ups:** (a) run the in-game smoke test (`examples/hook_typed`); (b)
  quiescent teardown — `remove` leaves the MinHook + FFI closure installed (dead-marked in the
  chain); reclaim on empty. (c) unify tiers on one VA (a typed hook that also runs Tier-1
  observers) — deferred; the xor rule is safe + simple for now.
- Executor's safepoint hook could become the first CLIENT of the `hooks.c` registry (unify);
  kept separate for now to not disturb the proven executor.

## Gotchas (hard-won)

- **RE addresses: the OpenSummoners `docs/decompiled/by-address/*.c` files are the BASE-GAME unpack
  (`sotes.unpacked.exe`, md5 `278bad…`) — a DIFFERENT edition than our target, the EN Special Edition
  (`sotes-ense-en.exe` == `sotes_en.exe` == `sotes-trainer-oss.exe`, md5 `3fe1bc9f…`).** The ddraw
  code is SHIFTED between editions: the RE labels `zdd_present` `0x5b8fc0`, but in EN-SE that address
  is mid-instruction (junk); the real present is **`0x5e1470`** (found by signature: the `+0x164`
  mode jump-table + the windowed `GetDC`+`BitBlt(0xcc0020)` case).  The mod loader's OTHER addresses
  (safepoint `0x437c70`, `render_root *0x92dd38`) happen to match because they were validated on EN-SE.
  **Lesson: never take a NEW hook VA straight from the by-address RE files — verify it's a real entry
  in EN-SE first** (`objdump -d -b pei-i386 --start-address=…`, or grep the `.text` disasm for the
  function's signature).  Editions also differ in surface format: EN-SE's back-buffer is **24bpp RGB888**
  (windowed, mode 2), not the base game's 16bpp.
- App-dir `version.dll` side-load works only from **NTFS**, not `\\wsl$` (Windows blocks remote-dir DLL load).
- **The game IDLES its input poll when its window is UNFOCUSED** — the safepoint (`0x437c70`) stops firing and the log freezes at `[exec] safepoint hook armed` (looks exactly like a hang/crash, but the game is fine at the title). **`keepactive` is now a DEFAULT-ON built-in** (re-posts `WM_ACTIVATEAPP(TRUE)` *only when the game isn't foreground*, so normal play is untouched; needed so the companion UI window doesn't freeze the game when clicked). Disable with `keepactive=0`. *This cost hours of misdiagnosis once — a known-good build "hung" the moment the window lost foreground.* If a run stops at "safepoint hook armed" with no `[mod]`/`[prof]` lines, suspect focus first, not the code.
- **Built-in defaults changed** (2026-07-19): `keepactive` and `skip_launcher` are now **default-ON**; the loader also arms the executor + UI **even with no `mods\` folder** (recognized game / `oss_loader.cfg` also trigger it) — the built-in QoL is independent of any mods being present. And it **only loads `mods\*.dll` that export `OssModInit`** (non-API DLLs are skipped, not run).
- **Reach gameplay + profile:** drop the **`autoload` mod** (`../sotes-mods/mods/autoload`) into `mods\` — it calls `mod.game.save.load` to drive the title/picker into a save (slot via its `[config]`, default newest).  (Was the `autoload=1` core flag — now a mod; the RE'd drive is `mod.game.save` in `sotes_bindings.c`.)  `profile=1` logs per-frame loader overhead every 5 s (`prof.c`). Real-gameplay numbers: `0x437c70` fires **~1000×/sec**, the loader is ~1% of the engine thread steady-state (safepoint + ui_build ≈ 9 µs), the UI machinery is negligible.
- The stock dir's `mods\sotes_trainer.dll` **also hooks `0x437c70`** — but the loader now **auto-skips any DLL that doesn't export `OssModInit`** (probed via `DONT_RESOLVE_DLL_REFERENCES`, so its DllMain never runs), so a stray legacy trainer/voice DLL in `mods\` can't fight the executor. One MinHook per target VA. (Port them to the API to load them.)
- WSL launch: `cmd /c start` blocks on the process-tree job → use `tools/dev-launch.sh` (PowerShell `Start-Process`).
- **Bootstrap must POSITIVELY match the game window** (`profile.window_class`, SotES `CLASS_LIZSOFT_SOTES`), never "first non-launcher top-level window": the game spawns an early TRANSIENT top-level window (+ a DirectShow `ActiveMovie` + IME windows) that the old heuristic latched and subclassed — then it got destroyed, silently dropping the subclass + the posted `WM_OSS_BOOT`, so the executor never armed (intermittent; looked like a Tier-2 failure but wasn't). Enumerate a live instance to fingerprint the real window: `tools/win-fingerprint.ps1 -TargetPid <pid>` (class/title/style/owner/size).
- Kills: exact pid via `tasklist.exe`/`taskkill.exe`, never `pkill` (siblings share the frida-server). After a kill, the exe file lock releases async — retry the `version.dll` restore.
- i686 mingw prefixes C symbols with `_`; the asm observer thunk uses `asm("name")` labels to match (see `executor.c`/`hooks.c`).
- LuaJIT is Lua **5.1** (`setfenv`, no `goto`/integer division semantics of 5.3).
- **UI: the two threads must NEVER share the lua_State — that was the LBL, and it lagged the game.**
  Lua runs only on the engine thread (`ui_build` at the safepoint records the snapshot). The UI
  thread consumes a lock-free **triple buffer** (snapshot) + writes **atomic input slots** — no lock,
  latest-wins, drops old data. If you extend the UI, keep this discipline: the UI thread reads the
  snapshot + writes atomics, and nothing more; anything Lua goes through `ui_build` on the engine
  thread. The mod.ui widgets are inert unless `g_building`, so a stray widget call off-build no-ops.
- **UI: `mod.ui` callbacks run on the ENGINE thread now** (inside `ui_build`, throttled ~30 Hz), not
  the UI thread — so they may read game state directly, but heavy work there costs the game frame
  (like `on_frame`); push it through `mod.main`. Input has ~1 build-cycle latency (imperceptible).
- **UI: linking is g++ now.** The final DLL + the host tests link with `i686-w64-mingw32-g++` and
  `-static-libstdc++` (the ImGui TUs pull in libstdc++). A plain-gcc link will fail to resolve C++
  symbols; a non-static link produces a DLL the game can't `LoadLibrary` (no libstdc++-6 on its path).
- **UI: the in-game overlay (takeover) and the companion window are MUTUALLY EXCLUSIVE — never run
  both.** They'd be two ImGui contexts on two threads racing ImGui's global current-context.  `ui_start`
  starts the companion ONLY when not in takeover; the overlay (engine thread, `ui_overlay_present`) runs
  ONLY in takeover.  Keep that invariant if you touch either.  Both consume the SAME snapshot — do NOT
  resurrect the transparent-DWM-window overlay; the snapshot abstraction is what makes the swap
  mod-invisible.  The overlay is MOUSE-ONLY (keyboard stays with the game); toggle is F10, not F8
  (F8 is SotES' in-game menu key).
