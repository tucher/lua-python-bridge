-- Minimal test helpers for the Lua-host test suite (no dependencies).
local T = {}

local passed, failed = 0, 0
local windows = package.config:sub(1, 1) == "\\"

T.windows = windows
T.lua = arg and arg[-1] or "lua"
T.lua_version = tonumber(_VERSION:match("(%d+%.%d+)"))

function T.test(name, fn)
  local ok, err = xpcall(fn, debug.traceback)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    io.stderr:write("FAIL ", name, "\n", tostring(err), "\n")
  end
end

function T.eq(a, b, msg)
  if a ~= b then
    error((msg and msg .. ": " or "") .. "expected " .. tostring(b) .. ", got " .. tostring(a), 2)
  end
end

function T.truthy(v, msg)
  if not v then error(msg or "expected a true value", 2) end
end

-- Calls fn and expects it to fail with an error whose text matches `pattern` (plain find).
function T.raises(fn, pattern)
  local ok, err = pcall(fn)
  if ok then error("expected an error matching '" .. pattern .. "'", 2) end
  local text = tostring(err)
  if not text:find(pattern, 1, true) then
    error("error '" .. text .. "' does not contain '" .. pattern .. "'", 2)
  end
  return err
end

local function quote(s)
  if windows then return '"' .. s .. '"' end
  return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- Runs a Lua chunk in a fresh interpreter (same executable, same module paths) with extra
-- environment variables. Returns ok, exit code, combined output.
function T.run_lua(code, env)
  local path = os.tmpname()
  local f = assert(io.open(path, "w"))
  f:write(code)
  f:close()
  local prefix = ""
  for k, v in pairs(env or {}) do
    if windows then
      prefix = prefix .. "set " .. k .. "=" .. v .. "&& "
    else
      prefix = prefix .. k .. "=" .. quote(v) .. " "
    end
  end
  local cmd = prefix .. quote(T.lua) .. " " .. quote(path) .. " 2>&1"
  if windows then cmd = '"' .. cmd .. '"' end
  local p = io.popen(cmd)
  local out = p:read("a")
  local ok, _, code = p:close()
  os.remove(path)
  return ok, code, out
end

function T.done()
  io.write(string.format("%d passed, %d failed\n", passed, failed))
  os.exit(failed == 0 and 0 or 1)
end

return T
