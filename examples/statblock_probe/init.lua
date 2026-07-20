-- examples/statblock_probe/init.lua — map the party stat-block offsets against the in-game numbers.
--
-- The dashboard reads atk/def/spirit/resist at stat_block +0x64/+0x68/+0x6c/+0x70, but the live game
-- shows two numbers per stat (effective/base) and spirit/resist don't line up — so some of those
-- offsets are wrong (0x6c/0x70 may be other fields, e.g. hit/evade).  This probe dumps the raw dwords
-- around that region for each present party member, ONCE, so the correct offsets can be read off.
--
-- Use: stage it into mods\, launch, LOAD A SAVE, then read oss_modloader.log for the "[statblock]"
-- lines and match the values to what the character sheet shows (e.g. Stella spirit 140/98).

local m, seen = mod.mem, {}

local function dump()
  local roster = mod.game.roster
  if not roster then return end
  for _, mem in ipairs(roster.members()) do
    local sb = mem.stat_block
    if sb and sb > 0 and not seen[mem.name] then
      seen[mem.name] = true
      -- what the dashboard currently reports, for cross-reference against the in-game sheet
      mod.log(string.format("[statblock] %s  sb=0x%08x  DASHBOARD atk=%d def=%d spi=%d res=%d  hp=%d/%d",
        mem.name, sb, mem.attack, mem.defense, mem.spirit, mem.resist, mem.hp, mem.hp_max))
      -- raw dwords across HP/MP + the combat stats + the equip/buff clusters
      local parts = {}
      for off = 0x50, 0xB4, 4 do
        parts[#parts + 1] = string.format("+%02x=%d", off, m.read_u32(sb + off) or -1)
      end
      mod.log("[statblock]   " .. table.concat(parts, "  "))
    end
  end
end

mod.on_frame(dump)
mod.log("statblock_probe armed — load a save; each member's stat block dumps once to oss_modloader.log")
