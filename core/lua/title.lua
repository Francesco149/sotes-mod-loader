-- core/lua/title.lua — the title-flow ops of mod.game (embedded): save.load(slot) + attract.set(on).
-- Receives (hook, mem, sym, log, game); fills game.save / game.attract; returns install (armed at the first frame).
--
-- Built on the engine's own title driver (build_title_drive; per-edition VAs in tools/symgen_manual.tsv),
-- every edition the same way — no input injection, no menu-state guessing:
--   title_menu[_se]  int __thiscall(this, int)            -> the chosen action: 28 Continue, 0 = attract timeout
--   save_list        int __thiscall(this, int*, u16*, int*) -> 12 + (kind, slot, quest) = load; 11 = back
--   menu_runner      int __thiscall(this, list, ...)      -> one list frame: 11 back, 12 confirm, 13 again
-- save.load(slot): the next title_menu returns 28 without showing; inside save_list the first list frame
-- moves the list's cursor onto the row whose slot == `slot` (rows: *(ctrl+0x17c) x16 {kind, slot}; cursor
-- *(*(ctrl+0x174)+0x14)) and confirms — retail then validates + loads that save itself ("cannot be loaded"
-- stays retail's own dialog).  slot < 0 = the row the list opens on (the last-used save).  The list has a row per
-- slot (SE: 80, empty ones included); an empty / unloadable slot is refused by the game itself and a slot with no
-- row backs out — either way the title comes back and status() = 'failed', why.
-- attract.set(false): a title_menu that returns 0 (idle timeout → demo) is turned into an unhandled action, so
-- the driver loops straight back to the title.
-- A post-game save opens a Quest Selection list next (Main Quest / Bonus Quest): its first row is confirmed.
local hook, mem, sym, log, game = ...
local function enabled(id) return game.enabled(id) end
local ffi = require('ffi')

local st = { want = nil, state = 'idle', why = nil, inlist = false, done = false, attract = true, installed = false }
local function addr(p) return p == nil and 0 or tonumber(ffi.cast('uintptr_t', p)) end

local function title_pre(ctx)
  if st.state == 'pending' then
    ctx.blocked, ctx.ret = true, 28
    st.state = 'list'
    log(string.format('[save] title -> Continue (slot %d)', st.want))
  end
end
local function title_post(ctx)
  if ctx.ret == 0 and not st.attract and not ctx.blocked then ctx.ret = 31 end   -- 31: no case → loop to the title
end

local function list_pre(ctx)
  if st.state == 'list' then st.inlist, st.done = true, false end
end
local function list_post(ctx)
  if not st.inlist then return end
  st.inlist = false
  if ctx.ret == 12 then
    st.state = 'loaded'
    local kind = mem.read_i32(addr(ctx.args[2])) or 0
    local slot = mem.read_u16(addr(ctx.args[3])) or 0
    log(string.format('[save] save list -> load kind %d slot %d', kind, slot))
  else
    st.state, st.why = 'failed', st.why or (st.picked
      and string.format('the game refused slot %d (empty or unloadable; save list -> %s)', st.want, tostring(ctx.ret))
      or ('save list returned ' .. tostring(ctx.ret)))
    log('[save] ' .. st.state .. ': ' .. st.why)
  end
end

local function runner_pre(ctx)
  if not st.inlist then return end
  local ctrl = addr(ctx.args[2])
  if ctrl == 0 then return end                       -- a dialog frame (no list) — leave it to retail
  if st.done then                                    -- a second list after the pick: a post-game save's Quest
    if st.picked and not st.qdone and ctrl ~= st.ctrl then   -- Selection (save_list's 0x56aa70) → its first row,
      st.qdone, ctx.blocked, ctx.ret = true, true, 12        -- Main Quest
    end
    return
  end
  st.done, st.picked, st.qdone, st.ctrl = true, false, false, ctrl
  ctx.blocked = true
  if st.want < 0 then ctx.ret, st.picked = 12, true; return end   -- the default row
  local hdr, rows = mem.read_u32(ctrl + 0x174) or 0, mem.read_u32(ctrl + 0x17c) or 0
  local n = hdr ~= 0 and (mem.read_u32(hdr + 0x10) or 0) or 0
  for i = 0, math.min(n, 512) - 1 do
    if (mem.read_i32(rows + 16 * i + 4) or -1) == st.want then
      local page = mem.read_u32(hdr + 0x0c) or 0
      mem.write_u32(hdr + 0x14, i)
      if page > 0 then mem.write_u32(hdr + 0x18, math.floor(i / page) * page) end
      ctx.ret = 12
      st.picked = true
      return
    end
  end
  st.why = string.format('slot %d not in the save list (%d rows)', st.want, n)
  ctx.ret = 11                                       -- back out to the title
end

local SIG_TITLE = 'int __thiscall(void*, int)'
local SIG_LIST = 'int __thiscall(void*, int*, unsigned short*, int*)'
local SIG_RUN = 'int __thiscall(void*, void*, int, void*, int, unsigned int)'
local function install()
  if st.installed then return true end
  local tm, sl, mr = sym.va('title_menu'), sym.va('save_list'), sym.va('menu_runner')
  if not (tm and sl and mr) then return nil, 'title symbols missing on this build' end
  hook.typed(tm, SIG_TITLE, { pre = title_pre, post = title_post })
  local tse = sym.va('title_menu_se')
  if tse then hook.typed(tse, SIG_TITLE, { pre = title_pre, post = title_post }) end
  hook.typed(sl, SIG_LIST, { pre = list_pre, post = list_post })
  hook.typed(mr, SIG_RUN, { pre = runner_pre })
  st.installed = true
  log(string.format('[save] title hooks armed (title_menu 0x%x%s, save_list 0x%x, menu_runner 0x%x)', tm,
    tse and string.format(' + 0x%x', tse) or '', sl, mr))
  return true
end

local save = {}
-- mod.game.save.load([slot]) -> true | nil, err.  Call before the title menu opens (a mod's init runs at the
-- first frame — in time on every edition); the load happens when the title driver next asks for an action.
function save.load(slot)
  if not enabled('save') then return nil, 'binding disabled' end
  local ok, err = install()
  if not ok then return nil, err end
  st.want, st.state, st.why = math.floor(tonumber(slot) or -1), 'pending', nil
  return true
end
-- mod.game.save.status() -> 'idle' | 'pending' | 'list' | 'loaded' | 'failed', why
function save.status() return st.state, st.why end
function save.cancel() if st.state == 'pending' then st.state = 'idle' end end

local attract = {}
-- mod.game.attract.set(on): false keeps the title up (no demo on idle); true restores retail.
function attract.set(on)
  if not enabled('attract') then return nil, 'binding disabled' end
  local ok, err = install()
  if not ok then return nil, err end
  st.attract = on and true or false
  return true
end
function attract.get() return st.attract end

game.save, game.attract = game.save or {}, game.attract or {}
for k, v in pairs(save) do game.save[k] = v end          -- the tables the binding registry made (toggle-aware)
for k, v in pairs(attract) do game.attract[k] = v end
return install
