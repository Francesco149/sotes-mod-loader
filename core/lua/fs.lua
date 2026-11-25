-- core/lua/fs.lua — mod.fs (files and folders) + mod.steam (find the user's other Steam games).   [api 1.11]
--
-- What a first-time setup step needs (setup.lua): read another game's files from where Steam put them, write the
-- converted result into the mod's own data folder (mod.data_dir).  Plain Win32 over FFI — no engine, any thread.
--
--   mod.fs.exists(p) / is_dir(p) / size(p)            -> bool / bool / bytes | nil
--   mod.fs.read(p[, offset[, len]])                   -> string | nil, err         (binary)
--   mod.fs.write(p, data)                             -> true | nil, err           (binary; parent folders created)
--   mod.fs.list(dir)                                  -> {{name=, dir=bool, size=}, ...} | nil   (no . / ..)
--   mod.fs.mkdir(p)                                   -> true | nil, err           (recursive)
--   mod.fs.remove(p)                                  -> bool                       (a file, or an EMPTY folder)
--   mod.fs.join(a, b, ...)                            -> "a\b\..."
--   mod.fs.game_dir                                   the game's folder (trailing \)
--
--   mod.steam.root()                                  -> path | nil    the Steam client folder
--   mod.steam.libraries()                             -> {path, ...}   every library folder (each holds steamapps\)
--   mod.steam.app(appid[, {dir=, marker=}])           -> {dir=, library=, name=, appid=} | nil, why
--       the game's install folder: by its appmanifest (any library), else by folder name `dir` under
--       steamapps\common (case-insensitive); `marker` = a file that must exist inside (e.g. "recettear.exe").
--   mod.steam.hint(path)                              add a folder to search (a Steam root or library; tests, odd setups)
--
-- Where Steam is looked for: the registry (HKCU\Software\Valve\Steam SteamPath, HKLM …\Valve\Steam InstallPath),
-- the library this game itself is installed in (…\steamapps\common\<game>), Proton's
-- STEAM_COMPAT_CLIENT_INSTALL_PATH / the usual Linux folders through wine's Z: drive, and hints.
local log = ...
local ffi = require('ffi')

ffi.cdef [[
typedef struct { uint32_t attr; uint32_t ct[2], at[2], wt[2]; uint32_t size_hi, size_lo; uint32_t r0, r1;
                 char name[260]; char alt[14]; } OSS_FIND_A;
void *FindFirstFileA(const char *, OSS_FIND_A *);
int FindNextFileA(void *, OSS_FIND_A *);
int FindClose(void *);
uint32_t GetFileAttributesA(const char *);
int CreateDirectoryA(const char *, void *);
int DeleteFileA(const char *);
int RemoveDirectoryA(const char *);
uint32_t GetModuleFileNameA(void *, char *, uint32_t);
uint32_t GetEnvironmentVariableA(const char *, char *, uint32_t);
long RegOpenKeyExA(uintptr_t, const char *, uint32_t, uint32_t, uintptr_t *);
long RegQueryValueExA(uintptr_t, const char *, uint32_t *, uint32_t *, uint8_t *, uint32_t *);
long RegCloseKey(uintptr_t);
]]
local K, ADV = ffi.C, ffi.load('advapi32')
local INVALID = 0xFFFFFFFF
local DIR = 0x10

local fs = {}
function fs.join(...)
  local parts = { ... }
  local out = tostring(parts[1] or '')
  for i = 2, #parts do
    local p = tostring(parts[i])
    if out ~= '' and not out:match('[\\/]$') then out = out .. '\\' end
    out = out .. p:gsub('^[\\/]+', '')
  end
  return out
end
local function attr(p) local a = K.GetFileAttributesA(p) return a ~= INVALID and a or nil end
function fs.exists(p) return attr(p) ~= nil end
function fs.is_dir(p) local a = attr(p) return a ~= nil and bit.band(a, DIR) ~= 0 end
function fs.size(p)
  local f = io.open(p, 'rb')
  if not f then return nil end
  local n = f:seek('end')
  f:close()
  return n
end
function fs.read(p, offset, len)
  local f, err = io.open(p, 'rb')
  if not f then return nil, err end
  if offset and offset > 0 then f:seek('set', offset) end
  local d = f:read(len or '*a')
  f:close()
  return d or ''
end
function fs.mkdir(p)
  p = p:gsub('[\\/]+$', '')
  if p == '' or p:match('^%a:$') or fs.is_dir(p) then return true end
  local parent = p:match('^(.*)[\\/][^\\/]+$')
  if parent and not fs.is_dir(parent) then
    local ok, err = fs.mkdir(parent)
    if not ok then return nil, err end
  end
  if K.CreateDirectoryA(p, nil) == 0 and not fs.is_dir(p) then return nil, 'cannot create ' .. p end
  return true
end
function fs.write(p, data)
  local parent = p:match('^(.*)[\\/][^\\/]+$')
  if parent then fs.mkdir(parent) end
  local f, err = io.open(p, 'wb')
  if not f then return nil, err end
  f:write(data)
  f:close()
  return true
end
function fs.list(dir)
  local fd = ffi.new('OSS_FIND_A')
  local h = K.FindFirstFileA(fs.join(dir, '*'), fd)
  if ffi.cast('intptr_t', h) == -1 then return nil end
  local out = {}
  repeat
    local name = ffi.string(fd.name)
    if name ~= '.' and name ~= '..' then
      out[#out + 1] = { name = name, dir = bit.band(fd.attr, DIR) ~= 0, size = fd.size_lo }
    end
  until K.FindNextFileA(h, fd) == 0
  K.FindClose(h)
  table.sort(out, function(a, b) return a.name:lower() < b.name:lower() end)
  return out
end
function fs.remove(p)
  if fs.is_dir(p) then return K.RemoveDirectoryA(p) ~= 0 end
  return K.DeleteFileA(p) ~= 0
end
do
  local buf = ffi.new('char[520]')
  local n = K.GetModuleFileNameA(nil, buf, 520)
  fs.game_dir = ffi.string(buf, n):match('^(.*[\\/])') or ''
end

-- ── steam ────────────────────────────────────────────────────────────────────────────────────────────────────────
local steam = {}
local hints = {}
local HKCU, HKLM = 0x80000001, 0x80000002
local function reg(root, key, value)
  for _, view in ipairs { 0, 0x200, 0x100 } do            -- default, then the 32- and 64-bit views
    local h = ffi.new('uintptr_t[1]')
    if ADV.RegOpenKeyExA(root, key, 0, 0x20019 + view, h) == 0 then
      local buf, n = ffi.new('uint8_t[1024]'), ffi.new('uint32_t[1]', 1024)
      local r = ADV.RegQueryValueExA(h[0], value, nil, nil, buf, n)
      ADV.RegCloseKey(h[0])
      if r == 0 and n[0] > 1 then return (ffi.string(buf, n[0]):gsub('%z+$', '')) end
    end
  end
end
local function env(name)
  local buf = ffi.new('char[1024]')
  local n = K.GetEnvironmentVariableA(name, buf, 1024)
  if n > 0 and n < 1024 then return ffi.string(buf, n) end
end
local function norm(p) return (p:gsub('/', '\\'):gsub('\\+$', '')) end
local function unix(p) return p and p:sub(1, 1) == '/' and ('Z:' .. p:gsub('/', '\\')) or p end   -- a wine path
local function is_library(p) return p and fs.is_dir(fs.join(p, 'steamapps')) end
local function candidates()
  local c = {}
  local function add(p) if p and p ~= '' then c[#c + 1] = norm(unix(p)) end end
  for _, h in ipairs(hints) do add(h) end
  add(reg(HKCU, 'Software\\Valve\\Steam', 'SteamPath'))
  add(reg(HKLM, 'SOFTWARE\\Valve\\Steam', 'InstallPath'))
  add(reg(HKLM, 'SOFTWARE\\WOW6432Node\\Valve\\Steam', 'InstallPath'))
  local own = fs.game_dir:lower():find('\\steamapps\\common\\', 1, true)   -- the library this game lives in
  if own then add(fs.game_dir:sub(1, own - 1)) end
  add(env('STEAM_COMPAT_CLIENT_INSTALL_PATH'))                              -- Proton
  for _, e in ipairs { 'STEAM_COMPAT_LIBRARY_PATHS', 'OSS_STEAM' } do
    for p in (env(e) or ''):gmatch('[^:;]+') do if #p > 2 then add(p) end end
  end
  local home = env('HOME') or (env('USER') and '/home/' .. env('USER')) or (env('USERNAME') and '/home/' .. env('USERNAME'))
  if home and home:sub(1, 1) == '/' then                                    -- wine without Proton
    add(home .. '/.steam/steam'); add(home .. '/.local/share/Steam')
    add(home .. '/.var/app/com.valvesoftware.Steam/.local/share/Steam')
  end
  add('C:\\Program Files (x86)\\Steam'); add('C:\\Program Files\\Steam')
  return c
end
function steam.hint(p) if type(p) == 'string' and p ~= '' then table.insert(hints, 1, p) end end
function steam.root()
  for _, p in ipairs(candidates()) do
    if is_library(p) or fs.exists(fs.join(p, 'steam.exe')) then return p end
  end
end
-- libraryfolders.vdf: "path" "D:\\SteamLibrary" entries (both the old flat and the new nested form)
local function vdf_paths(text, out, seen)
  for p in text:gmatch('"path"%s*"([^"]+)"') do
    p = norm(unix((p:gsub('\\\\', '\\'))))
    if not seen[p:lower()] then seen[p:lower()] = true; out[#out + 1] = p end
  end
  for p in text:gmatch('"%d+"%s*"([^"]+)"') do
    p = norm(unix((p:gsub('\\\\', '\\'))))
    if (p:match('^%a:') or p:sub(1, 2) == '\\\\') and not seen[p:lower()] then seen[p:lower()] = true; out[#out + 1] = p end
  end
end
function steam.libraries()
  local out, seen = {}, {}
  for _, c in ipairs(candidates()) do
    if is_library(c) and not seen[c:lower()] then seen[c:lower()] = true; out[#out + 1] = c end
    for _, rel in ipairs { 'steamapps\\libraryfolders.vdf', 'config\\libraryfolders.vdf' } do
      local t = fs.read(fs.join(c, rel))
      if t then vdf_paths(t, out, seen) end
    end
  end
  local ok = {}
  for _, p in ipairs(out) do if is_library(p) then ok[#ok + 1] = p end end
  return ok
end
function steam.app(appid, o)
  o = o or {}
  local libs = steam.libraries()
  local function good(dir) return fs.is_dir(dir) and (not o.marker or fs.exists(fs.join(dir, o.marker))) end
  if appid then
    for _, lib in ipairs(libs) do
      local t = fs.read(fs.join(lib, 'steamapps', ('appmanifest_%d.acf'):format(appid)))
      local inst = t and t:match('"installdir"%s*"([^"]+)"')
      if inst then
        local dir = fs.join(lib, 'steamapps', 'common', inst)
        if good(dir) then return { dir = dir .. '\\', library = lib, name = t:match('"name"%s*"([^"]+)"') or inst, appid = appid } end
      end
    end
  end
  if o.dir then
    for _, lib in ipairs(libs) do
      for _, e in ipairs(fs.list(fs.join(lib, 'steamapps', 'common')) or {}) do
        if e.dir and e.name:lower() == o.dir:lower() then
          local dir = fs.join(lib, 'steamapps', 'common', e.name)
          if good(dir) then return { dir = dir .. '\\', library = lib, name = e.name, appid = appid } end
        end
      end
    end
  end
  return nil, (#libs == 0) and 'no Steam library found' or
    ('not installed in %d Steam librar%s'):format(#libs, #libs == 1 and 'y' or 'ies')
end

return { fs = fs, steam = steam }
