# Mod format

A mod is a folder (Lua mod) or a single DLL (native mod) under the game's `mods\`
directory. The loader scans `mods\` once at startup and loads both kinds into the
same process; they share one hook/UI registry so they interleave safely.

## Lua mod (default)

```
mods\<name>\
  init.lua      # REQUIRED — the entry point the loader runs
  setup.lua     # optional — a ONE-TIME setup step, run only after the user agrees (see "First-time setup")
  mod.toml      # recommended (REQUIRED to publish to a registry)
  ...           # any assets / extra .lua the mod requires
```

`init.lua` runs once at startup with a per-mod **`mod`** table already in scope — no
`require` needed. It runs on the **engine (main) thread** (deferred to the first
per-frame safepoint by the executor), so a mod may touch the engine directly from init.
Each mod runs in its **own environment** (its globals don't collide with other mods')
inside a **protected call**: a Lua error is caught, the mod is flagged, and neither the
game nor the other mods are affected.

> **Threading model:** all mod code — `init.lua`, `on_frame`, and `mod.main` jobs — runs
> on the one engine thread, so engine reads/writes/calls are safe by construction. From
> another thread (e.g. a future UI thread) do reads via guarded `mod.mem` and push any
> engine work through `mod.main`. (Without a game window the executor stays disarmed and
> mods run on the loader thread instead — degraded, no `on_frame`.)

### `mod.*` API — available now (P0–P5)

| field | type | meaning |
|---|---|---|
| `mod.name` | string | this mod's folder name |
| `mod.dir` | string | this mod's absolute folder (trailing `\`) |
| `mod.loader` / `mod.loader_version` | string | `"SotES Mod Loader"` / host version |
| `mod.log(...)` | fn | print-style line → `oss_modloader.log`, attributed to this mod |
| `mod.mem.*` | table | **guarded** memory service (below) — never faults on a bad address |
| `mod.game.*` | table | **togglable** game-knowledge bindings (below) — the RE'd structs/pointers |
| `mod.on_frame(fn)` | fn | run `fn()` every frame on the engine thread |
| `mod.main(fn)` | fn | run `fn()` once at the next safepoint on the engine thread (fire-and-forget) |
| `mod.hook.*` | table | chained multi-mod hook registry (below) — observe any function |
| `mod.ui.*` | table | **ImGui UI** (below) — add panels/windows to the loader window + in-game overlay |

**`mod.mem`** (all reads/writes VirtualQuery-guarded — a bad address returns `nil`/`false`, never a crash):

```
mod.mem.read_u8/u16/u32/i8/i16/i32/f32/f64/ptr(addr)   -> number | nil
mod.mem.write_u8/.../f64/ptr(addr, v)                  -> bool (ok)
mod.mem.read_bytes(addr, n) / read_cstr(addr [,max])   -> string | nil
mod.mem.write_bytes(addr, str)                         -> bool
mod.mem.readable(addr [,n]) / writable(addr [,n])      -> bool
mod.mem.patch(addr, str)                               -> bool   (overwrite CODE too: RWX + memcpy + icache flush)
mod.mem.scan("48 8B ?? C3")                            -> addr | nil   (AOB over the exe image; ?? = wildcard)
mod.mem.module(name) -> base | nil   mod.mem.base() -> exe base   mod.mem.reloc(va) -> va + ASLR delta
```

**`mod.res`** (api 1.7) — the **resource overlay**: add or replace the game's PE resources in memory.  The loader
patches the EXE's imports of `FindResourceA` / `LoadResource` / `SizeofResource` / `LockResource` (on the first
`set` only), so the game's own loaders — maps (`load_room` → `FindResourceA(map_module, id, "DATA")`), sprite
sheets (the bitmap session, `"DATA"` in sotesd.dll) — parse what a mod serves as theirs.  Nothing touches disk;
build the bytes from the user's own data (`read`) — never ship game bytes in a mod.

```
mod.res.read(type, id[, hmod])        -> string | nil  the REAL resource (bypasses the overlay); hmod default = the exe
mod.res.set(type, id, data[, hmod])   -> bool          add / replace (hmod nil = any module); data copied
mod.res.clear(type, id[, hmod])       -> bool
mod.res.list()                        -> {{type, id, hmod, size, hits}, ...}   hits = times the game found it
mod.res.hits(type, id[, hmod])        -> n
-- type / id: a number (MAKEINTRESOURCE) or a name ("DATA").  Map data module: mod.mem.read_u32(mod.sym.va("map_module"))
-- (0 = the exe).  Example (../sotes-mods/mods/testroom): parse DATA 1025 (Arche's house), add a door, set it back.
```

`patch` is the **arbitrary-patching** escape hatch for RE that isn't yet a semantic loader op; once
a technique is RE'd + generalized it graduates to a `mod.game.*` op (below) and mods stop patching by
hand.

**`mod.game`** — the loader centralizes the RE'd engine structs + pointer chains as
**togglable bindings** (see DESIGN.md “Game bindings”), each surfaced as `mod.game.<id>`:

```
mod.game.list()                 -> { {id=, desc=, enabled=}, ... }
mod.game.enable(id)/disable(id)/enabled(id)     -- live toggle (a stability valve)
-- SotES bindings (grow as we RE more) — DATA accessors + semantic OPS:
mod.game.roster.members()       -> { {code,id,name,actor,slot,stat_block, level,char_level,combat_level,
                                       combat_level_max,adventurer_level, x,y, hp,hp_max,mp,mp_max,
                                       attack(_base),defense(_base),spirit(_base),resist(_base), exp,exp_max,
                                       active}, ... }       -- every active room party slot, in slot order
                                   -- level = char_level = combat + adventurer (the HUD / save-list "Lv");
                                   -- combat_level/_max = "Combat Level M/N" (N = the HUD stars).
                                   -- id = ASCII by code (Arche/Sana/Stella/Chiffon); name = the game's text
                                   -- (Shift-JIS on ense-jp).  active = the controlled member (room->leader).
mod.game.roster.world()         -> {world, room, view, nflags} raw pointers (nil outside a scene)
mod.game.roster.layout          -- this edition's struct offsets (core/sotes_layout.h) for mods doing raw reads
mod.game.coordinates.get([code])/player()/target()   -> {x,y,actor,code}   (centi-px; target = controlled)
mod.game.mouse.get()            -> {screen_x, screen_y, over, world_x, world_y}
mod.game.camera.get()           -> {left, top, width, height, map_w, map_h, view}   (world centi-px)     [api 1.1]
mod.game.map.get()              -> {area, map, record, areas, nareas}   current area id (room +0x4024) + its map
mod.game.flags.story.get(id)    -> v, v2 | nil      the persistent event flags the scripts use (saved)
mod.game.flags.story.list()     -> {{id, v, v2}, ...}
mod.game.flags.story.set(id, v[, v2]) -> true      engine semantics: find-or-add; v = v2 = 0 CLEARS the id
mod.game.flags.area.get(id) / .list() -> state / {{id, state}, ...}   the room's area state (1 = done)
mod.game.flags.area.set(id, state)    -> true
mod.game.items.list()           -> {{index, id, name, count, owner, stored}, ...}  (owner = wearer's party-slot
                                --   id, 0 = unworn; name in the edition's own encoding — Shift-JIS on ense-jp)
mod.game.items.gold()           -> cp, cap          (100 CP = 1 GP; the HUD counter shows CP)
mod.game.items.set_gold(cp)     -> cp (clamped to [0, cap])
                                -- every WRITE runs only on the engine thread (on_frame / main / dev-server lua);
                                --   elsewhere it returns nil, "...refused..."
-- sandbox ops (api 1.2) — engine-thread only; each is the engine's own mechanism, not a re-implementation:
mod.game.map.rooms()            -> {{key, map, loadable, exits = {{gate, to}, ...}}, ...}  the area table = every room
                                --   the scene loop can load; loadable = its map data exists (retail's own lookup)
mod.game.map.room(key)          -> one of those | nil
mod.game.map.warp(key[, gate[, force]]) -> true | nil, why   warp the party (gate 0 / unknown = the room's first gate).
                                --   The warp item's own request: retail's rules apply (no warp in towns, events,
                                --   rooms that forbid it → "cannot teleport here", request dropped), its dissolve
                                --   plays, the scene loop loads the room.  force = skip the rules + the dissolve
                                --   (the countdown armed at 1).  Refused: unknown room, a room without map data
                                --   (retail would exit "The map data is not found."), an event running, a warp running.
mod.game.map.warp_status()      -> 'idle' | 'requested' | 'warping', key
mod.game.map.add_room{key=, like=, map=[, town=]} -> true | nil, why   (api 1.7) a NEW room: a copy of room `like`'s record
                                --   (kind, tileset, town flag, BGM, name) with id `key` and map DATA id `map` (u16;
                                --   serve its bytes with mod.res.set("DATA", map, ...)), no exits.  The area table is
                                --   rebuilt each session, so the loader re-applies additions on every area_find (a
                                --   save made in the room loads while the mod is installed).  Keep `like` in the
                                --   same area (key / 1000) so area state carries.  town = false / true (api 1.8)
                                --   overrides the town kind (record +0x114 = 2): in a town hits do NO damage either
                                --   way (barrels still break) — a house-looking arena needs town = false.
mod.game.map.set_exit(room, zone, to[, gate]) -> true | nil, why   (api 1.7) room's exit for `zone` → room `to`,
                                --   arriving at its zone actor `gate` (default 1).  The zone needs a zone device in
                                --   the room's MAP (70101 walk-in, 70102 press-Up door; A[0] = zone) — the arrival
                                --   room needs one with id `gate`, or retail exits "The gate is not found."
mod.game.map.event()            -> bool    an event script owns the game (cutscene / conversation)
mod.game.items.give(id[, count]) -> index | nil, why   the engine's room_item_add (its capacity rules; count <= 0 =
                                --   the item's default); no message
mod.game.items.catalog()        -> {{id, name}, ...}   every item definition
mod.game.roster.set(code|nil, {hp=, mp=}) -> true      current hp / mp (nil code = the controlled member)
mod.game.coordinates.set(code|nil, x, y) -> true       teleport: the member's body box (x sticks, y settles)
mod.game.dialogue.skip(on) -> on                         [api 1.3]   skip dialogue retail's own way: the message
                                --   window's skip (held input axis 6 = DIK_TAB) is held through mod.input's DirectInput
                                --   layer, ONLY while map.event() (free-roam TAB untouched).  Choice lists ignore it (enold
                                --   grid_nav has no key-8 case).  Engine-thread state; persists until skip(false).
mod.game.dialogue.get()         -> on, holding_now
mod.game.actors.list()          -> {{slot, code, origin, party, leader, spawned, x, y, w, h, facing}, ...}   [api 1.4]
                                --   the room's live characters (the 32-slot pool); origin 1 = placed by the map,
                                --   2 = party, 4 = spawned (skill / this API); x, y = body top-left (centi-px);
                                --   facing 1 right / 3 left
mod.game.actors.codes()         -> {code, ...}   every code this edition's char_init takes (its own dispatch,
                                --   tools/charcodes.py: 122 enold / 134 SE)
mod.game.actors.catalog()       -> {{code, name, en}, ...}   every code + its stat-table names (name = the edition's
                                --   encoding, Shift-JIS on ense-jp; en = English); nil for codes without a row
                                --   (most townsfolk)
mod.game.actors.valid(code)     -> bool
mod.game.actors.spawn(code[, {x=, y=} | {dx=, dy=}, facing=]) -> slot | nil, why   retail's summon_spawn (the
                                --   summoning skills' path: next free pool slot + char_init, AI live).  x, y = the
                                --   feet (centre x, bottom y); else the leader's feet + dx (ahead; default 6000) + dy.
                                --   Refused: a code char_init lacks (retail would exit "Unknown Object Type"),
                                --   pool full.  Any code draws — sprite banks need no preparation.
                                --   handle = n (api 1.8): talkable — see script.on_talk.  A spawned monster is a real
                                --   enemy outside towns (it fights, takes damage, drops coins).
mod.game.actors.remove(slot)    -> true | nil, why   retail's char_unload (a script's removal); party refused
mod.game.actors.set_handle(slot, h) -> true | nil, why   (api 1.8) the actor's script handle (entity_set_handle:
                                --   +0x1d8 + the world's handle list the talk probe walks).  list() rows carry
                                --   `handle` and `ptr` (the character) since 1.8.
mod.game.script                 -- [api 1.8] EVENT SCRIPTS IN LUA on retail's own scene machinery (core/lua/script.lua;
                                --   hooks arm on first use).  A retail conversation = a C function the script engine
                                --   picks by (room, mode, the talked-to actor's handle) that fills the event runner's
                                --   command block and runs frames (script_wait); a Lua script does the same over FFI.
  .on_talk(handle, fn[, {room=, name=}])   talk (V) to a character / examine (X, sword sheathed) a device whose
                                --   handle == handle (a map record's A[1], actors.spawn{handle=}, set_handle).  Runs
                                --   retail's talk beat (camera, party, weapons away, face each other) then fn(s).
  .off_talk(handle)
  .run(fn[, {name=}])           a script with no actor at the next free-roam frame (gimmick_pass calls event_run,
                                --   then retail's post-talk input reset) — e.g. a reward cutscene
  .after(fn)                    fn() at the next free-roam frame: warps (one armed inside an event is lost) etc.
  .on_tick(fn) -> h / .off_tick(h)   [api 1.10] fn() on every free-roam GAME TICK (60 a second whatever the display's
                                --   frame rate; none during events, menus, room changes).  For anything that moves
                                --   or counts: mod.on_frame runs per presented frame (60..200+ a second by machine).
  .running() / .pending() / .errors()
  -- fn(s): s.actor, s.handle, s.room, s.name (the box title) and, all retail's own:
  s:say(text[, {name=, actor=('leader'|ptr), own_name=, voice=, face=}])   a message box (\n = retail's %n)
                                --   api 1.13: no actor (a script.run scene with none given) = the party's leader —
                                --   retail's box needs one: before, such a say showed nothing and did not wait
  s:narrate(text)               a narration box (kind 3, no speaker — what examine messages use)
                                --   api 1.9: long text is PAGED: the box holds 3 lines of 44 bytes (36 with a portrait;
                                --   narration 2 × 60) and retail has no paging — overflow typed past the bottom, only
                                --   the last lines stayed.  The text is wrapped with retail's word rules (text_wrap
                                --   0x40e5e0) and each page of lines becomes its own message.  script.wrap_pages(t, w, n)
  s:choose({...}[, {default=, no_cancel=, x=, y=, w=}]) -> i | nil   the runner's choice list, ≤ 16 rows
  s:yes_no([no_cancel]) -> bool  retail's localized Yes / No
  s:shop({item ids}[, {buy=, sell=}])   the shop (Buy / Sell, equip preview, prices) on any stock
  s:inn{price=, offer=('%d'), decline=, broke=, morning=, bye=}   the whole inn: price window, payment, rest, heal
  s:rest() / s:heal()           the inn rest alone (jingle, black, heal, back) / the party to full
  s:storage()                   the storage box
  s:book(title, {pages})        the book reader (≤ 2047 chars a page)
  s:give(what, n[, quiet]) -> got   the story gift ("You obtained ..."; full bag: retail's refusal); what = an item
                                --   id or 1 / 2 / 3 = n copper / silver / gold coins
  s:fade_out([ms]) / s:fade_in([ms]) / s:wait([ms]) / s:pause(n) / s:camera(actor[, speed]) / s:sound(id) / s:leader()
                                --   s:wait(ms) = the tick-counted pause (60 ticks a second: real time); s:pause(n)
                                --   = n ticks.  (Before api 1.10 s:wait rode the effect wait and always lasted ~80
                                --   frames, whatever ms said.)
  -- A wait that returns 6 (quit / game over) aborts the script like retail; a Lua error ends the event (logged).
mod.game.sound                  -- [api 1.8] sound effects
  .play(id[, vol[, freq]])      a stock pool slot (0..464 enold / 482 SE) or a .add handle; vol in 1/100 dB
  .add(wav[, {voices=}]) -> h   RIFF WAVE bytes → a new sound: served through the resource overlay, played from the
                                --   loader's own pool (swapped into a slot only inside the engine's sfx_play): no
                                --   stock sound changes
  .wav(samples[, rate]) -> bytes   16-bit mono PCM from -1..1 floats (synthesize in Lua: nothing shipped)
  .status(id) -> {loaded, voices, playing}   (playing = DSBSTATUS_PLAYING voices; for tests)
mod.game.sprites                -- [api 1.9] CUSTOM SPRITES on retail's own sprite banks (core/lua/sprites.lua; no hooks)
                                --   A sheet = a DATA resource (header + 256-colour palette + 8bpp bottom-up pixels,
                                --   index 0 = transparent) cut into a grid of cells; a bank = one sheet (909 retail
                                --   banks on enold, 925 SE EN, 929 SE JP — and as many new ones as you like, below);
                                --   a character draws its track 0 (char +0x48 = the bank) through its body's clip
                                --   (frame = base + delta[f]); facing left adds the bank's mirror offset.
  .decode(blob) -> img          an 8bpp sheet → img {w, h, pal[0..255] = {r, g, b}, px}  (img:get / set / fill /
                                --   blit(src, sx, sy, w, h, dx, dy[, map]) / clone)
  .encode(img, template) -> blob   back to sheet bytes (template = any stock sheet: its header is reused)
  .bmp(bytes) -> img            an 8-bit indexed, uncompressed BMP (what a mod ships: draw in any editor)
  .image(w, h[, pal]) -> img    a blank image
  .bank(i) -> {index, res, module, cell_w, cell_h, mirror, frames, decoded, ours}
  .sheet(i) -> img, blob        bank i's sheet, read from the user's own game data
  .new_bank{like = i, image = img | sheet = blob[, cell_w=, cell_h=, mirror=, retail=]} -> index | nil, why
                                --   retail's slot_clone (0x4179b0 / SE 0x411b10) makes the bank a copy of `like`'s
                                --   descriptor (cell grid, colour key, mirror offset); its DATA id points at the new
                                --   bytes in the resource overlay; the per-cell trim records are recomputed.  Same
                                --   layout as `like` (at most its cell count).
                                --   [api 1.10] The bank is a NEW one past the game's own (index ≥ the retail count):
                                --   on first use the loader relocates the engine's two bank tables into 8192-entry
                                --   ones of its own (every instruction indexing them is rebased — 97 / 113 / 115
                                --   sites per edition, each verified first; tools/banksites.py), so banks are no
                                --   longer a scarce resource (7000+).  retail = true takes one of the game's own
                                --   unused banks instead (2–4 per edition) — also the fallback on an unknown build.
  .free_banks() -> n
  .capacity() -> {retail, total, used, free, extended[, error]}   [api 1.10]
  .clip{frames = {..}[, base=, dur=14, loop=true, loop_to=0, dx={..}, dy={..}]} -> clip   an animation (retail's
                                --   anim_clip: up to 32 frames, dur = ticks per frame, per-frame offsets)
  .set_look(actor, bank)        actor = an actors.list() row or a char pointer
  .set_clips(actor, {stand=, walk=, run=, air=})   the state clips (stand +0x15914 / SE +0x15934 …)
  .get_clips(actor) -> {stand, walk, run, air0, air1}   (pointers)
  .play(actor, clip)            on the body now (the state machine replaces it at the next state change)
mod.game.actors.name(actor) / .set_name(actor, s)   [api 1.9] the display name (char +0x274, 19 chars: the target
                                --   window's label)
mod.game.actors.hp(actor) -> cur, max / .set_hp(actor, cur[, max])   [api 1.9] the live stat block
mod.game.actors.define{code=, base=[, name=, hp=, bank=, clips=, on_spawn=, on_tick=, on_death=]} -> true | nil, why
                                --   [api 1.10] a NEW CHARACTER TYPE under its own code.  The game's codes are cases
                                --   of a compiled switch (nothing to add a row to), so a type = a code char_init
                                --   never sees: a hook swaps it for `base` (a retail code: its body, AI, stats,
                                --   sounds) on the way in and puts the type's look (bank = an index or
                                --   function(char_ptr) -> index), clips ({stand=, walk=, run=, air=}), name and HP
                                --   on the new character on the way out.  Works wherever a code does: MAP RECORDS
                                --   and actors.spawn.  code: 50001..59999 (the range the map loader hands to
                                --   char_init), not one of the game's.  The character carries `base` as its code
                                --   (everything retail dispatches on); actors.list() rows add type = your code.
                                --   on_spawn(a) / on_tick(a): free-roam game ticks (60 a second, not during events),
                                --   a = its actors.list() row; on_death(a): once, when its HP reaches 0 or it
                                --   disappears while its room is still loaded (a = the last row seen).
mod.game.actors.defined(code) -> the definition | nil
-- PLACING A CHARACTER IN A MAP: a record's x / y = the top-left of its SPRITE CELL; char_init hangs the body in the
-- cell (x centred, feet on the cell's bottom row).  So y = floor - the sheet's cell height, which differs per
-- character (sprites.bank(track).cell_h: townsfolk 96, guard / witch 128, children 80) — one y for everyone leaves the
-- tall ones a tile deep in the floor.
mod.game.attract.set(on) / .get() -- false keeps the title up (idle → no demo); true = retail
mod.game.save.load([slot])      -> true | nil, err   -- load save `slot` from the title (nil / <0 = the list's
                                --   default row = last used).  Typed hooks on the title driver (any edition):
                                --   the title menu answers Continue unseen, the save list's cursor goes to the
                                --   slot's row and confirms, a post-game save's Quest Selection takes Main Quest;
                                --   the game itself validates + loads.  Ask at mod init (first frame) — or any
                                --   time with attract off (served at the title's next idle tick).
mod.game.save.status()          -> 'idle'|'pending'|'list'|'loaded'|'failed', why   (empty slot → 'failed')
mod.game.save.cancel()          -- drop a still-pending request
mod.game.voice.status()         -> {state, why, wait_ms, registrar, je, bank, rows, shadowed, deluxe0, deluxe0_ok,
                                --   deluxe, deluxe_ok, deluxe_off}   [api 1.5]   the built-in boot-time monster-SFX fix
                                --   (SE only; oss_loader.cfg voice, default on): state patched / unpatched (voice=0:
                                --   the shipped registrar) / waiting / failed / off; the counts = the sound-def table vs
                                --   the registered slots (deluxe0_ok == deluxe0 ⇔ every no-variant row, i.e. monster
                                --   SFX, registered)
```

The **ops** (`attract`, `save`) are the model for core→mod work: known RE (here the menu-drive) is
exposed as a semantic loader op, so a mod ORCHESTRATES it (`mod.game.save.load(slot)`) instead of
reimplementing it.  The `autoload` mod is exactly this — read its `slot` config, call `save.load`.

Every SotES binding resolves through the **edition layer** (enold / ense-en / ense-jp): VAs by symbol
(`mod.sym`), struct offsets from `core/sotes_layout.h`.  An unknown build → no layout → nil.
**v2 (2026-11-24):** `roster` / `coordinates` / `mouse` are rebuilt on it (world → room → party slots, no
heap scan; `level` is now the HUD level, was combat-max).  `attract` / `save` / `input.press` are still the
v1 address set and REFUSE (log + false) until rebuilt.

**`mod.hook`** — one trampoline per target VA dispatches an ordered chain, so multiple mods
hook the same function without clobbering each other. Two tiers (a stability gradient):

**Tier 1 — `mod.hook.entry` (observe, ultra-stable, DEFAULT).** No signature, can't modify
args or block → cannot destabilize the callee.

```
local h = mod.hook.entry(addr, function(ctx) ... end)   -- addr ABSOLUTE (mod.mem.reloc(va))
mod.hook.remove(h)          mod.hook.count()
```

The callback receives `ctx = {ecx, edx, eax, esp, ret, va}` (read stack args via
`mod.mem.read_u32(ctx.esp + 4 + 4*n)`), runs on the engine thread inside a `pcall` — a
faulting cb is auto-disabled.

**Tier 2 — `mod.hook.typed` (modify args / block / modify return; opt-in, powerful).** The
mod declares the target's real C signature; a **LuaJIT FFI closure** marshals it:

```
local h = mod.hook.typed(addr, "int __thiscall(void*, int)", {
  pre  = function(ctx) ... end,   -- runs before the original
  post = function(ctx) ... end,   -- runs after
})
mod.hook.remove(h)          mod.hook.typed_count()
```

The callback receives `ctx = { args = {...}, n, va, blocked, ret }` where `ctx.args` are the
**typed** call arguments (arg 1 = `this` for `__thiscall`). A **pre** hook may mutate
`ctx.args[i]` (the modified args reach the original), or set `ctx.blocked = true` (+ optional
`ctx.ret`) to skip the original entirely. A **post** hook may rewrite `ctx.ret` (becomes the
returned value). Multiple typed hooks on one VA chain (pre in order, post in reverse);
first-to-block wins. Cbs run on the engine thread inside a `pcall` (a faulting one is disabled).

- **Signature form:** `"<ret> [__conv](<args>)"`, e.g. `"void(int)"`, `"int __stdcall(int, int)"`,
  `"int __thiscall(void*, int)"` (conv defaults to `__cdecl`). It **must match the target's
  real convention + arg count** — a wrong count corrupts the caller's stack (that's why Tier-1
  is the default; only reach for typed when you know the signature).
- **One tier per VA:** a VA is Tier-1 **xor** Tier-2 (one MinHook per target). Mixing tiers on
  one VA is refused + logged; within a tier, mods chain freely.
- **Off-thread safety:** a tiny C gate runs the closure only on the engine thread; a call to the
  target from another thread runs the original untouched (never reenters Lua).

**`mod.ui`** — the loader hosts a **companion window** (its own ImGui/DX11 window, toggled with
**F8**). A mod registers **panels** (collapsing sections in the loader's host window) and **windows**
(standalone floating windows); the loader renders them.

```
local h = mod.ui.panel(name, draw)    -- a collapsing section in the loader's host window
local h = mod.ui.window(name, draw)   -- a standalone floating ImGui window
```

`draw` is a function the loader runs to emit widgets:

```
mod.ui.text(s)   .text_disabled(s)   .text_wrapped(s)   .text_colored(r,g,b,a, s)   .bullet(s)
mod.ui.separator()    .spacing()    .same_line()
mod.ui.button(label [,w,h]) -> pressed          mod.ui.small_button(label) -> pressed
mod.ui.checkbox(label, value)           -> new_value, changed
mod.ui.slider_int(label, v, min, max)   -> new_v, changed
mod.ui.slider_float(label, v, min, max) -> new_v, changed
mod.ui.progress(frac [,text])
mod.ui.combo_search(label, {item, ...}, index) -> new_index, changed   [api 1.6]   a dropdown with a search box:
                                -- typing filters the items (case-insensitive; every word must match, as a substring or
                                -- scattered letters; best matches first), Enter takes the top hit.  1-based (0 = none).
                                -- While the box has focus the game gets no real keys (DirectInput is masked).
mod.ui.synth{x=, y= | click=true | text="..." | key="enter"|"escape"|"backspace"|"down"|"up" | release=true}
                                -- -> queued   synthetic UI input for agents / tests: fed straight into ImGui one action
                                --   per frame (no OS mouse / keyboard); the mouse stays at the synth position until
                                --   release.  tools/win/ui.ps1 drives it over the dev server.
```

Widgets return their *new* state (plus a `changed` flag where useful) — the immediate-mode idiom:
keep the value in a Lua variable and reassign it each frame, e.g. `open = mod.ui.checkbox("Open", open)`.

- **Threading model (how it stays fast):** the loader runs `draw` on the **engine thread**, throttled
  (~30 Hz), and *records* its widgets into a plain-data snapshot; a separate UI thread renders the
  latest snapshot. The two are connected by a **lock-free** pipeline (a triple buffer one way, atomic
  input slots the other) that drops stale data — so the UI never stalls the game and the game never
  stalls the UI. In practice: a `draw` callback runs where `on_frame` does, so it may **READ** game
  state directly (`mod.mem` / `mod.game`), but heavy work there costs the game frame — push it through
  `mod.main`. Interactions (a click, a new slider value) reach your callback on the **next** build
  (~1 cycle later); a UI-thread cache keeps sliders smooth in the meantime.
- **Isolation:** each callback runs inside a `pcall`; a fault disables just that panel/window (+ logs
  it), never the game or other mods. Widget calls made *outside* a build (e.g. from init) are inert
  no-ops, so they can't crash.
- **Config:** the UI is on by default once the executor arms; `ui=0` in `oss_loader.cfg` disables it,
  `ui_key` (a VK code) overrides the F10 toggle, and `ui_hz` sets the build rate (default 30).
- **In-game overlay:** in borderless takeover (`ddraw_takeover=1`) the same panels draw as an overlay
  ON TOP of the game frame (F10 toggles), from the same snapshot — no mod change either way.  The
  overlay is mouse-only; the keyboard stays with the game.  Otherwise the panels show in a companion
  window.

## First-time setup (api 1.11)

A mod that needs to prepare data once — typically: find another game the user owns, read its files, convert them —
ships **`setup.lua`** beside `init.lua` and describes the step in `mod.toml`:

```toml
[setup]
version = "1"        # bump to make the step run again (new conversion, new files)
summary = "Finds your Steam copy of X, reads ..., writes ... to mod_data\\<mod>. Changes nothing else."
```

What the loader does, before the game opens its first window (the game's main thread is held meanwhile, so nothing
fights a fullscreen game and every mod is initialized before the first frame):

1. **Asks the user**, per mod, in a dialog that names the mod, its author, its folder and the mod's own `summary`,
   with a **security warning**: mods are programs with the same access to the computer as the game, and the loader
   cannot check what one does.  *No* (the default button) = the game starts **without that mod** and asks again next
   time.  Nothing of the mod has run at that point.
2. On *Yes* runs `setup.lua` (loader thread; the full `mod` table, but the game is not up: use `mod.fs`,
   `mod.steam`, `mod.image`, `mod.config`, `mod.log`) behind a progress window: `mod.setup.progress(frac[, text])`.
   Write results to **`mod.data_dir`** (`<game>\mod_data\<mod>\`, outside the mod's folder so updates keep it).
3. Success (no error, not `return false`) is recorded in `mod_data\<mod>\setup.state`; the mod's `init.lua` then loads
   as usual.  An error shows the message, skips the mod this session and offers the step again next start.

`mod.setup` = `{ done, needed, running, version, progress }`; `init.lua` should start with
`if not mod.setup.done then return end` when it depends on the data.  Automation / tests: `oss_loader.cfg`
`setup_trust = <mod>,<mod>` (or `*`) pre-approves without a dialog — never ship a cfg with it.

```
mod.fs      exists / is_dir / size / read(p[, off, len]) / write(p, data) / list(dir) / mkdir / remove / join, .game_dir
mod.steam   root() / libraries() / app(appid[, {dir = "Folder", marker = "file.exe"}]) -> {dir, library, name} / hint(path)
            (registry, the library this game is in, Proton's env, the usual Linux folders through wine's Z:)
mod.image   true-colour images: new(w, h) / decode(bytes) (BMP 8 / 24 / 32, TGA) / quantize(img[, {colors=}]) ->
            an indexed image for sprites.new_bank / pack_indexed / unpack_indexed (a cache format);
            img:get / set / crop / blit / flip_h / scale / key(r, g, b[, tol]) / bounds / bmp
```
Example: `../sotes-mods/mods/recettear` (Recettear's archive → sprites, portraits, music).

## Shared content modules (api 1.11)

```
mod.game.maps                   -- room maps as objects, SHARED between mods (core/lua/maps.lua)
  .read(id) -> Map              the game's real map;  .parse(bytes) / m:serialize()
  .edit(id, fn[, who])          change a map the game ships: fn(m).  Edits COMPOSE — every mod's edit of one map is
                                applied in order to one copy of the real data (two mods can each add a door to the same
                                room).  Served for the map module only (ids collide between modules: DATA 1026 is a map
                                in the exe and a sprite sheet in sotesd.dll).
  .define(id, map | bytes | fn[, who])   a NEW map; refused when the game or another mod has the id
  .set(id, map | bytes | fn, who)        [api 1.13] new bytes for a map `who` defined — a generated room re-served before
                                each visit (the game reads a room's map at every load).  m:serialize() writes the
                                header's cols, rows, planes and object count from the Map (before 1.13 only cols and the
                                count: a map with more rows than its source was misread — the objects came out of the
                                cells, a crash in the object loader 0x5b4130)
  .bytes(id) / .owner(id) / .clone(obj)
  Map: cols, rows, planes, cells, objs; m:cell / set_cell / find / find_all / filter / next_id / widen / add(obj[, first])
       m:add_door{like = zone, zone =, x =}     m:add_character{like = code | obj, code =, x =, floor =, cell_h =,
                                                                 handle =, facing =}
mod.game.sprites.new_bank       -- a sheet may have MORE cells than `like` (its own trim records); image = a mod.image
                                   (RGBA) + a 24-bit `like` (a portrait bank, 160 x 176 cells) = a 24-bit bank
mod.game.sprites.encode24(img, template)
s:say(text, {portrait = bank, frame = n})   the speaker's bust from any bank of 160 x 176 cells (the runner's own
                                   portrait fields +0x84 / +0x88); the text wraps at 36 columns as with retail's
mod.game.sound.room_music(room, file | nil[, {loop =, volume =, normalize =}])   a PCM .wav as that room's music, at the
                                   game's own music volume setting (followed live) and levelled to the game's tracks
                                   (-14.7 dB mean); volume = a trim on top, 1/100 dB.  The loader plays
                                   it (DirectSound) and answers the engine's bgm_pick with 0 there, so the game fades
                                   its own BGM out and brings the next room's back by itself
mod.game.sound.music(file | nil[, opts]) / .music_status()
mod.game.items.catalog()        -- rows now carry price (CP, def +0x60; 0 = not for sale) and icon (def +0x54)
```

## New items (api 1.12)

`core/lua/items.lua` (embedded) adds to `mod.game.items`.  Items of a mod's own, **past the retail table** — no
retail row is replaced and no free slot is used up: the static item table (enold 215 rows of 736 B, SE 306 of 940 B,
0-id-terminated, scanned inline everywhere) is relocated into a 4095-row copy on the first `define`, every one of its
77 / 106 / 106 references rebased (`tools/itemsites.py`; each verified before anything is written).  A row is a copy
of a game row (`like`) with fields replaced, so the item behaves as the game's own of that kind: equip slot, class
fit, list kind, use effect, carry caps, shop / sell, saves.

```
mod.game.items.define{id=, like=[, name=, desc=, price=, icon=, attack=, defense=, spirit=, resist=, max_hp=,
                      max_mp=, use = {effect=, a=, b=, c=}, set = {[offset] = int32}]} -> id | nil, why
    id     the number saves store: never a retail id, never 910000..910511 (SE's generated gear), stable across
           versions of the mod.  A save holding it loaded WITHOUT the mod: the game drops those records (no crash).
    like   a retail (or defined) id: the row copied
    icon   a cell number, or a 32 x 32 image (mod.image RGBA, or an indexed sprites image) — see items.icon
    use    a consumable's effect (+0x58) and numbers (+0x68 a / +0x6c b / +0x70 c).  SE: 8 = HP a + b*rand,
           17 = MP, 18 = a permanent stat (a: 1 attack 2 defense 3 spirit 4 resist 5 max HP 6 max MP; b + c*rand)
mod.game.items.def(id)        -> {id, ptr, name, desc, price, icon, kind, slot, fit, attack.., effect, a, b, c, ours}
mod.game.items.defined(id) / .capacity() -> {retail, defined, total, extended, error}
mod.game.items.icon(img)      -> cell   appended to the icon sheet (below)
mod.game.items.icons()        -> {first, added, built, bank, banks, res}
mod.game.items.equip(index, member)    room_item_equip: the record on the member (roster entry; nil = the first
                                       active), its equip slot freed, stats recomputed (class fit = the caller's)
mod.game.items.use(index, member) -> used, message    SE only: retail's field use of the record (heal / stat / ...)
mod.game.items.take(id[, n]) -> taken | nil, why     [api 1.13] n units (default 1) off the bag the story scripts' way:
                                       retail's item_take (enold 0x406500 / SE 0x4039e0 / JP 0x403a30) removes the
                                       unstored record of `id` with the smallest count, moves the rest down, recomputes
                                       the party's stats; a record holding more units than are left to take only has its
                                       count lowered.  The box is never touched.  -> the units taken.
mod.game.items.drop(id, where) -> true | nil, why     an item lying on the floor (pickup_spawn, as a foe drops one);
                                       where = an actors entry, a roster member, a char pointer or {x=, y=}
mod.game.actors.define{..., drops = {{id=, chance= per mille}, ...}, coins = n}
                                       the type's drops: the first through retail's own drop roll (foe_drops), the
                                       rest rolled after it at the same spot; coins replaces its money
mod.game.items.catalog()               also lists the defined items (ours = true)
```

**Icons.** The item icons are one 8-bit sheet (256 x 1024, 8 cells a row) that two banks are cut from — the menus'
(50 enold / 56 SE EN / 60 SE JP) and the world's (437 / 446 / 450: the pickup sparkle shows the picked item's icon).
New icons go in rows appended below the retail cells (cell 256 on), take the 128 palette entries no retail icon uses
(median cut over every mod's icons; each pixel the nearest of the whole palette), and the sheet is rebuilt at most
once a frame under a new DATA id of the resource overlay.  Both banks switch to it; a decoded one is freed with
retail's `sprite_set_free` and its per-cell trim records (+0x34, made once at the first decode and reused) cleared so
the slicer makes a set for the taller sheet.

### `mod.*` API — roadmap (later phases, see DESIGN.md)

```
mod.call(va, sig, ...)                                        -- FFI call (via mod.main)
mod.overlay.panel(...) / mod.overlay.draw(fn)                 -- richer opt-in DDraw7 overlay layers
mod.on.{scene_change, settle, unload, ...}                   -- lifecycle events
```

## Native mod

```
mods\<name>.dll     # a plain DLL that exports OssModInit
```

Loaded with `LOAD_WITH_ALTERED_SEARCH_PATH` (so it can ship co-located deps in `mods\`).
A native mod `#include`s **`oss_mod_api.h`** (the whole contract — copy it beside your
source) and exports one entry:

```c
#include "oss_mod_api.h"
__declspec(dllexport) int OssModInit(const OssApi *api) {
    if (api->abi_version != OSS_ABI_VERSION) return 1;
    api->log("hello from a native mod");
    uintptr_t va = api->mem_reloc(0x5e2a10);
    int h = api->hook_entry(va, my_observer, /*user=*/NULL);   // shares the chain with Lua mods
    api->on_frame(my_per_frame, NULL);
    return 0;   // 0 = ok
}
```

The loader resolves `OssModInit` and calls it **once on the engine thread at the first
safepoint** (after the DLL's `DllMain`), handing over the **`OssApi`** vtable — the C-side
of the same services Lua mods get: `log`; guarded `mem_read`/`mem_write`/`mem_readable`/
`mem_writable`/`mem_base`/`mem_reloc`/`mem_scan`; `main_enqueue`/`on_frame` (run on the
engine thread); and `hook_entry`/`hook_remove` (Tier-1 observers — a native `OssHookEntryFn`
gets `OssHookCtx{ecx,edx,eax,esp,ret,va}`, joining the **same per-VA chain** as Lua
`mod.hook.entry`). So native and Lua mods interleave in one registry — ship a perf-critical
hook or a big feature native, keep trivial mods in Lua.

- **Stability:** `OssApi` is versioned (`abi_version` + `struct_size`, append-only) so a
  mod built against an older header keeps working. **Note:** native callbacks are **not**
  yet SEH-isolated (a Lua mod gets `pcall` isolation; a faulting native mod can fault the
  game) — native is the powerful/you-are-responsible tier. Do engine work on the main
  thread (inside `OssModInit`, `on_frame`, or a `main_enqueue` job); off-thread, only read
  via the guarded `mem_*` and push work through `main_enqueue`.
- A DLL **without** `OssModInit` still loads (its `DllMain` runs) as a legacy standalone
  native mod — the loader just doesn't hand it the ABI.
- Example: `examples/native_hello/` (build: `make -C examples/native_hello`).
- Tier-2 typed hooks, UI panels, and `mod.game.*` are not in the ABI yet (appended later).

## `mod.toml`

```toml
[mod]
id          = "hello"                 # unique id (also the registry key)
name        = "Hello World"           # display name
version     = "1.0.0"                 # semver
description = "One line."
authors     = ["you"]
target_games = ["sotes_en"]           # profile ids, or ["*"] for any game

[loader]
api         = "1.0"                   # the mod API this mod targets (MAJOR.MINOR) — see "Versioning"
min_version = "0.1.0"                 # refuse to load under an older loader host

[load]
order = 0                             # optional: lower loads first
after = ["some_other_mod"]            # optional: soft dependency ordering

# Settings the mod takes — the loader parses this (schema) and mod.config reads/writes the values;
# the launcher renders the SAME table.  type = bool|int|float|string; default required; min/max/label/
# description optional.  Values persist to oss_mods.cfg, namespaced "<id>.<key>".
[config.enabled]
type = "bool"
default = true
label = "Enabled"
```

Read them in the mod: `mod.config.get("enabled")` (typed value or the default), `mod.config.set(k, v)`
(clamps + persists, debounced), `mod.config.schema()` (for a generic editor).

The manifest is optional for a hand-dropped local mod (the loader runs any
`mods\<name>\init.lua` regardless) but **required to publish to a registry**, where the
launcher reads it for name/version/description, config, and load-order resolution.

## Versioning

Three independent version axes keep incompatible pieces from loading:

- **Mod API** (`[loader] api = "MAJOR.MINOR"`) — the loader↔mod contract (`mod.mem`/`mod.game`/`mod.ui`/…).
  The loader has its own (`mod.api_version`); it **refuses a mod** whose MAJOR differs (a breaking change,
  either direction) or whose MINOR is newer than the loader's (needs features this loader lacks).  Absent
  ⇒ assumed `"1.0"`.  MAJOR bumps on a break; MINOR bumps on additive-only changes.
- **Loader host version** (`[loader] min_version`, `mod.loader_version`) — the release/marketing version.
- **Registry schema** (`registry_version` in a source's `registry.json`) — the package-manager format the
  launcher reads (REGISTRY.md).

(Native mods have a fourth: `OSS_ABI_VERSION` + struct-size growth in `oss_mod_api.h`.)

## Load order

Mods load in the order `mods\` enumerates unless a manifest constrains it (`load.order`,
`load.after`). Explicit order is honored first, then `after` dependencies, then name.
(Dependency resolution across registry installs is the launcher's job — REGISTRY.md.)
