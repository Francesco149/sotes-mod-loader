# SotES Mod Loader

A stability-first, game-agnostic native **mod loader + mod API** for *Fortune
Summoners: Secret of the Elemental Stone*. Ships as a drop-in **`version.dll`** (the
game exe is never modified) that hosts a **LuaJIT** modding runtime and native DLL mods
under one hardened base — a **managed, chained hook registry** so multiple mods can hook
the same function without clobbering each other, a **main-thread executor** so engine
calls are safe by construction, and a **shared ImGui UI**. A **launcher** installs mods
package-manager style from git-repo sources.

> **North star: a bad mod can't crash the game, and two mods can't clobber each other.**
> Everything is subordinate to that. See [`docs/DESIGN.md`](docs/DESIGN.md).

## Status — P0 (scaffold)

- ✅ Proxy `version.dll` — forwards the real `version.dll` → `realver.dll`; loads both
  native (`mods\<name>.dll`) and Lua (`mods\<name>\init.lua`) mods.
- ✅ LuaJIT runtime — **JIT compiled out** (interpreter + FFI only, the stable mode for an
  injected DLL), cross-built to the i686 Windows target and **proven to run on-target**
  (`build/lj_smoke.exe`: JIT off, FFI call into kernel32 works).
- ✅ `mod` table with `mod.log` + mod identity; per-mod env + `pcall` isolation.
- ⏳ Injection test on the real game (Windows-side, next).

Roadmap (P1 memory → P2 main-thread executor → P3 hooks → P4 native bridge → P5 UI →
P6 voice mod → P7 trainer → P8 launcher): [`docs/DESIGN.md`](docs/DESIGN.md).

## Layout

```
core/        the loader host — proxy version.dll + LuaJIT runtime + (P1+) mem/hooks/exec/UI
  loader.c       DllMain, proxy attach, mods\ scan (native + Lua)
  lua_host.c     LuaJIT bring-up, the mod.* table, per-mod isolation
  version.def    the 17 version.dll export forwards → realver
  Makefile       cross-builds LuaJIT (static, JIT off) then version.dll
  test/          on-target smoke tests (lj_smoke.c)
launcher/    the package-manager launcher (ImGui) — later phase (P8)
profiles/    per-game facts, auto-selected by exe (profiles/sotes_en.lua)
examples/    reference mods (examples/hello — the minimal Lua mod)
docs/        DESIGN.md · MOD-FORMAT.md · REGISTRY.md
```

## Build

Everything runs inside the repo's own `nix develop` (self-contained: 32-bit mingw +
ImGui + LuaJIT source + a 32-bit host gcc for LuaJIT's buildvm).

```
nix develop --command make -C core        # -> build/version.dll
```

## Install (hand install; the launcher automates this later)

Beside the game exe (`…\steamapps\common\sotes\`):

```
version.dll             <- build/version.dll (this loader)
realver.dll             <- a copy of C:\Windows\SysWOW64\version.dll
mods\<name>\init.lua     <- one or more Lua mods
mods\<name>.dll          <- and/or native mods
```

Launch normally. `oss_modloader.log` (beside the exe) records each mod loaded.

## Writing a mod

See [`docs/MOD-FORMAT.md`](docs/MOD-FORMAT.md). The smallest mod is
[`examples/hello/init.lua`](examples/hello/init.lua):

```lua
mod.log("hello from", mod.name)
```

## Related

- **`../sotes-mods`** — the author's mods (EN-SE trainer + JP voice patch) + the default
  registry the launcher ships with.
- **`../OpenSummoners`** — the parity port this loader grew out of; the current shipping
  voice patch lives there (`tools/ennse_voice`) until the mod-loader-based one is proven.
