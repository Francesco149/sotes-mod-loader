# HANDOFF — current checkpoint

Rolling "where we are / what's next" for a fresh session. Orient: this → `DESIGN.md`
(architecture + phase table) → `MOD-FORMAT.md` (the `mod.*` API) → `TESTING.md`.

> **Cross-reference `../OpenSummoners`** — the parity port + the **canonical RE home** for the
> SAME `sotes.exe`. Look there before reversing (decompile `docs/decompiled/`, findings, Frida
> harness, trainer `tools/sotes_trainer/SE_CODE_MAP.md`); and **record any NEW engine RE finding
> THERE** (`SE_CODE_MAP.md` / `engine-quirks.md`), not only in this repo — loader docs cover the
> loader, game facts belong to the port so both projects benefit. (README "Related".)

## State: P0–P5 DONE (incl. **Tier-2 typed hooks**, the **native-mod C ABI**, and the **ImGui UI**). Next = **P6** (voice mod).

The loader is a usable modding API — in Lua AND native C: a mod reads guarded memory
(`mod.mem` / `api->mem_*`), reads live RE'd game state (`mod.game.*`), runs per-frame/one-shot
on the engine thread (`mod.on_frame`/`mod.main` / `api->on_frame`/`main_enqueue`), **hooks
any function alongside other mods** (`mod.hook` — Tier-1 observe + Tier-2 typed; native
`api->hook_entry` shares the same chain), and adds **ImGui panels/windows** to a loader window
+ an in-game overlay mirror (`mod.ui`).

| phase | what | status |
|---|---|---|
| P0 | proxy `version.dll` + LuaJIT host + `mods\` scan (native + Lua) | ✅ |
| P1 | `mod.mem` (guarded) + togglable `mod.game.*` bindings (roster, coordinates) | ✅ |
| P2 | main-thread executor: WndProc bootstrap → safepoint hook `0x437c70` → `mod.main`/`on_frame` | ✅ |
| P3 | chained hook registry — **Tier-1 observers** (`mod.hook.entry`) **+ Tier-2 typed** (`mod.hook.typed`) | ✅ |
| P4 | native-mod C ABI: `OssModInit(const OssApi*)`, shared hook + executor registries | ✅ |
| P5 | ImGui UI: loader-owned main window + in-game mirror (hotkey) — `mod.ui` | ✅ (host-smoke; in-game pending) |
| P6 | voice patch → a Lua mod; exhaustive install/launch test → swap the release | ⬜ **next** |
| P7 | trainer → a mod; coexistence verified | ⬜ |
| P8 | launcher (package manager: git-repo sources, install/update, Launch) | ⬜ |

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
  builds each mod's `mod` table (mem/game/main/on_frame/hook/**ui**); owns the **Lua Big Lock**
  (`lh_lock`/`lh_unlock` — the recursive lock the UI + engine threads share).
- `ui.cpp` / `ui.h` — the ImGui/DX11 UI host (P5): the UI thread, the two windows (main + overlay),
  two ImGui contexts, the `mod.ui` binding + widgets. The only C++ TU; C-linkage seam in `ui.h`.
- `mem.c` — guarded r/w (VirtualQuery), scan, module/reloc. `mem.h` guards used by bindings.
- `game_bindings.c` — the togglable `mod.game.*` registry (dynamic-resolve metatable + enable/disable).
- `sotes_bindings.c` — SotES roster+coordinates, profile-gated. **PORT-DEBT: heap-scan (task #7).**
- `executor.c` — WndProc bootstrap (positive `profile.window_class` match — see gotchas),
  safepoint hook (register-capture asm thunk), job queue + on_frame, launcher dismiss
  (`skip_launcher`/`windowed`), `exec_main_tid`/`exec_main_tid_ptr`/`exec_ti_mgr`/`exec_game_hwnd`.
  The safepoint drain now runs under the **LBL** (excludes the UI thread while it touches Lua).
- `hooks.c` — the chained hook registry (both tiers): Tier-1 per-VA codegen thunk →
  `hook_dispatch` → observer chain; Tier-2 per-VA main-thread gate → FFI-closure dispatcher
  (the embedded `TIER2_LUA`) → typed chain. `g_hk[]` carries a `tier` (VA is T1 xor T2).
  Both dispatchers now bracket their Lua in the **LBL** (Tier-2 via `hook.__lock`/`__unlock`).
- `oss_mod_api.h` — the STABLE native-mod ABI (the only header a native mod needs): `OssApi`
  vtable, `OssHookCtx`, `OssModInit`.  `native_bridge.c` — fills that vtable from mem/executor/
  hooks and hands it to each native mod (`oss_api()`).
- `profile.c` — the C game profile (safepoint VA, **window class**, launcher class + control ids).
- `config.c` — `oss_loader.cfg` (key=value) reader. `minhook/` — vendored trampoline backend.

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

An ImGui/DX11 UI on a dedicated **UI thread**, backing the Lua `mod.ui` table (`push_mod_table`).
Two loader-managed ImGui **contexts**, each its own **DX11 device + swapchain** (the trainer's
stock-example helper — HW→WARP fallback, `R8G8B8A8`, DISCARD, `Present(1,0)` vsync):

- **Main window** — `WS_OVERLAPPEDWINDOW`, opaque, default-visible, toggle **F8**. The default UI.
- **In-game overlay** — borderless top-most window **tracked over the game's client rect**, toggle
  **INSERT**, hidden until toggled. Transparent via **DWM glass**: `DwmExtendFrameIntoClientArea`
  with `{-1,-1,-1,-1}` + a null-region `DwmEnableBlurBehindWindow`, RT cleared to `(0,0,0,0)` — DWM
  composites the alpha, opaque panels float over the game. No renderer hook, no `WS_EX_LAYERED`
  (stays interactive when shown). **This transparency path is the one P5 piece NOT yet validated
  in-game** (host smoke can't check compositing) — see gotchas.
- `mod.ui.panel(name, draw)` / `mod.ui.window(name, draw)` store a Lua closure ref in a registry the
  UI thread draws into **both** contexts each frame — so the overlay is a literal mirror of the
  window. Widget set: `text[_disabled/_wrapped/_colored]`, `bullet`, `separator`/`spacing`/
  `same_line`, `button`/`small_button`, `checkbox`, `slider_int`/`slider_float`, `header`,
  `progress`, `overlay_toggle`/`overlay_visible`. Widgets are **inert unless mid-draw on the UI
  thread** (a `drawing()` guard), so calling one from init is a harmless no-op, not a crash. Each
  callback runs in a `pcall`; a fault disables just that panel/window.

**The Lua Big Lock (LBL) is the load-bearing change.** LuaJIT is single-threaded; the UI thread now
runs Lua. `lua_host.c` owns a **recursive** lock (`lh_lock`/`lh_unlock`, lazily init'd so host
harnesses that skip `lh_init` still work). The UI thread holds it around each frame's callbacks;
**every** engine-thread Lua entry point holds it too — the safepoint drain (`executor.c`) and both
hook dispatchers (Tier-1 `hook_dispatch` in C; Tier-2's `make_dispatch` brackets its body via
`hook.__lock`/`__unlock` in the embedded `TIER2_LUA`). Recursive so a hook firing during the
safepoint drain (already holding it) doesn't self-deadlock. The UI never installs hooks / calls the
engine, so there's no lock-order inversion (LBL is the only shared lock; the job-queue CS nests
under it consistently).

**Build:** `core/Makefile` now compiles `ui.cpp` + the ImGui TUs (core + `imgui_impl_win32/dx11`)
with **g++** and links the whole DLL with **g++ + `-static -static-libgcc -static-libstdc++`** (else
the game's DLL search path — no `libstdc++-6`/`libgcc_s` — can't `LoadLibrary` us). Libs added:
`-lgdi32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32`. The DLL is ~1.3 MB, imports only system
DLLs (verified: `d3d11`, `d3dcompiler_47`, `dwmapi`, `gdi32`, …; **no** dynamic libstdc++/libgcc).
Host tests link the UI objects too (via g++) and still pass. **Config:** `ui=0` disables the UI;
`ui_key`/`overlay_key` (VK codes) override F8/INSERT.

**Validated:** `make -C core tests` still green (`EXEC_OK` / `TYPED_HOOK_OK` / `NATIVE_ABI_OK` —
the LBL + the Tier-2 dispatcher rework didn't regress). New host smoke `core/test/ui_smoke.c`
(`make -C core ../build/ui_smoke.exe && (cd build && ./ui_smoke.exe 3)`) stood up the real main
window + DX11 device + ImGui context and rendered a Lua panel + window for 3 s under the LBL, then
tore down cleanly (`>> UI_SMOKE_OK`). **In-game (both windows, esp. overlay compositing) pending.**

UI gaps (deferred): overlay is show/hide-interactive, not a click-through HUD (that + `mod.overlay.*`
freehand/DDraw7 layers come later); no `mod.ui` in the native ABI yet (Lua-only); no input-text /
combo / tree widgets yet (curated set — extend as mods need); no per-panel remove (`ui.panel`
returns a handle for a future `ui.remove`).

## Open debts / follow-ups

- **#7 — retire the roster heap-scan** (`PORT-DEBT(sotes-roster-heapscan)`): replace the
  throttled full-heap actor scan in `sotes_bindings.c` with a direct pointer — candidate:
  `render_root+0x11e0` CHARACTER band (128 actor slots), `render_root=*(0x92dd38)`; validate live.
- **#12 — Tier-2 follow-ups:** (a) run the in-game smoke test (`examples/hook_typed`); (b)
  quiescent teardown — `remove` leaves the MinHook + FFI closure installed (dead-marked in the
  chain); reclaim on empty. (c) unify tiers on one VA (a typed hook that also runs Tier-1
  observers) — deferred; the xor rule is safe + simple for now.
- Executor's safepoint hook could become the first CLIENT of the `hooks.c` registry (unify);
  kept separate for now to not disturb the proven executor.

## Gotchas (hard-won)

- App-dir `version.dll` side-load works only from **NTFS**, not `\\wsl$` (Windows blocks remote-dir DLL load).
- The stock dir's `mods\sotes_trainer.dll` **also hooks `0x437c70`** — move it aside before testing our loader (else it fights the executor). One MinHook per target VA.
- WSL launch: `cmd /c start` blocks on the process-tree job → use `tools/dev-launch.sh` (PowerShell `Start-Process`).
- **Bootstrap must POSITIVELY match the game window** (`profile.window_class`, SotES `CLASS_LIZSOFT_SOTES`), never "first non-launcher top-level window": the game spawns an early TRANSIENT top-level window (+ a DirectShow `ActiveMovie` + IME windows) that the old heuristic latched and subclassed — then it got destroyed, silently dropping the subclass + the posted `WM_OSS_BOOT`, so the executor never armed (intermittent; looked like a Tier-2 failure but wasn't). Enumerate a live instance to fingerprint the real window: `tools/win-fingerprint.ps1 -TargetPid <pid>` (class/title/style/owner/size).
- Kills: exact pid via `tasklist.exe`/`taskkill.exe`, never `pkill` (siblings share the frida-server). After a kill, the exe file lock releases async — retry the `version.dll` restore.
- i686 mingw prefixes C symbols with `_`; the asm observer thunk uses `asm("name")` labels to match (see `executor.c`/`hooks.c`).
- LuaJIT is Lua **5.1** (`setfenv`, no `goto`/integer division semantics of 5.3).
- **UI: the overlay's DWM transparency is the one P5 path not yet proven in-game.** The host smoke
  (`ui_smoke.exe`) can't check compositing (no game window). If the overlay renders as an opaque
  black rectangle in-game, DWM isn't honouring the framebuffer alpha on this machine — fallback:
  add `WS_EX_LAYERED` + `SetLayeredWindowAttributes(h, RGB(0,0,0), 0, LWA_COLORKEY)` and clear to
  pure black (the theme already avoids `#000000`), or move to a DirectComposition swapchain. The
  main window is the proven trainer model and is unaffected. **Test in WINDOWED mode** — DWM
  composition (thus the transparent overlay) is bypassed by exclusive-fullscreen; the loader
  already defaults to windowed (launcher-skip selects it), and a fullscreen overlay is the later
  DDraw7 `mod.overlay.*` job.
- **UI: anything the UI thread runs as Lua MUST be under the LBL.** If you add a new engine-thread
  Lua entry point (another hook kind, a callback), wrap its Lua in `lh_lock`/`lh_unlock` too, or it
  can race the UI thread's `mod.ui` callbacks and corrupt the shared `lua_State`. Never block inside
  a `mod.ui` draw callback — it stalls the UI and (while it holds the LBL) briefly the game.
- **UI: linking is g++ now.** The final DLL + the host tests link with `i686-w64-mingw32-g++` and
  `-static-libstdc++` (the ImGui TUs pull in libstdc++). A plain-gcc link will fail to resolve C++
  symbols; a non-static link produces a DLL the game can't `LoadLibrary` (no libstdc++-6 on its path).
