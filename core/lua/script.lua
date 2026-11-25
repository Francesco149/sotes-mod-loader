-- core/lua/script.lua — mod.game.script + mod.game.sound (api 1.8, embedded).
-- Receives (hook, mem, sym, log, game, res, edition_fn); fills game.script / game.sound; returns install (armed at the
-- first frame, like title.lua — .text is decrypted by then on SteamStub builds).
--
-- EVENT SCRIPTS IN LUA, ON RETAIL'S OWN SCENE MACHINERY.  Every retail cutscene / conversation is a C function the
-- script engine (script_engine, thiscall on the event runner `ev` = *(world+0x1054)) picks by (room, mode ev+0x14,
-- actor ev+0x4 → its handle +0x1d8).  It fills the runner's command block (a message, a choice list, a fade, a camera
-- pan, cast moves) and runs the frames with script_wait (the scene command runner; 6 = the scene was ended / quit).
-- A Lua script here does exactly that: a typed pre-hook on script_engine claims the event and calls the same
-- primitives over FFI.  Their frame loops re-enter Lua (on_frame, the dev server) — fine: FFI calls are plain C calls.
--
--   script.on_talk(handle, fn[, {room=key}])  talk (mode 2) / examine (mode 3) of an actor whose handle == handle
--                                             (a map record's A[1], or actors.spawn{handle=} / actors.set_handle)
--   script.off_talk(handle)
--   script.run(fn)                            queue a script with no actor: it starts at the next free-roam frame
--                                             (gimmick_pass, where retail starts switch / chest events, calls
--                                             event_run itself; then touch_pass's post-talk input reset)
--   script.after(fn)                          fn() at the next free-roam frame (after the running event): warps etc.
--   script.running() -> bool                  a mod script owns the game right now
--
-- Text: \n becomes retail's %n line break.
-- fn(s): s.ev (runner), s.actor (the talked-to actor or nil), s.handle, s.room (area key) and
--   s:say(text[, {name=, actor=, own_name=, voice=, face=}])   a message box (spoken by s.actor, titled s.name)
--   s:narrate(text)                                           a narration box (no speaker)
--   s:choose({"a", "b", ...}[, {default=, cancel=, x=, y=, w=}]) -> index | nil (cancelled)  — up to 16 rows
--   s:yes_no([no_cancel]) -> bool                             retail's own Yes / No (its localized labels)
--   s:shop({item ids}[, {buy=true | sell=true}])              retail's shop (Buy / Sell) on a custom stock
--   s:storage()                                               retail's storage box
--   s:book(title, {page, ...})                                retail's book reader on custom pages
--   s:give(what, n[, quiet]) -> got                            retail's story gift ("You obtained ..."): what = an
--                                                              item id, or 1 / 2 / 3 = n copper / silver / gold
--   s:heal()                                                  the whole party to full (the inn's room_party_heal)
--   s:rest()                                                  retail's inn rest: jingle, fade out, heal, fade back
--   s:inn{price=, offer=, decline=, broke=, morning=, bye=}   retail's whole inn (its price window + Yes / payment)
--   s:fade_out([ms]) / s:fade_in([ms]) / s:wait([ms])          screen fades / a plain wait (default 1000 ms)
--   s:pause(n)                                                the scene countdown (+0x57c; retail uses 100..200)
--   s:camera(actor[, speed])                                  pan to an actor (nil = the leader)
--   s:sound(id[, vol[, freq]])                                a sound (stock pool id or a sound.add handle)
--   s:leader() -> char ptr
-- Any wait that returns 6 (quit / game over / the scene ended) aborts the Lua script and returns 6, as retail does.
-- A Lua error ends the event cleanly (script_end) and is logged.
--
-- SOUND.  Effects play from the engine's sound pools (sys + 0x364 / SE 0x378 / JP 0x374, 465 / 483 slots, all in use):
-- a pool = {u16 n, u16 next, voices*, dsound*, u16 res, -, hmodule, u16 group} whose WAVE resource (a plain RIFF) is
-- loaded lazily through FindResourceA(module, res, "WAVE").  sound.add(wav) serves the bytes through the resource
-- overlay under a fresh id and builds our own pool; sound.play(handle) puts it into a slot ONLY for the duration of
-- the engine's own sfx_play call (the playing DirectSound buffer is ours), so no stock sound changes.
--   sound.play(id[, vol[, freq]])  id = a stock pool slot or a sound.add handle; vol = hundredths of dB added to the
--                                  effects volume (0 = as is); freq 0 = the wave's own rate
--   sound.add(wav) -> handle       RIFF WAVE bytes (PCM)
--   sound.wav(samples, rate) -> bytes   16-bit mono PCM from a Lua array of -1..1 floats
--   sound.slots() -> n
--   sound.status(id) -> {loaded, voices, playing}   (a check for tests: did the WAVE load, is a buffer playing)
local hook, mem, sym, log, game, res, edition_fn = ...
local ffi = require('ffi')

local ED = {
  enold       = { room = 0x2784, stride = 0x4120, pools = 0x364, npools = 465, se = false },
  ['ense-en'] = { room = 0x3788, stride = 0x4124, pools = 0x378, npools = 483, se = true },
  ['ense-jp'] = { room = 0x3788, stride = 0x4124, pools = 0x374, npools = 483, se = true },
}
local L, edition                            -- resolved on first use (layout())
local function layout() if not L then edition = edition_fn(); L = ED[edition] end return L end
local MODE_MOD = 0x4d4f44                   -- 'MOD': event_run's mode for a queued script (no retail script matches)
local ABORT = {}                            -- error sentinel: a wait returned 6

local st = { installed = false, talk = {}, queue = {}, after = {}, running = false, errors = 0 }
local P = {}                                -- bound primitives

local function addr(p) return p == nil and 0 or tonumber(ffi.cast('uintptr_t', p)) end
local function at(p, off) return ffi.cast('uint8_t *', p) + off end
local function u32(p, off) return ffi.cast('uint32_t *', at(p, off))[0] end
local function i32(p, off) return ffi.cast('int32_t *', at(p, off))[0] end
local function w32(p, off, v) ffi.cast('uint32_t *', at(p, off))[0] = v end
local function w16(p, off, v) ffi.cast('uint16_t *', at(p, off))[0] = v end
local function r16(p, off) return ffi.cast('uint16_t *', at(p, off))[0] end
local function world() local g = sym.va('g_world') return g and mem.read_u32(g) or 0 end

local SIGS = {
  script_begin = 'void (__thiscall *)(void *)',
  script_end = 'void (__thiscall *)(void *)',
  script_leader = 'void *(__thiscall *)(void *)',
  script_party_cast = 'void (__thiscall *)(void *)',
  script_wait = 'int (__thiscall *)(void *)',
  scene_party_stance = 'int (__thiscall *)(void *, int, void *)',
  scene_talk_face = 'int (__thiscall *)(void *, int, void *, int, int)',
  scene_msg = 'void (__thiscall *)(void *, uint32_t, void *, const char *, int, int, int, const char *, int, int, uint32_t)',
  storage_run = 'int (__thiscall *)(void *)',
  book_read = 'int (__thiscall *)(void *, const void *)',
  item_give = 'int (__thiscall *)(void *, int, int, int, int *)',
  event_run = 'int (__thiscall *)(void *, void *, void *, void *, int, int, int)',
  sfx_play = 'int (__thiscall *)(void *, uint16_t, int, int)',
  si_41a160 = 'int (__thiscall *)(void *)',
  el_41c390 = 'int (__thiscall *)(void *, int, int, int, void *, const char *, const char *, const char *, const char *, const char *)',
  room_input_flush = 'void (__thiscall *)(void *)',
  ts_40fe00 = 'void (__thiscall *)(void *)',
}
local function bind()
  for name, sig in pairs(SIGS) do
    local va = sym.va(name)
    if va then P[name] = ffi.cast(sig, va) end
  end
  -- edition-specific shapes: SE's Yes / No takes `no_cancel`, SE's shop a 4th mode argument
  local yn, sh = sym.va('scene_yes_no'), sym.va('shop_run')
  local hl = sym.va('room_party_heal')     -- SE: (room, all, mp) — retail's inns pass (1, 1); enold: (room, all)
  if L.se then
    local h3 = hl and ffi.cast('void (__thiscall *)(void *, int, int)', hl)
    P.heal = h3 and function(room) h3(room, 1, 1) end
  else
    local h2 = hl and ffi.cast('void (__thiscall *)(void *, int)', hl)
    P.heal = h2 and function(room) h2(room, 1) end
  end
  if L.se then
    P.yes_no = yn and ffi.cast('int (__thiscall *)(void *, int)', yn)
    P.shop = sh and ffi.cast('int (__thiscall *)(void *, const int32_t *, int, int, int)', sh)
  else
    local y0 = yn and ffi.cast('int (__thiscall *)(void *)', yn)
    P.yes_no = y0 and function(ev) return y0(ev) end
    local s0 = sh and ffi.cast('int (__thiscall *)(void *, const int32_t *, int, int)', sh)
    P.shop = s0 and function(ev, list, b, s) return s0(ev, list, b, s) end
  end
end
local function need(name)
  if not P[name] then error(('mod.game.script: %s is unavailable on %s'):format(name, tostring(edition)), 3) end
  return P[name]
end

-- ── the script object ────────────────────────────────────────────────────────────────────────────────────────────
local S = {}
S.__index = S
local function wait(s)
  local r = P.script_wait(s.ev)
  if r == 6 then error(ABORT, 0) end
  return r
end
local function camera_on(ev, target, speed)   -- the pan command (scene_camera's fields): +0x5c target, +0x58 speed
  w32(ev, 0x4c, 0); w32(ev, 0x5c, addr(target)); w32(ev, 0x50, 0); w32(ev, 0x54, 0); w32(ev, 0x58, speed or 800)
  w32(ev, 0x20, 3)
end
function S:leader() return need('script_leader')(self.ev) end
function S:camera(actor, speed) camera_on(self.ev, actor or P.script_leader(self.ev), speed); wait(self) end
local function msgtext(t) return (tostring(t):gsub('\n', '%%n')) end   -- retail's line break token is %n
function S:say(text, o)
  o = o or {}
  local actor = o.actor == 'leader' and P.script_leader(self.ev) or o.actor or self.actor
  local name = o.name or self.name
  need('scene_msg')(self.ev, 0, actor, msgtext(text), 0, 1, (o.own_name or not name) and 1 or 0, name, o.voice or 0, 0,
    o.face or 0)
  return wait(self)
end
function S:narrate(text)
  need('scene_msg')(self.ev, 3, nil, msgtext(text), 0, 1, 0, nil, 0, 0, 0)
  return wait(self)
end
-- The scene runner's choice list (scene_run: +0x584 count, +0x588 the pick / default id, +0x58c the pick on cancel,
-- +0x590 no cancel, the window +0x594 w / +0x598 x / +0x59c y (height 28 n + 64), 0x58-byte rows from +0x5a0:
-- text[0x50], +0x50 kind, +0x54 id) and wait mode 5 — what scene_yes_no fills with two rows.
function S:choose(items, o)
  o = o or {}
  local ev, n = self.ev, math.min(#items, 16)
  assert(n >= 1, 'choose: no items')
  local longest = 0
  for i = 1, n do longest = math.max(longest, #tostring(items[i])) end
  local w = o.w or math.max(160, math.min(600, 9 * longest + 88))
  local h = 28 * n + 64
  w16(ev, 0x584, 0)
  w32(ev, 0x588, o.default or 1)
  w32(ev, 0x58c, 0)                       -- cancel → id 0 → nil
  w32(ev, 0x590, o.no_cancel and 1 or 0)
  w32(ev, 0x594, w)
  w32(ev, 0x598, o.x or math.floor((640 - w) / 2))
  w32(ev, 0x59c, o.y or math.max(8, math.min(240, 472 - h)))   -- retail's Yes / No: (240, 240)
  for i = 1, n do
    local rec = at(ev, 0x5a0 + 0x58 * (i - 1))
    local t = tostring(items[i]):sub(1, 0x4f)
    ffi.fill(rec, 0x50); ffi.copy(rec, t, #t)
    w32(ev, 0x5a0 + 0x58 * (i - 1) + 0x50, 0)
    w32(ev, 0x5a0 + 0x58 * (i - 1) + 0x54, i)
  end
  w16(ev, 0x584, n)
  w32(ev, 0x20, 5)
  wait(self)
  local pick = i32(ev, 0x588)
  if pick < 1 or pick > n then return nil end
  return pick
end
function S:yes_no(no_cancel)
  local r = need('yes_no')(self.ev, no_cancel and 1 or 0)
  if r == 6 then error(ABORT, 0) end
  return r == 14
end
function S:shop(ids, o)
  o = o or {}
  local list = ffi.new('int32_t[?]', #ids + 1)
  for i, id in ipairs(ids) do list[i - 1] = id end
  self._keep[#self._keep + 1] = list
  local r = need('shop')(self.ev, list, o.buy and 1 or 0, o.sell and 1 or 0, 1)
  if r == 6 then error(ABORT, 0) end
  return r
end
function S:storage()
  local r = need('storage_run')(self.ev)
  if r == 6 then error(ABORT, 0) end
  return r
end
-- book_read's book: the title (0x28 bytes) then 2052-byte pages {text[2048], u32 end}; the page whose `end` != 0
-- is the terminator (the count = the pages before it).
function S:book(title, pages)
  local n = #pages
  local b = ffi.new('uint8_t[?]', 0x28 + 2052 * (n + 1))
  local t = tostring(title):sub(1, 0x27)
  ffi.copy(b, t, #t)
  for i = 1, n do
    local p = msgtext(pages[i]):sub(1, 2047)
    ffi.copy(b + 0x28 + 2052 * (i - 1), p, #p)
  end
  ffi.cast('uint32_t *', b + 0x28 + 2052 * n + 2048)[0] = 1
  self._keep[#self._keep + 1] = b
  local r = need('book_read')(self.ev, b)
  if r == 6 then error(ABORT, 0) end
  return r
end
function S:give(what, n, quiet)
  local got = ffi.new('int[1]')
  local r = need('item_give')(self.ev, what, n or 0, quiet and 0 or 1, got)
  if r == 6 then error(ABORT, 0) end
  return got[0] ~= 0
end
function S:heal() need('heal')(ffi.cast('void *', u32(self.ev, 0x8))) end
-- the effect wait (scene_wait / scene_wait_fade: +0x20 = 2, +0x24 = 1, +0x28 1 / 2, +0x2c ms)
local function effect_wait(s, kind, t) local ev = s.ev w32(ev, 0x24, 1); w32(ev, 0x28, kind); w32(ev, 0x2c, t); w32(ev, 0x20, 2); return wait(s) end
-- retail's inn rest (si_41a160: the inn jingle, a 1 s fade to black, the world tick off +0x1c) and what retail's inn
-- (el_41c390) does after it: the party healed, a 200 pause, a 500 ms fade back with a 1 s wait, the world tick on.
function S:rest()
  local r = need('si_41a160')(self.ev)
  if r == 6 then error(ABORT, 0) end
  self:heal()
  self:pause(200)
  local ev = self.ev
  w32(ev, 0x30, 1); w32(ev, 0x34, 2); w32(ev, 0x3c, 500); w32(ev, 0x38, 1)
  local r2 = effect_wait(self, 1, 1000)
  w32(ev, 0x1c, 0)
  return r2
end
-- retail's whole inn (el_41c390, the innkeepers' own): `offer` (a printf with the price, %d) → the "Inn - N to stay"
-- window (Stay / Leave / the party) → not enough money: `broke`; declined: `decline`; else paid, rest (as above),
-- `morning`, then `bye`.  Lines nil = skipped.  Returns the game's result (6 = the scene ended).
function S:inn(o)
  o = o or {}
  local keep = self._keep
  local function str(t) if t == nil then return nil end t = msgtext(t) keep[#keep + 1] = t return t end
  local r = need('el_41c390')(self.ev, o.price or 1000, 1, 0, self.actor, str(o.offer), str(o.decline), str(o.broke),
    str(o.morning), str(o.bye))
  if r == 6 then error(ABORT, 0) end
  return r
end
-- the fade command (scene_fade_out / _in: +0x30 = 1, +0x34 3 out / 2 in, +0x3c ms, +0x38 0 / 1) and the effect wait
-- (scene_wait / scene_wait_fade: +0x20 = 2, +0x24 = 1, +0x28 1 / 2, +0x2c ms) — what retail's inn rest runs
function S:fade_out(t) local ev = self.ev w32(ev, 0x30, 1); w32(ev, 0x34, 3); w32(ev, 0x3c, t or 1000); w32(ev, 0x38, 0); return effect_wait(self, 2, t or 1000) end
function S:fade_in(t) local ev = self.ev w32(ev, 0x30, 1); w32(ev, 0x34, 2); w32(ev, 0x3c, t or 1000); w32(ev, 0x38, 1); local r = effect_wait(self, 1, t or 1000) w32(ev, 0x1c, 0) return r end
function S:wait(ms) return effect_wait(self, 1, ms or 1000) end
-- the countdown (scene_pause): +0x20 = 6, +0x57c = ticks
function S:pause(t) local ev = self.ev w32(ev, 0x57c, t or 30); w32(ev, 0x20, 6); return wait(self) end
function S:sound(id, vol, freq) return game.sound.play(id, vol, freq) end

-- ── running a script on the event ────────────────────────────────────────────────────────────────────────────────
local function run_script(ev, job, actor)
  local s = setmetatable({ ev = ev, actor = actor, handle = job.handle, name = job.name, _keep = {} }, S)
  local m = game.map and game.map.get and game.map.get()
  s.room = m and m.area
  st.running = true
  local ok, err = pcall(function()
    P.script_begin(ev)
    camera_on(ev, P.script_leader(ev))
    P.script_party_cast(ev)
    wait(s)
    if actor ~= nil then                    -- retail's talk beat: weapons away, face each other, camera back
      if P.scene_party_stance(ev, 0, ffi.cast('void *', u32(ev, 0x8))) ~= 0 then wait(s) end
      -- a character turns back and Arche steps away if too close (1, 1); furniture / devices: neither (0, 0) —
      -- retail's bookshelves do (0, 0), and a device asked to face back never finishes the cast wait
      local code = i32(actor, 0x1d4)
      local person = code >= 50000 and code < 60000
      P.scene_talk_face(ev, 0, actor, person and 1 or 0, person and 1 or 0)
      wait(s)
      camera_on(ev, P.script_leader(ev))
      wait(s)
    end
    s.result = job.fn(s)
  end)
  st.running = false
  if not ok then
    if err == ABORT then return 6 end
    st.errors = st.errors + 1
    log('[script] ' .. tostring(job.what) .. ': ' .. tostring(err))
  end
  P.script_end(ev)
  return type(s.result) == 'number' and s.result or 0
end

local function engine_pre(ctx)
  local ev = ctx.args[1]
  local mode = u32(ev, 0x14)
  if mode == MODE_MOD then
    local job = table.remove(st.queue, 1)
    if not job then return end
    ctx.blocked, ctx.ret = true, run_script(ev, job, nil)
    return
  end
  if mode ~= 2 and mode ~= 3 then return end
  local actor = ffi.cast('void *', u32(ev, 0x4))
  if actor == nil then return end
  local h = i32(actor, 0x1d8)
  local job = st.talk[h]
  if not job then return end
  if job.room then
    local m = game.map.get()
    if not m or m.area ~= job.room then return end
  end
  ctx.blocked, ctx.ret = true, run_script(ev, job, actor)
end

-- gimmick_pass(rl, leader) runs every free-roam frame (the room loop, room loaded, UI unlocked) and starts events itself
-- (switches, chests: event_run).  touch_pass — the talk check — only runs on the talk / examine keys.
local function frame_pre(ctx)
  if #st.after > 0 then
    local fns = st.after
    st.after = {}
    for _, f in ipairs(fns) do
      local ok, err = pcall(f)
      if not ok then log('[script] after: ' .. tostring(err)) end
    end
  end
  if #st.queue == 0 then return end
  local w = world()
  if w == 0 then return end
  local rl = ctx.args[1]
  local ev = ffi.cast('void *', mem.read_u32(w + 0x1054))
  local r = P.event_run(ev, rl, ffi.cast('void *', mem.read_u32(w + L.room)), nil, 0, 0, MODE_MOD)   -- the room (world +0x2784 / SE +0x3788)
  -- what touch_pass does after a handled talk (so the confirm key doesn't leak into play): the input latches reset, every room's input flushed
  if P.ts_40fe00 then P.ts_40fe00(ffi.cast('void *', sym.va('g_sys'))) end
  if P.room_input_flush then
    local rooms, n = u32(rl, 0), r16(rl, 4)
    for i = 0, n - 1 do P.room_input_flush(ffi.cast('void *', rooms + L.stride * i)) end
  end
  ctx.blocked, ctx.ret = true, r == 6 and 6 or 0
end

local function install()
  if st.installed then return true end
  if not layout() then return nil, 'unknown edition ' .. tostring(edition) end
  local se, tp = sym.va('script_engine'), sym.va('gimmick_pass')
  if not (se and tp) then return nil, 'script symbols missing on this build' end
  bind()
  for _, n in ipairs { 'script_begin', 'script_end', 'script_leader', 'script_party_cast', 'script_wait',
                       'scene_party_stance', 'scene_talk_face', 'event_run' } do
    if not P[n] then return nil, 'symbol ' .. n .. ' missing on this build' end
  end
  hook.typed(se, 'int __thiscall(void *)', { pre = engine_pre })
  hook.typed(tp, 'int __thiscall(void *, void *)', { pre = frame_pre })
  st.installed = true
  log(string.format('[script] armed (script_engine 0x%x, gimmick_pass 0x%x, %s)', se, tp, edition))
  return true
end

local script = {}
function script.on_talk(handle, fn, o)
  assert(type(handle) == 'number' and type(fn) == 'function', 'on_talk(handle, fn[, opts])')
  local ok, err = install()
  if not ok then return nil, err end
  o = o or {}
  st.talk[handle] = { fn = fn, handle = handle, room = o.room, name = o.name, what = 'talk ' .. handle }
  return true
end
function script.off_talk(handle) st.talk[handle] = nil return true end
function script.run(fn, o)
  local ok, err = install()
  if not ok then return nil, err end
  o = o or {}
  st.queue[#st.queue + 1] = { fn = fn, name = o.name, what = o.what or 'run' }
  return true
end
function script.after(fn)
  local ok, err = install()
  if not ok then return nil, err end
  st.after[#st.after + 1] = fn
  return true
end
function script.running() return st.running end
function script.pending() return #st.queue end
function script.errors() return st.errors end

-- ── sound ────────────────────────────────────────────────────────────────────────────────────────────────────────
local snd = { pools = {}, next_res = 0xF000 }
local function pool_slot(i) return sym.va('g_sys') + L.pools + 4 * i end
local sound = {}
local function bind_once() if not P.sfx_play and layout() then bind() end end
function sound.slots() return layout() and L.npools or 0 end
function sound.play(id, vol, freq)
  bind_once()
  local sys = sym.va('g_sys')
  if not (sys and P.sfx_play) then return nil, 'sfx_play unavailable' end
  local custom = snd.pools[id]
  if custom then
    local slot = pool_slot(0)
    local keep = mem.read_u32(slot)
    if (mem.read_u32(custom.p + 8) or 0) == 0 then   -- the DirectSound device: borrow a stock pool's
      for i = 0, L.npools - 1 do
        local q = mem.read_u32(pool_slot(i))
        if q and q ~= 0 and (mem.read_u32(q + 8) or 0) ~= 0 then mem.write_u32(custom.p + 8, mem.read_u32(q + 8)) break end
      end
    end
    mem.write_u32(slot, custom.p)
    local ok, r = pcall(P.sfx_play, ffi.cast('void *', sys), 0, vol or 0, freq or 0)
    mem.write_u32(slot, keep)
    if not ok then return nil, r end
    return r ~= 0
  end
  if type(id) ~= 'number' or id < 0 or id >= L.npools then return nil, 'no sound ' .. tostring(id) end
  return P.sfx_play(ffi.cast('void *', sys), id, vol or 0, freq or 0) ~= 0
end
function sound.add(wav, o)
  o = o or {}
  assert(type(wav) == 'string' and wav:sub(1, 4) == 'RIFF' and wav:sub(9, 12) == 'WAVE', 'sound.add: RIFF WAVE bytes')
  local id = snd.next_res
  snd.next_res = id + 1
  if not res.set('WAVE', id, wav) then return nil, 'resource overlay refused' end
  local buf = ffi.new('uint8_t[0x18]')
  local p = addr(buf)
  ffi.cast('uint16_t *', buf)[0] = o.voices or 2          -- n: simultaneous plays
  ffi.cast('uint16_t *', buf + 0xc)[0] = id               -- the WAVE resource id
  ffi.cast('uint32_t *', buf + 0x10)[0] = 0               -- module: 0 = the exe (the overlay answers any module)
  local h = 0x10000 + id
  snd.pools[h] = { p = p, buf = buf, res = id }
  return h
end
-- sound.status(id) -> {loaded=, voices=, playing=}   a pool's state: loaded = its voices exist (the WAVE was found and
-- decoded); playing = voices whose DirectSound buffer reports DSBSTATUS_PLAYING (voice {wrapper*, -}, wrapper +0 =
-- the IDirectSoundBuffer; GetStatus = vtable slot 9).
function sound.status(id)
  if not layout() then return nil end
  local c = snd.pools[id]
  local p = c and c.p or (type(id) == 'number' and id >= 0 and id < L.npools and mem.read_u32(pool_slot(id)))
  if not p or p == 0 then return nil end
  local n, v = mem.read_u16(p), mem.read_u32(p + 4) or 0
  local out = { loaded = v ~= 0, voices = n, playing = 0 }
  if v == 0 then return out end
  for i = 0, n - 1 do
    local w = mem.read_u32(v + 8 * i) or 0
    local dsb = w ~= 0 and mem.read_u32(w) or 0
    if dsb ~= 0 then
      local vt = ffi.cast('void ***', dsb)[0]
      local gs = ffi.cast('long (__stdcall *)(void *, uint32_t *)', vt[9])
      local stt = ffi.new('uint32_t[1]')
      if gs(ffi.cast('void *', dsb), stt) == 0 and bit.band(stt[0], 1) ~= 0 then out.playing = out.playing + 1 end
    end
  end
  return out
end
function sound.wav(samples, rate)
  rate = rate or 22050
  local n = #samples
  local out = {}
  for i = 1, n do
    local v = math.floor(math.max(-1, math.min(1, samples[i])) * 32767 + 0.5)
    if v < 0 then v = v + 65536 end
    out[i] = string.char(v % 256, math.floor(v / 256))
  end
  local function le32(v) return string.char(v % 256, math.floor(v / 256) % 256, math.floor(v / 65536) % 256, math.floor(v / 16777216) % 256) end
  local function le16(v) return string.char(v % 256, math.floor(v / 256) % 256) end
  local data = table.concat(out)
  return 'RIFF' .. le32(36 + #data) .. 'WAVE' .. 'fmt ' .. le32(16) .. le16(1) .. le16(1) .. le32(rate) .. le32(rate * 2)
      .. le16(2) .. le16(16) .. 'data' .. le32(#data) .. data
end

game.script, game.sound = game.script or {}, game.sound or {}
for k, v in pairs(script) do game.script[k] = v end
for k, v in pairs(sound) do game.sound[k] = v end
return install
