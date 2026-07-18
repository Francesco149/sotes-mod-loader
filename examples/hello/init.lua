-- examples/hello/init.lua — the minimal reference mod for the SotES Mod Loader.
--
-- The loader runs this file once at startup with a per-mod `mod` table already in
-- scope (no require needed).  This is the smallest possible mod: it just logs.
-- The loader catches any error in here (a mod can't crash the game or other mods),
-- so it's also the canary that proves the Lua host is alive.
--
-- P0 API surface (see docs/MOD-FORMAT.md):
--     mod.name            this mod's folder name          (string)
--     mod.dir             this mod's absolute folder       (string)
--     mod.loader          "SotES Mod Loader"               (string)
--     mod.loader_version  the loader host version          (string)
--     mod.log(...)        print-style log -> oss_modloader.log, attributed to this mod
--
-- Later phases hang mod.mem / mod.hook / mod.main / mod.on_frame / mod.ui off the
-- same table — see docs/DESIGN.md.

mod.log("hello from", mod.name)
mod.log("  dir    =", mod.dir)
mod.log("  loader =", mod.loader, mod.loader_version)
mod.log("  lua    =", _VERSION, jit and ("(" .. jit.version .. ", jit=" .. tostring(jit.status()) .. ")") or "")
