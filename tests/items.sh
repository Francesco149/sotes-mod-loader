#!/usr/bin/env bash
# tests/items.sh R — new items (api 1.12) on a running `sml` run R started with `--mods itemtest` and continued into a
# save (a Steam save with Arche in a field room: items are used and a slime is fought there).
#   table      the item table relocated (every site), the 4 rows readable through items.def, catalog lists them
#   icons      4 cells appended past the retail sheet; the icon bank decodes them (frames > the retail cells)
#   give       through room_item_add: each new id lands in the inventory under its own name
#   equip      the Test Blade on Arche: attack = before - the old weapon's + 21 (room_item_equip's stat refresh)
#   use        (SE) the Test Bun heals 150 (HP 20 -> 170), the Test Drop adds 5 to attack for good; retail's message
#   floor      items.drop at the leader: a pickup, touched → the inventory gains it
#   drops      a Test Slime (1 HP, type with drops) killed by Arche's sword: its two items + its 3 CP fall and are
#              picked up (inventory +2 of ours)
# PASS / FAIL on stdout, exit 1 on a FAIL.  Looks: SHOTS=DIR writes a PNG per stage there.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
P() { "$SML" play "$R" "$1" --wait >/dev/null; }
W() { "$SML" wait "$R" "$1" --timeout "${2:-300}" >/dev/null; }
shot() { [ -n "${SHOTS:-}" ] && "$SML" shot "$R" "$SHOTS/$1" >/dev/null; return 0; }
fail() { echo "FAIL  items.sh  $*"; shot "fail.png"; exit 1; }
ok() { echo "  ok  $*"; }
is() { [ "$(L "$1")" = true ] || fail "$2: $(L "${3:-return nil}")"; ok "$2"; }

[ "$(L 'return itemtest ~= nil and mod.game.items.define ~= nil')" = true ] || fail "no itemtest mod / api 1.12"
W 'return not mod.game.map.event()' || fail "an event never ended"
ED=$(L 'return mod.edition' | tr -d '"')

# table
is 'local c = mod.game.items.capacity() return c.extended and c.defined == 4 and c.total > c.retail + 1000' \
   "table relocated, 4 defined" 'return mod.game.items.capacity()'
is 'local I, ID = mod.game.items, itemtest.ID local b, d = I.def(ID.blade), I.def(ID.drop)
    return b.name == "Test Blade" and b.attack == 21 and b.price == 999 and b.slot == I.def(110010).slot and b.ours
       and d.a == 1 and d.b == 5 and d.c == 0 and d.effect == I.def(202010).effect' \
   "rows: fields set, the rest from like" 'return {mod.game.items.def(itemtest.ID.blade), mod.game.items.def(itemtest.ID.drop)}'
is 'local n = 0 for _, c in ipairs(mod.game.items.catalog()) do if c.ours then n = n + 1 end end return n == 4' "catalog lists them"
is 'local I = mod.game.items local ic = I.icons() return ic.built == 4 and I.def(itemtest.ID.mail).icon == ic.first + 1 and #ic.banks >= 2' \
   "4 icons appended after the retail cells" 'return mod.game.items.icons()'

# give (room for 4: store 20 unworn records, as the storage box does)
L 'local w = mod.mem.read_u32(mod.sym.va("g_world")) local room = mod.mem.read_u32(w + (mod.edition == "enold" and 0x2784 or 0x3788))
   local k = 0 for i = mod.mem.read_u16(room + 0x4010) - 1, 0, -1 do local r = room + 0x2010 + 16 * i
   if k < 20 and mod.mem.read_u32(r + 4) == 0 and mod.mem.read_u8(r + 0xa) == 0 then mod.mem.write_u8(r + 0xa, 1) k = k + 1 end end return k' >/dev/null
is 'local I, ID = mod.game.items, itemtest.ID _G.IX = {}
    for k, id in pairs(ID) do IX[k] = I.give(id, 1) if not IX[k] then return false end end
    local names = {} for _, it in ipairs(I.list()) do names[it.index] = it.name end
    return names[IX.blade] == "Test Blade" and names[IX.mail] == "Test Mail" and names[IX.bun] == "Test Bun" and names[IX.drop] == "Test Drop"' \
   "give: 4 records under their names" 'return IX'

# icons drawn: open the item list (decodes the bank), check the bank's frame count
P ".*30 f1*10 .*150 ent*10 .*120 U*8 .*60"
shot items_menu.png
is 'local b = mod.game.sprites.bank(mod.game.items.icons().bank) return b.decoded and b.frames >= mod.game.items.icons().first + 4' \
   "icon bank decoded with the appended cells" 'return mod.game.sprites.bank(mod.game.items.icons().bank)'
P "esc*8 .*30 esc*8 .*30 esc*8 .*60"

# equip
is 'local I = mod.game.items local a = mod.game.roster.members()[1] local before = a.attack
    local old = 0 for _, it in ipairs(I.list()) do if it.owner == 100000101 and I.def(it.id).slot == I.def(110010).slot then old = I.def(it.id).attack end end
    I.equip(IX.blade, a) local after = mod.game.roster.members()[1].attack
    _G.EQ = {before = before, old = old, after = after} return after == before - old + 21' \
   "equip: attack = before - old weapon + 21" 'return EQ'

# use (SE)
if [ "$ED" != enold ]; then
  is 'local I, a = mod.game.items, mod.game.roster.members()[1] mod.game.roster.set(nil, {hp = 20})
      local used, msg = I.use(IX.bun, a) local hp = mod.game.roster.members()[1].hp _G.US = {used, msg, hp}
      return used and hp == math.min(170, a.hp_max) and msg:find("Test Bun") ~= nil' "use: the Test Bun heals 150" 'return US'
  is 'local I, a = mod.game.items, mod.game.roster.members()[1] local before = a.attack_base
      local idx for _, it in ipairs(I.list()) do if it.id == itemtest.ID.drop then idx = it.index end end
      local used, msg = I.use(idx, a) local after = mod.game.roster.members()[1].attack_base _G.US = {used, msg, before, after}
      return used and after == before + 5' "use: the Test Drop, attack +5 for good" 'return US'
fi

# on the floor
count() { L "local n = 0 for _, it in ipairs(mod.game.items.list()) do if it.id == $1 and not it.stored then n = n + math.max(1, it.count) end end return n"; }  # gear: one record each, count 0
B0=$(count 7000103)
is 'return mod.game.items.drop(itemtest.ID.bun, mod.game.roster.members()[1]) == true' "items.drop: a pickup at the leader"
# the pickup is thrown sideways from the spot: walk both ways until the leader has touched it
# the world's pickups (SE world +0x33e0 / enold +0x23e0: 96 pointers; +0x1d0 live, +0x1fc kind 2 = an item, +0x208
# the id; body +0x40): put the leader on each live one in turn until the condition holds
PICK='local w = mod.mem.read_u32(mod.sym.va("g_world")) local base = w + (mod.edition == "enold" and 0x23e0 or 0x33e0)
  for i = 0, 95 do local p = mod.mem.read_u32(base + 4 * i)
    if p ~= 0 and mod.mem.read_u32(p + 0x1d0) ~= 0 and mod.mem.read_u32(p + 0x1fc) == 2 and mod.mem.read_i32(p + 0x204) <= 0 then local b = mod.mem.read_u32(p + 0x40)
      local lb = mod.mem.read_u32(mod.game.roster.members()[1].actor + 0x40)
      mod.game.coordinates.set(nil, mod.mem.read_i32(b + 4) + math.floor(mod.mem.read_i32(b + 0xc) / 2) - math.floor(mod.mem.read_i32(lb + 0xc) / 2),
        mod.mem.read_i32(b + 8) + mod.mem.read_i32(b + 0x10) - mod.mem.read_i32(lb + 0x10) - 100) return true end end
  return false'
sweep() { P ".*90"; for _ in $(seq 12); do eval "$1" && return 0; L "$PICK" >/dev/null; P ".*60"; done; eval "$1"; }
sweep '[ "$(count 7000103)" -gt "$B0" ]'
shot floor.png
[ "$(count 7000103)" -gt "$B0" ] || fail "the dropped Test Bun was not picked up ($B0 -> $(count 7000103))"
ok "picked up"

# a type's drops: a 1-HP Test Slime in front of Arche, one swing
B0=$(count 7000103); S0=$(count 7000101); G0=$(L 'return (mod.game.items.gold())')
# 150 px left of the leader, on its floor (away from anything breakable on its right: the Steam save's room has a
# story crate there)
L 'local lb = mod.mem.read_u32(mod.game.roster.members()[1].actor + 0x40)
   return mod.game.actors.spawn(itemtest.SLIME, {x = mod.mem.read_i32(lb + 4) - 15000, y = mod.mem.read_i32(lb + 8) + mod.mem.read_i32(lb + 0x10)})' >/dev/null
# stand 30 px left of it (feet on its floor), facing it, and swing until it is gone
NEAR='for _, a in ipairs(mod.game.actors.list()) do if a.type == itemtest.SLIME then
  local b, lb = mod.mem.read_u32(a.ptr + 0x40), mod.mem.read_u32(mod.game.roster.members()[1].actor + 0x40)
  mod.game.coordinates.set(nil, mod.mem.read_i32(b + 4) - 3000, mod.mem.read_i32(b + 8) + mod.mem.read_i32(b + 0x10) - mod.mem.read_i32(lb + 0x10) - 100)
  return true end end return false'
P ".*20 Z*10 .*40"
for _ in $(seq 12); do [ "$(L "$NEAR")" = true ] || break; P ".*10 X*8 .*40"; done
[ "$(L "$NEAR")" = false ] || fail "the Test Slime did not die"
shot hit.png
sweep '[ "$(count 7000103)" -gt "$B0" ] && [ "$(count 7000101)" -gt "$S0" ]'
shot drops.png
B1=$(count 7000103); S1=$(count 7000101); G1=$(L 'return (mod.game.items.gold())')
[ "$B1" -gt "$B0" ] && [ "$S1" -gt "$S0" ] || fail "slime drops not picked up (bun $B0->$B1, blade $S0->$S1, gold $G0->$G1)"
ok "slime drops: bun $B0->$B1, blade $S0->$S1, gold $G0->$G1"
echo "PASS  items.sh ($ED)"
