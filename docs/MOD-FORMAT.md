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
`require` needed. Each mod runs in its **own environment** (its globals don't collide
with other mods') inside a **protected call**: a Lua error is caught, the mod is
flagged, and neither the game nor the other mods are affected.

### `mod.*` API — P0 (today)

| field | type | meaning |
|---|---|---|
| `mod.name` | string | this mod's folder name |
| `mod.dir` | string | this mod's absolute folder (trailing `\`) |
| `mod.loader` | string | `"SotES Mod Loader"` |
| `mod.loader_version` | string | loader host version |
| `mod.log(...)` | fn | print-style line → `oss_modloader.log`, attributed to this mod |

### `mod.*` API — roadmap (later phases, see DESIGN.md)

```
mod.mem.{read,write,scan,module}   mod.reloc(va)              -- P1 memory + reloc
mod.main(fn) / mod.on_frame(fn)                               -- P2 main-thread exec
mod.hook.entry(va, cb)                                        -- P3 Tier-1 observe
mod.hook.typed(va, sig, {pre=,post=})  mod.hook.remove(h)     -- P3 Tier-2 typed
mod.call(va, sig, ...)                                        -- FFI call (via mod.main)
mod.ui.panel(name, draw)  mod.ui.window(name, draw, opts)     -- P5 UI (main window + in-game mirror)
mod.overlay.panel(...) / mod.overlay.draw(fn)                 -- later DDraw7 overlay
mod.game.*                                                    -- the loaded game profile
mod.on.{scene_change, settle, unload, ...}                   -- lifecycle events
```

## Native mod

```
mods\<name>.dll     # a plain DLL with its own DllMain
```

Loaded with `LOAD_WITH_ALTERED_SEARCH_PATH`, so it can ship co-located deps in
`mods\`. A native mod registers into the **same** hook + UI registries as Lua mods
through the C ABI (`OssModInit(const OssApi*)`, P4) — so a perf-critical hook or a big
feature can ship native while trivial mods stay Lua.

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
