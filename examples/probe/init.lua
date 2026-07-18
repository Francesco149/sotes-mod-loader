-- examples/probe/init.lua — a loader self-probe / API smoke test.
--
-- Exercises the P1 memory service (mod.mem) + the game-binding registry (mod.game)
-- without needing the actual game: it reads the HOST exe's own PE header (proves
-- guarded reads + scan work) and lists/toggles the registered bindings.  Under the
-- real game, mod.game.roster/coordinates return live data; under the test stub they
-- return empty/nil (no party actors) — the point here is that nothing faults.

local m = mod.mem
local base = m.base()
mod.log(string.format("mem.base()        = 0x%08x", base))
mod.log(string.format("mem.read_u16(base)= 0x%04x  (want 0x5a4d 'MZ')", m.read_u16(base) or -1))
mod.log(string.format("mem.module(k32)   = 0x%08x", m.module("kernel32.dll") or 0))
mod.log(string.format("mem.reloc(base)   = 0x%08x", m.reloc(0)))   -- 0 + delta = ASLR delta
local hit = m.scan("4d 5a")                                        -- find 'MZ'
mod.log(string.format("mem.scan('4d 5a') = %s", hit and string.format("0x%08x", hit) or "nil"))
mod.log("mem.read(unmapped) = " .. tostring(m.read_u32(0x10)))     -- guarded: nil, no fault

mod.log("game bindings:")
for _, b in ipairs(mod.game.list()) do
  mod.log(string.format("  %-12s enabled=%s  (%s)", b.id, tostring(b.enabled), b.desc))
end

-- roster / coordinates (nil/empty without the game — must not fault)
local roster = mod.game.roster
mod.log("mod.game.roster present = " .. tostring(roster ~= nil))
if roster then mod.log("  roster.members() count = " .. tostring(#roster.members())) end
mod.log("mod.game.coordinates.player() = " .. tostring(mod.game.coordinates and mod.game.coordinates.player()))

-- toggle test: disabling hides the binding table AND makes it inert
mod.game.disable("roster")
mod.log("after disable: mod.game.roster = " .. tostring(mod.game.roster))
mod.game.enable("roster")
mod.log("after enable:  mod.game.roster = " .. tostring(mod.game.roster ~= nil and "present" or "nil"))

mod.log("PROBE_OK")
