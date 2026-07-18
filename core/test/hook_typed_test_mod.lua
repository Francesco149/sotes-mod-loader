-- core/test/hook_typed_test_mod.lua — the mod side of hook_typed_test.c.
-- Installs Tier-2 typed hooks on the harness's target functions (addresses passed as
-- globals) and reports facts back through the shared RESULTS table.

local ffi = require("ffi")

-- cdecl: double the first arg (pre), then +100 to the return (post).
mod.hook.typed(T_PP, "int(int, int)", {
  pre  = function(ctx) ctx.args[1] = ctx.args[1] * 2 end,
  post = function(ctx) ctx.ret = ctx.ret + 100 end,
})

-- cdecl: block — the original never runs; the chain's return stands.
mod.hook.typed(T_BLOCK, "int(int, int)", {
  pre = function(ctx) ctx.blocked = true; ctx.ret = 999 end,
})

-- __stdcall: post-multiply (exercises stdcall callee stack cleanup on both closure + orig).
mod.hook.typed(T_SC, "int __stdcall(int, int)", {
  post = function(ctx) ctx.ret = ctx.ret * 10 end,
})

-- __thiscall: capture `this` (ecx = args[1]); bump the stack arg, then let the original run.
mod.hook.typed(T_THIS, "int __thiscall(void*, int)", {
  pre = function(ctx)
    RESULTS.seen_self = tonumber(ffi.cast("uintptr_t", ctx.args[1]))
    ctx.args[2] = ctx.args[2] + 1
  end,
})

-- Cross-tier exclusion: an entry (Tier-1) observer on a typed VA must be refused (nil).
RESULTS.excl_entry_on_typed = (mod.hook.entry(T_PP, function() end) == nil)

-- ...and a typed hook on an entry VA must be refused (raises -> pcall catches).
mod.hook.entry(T_ENTRYVA, function() end)
local ok = pcall(function() mod.hook.typed(T_ENTRYVA, "int(int, int)", { pre = function() end }) end)
RESULTS.excl_typed_on_entry = (ok == false)

RESULTS.typed_count = mod.hook.typed_count()
mod.log("hook_typed_test_mod installed; typed_count=" .. tostring(RESULTS.typed_count))
