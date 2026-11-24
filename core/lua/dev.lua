-- core/lua/dev.lua — helpers for the dev server's persistent env (embedded into version.dll).
-- dev.repr = a compact JSON encoder: a table whose keys are exactly 1..n → array, else object
-- (keys sorted); functions / userdata / cdata → their tostring.
dev = {}
mem, game, sym = mod.mem, mod.game, mod.sym

local ESC = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
local function esc(s)
  return (s:gsub('[%c"\\]', function(c) return ESC[c] or string.format('\\u%04x', c:byte()) end))
end

local function enc(v, depth)
  local t = type(v)
  if t == 'nil' then return 'null' end
  if t == 'boolean' then return tostring(v) end
  if t == 'number' then
    if v ~= v or v == math.huge or v == -math.huge then return 'null' end
    if v == math.floor(v) and math.abs(v) < 2^53 then return string.format('%d', v) end
    return string.format('%.17g', v)
  end
  if t == 'string' then return '"' .. esc(v) .. '"' end
  if t ~= 'table' or depth > 12 then return '"' .. esc(tostring(v)) .. '"' end
  local n, isarr = #v, true
  for k in pairs(v) do
    if type(k) ~= 'number' or k < 1 or k > n or k ~= math.floor(k) then isarr = false break end
  end
  local out = {}
  if isarr then
    for i = 1, n do out[i] = enc(v[i], depth + 1) end
    return '[' .. table.concat(out, ',') .. ']'
  end
  local keys = {}
  for k in pairs(v) do keys[#keys + 1] = k end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
  for _, k in ipairs(keys) do out[#out + 1] = '"' .. esc(tostring(k)) .. '":' .. enc(v[k], depth + 1) end
  return '{' .. table.concat(out, ',') .. '}'
end

function dev.repr(...)
  local n = select('#', ...)
  if n == 0 then return 'null' end
  if n == 1 then return enc((...), 0) end
  return enc({...}, 0)
end
dev.json = enc
