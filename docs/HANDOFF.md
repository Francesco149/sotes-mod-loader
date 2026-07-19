# HANDOFF — current checkpoint

Rolling "where we are / what's next" for a fresh session. Orient: this → `DESIGN.md`
(architecture + phase table) → `MOD-FORMAT.md` (the `mod.*` API) → `TESTING.md`.

> **Cross-reference `../OpenSummoners`** — the parity port + the **canonical RE home** for the
> SAME `sotes.exe`. Look there before reversing (decompile `docs/decompiled/`, findings, Frida
> harness, trainer `tools/sotes_trainer/SE_CODE_MAP.md`); and **record any NEW engine RE finding
> THERE** (`SE_CODE_MAP.md` / `engine-quirks.md`), not only in this repo — loader docs cover the
> loader, game facts belong to the port so both projects benefit. (README "Related".)

## State: P0–P4 DONE (incl. **Tier-2 typed hooks** + the **native-mod C ABI**). Next = **P5** (UI).

The loader is a usable modding API — in Lua AND native C: a mod reads guarded memory
(`mod.mem` / `api->mem_*`), reads live RE'd game state (`mod.game.*`), runs per-frame/one-shot
on the engine thread (`mod.on_frame`/`mod.main` / `api->on_frame`/`main_enqueue`), and **hooks
any function alongside other mods** (`mod.hook` — Tier-1 observe + Tier-2 typed; native
`api->hook_entry` shares the same chain).

| phase | what | status |
|---|---|---|
| P0 | proxy `version.dll` + LuaJIT host + `mods\` scan (native + Lua) | ✅ |
| P1 | `mod.mem` (guarded) + togglable `mod.game.*` bindings (roster, coordinates) | ✅ |
| P2 | main-thread executor: WndProc bootstrap → safepoint hook `0x437c70` → `mod.main`/`on_frame` | ✅ |
| P3 | chained hook registry — **Tier-1 observers** (`mod.hook.entry`) **+ Tier-2 typed** (`mod.hook.typed`) | ✅ |
| P4 | native-mod C ABI: `OssModInit(const OssApi*)`, shared hook + executor registries | ✅ |
| P5 | ImGui UI: loader-owned main window + in-game mirror (hotkey) | ⬜ **next** |
| P6 | voice patch → a Lua mod; exhaustive install/launch test → swap the release | ⬜ |
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

- `loader.c` — DllMain, proxy attach, `config_load`, `mods\` scan (defers Lua mods to the executor).
- `lua_host.c` — LuaJIT bring-up (JIT compiled OUT, FFI on), per-mod env + `pcall` isolation,
  builds each mod's `mod` table (mem/game/main/on_frame/hook).
- `mem.c` — guarded r/w (VirtualQuery), scan, module/reloc. `mem.h` guards used by bindings.
- `game_bindings.c` — the togglable `mod.game.*` registry (dynamic-resolve metatable + enable/disable).
- `sotes_bindings.c` — SotES roster+coordinates, profile-gated. **PORT-DEBT: heap-scan (task #7).**
- `executor.c` — WndProc bootstrap (positive `profile.window_class` match — see gotchas),
  safepoint hook (register-capture asm thunk), job queue + on_frame, launcher dismiss
  (`skip_launcher`/`windowed`), `exec_main_tid`/`exec_main_tid_ptr`/`exec_ti_mgr`.
- `hooks.c` — the chained hook registry (both tiers): Tier-1 per-VA codegen thunk →
  `hook_dispatch` → observer chain; Tier-2 per-VA main-thread gate → FFI-closure dispatcher
  (the embedded `TIER2_LUA`) → typed chain. `g_hk[]` carries a `tier` (VA is T1 xor T2).
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
