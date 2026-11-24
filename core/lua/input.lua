-- core/lua/input.lua — the Lua half of mod.input (embedded).  Receives the C table, adds:
--   input.KEYS            name -> DIK (the rd / rhost key table: U D L R X C Z V F ent esc bk f1..f6 n0..n9 pu pd home sp)
--   input.play(movie)     rd movie syntax, appended to the script:  "RX*8 .*30 ent R+ent 0x30*4"
--                         one-letter keys combine (RX = Right+X), '+' joins names, '.' = nothing, *N = N frames
--   input.tap(name[, n])  press for n frames (default 2) then release for 2
local c = ...
local K = { U = 0xc8, D = 0xd0, L = 0xcb, R = 0xcd, X = 0x2d, C = 0x2e, Z = 0x2c, V = 0x2f, F = 0x21,
  ent = 0x1c, esc = 0x01, bk = 0x0e, f1 = 0x3b, f2 = 0x3c, f3 = 0x3d, f4 = 0x3e, f5 = 0x3f, f6 = 0x40,
  n0 = 0x52, n1 = 0x4f, n2 = 0x50, n3 = 0x51, n4 = 0x4b, n5 = 0x4c, n6 = 0x4d, n7 = 0x47, n8 = 0x48, n9 = 0x49,
  pu = 0xc9, pd = 0xd1, home = 0xc7, sp = 0x39 }
c.KEYS = K

local function keys_of(body)
  local out = {}
  if body == "." then return out end
  for part in body:gmatch("[^+]+") do
    if part:match("^0[xX]%x+$") then out[#out + 1] = tonumber(part)
    elseif K[part] and #part > 1 then out[#out + 1] = K[part]
    elseif part:match("^[UDLRXCZVF]+$") then for ch in part:gmatch(".") do out[#out + 1] = K[ch] end
    elseif K[part] then out[#out + 1] = K[part]
    else error("bad key '" .. part .. "' (keys: U D L R X C Z V F ent esc bk f1-f6 n0-n9 pu pd home sp, 0xNN)") end
  end
  return out
end

function c.play(movie)
  local total = 0
  for tok in tostring(movie):gmatch("%S+") do
    local body, n = tok:match("^([^*]+)%*(%d+)$")
    if not body then body, n = tok, 1 end
    n = tonumber(n)
    c.push(keys_of(body), n)
    total = total + n
  end
  return total
end

function c.tap(name, n)
  c.push(keys_of(name), n or 2)
  c.push({}, 2)
end

return c
