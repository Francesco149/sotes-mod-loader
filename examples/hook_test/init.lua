-- examples/hook_test/init.lua — in-game validation of the P3 chained hook registry.
--
-- Hooks the engine keyboard poll (0x5e2a10, thiscall, called every frame on the main
-- thread; ecx = the keyboard device object *(0x92d5bc)).  Proves: the codegen thunk
-- installs on a real fn without crashing, the register capture is correct (ctx.ecx ==
-- the real `this`), TWO callbacks on the SAME va both fire (the multi-mod chain), and
-- mod.hook.remove works.  All observable at the title (no save needed).

local KB_POLL   = mod.mem.reloc(0x5e2a10)
local KB_GLOBAL = mod.mem.reloc(0x92d5bc)          -- -> the keyboard device object (the expected `this`)
local expect_this = mod.mem.read_u32(KB_GLOBAL)

mod.log(string.format("=== hook_test: hooking kb_poll @ 0x%08x (expected this=0x%08x) ===", KB_POLL, expect_this or 0))

local n1, n2, shown = 0, 0, false
local h1 = mod.hook.entry(KB_POLL, function(ctx)
  n1 = n1 + 1
  if not shown then
    shown = true
    mod.log(string.format("  first hit: ctx.ecx=0x%08x match_this=%s  ret=0x%08x  esp=0x%08x",
      ctx.ecx, tostring(ctx.ecx == expect_this), ctx.ret, ctx.esp))
  end
end)
local h2 = mod.hook.entry(KB_POLL, function(ctx) n2 = n2 + 1 end)   -- 2nd cb on the SAME va
mod.log(string.format("  h1=%s h2=%s  installed hooks=%d", tostring(h1), tostring(h2), mod.hook.count()))

local f, removed = 0, false
mod.on_frame(function()
  f = f + 1
  if f % 120 ~= 0 then return end
  mod.log(string.format("[frame %d] kb_poll  cb1=%d  cb2=%d  (both ~= frames => chain fires)", f, n1, n2))
  if not removed and f >= 600 then
    mod.hook.remove(h2); removed = true
    mod.log("  removed h2 -> cb2 should stop; cb1 keeps climbing")
  end
end)
mod.log("=== hook_test armed ===")
