# Mod format

A mod is a folder (Lua mod) or a single DLL (native mod) under the game's `mods\`
directory. The loader scans `mods\` once at startup and loads both kinds into the
same process; they share one hook/UI registry so they interleave safely.

## Lua mod (default)

```
mods\<name>\
  init.lua      # REQUIRED — the entry point the loader runs
  mod.toml      # recommended (REQUIRED to publish to a registry)
  ...           # any assets / extra .lua the mod requires
```

`init.lua` runs once at startup with a per-mod **`mod`** table already in scope — no
`require` needed. It runs on the **engine (main) thread** (deferred to the first
per-frame safepoint by the executor), so a mod may touch the engine directly from init.
Each mod runs in its **own environment** (its globals don't collide with other mods')
inside a **protected call**: a Lua error is caught, the mod is flagged, and neither the
game nor the other mods are affected.

> **Threading model:** all mod code — `init.lua`, `on_frame`, and `mod.main` jobs — runs
> on the one engine thread, so engine reads/writes/calls are safe by construction. From
> another thread (e.g. a future UI thread) do reads via guarded `mod.mem` and push any
> engine work through `mod.main`. (Without a game window the executor stays disarmed and
> mods run on the loader thread instead — degraded, no `on_frame`.)

### `mod.*` API — available now (P0–P1)

| field | type | meaning |
|---|---|---|
| `mod.name` | string | this mod's folder name |
| `mod.dir` | string | this mod's absolute folder (trailing `\`) |
| `mod.loader` / `mod.loader_version` | string | `"SotES Mod Loader"` / host version |
| `mod.log(...)` | fn | print-style line → `oss_modloader.log`, attributed to this mod |
| `mod.mem.*` | table | **guarded** memory service (below) — never faults on a bad address |
| `mod.game.*` | table | **togglable** game-knowledge bindings (below) — the RE'd structs/pointers |
| `mod.on_frame(fn)` | fn | run `fn()` every frame on the engine thread |
| `mod.main(fn)` | fn | run `fn()` once at the next safepoint on the engine thread (fire-and-forget) |
| `mod.hook.*` | table | chained multi-mod hook registry (below) — observe any function |

**`mod.mem`** (all reads/writes VirtualQuery-guarded — a bad address returns `nil`/`false`, never a crash):

```
mod.mem.read_u8/u16/u32/i8/i16/i32/f32/f64/ptr(addr)   -> number | nil
mod.mem.write_u8/.../f64/ptr(addr, v)                  -> bool (ok)
mod.mem.read_bytes(addr, n) / read_cstr(addr [,max])   -> string | nil
mod.mem.write_bytes(addr, str)                         -> bool
mod.mem.readable(addr [,n]) / writable(addr [,n])      -> bool
mod.mem.scan("48 8B ?? C3")                            -> addr | nil   (AOB over the exe image; ?? = wildcard)
mod.mem.module(name) -> base | nil   mod.mem.base() -> exe base   mod.mem.reloc(va) -> va + ASLR delta
```

**`mod.game`** — the loader centralizes the RE'd engine structs + pointer chains as
**togglable bindings** (see DESIGN.md “Game bindings”), each surfaced as `mod.game.<id>`:

```
mod.game.list()                 -> { {id=, desc=, enabled=}, ... }
mod.game.enable(id)/disable(id)/enabled(id)     -- live toggle (a stability valve)
-- SotES bindings (grow as we RE more):
mod.game.roster.members()       -> { {code,name,actor,level,x,y,hp,hp_max,mp,mp_max,active}, ... }
mod.game.coordinates.get([code])/player()/target()   -> {x,y,actor,code}   (centi-px)
```

`active` / `coordinates.target()` identify the **controlled** member — they resolve once
the executor is armed (they use the input manager it captures at the safepoint).

**`mod.hook`** — one trampoline per target VA dispatches an ordered chain, so multiple mods
hook the same function without clobbering each other. Two tiers (a stability gradient):

**Tier 1 — `mod.hook.entry` (observe, ultra-stable, DEFAULT).** No signature, can't modify
args or block → cannot destabilize the callee.

```
local h = mod.hook.entry(addr, function(ctx) ... end)   -- addr ABSOLUTE (mod.mem.reloc(va))
mod.hook.remove(h)          mod.hook.count()
```

The callback receives `ctx = {ecx, edx, eax, esp, ret, va}` (read stack args via
`mod.mem.read_u32(ctx.esp + 4 + 4*n)`), runs on the engine thread inside a `pcall` — a
faulting cb is auto-disabled.

**Tier 2 — `mod.hook.typed` (modify args / block / modify return; opt-in, powerful).** The
mod declares the target's real C signature; a **LuaJIT FFI closure** marshals it:

```
local h = mod.hook.typed(addr, "int __thiscall(void*, int)", {
  pre  = function(ctx) ... end,   -- runs before the original
  post = function(ctx) ... end,   -- runs after
})
mod.hook.remove(h)          mod.hook.typed_count()
```

The callback receives `ctx = { args = {...}, n, va, blocked, ret }` where `ctx.args` are the
**typed** call arguments (arg 1 = `this` for `__thiscall`). A **pre** hook may mutate
`ctx.args[i]` (the modified args reach the original), or set `ctx.blocked = true` (+ optional
`ctx.ret`) to skip the original entirely. A **post** hook may rewrite `ctx.ret` (becomes the
returned value). Multiple typed hooks on one VA chain (pre in order, post in reverse);
first-to-block wins. Cbs run on the engine thread inside a `pcall` (a faulting one is disabled).

- **Signature form:** `"<ret> [__conv](<args>)"`, e.g. `"void(int)"`, `"int __stdcall(int, int)"`,
  `"int __thiscall(void*, int)"` (conv defaults to `__cdecl`). It **must match the target's
  real convention + arg count** — a wrong count corrupts the caller's stack (that's why Tier-1
  is the default; only reach for typed when you know the signature).
- **One tier per VA:** a VA is Tier-1 **xor** Tier-2 (one MinHook per target). Mixing tiers on
  one VA is refused + logged; within a tier, mods chain freely.
- **Off-thread safety:** a tiny C gate runs the closure only on the engine thread; a call to the
  target from another thread runs the original untouched (never reenters Lua).

### `mod.*` API — roadmap (later phases, see DESIGN.md)

```
mod.call(va, sig, ...)                                        -- FFI call (via mod.main)
mod.ui.panel(name, draw)  mod.ui.window(name, draw, opts)     -- P5 UI (main window + in-game mirror)
mod.overlay.panel(...) / mod.overlay.draw(fn)                 -- later DDraw7 overlay
mod.on.{scene_change, settle, unload, ...}                   -- lifecycle events
```

## Native mod

```
mods\<name>.dll     # a plain DLL that exports OssModInit
```

Loaded with `LOAD_WITH_ALTERED_SEARCH_PATH` (so it can ship co-located deps in `mods\`).
A native mod `#include`s **`oss_mod_api.h`** (the whole contract — copy it beside your
source) and exports one entry:

```c
#include "oss_mod_api.h"
__declspec(dllexport) int OssModInit(const OssApi *api) {
    if (api->abi_version != OSS_ABI_VERSION) return 1;
    api->log("hello from a native mod");
    uintptr_t va = api->mem_reloc(0x5e2a10);
    int h = api->hook_entry(va, my_observer, /*user=*/NULL);   // shares the chain with Lua mods
    api->on_frame(my_per_frame, NULL);
    return 0;   // 0 = ok
}
```

The loader resolves `OssModInit` and calls it **once on the engine thread at the first
safepoint** (after the DLL's `DllMain`), handing over the **`OssApi`** vtable — the C-side
of the same services Lua mods get: `log`; guarded `mem_read`/`mem_write`/`mem_readable`/
`mem_writable`/`mem_base`/`mem_reloc`/`mem_scan`; `main_enqueue`/`on_frame` (run on the
engine thread); and `hook_entry`/`hook_remove` (Tier-1 observers — a native `OssHookEntryFn`
gets `OssHookCtx{ecx,edx,eax,esp,ret,va}`, joining the **same per-VA chain** as Lua
`mod.hook.entry`). So native and Lua mods interleave in one registry — ship a perf-critical
hook or a big feature native, keep trivial mods in Lua.

- **Stability:** `OssApi` is versioned (`abi_version` + `struct_size`, append-only) so a
  mod built against an older header keeps working. **Note:** native callbacks are **not**
  yet SEH-isolated (a Lua mod gets `pcall` isolation; a faulting native mod can fault the
  game) — native is the powerful/you-are-responsible tier. Do engine work on the main
  thread (inside `OssModInit`, `on_frame`, or a `main_enqueue` job); off-thread, only read
  via the guarded `mem_*` and push work through `main_enqueue`.
- A DLL **without** `OssModInit` still loads (its `DllMain` runs) as a legacy standalone
  native mod — the loader just doesn't hand it the ABI.
- Example: `examples/native_hello/` (build: `make -C examples/native_hello`).
- Tier-2 typed hooks, UI panels, and `mod.game.*` are not in the ABI yet (appended later).

## `mod.toml`

```toml
[mod]
id          = "hello"                 # unique id (also the registry key)
name        = "Hello World"           # display name
version     = "1.0.0"                 # semver
description = "One line."
authors     = ["you"]
target_games = ["sotes_en"]           # profile ids, or ["*"] for any game

[loader]
min_version = "0.1.0"                 # refuse to load under an older loader

[load]
order = 0                             # optional: lower loads first
after = ["some_other_mod"]            # optional: soft dependency ordering
```

The manifest is optional for a hand-dropped local mod (the loader runs any
`mods\<name>\init.lua` regardless) but **required to publish to a registry**, where the
launcher reads it for name/version/description and load-order resolution.

## Load order

Mods load in the order `mods\` enumerates unless a manifest constrains it (`load.order`,
`load.after`). Explicit order is honored first, then `after` dependencies, then name.
(Dependency resolution across registry installs is the launcher's job — REGISTRY.md.)
