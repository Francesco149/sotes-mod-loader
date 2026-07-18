# HANDOFF — current checkpoint

Rolling "where we are / what's next" for a fresh session. Orient: this → `DESIGN.md`
(architecture + phase table) → `MOD-FORMAT.md` (the `mod.*` API) → `TESTING.md`.

## State: P0–P3 DONE, all validated IN-GAME. Next = **P3 Tier-2** (typed hooks).

The loader is a usable modding API: a mod reads guarded memory (`mod.mem`), reads live
RE'd game state (`mod.game.*`), runs per-frame/one-shot on the engine thread
(`mod.on_frame`/`mod.main`), and **hooks any function alongside other mods** (`mod.hook`).

| phase | what | status |
|---|---|---|
| P0 | proxy `version.dll` + LuaJIT host + `mods\` scan (native + Lua) | ✅ |
| P1 | `mod.mem` (guarded) + togglable `mod.game.*` bindings (roster, coordinates) | ✅ |
| P2 | main-thread executor: WndProc bootstrap → safepoint hook `0x437c70` → `mod.main`/`on_frame` | ✅ |
| P3 | chained hook registry — **Tier-1 observers** (`mod.hook.entry`/`remove`) | ✅ Tier-1; **Tier-2 next** |
| P4 | native-mod C ABI (`OssModInit`) | ⬜ |
| P5 | ImGui UI: loader-owned main window + in-game mirror (hotkey) | ⬜ |
| P6 | voice patch → a Lua mod; exhaustive install/launch test → swap the release | ⬜ |
| P7 | trainer → a mod; coexistence verified | ⬜ |
| P8 | launcher (package manager: git-repo sources, install/update, Launch) | ⬜ |

**In-game validation** (unpacked EN-SE `sotes-trainer-oss.exe`, 2026-07-19): executor armed
on the real `0x437c70` (game ran 24 min, no crash); `mod.game.roster` read all 3 live party
members (correct coords/HP + `active` detect); the hook registry's codegen thunk installed
on `kb_poll`, register capture correct (`ctx.ecx == the real this`), 2 cbs on 1 VA both
fired, `remove` worked. Evidence: `TESTING.md`.

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
- `executor.c` — WndProc bootstrap, safepoint hook (register-capture asm thunk), job queue +
  on_frame, launcher dismiss (`skip_launcher`/`windowed`), `exec_main_tid`/`exec_ti_mgr`.
- `hooks.c` — the chained hook registry: per-VA RWX codegen thunk → `hook_dispatch` → chain.
- `profile.c` — the C game profile (safepoint VA, launcher class + control ids).
- `config.c` — `oss_loader.cfg` (key=value) reader. `minhook/` — vendored trampoline backend.

## NEXT: P3 Tier-2 — typed hooks (`mod.hook.typed(va, sig, {pre=,post=})`)

Tier-1 is an OBSERVER (the thunk `jmp`s to the original — can't modify args/block/return).
Tier-2 must **modify args / block / modify the return**, so it needs a real detour that
CALLS the chain and conditionally calls the original.

Design (from `DESIGN.md`): the mod declares the C signature; **LuaJIT FFI closures** marshal.
Cleanest path — an FFI closure IS the detour: `ffi.cast("<ret>(<conv>*)(<args>)", fn)` makes a
Lua fn C-callable; use it as the MinHook detour directly (no codegen). The Lua pre-hook gets
typed args (can modify / return a block value), then calls the original via an `ffi.cast` of
the MinHook trampoline, then the post-hook can modify the return. Chain order defined;
first-to-block wins + logged. Note: FFI callbacks in interpreter mode work (slower — fine here).
Open Q: unify with the Tier-1 chain on the same VA, or a typed hook owns the VA (simpler v1).
Reuse `hooks.c`'s per-VA rec/trampoline. Task **#11**.

## Open debts / follow-ups

- **#7 — retire the roster heap-scan** (`PORT-DEBT(sotes-roster-heapscan)`): replace the
  throttled full-heap actor scan in `sotes_bindings.c` with a direct pointer — candidate:
  `render_root+0x11e0` CHARACTER band (128 actor slots), `render_root=*(0x92dd38)`; validate live.
- **#11 — P3 Tier-2** (above).
- Executor's safepoint hook could become the first CLIENT of the `hooks.c` registry (unify);
  kept separate for now to not disturb the proven executor.
- Quiescent hook teardown: `mod.hook.remove` on an empty chain leaves the MinHook installed
  (dispatch is a cheap no-op); reclaim/disable is a later refinement.

## Gotchas (hard-won)

- App-dir `version.dll` side-load works only from **NTFS**, not `\\wsl$` (Windows blocks remote-dir DLL load).
- The stock dir's `mods\sotes_trainer.dll` **also hooks `0x437c70`** — move it aside before testing our loader (else it fights the executor). One MinHook per target VA.
- WSL launch: `cmd /c start` blocks on the process-tree job → use `tools/dev-launch.sh` (PowerShell `Start-Process`).
- Kills: exact pid via `tasklist.exe`/`taskkill.exe`, never `pkill` (siblings share the frida-server). After a kill, the exe file lock releases async — retry the `version.dll` restore.
- i686 mingw prefixes C symbols with `_`; the asm observer thunk uses `asm("name")` labels to match (see `executor.c`/`hooks.c`).
- LuaJIT is Lua **5.1** (`setfenv`, no `goto`/integer division semantics of 5.3).
