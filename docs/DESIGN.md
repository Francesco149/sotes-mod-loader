# SotES Mod Loader — design

**North star: MAX STABILITY.** A buggy or careless mod must not crash the game; two
mods hooking the same function must not clobber each other; nothing touches engine
state off the main thread. Everything below is subordinate to that.

A game-agnostic native loader that hosts (a) a **Lua** modding API (LuaJIT), (b)
optional **native DLL mods** through the same registry, and (c) a **shared ImGui UI**
each mod adds panels to. Plus a **launcher** (package manager over git-repo mod sources)
and a rework of the **voice patch** + **trainer** onto it.

> Origin / full rationale + history: `OpenSummoners/docs/plans/mod-loader-v2.md`
> (the loader was designed there, then split into this standalone repo). Contracts:
> [MOD-FORMAT.md](MOD-FORMAT.md), [REGISTRY.md](REGISTRY.md).

## Enforced invariants (hard-won)

1. **No engine calls / heap allocs off the main thread.** Every engine call, poke, and
   alloc runs on the safepoint queue; worker/UI threads only READ (guarded) + ENQUEUE.
2. **Don't hook teardown/rebuild/boot-init.** Hooks install/remove on the safepoint,
   gated on a stable scene state; the game profile lists never-hook VAs + boot windows.
3. **Even correct hooks can create inconsistent GAME state.** The loader exposes
   scene-state signals (settle, scripted/cutscene flags) so a mod can self-gate.
4. **Never fault on a bad read/write.** All memory access is VirtualQuery-guarded.
5. **Survive crashes for post-mortem.** Crash-resilient flush-to-file log + safepoint marks.
6. **Single instance, clean unload.** Refuse a double-load; restore hook bytes on unload.
7. **Exception isolation (the multi-mod keystone).** Every mod callback (Lua or native,
   hook or UI) runs inside an SEH / `pcall` boundary. A faulting hook is disabled +
   logged, not propagated; a Lua error is caught + the mod flagged. One bad mod never
   takes down the game or the other mods.

## Architecture (bottom → top)

```
 version.dll proxy (host)  ── forwards version.* → realver.dll; single-instance; crash log
   ├─ Memory service       ── guarded r/w, AOB scan, module base + ASLR reloc, struct/cdef
   ├─ Main-thread executor ── WndProc bootstrap → per-frame safepoint hook → job queue
   ├─ Hook registry        ── ONE trampoline per target VA → dispatcher → ordered mod chain
   │                          (Tier-1 entry observers | Tier-2 typed pre/post/replace)
   ├─ Native-mod bridge    ── stable C ABI so mods\*.dll register into the SAME registries
   ├─ Lua runtime          ── LuaJIT front-end onto all of the above (mods\<name>\init.lua)
   ├─ UI host              ── one ImGui context; main window + in-game mirror; mod panels
   └─ Game profiles        ── per-game facts (safepoint VA, window class, structs, no-hook)
```

### Hooking (safe even with multiple mods)

- **One trampoline per hooked VA** (MinHook) → the loader's central dispatcher → an
  ordered, per-owner chain. Add/remove = editing the chain, never re-patching bytes, so
  **no two mods fight over the 5 bytes**.
- **Tier 1 — entry observers (default, ultra-stable):** a register-capture thunk
  (`{ecx,edx,esp,ret,args}`), the trainer's model. No signature, convention-agnostic,
  can't modify/block → cannot destabilize the callee. Covers most needs.
- **Tier 2 — typed hooks (opt-in, powerful):** the mod declares the C signature;
  **LuaJIT FFI closures** marshal so a **pre** hook can modify args / block, a **post**
  hook can modify the return. First-to-block wins + logged.
- Lock-free read path (snapshot/RCU); mutations on the safepoint under a lock. A hook is
  removed only when no call to it is in flight (quiescent state). Reentrancy-safe.

### Main-thread executor

- **Bootstrap = WndProc subclass** on the game window (the shipped voice-patch pattern):
  guaranteed main thread, no engine VA, game-agnostic. Then install the per-frame
  safepoint hook (profile `safepoint_va`; SotES `0x437c70`) and drain the job queue there.
- `mod.main(fn)` runs `fn` on the main thread at the next safepoint (await or
  fire-and-forget); `mod.on_frame(fn)` registers a per-frame callback. Engine calls
  inside are safe by construction.

### UI host — main window + in-game mirror

Two loader-**managed** ImGui instances (a mod never creates its own context):

- **(1) Main window** — a loader-owned ImGui window with its own DX11 device (the
  trainer's proven, stable model; no hooking the game's renderer). The **default** UI
  path. `mod.ui.panel(name, draw)` = a section/tab here; `mod.ui.window(name, draw)` = a
  mod-owned floating window (the trainer's map graph).
- **(2) In-game overlay = a mirror of the main window** (a SECOND managed instance drawn
  on top of the game), **toggled by a hotkey**. Same in-process draw callbacks, second
  render target — so the panels are literally the same. Opt-in richer layers
  (`mod.overlay.panel` loader-arranged / `mod.overlay.draw` freehand) come later on a
  DDraw7 backend behind the backend-agnostic `mod.overlay.*` API.
- **UI thread = read-only + enqueue:** draw callbacks may READ game memory (guarded); any
  write/engine-call goes through `mod.main`.

### Launcher (package manager) — same repo, `launcher/`

A standalone ImGui desktop app (styled like the trainer): add **git-repo mod sources**
(trust = per-repo), browse/search, install/update mods package-manager style (versions +
release notes, hash-verified files), then **Launch** the game with the loader present.
Contract: [REGISTRY.md](REGISTRY.md). Built **after** the injected core proves out.

### Game profiles — `profiles/<id>.lua`

Per-game facts, auto-selected by the host exe: per-frame safepoint VA, window class,
no-hook / boot-teardown windows, shared struct cdefs + key globals. Keeps the core
game-agnostic; sibling projects drop in their own profile. (`profiles/sotes_en.lua`.)

### Memory service — `mod.mem` (P1)

The low-level substrate everything reads/writes through. Every access is
**VirtualQuery-guarded** (loader invariant #4): a stale/garbage pointer yields
`nil`/`false`, never an access violation. Typed reads/writes (`u8..f64`, `bytes`,
`cstr`, `ptr`), an AOB `scan` over the exe image, `module`/`base`, and `reloc` (VA +
ASLR delta from the PE ImageBase). Lifted from the trainer's proven guards. FFI
(`ffi.cdef` + `ffi.cast`) composes on top for typed `this->field` struct views.

### Game bindings — `mod.game.*` (the RE'd engine knowledge)

As we reverse the engine, the known **structs + pointer chains** (roster, coordinates,
camera, map, scene, saves, …) are centralized in the loader core and exposed to every
mod as `mod.game.<id>` — so no mod re-RE's them, and a fix propagates once. This mirrors
what the trainer already computes (party roster, player coordinates), now shared.

- **Togglable per module (the stability valve).** Each binding is independently
  enable/disable-able at runtime (`mod.game.enable/disable/list`). Disabling **hides**
  `mod.game.<id>` *and* makes its accessors inert through any captured reference — so if
  a binding proves unstable it's turned off without touching the rest. (All reads are
  guarded, so a binding returns `nil` on bad data rather than faulting; a future
  refinement auto-disables a binding that trips repeatedly.)
- **Core generic, bindings profile-local.** The registry + `mod.game` plumbing are
  game-agnostic; the SotES bindings live in their own TU (`sotes_bindings.c`), gated on
  the host exe, and can move to a loadable profile module later (P4).
- **First bindings:** `mod.game.roster` (party members by entity code `0xc35a/b/c`) and
  `mod.game.coordinates` (member world x/y), lifting the trainer's discovery + validation.
- **PORT-DEBT(sotes-roster-heapscan):** locating actors is currently a throttled
  full-heap scan (robust but fragile — can momentarily see a post-warp "ghost"). It's a
  known-good *temporary* method; retire it via further RE of a **direct roster/party
  pointer** (candidate: the `render_root+0x11e0` CHARACTER band, `render_root=*(0x92dd38)`).

## Locked decisions

- **Name:** *SotES Mod Loader*; Lua namespace `mod.*`; DLL `version.dll`; log `oss_modloader.log`.
- **Lua:** LuaJIT — **JIT compiled OUT** (interpreter-only, most stable inside an injected
  DLL: no JIT code pages in the game's address space) + **FFI ON** (native calls + struct
  cdefs). Typed-hook marshaling uses LuaJIT FFI closures.
- **UI:** loader-owned main ImGui window (default) + an in-game mirror of it (hotkey);
  richer opt-in overlay layers later on a DDraw7 backend.
- **Hooks:** entry-observer default + typed opt-in (the stability gradient).
- **Voice patch → a Lua mod** (dogfoods `mod.main`; ~10-line main-thread seed).
- **Repos:** this loader + launcher here (`sotes-mod-loader`); the author's mods
  (trainer + voice) + the default registry in `../sotes-mods`. The current shipping
  voice patch stays in `OpenSummoners/tools/ennse_voice`, live + unchanged, until the
  new mod-loader-based one is proven — then the release swaps.

## Phased build plan

| Phase | What | Status |
|---|---|---|
| **P0** | Scaffold: host LuaJIT, scan `mods\*` (native + Lua), `mod.log`, hello-world boots | **done** (LuaJIT proven on-target; loader path proven via `host_stub`) |
| **P1** | `mod.mem` (guarded r/w, scan, module/reloc) **+ togglable `mod.game.*` bindings** (roster, coordinates) | **done** (verified via `examples/probe`; live data pending in-game) |
| **P2** | Main-thread executor: WndProc bootstrap → safepoint hook → `mod.main`/`on_frame`; profile | **done** (drain/on_frame/`ti_mgr` verified via `exec_test`; MinHook install + bootstrap in-game) |
| P3 | Hook registry: Tier-1 chain (MinHook + dispatcher), then Tier-2 typed; safepoint-gated | next |
| P4 | Native bridge: C ABI, shared registry, `OssModInit` | |
| P5 | UI host: DX11 main window + in-game mirror, `ui.panel`/`ui.window` | |
| P6 | Voice mod: port + exhaustive install/launch test → swap the release | |
| P7 | Trainer mod: port + map-graph window; coexistence verified | |
| P8 | Launcher: git-repo sources, install/update, Launch | |

Each phase is independently testable; P6 gates on a 100%-green install→launch→voice flow
before any release swap.

**P0–P2 validated in-game** (2026-07-19, unpacked EN-SE): the executor armed on the real
`0x437c70` (game kept running), `on_frame` fired every frame, and `mod.game.roster` read
all three live party members with correct coords/HP + controlled-member detection. Evidence
+ reproduce steps: [TESTING.md](TESTING.md).
