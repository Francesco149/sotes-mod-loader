-- examples/hook_typed/init.lua — in-game validation of the P3 Tier-2 TYPED hook.
--
-- Hooks the engine keyboard poll (0x5e2a10, `kb_poll(this)`: thiscall, this-only, 0 stack
-- args, called every frame on the main thread; ecx = the keyboard device *(0x92d5bc)) as a
-- TYPED hook.  Proves the FFI-closure detour marshals a real engine thiscall: `this` arrives
-- as ctx.args[1] (a typed void* cdata) and the post hook sees the return — with the C
-- main-thread gate letting the per-frame call through.  OBSERVE-ONLY (no modify, no block) so
-- the game is unaffected; a real Tier-2 hook may set ctx.args[i] / ctx.blocked / ctx.ret.
--
-- (Signature MUST match the target's real convention + arg count — a wrong `ret N` corrupts
-- the caller's stack.  Here: 0 stack args -> ret 0, safe.  Tier-1 mod.hook.entry needs no sig.)

local ffi = require("ffi")

local KB_POLL   = mod.mem.reloc(0x5e2a10)
local KB_GLOBAL = mod.mem.reloc(0x92d5bc)         -- -> the keyboard device object (the expected `this`)
local expect_this = mod.mem.read_u32(KB_GLOBAL)

mod.log(string.format("=== hook_typed: typed-hooking kb_poll @ 0x%08x (expect this=0x%08x) ===",
  KB_POLL, expect_this or 0))

local hits, this_ok, ret_seen, shown = 0, false, nil, false
local h = mod.hook.typed(KB_POLL, "int __thiscall(void*)", {
  pre = function(ctx)
    hits = hits + 1
    this_ok = (tonumber(ffi.cast("uintptr_t", ctx.args[1])) == expect_this)
    -- (a real hook could here do: ctx.args[1] = ... ; or ctx.blocked = true; ctx.ret = 0)
  end,
  post = function(ctx)
    ret_seen = ctx.ret
    if not shown then
      shown = true
      mod.log(string.format("  first typed hit: this match=%s  ret=%s",
        tostring(this_ok), tostring(ret_seen)))
    end
  end,
})
mod.log(string.format("  handle=%s  typed VAs=%d", tostring(h), mod.hook.typed_count()))

local f, removed = 0, false
mod.on_frame(function()
  f = f + 1
  if f % 120 ~= 0 then return end
  mod.log(string.format("[frame %d] kb_poll typed hits=%d this_ok=%s ret=%s",
    f, hits, tostring(this_ok), tostring(ret_seen)))
  if not removed and f >= 600 then
    mod.hook.remove(h); removed = true
    mod.log("  removed the typed hook -> hits should stop climbing")
  end
end)
mod.log("=== hook_typed armed ===")
