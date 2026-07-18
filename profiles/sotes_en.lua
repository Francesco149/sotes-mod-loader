-- profiles/sotes_en.lua — game profile for Fortune Summoners: Secret of the
-- Elemental Stone, English Special Edition (sotes_en.exe, 32-bit, unpacked
-- ImageBase 0x400000).
--
-- A profile keeps the loader CORE game-agnostic: all per-game facts live here and
-- are selected by the host exe name at boot.  A mod reads them via `mod.game.*`
-- instead of hard-coding, so the same mod can (in principle) target the JP build or
-- a future patch by shipping a second profile.  Sibling projects (openrecet /
-- OpenMare) drop in their own profile file.
--
-- STATUS: P0 stub.  Only `match` + `safepoint_va` are consumed today (the main-thread
-- executor lands in P2).  The struct cdefs / key globals are placeholders to be filled
-- from the OpenSummoners RE (SE_CODE_MAP.md) as the executor + typed hooks come online.

return {
  id = "sotes_en",

  -- Host-exe match: the loader picks this profile when the running exe's base name
  -- contains any of these (case-insensitive).
  match = { "sotes_en", "sotes" },

  image_base = 0x400000,   -- unpacked ImageBase; runtime delta = GetModuleHandle(NULL) - this

  -- Per-frame SAFEPOINT: a Tier-1 hook here is where the main-thread executor drains
  -- its job queue each frame (guaranteed on the engine thread).  (SotES: 0x437c70,
  -- the trainer's proven per-frame fn.)  [consumed in P2]
  safepoint_va = 0x437c70,

  -- The game window class (used by the WndProc bootstrap to find the engine thread).
  -- SotES shows a #32770 launcher dialog first, then the real game window — the
  -- bootstrap skips #32770.  [consumed in P2]
  window = { skip_classes = { "#32770" } },

  -- Windows / VAs the loader must NEVER hook, and boot/teardown ranges where installing
  -- or removing a hook is unsafe (render-root free+realloc, input-device activation,
  -- scene rebuild).  [consumed in P3; fill from findings/ as hooks come online]
  no_hook = {},

  -- Shared struct cdefs (LuaJIT ffi.cdef) + key globals, so mods read this->field
  -- instead of raw offsets.  [consumed in P1/P2; fill from SE_CODE_MAP.md]
  cdefs = [[
    // placeholder — engine structs go here as they're needed by mods
  ]],
  globals = {
    -- name = va   (absolute, ImageBase-relative; reloc'd by the loader)
  },
}
