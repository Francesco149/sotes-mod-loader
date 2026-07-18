-- core/test/exec_test_mod.lua — the mod exec_test drives.
-- init runs at the FIRST safepoint (deferred); on_frame fires each safepoint; mod.main
-- runs its job (immediately here, since the test doesn't arm the real hook).
mod.log("[mod] init running (expected: at safepoint 1, not before)")
local n = 0
mod.on_frame(function() n = n + 1; mod.log("[mod] on_frame tick " .. n) end)
mod.main(function() mod.log("[mod] main job ran") end)
mod.log("[mod] init done")
