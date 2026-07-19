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

## Status — P0–P4 done (rolling checkpoint: [`docs/HANDOFF.md`](docs/HANDOFF.md))

Proxy `version.dll` + LuaJIT host (P0) · guarded `mod.mem` + togglable `mod.game.*`
bindings (P1) · main-thread executor (WndProc bootstrap → safepoint hook → `mod.main`/
`on_frame`, P2) · the chained hook registry — **Tier-1 observers + Tier-2 typed** hooks
(P3) · the **native-mod C ABI** `OssModInit(const OssApi*)` (P4). Validated **in-game** on
the real unpacked EN-SE (executor + Tier-2 hook rode the live `kb_poll`) and host-side
(`make -C core tests`). **Next: P5 UI** (loader-owned ImGui window + in-game mirror).

Roadmap (P5 UI → P6 voice mod → P7 trainer → P8 launcher): [`docs/DESIGN.md`](docs/DESIGN.md).
**A fresh session orients from [`docs/HANDOFF.md`](docs/HANDOFF.md).**

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
examples/    reference mods (hello, hook_typed — Lua; native_hello — native C)
docs/        HANDOFF.md · DESIGN.md · MOD-FORMAT.md · REGISTRY.md · TESTING.md
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

**Config** (optional): drop `oss_loader.cfg` beside `version.dll` (see
[`oss_loader.cfg.example`](oss_loader.cfg.example)). `skip_launcher=1` auto-dismisses the
game's `#32770` launcher for a hands-free boot (OFF by default — end users click Launch).

## Writing a mod

See [`docs/MOD-FORMAT.md`](docs/MOD-FORMAT.md). The smallest mod is
[`examples/hello/init.lua`](examples/hello/init.lua):

```lua
mod.log("hello from", mod.name)
```

## Related

- **`../sotes-mods`** — the author's mods (EN-SE trainer + JP voice patch) + the default
  registry the launcher ships with.
- **`../OpenSummoners`** — the parity port this loader grew out of, and the **canonical RE
  home for `sotes.exe`** (the loader targets the SAME unpacked EN-SE binary). Its Ghidra
  decompile (`docs/decompiled/`), findings (`docs/findings/`, `docs/findings/engine-quirks.md`),
  Frida parity harness, and the trainer's `tools/sotes_trainer/SE_CODE_MAP.md` are the go-to
  reference for engine VAs/structs.
  - **Cross-reference it before reversing** anything about the game — a VA/struct/offset is
    often already reversed there (e.g. safepoint `0x437c70`, `kb_poll 0x5e2a10`, base-stat
    table `FUN_00426fd0`).
  - **Record NEW game RE findings THERE**, not only here: the port is the consolidated home for
    game knowledge (`SE_CODE_MAP.md` for offsets/behaviour, `engine-quirks.md` for retail
    ground-truth quirks). Loader-side docs cover the *loader*; genuinely-new *engine* facts go
    to OpenSummoners so both projects benefit. Its `CLAUDE.md` points back here.
  - The current shipping voice patch also lives there (`tools/ennse_voice`) until the
    mod-loader-based one is proven.
