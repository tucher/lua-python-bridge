-- Runs every tests/lua/test_*.lua in its own interpreter process, so that crashes and exit codes
-- are reported as failures. Usage: lua tests/lua/run.lua [pattern]
-- The Lua module must be on LUA_CPATH; LUA_PYTHON_EXECUTABLE selects the Python.
local sep = package.config:sub(1, 1)
local dir = arg[0]:match("^(.*)[/\\]") or "."
local lua = arg[-1]
local filter = arg[1]
local windows = sep == "\\"

local function list_tests()
  local cmd = windows and ('dir /b "' .. dir .. '\\test_*.lua"') or ('ls "' .. dir .. '"/test_*.lua')
  local files = {}
  for line in io.popen(cmd):lines() do
    local name = line:match("([^/\\]+%.lua)$")
    if name and (not filter or name:find(filter, 1, true)) then
      files[#files + 1] = name
    end
  end
  table.sort(files)
  return files
end

local env_path = dir .. sep .. "?.lua;" .. (os.getenv("LUA_PATH") or ";;")
local failures = 0
for _, name in ipairs(list_tests()) do
  local cmd
  if windows then
    cmd = string.format('"set "LUA_PATH=%s" && "%s" "%s%s%s" 2>&1"', env_path, lua, dir, sep, name)
  else
    cmd = string.format("LUA_PATH='%s' '%s' '%s%s%s' 2>&1", env_path, lua, dir, sep, name)
  end
  local p = io.popen(cmd)
  local out = p:read("a")
  local ok, _, code = p:close()
  local status = ok and "ok" or ("FAILED (exit " .. tostring(code) .. ")")
  io.write(string.format("%-28s %s\n", name, status))
  if not ok then
    failures = failures + 1
    io.write(out, "\n")
  elseif os.getenv("LPB_TEST_VERBOSE") then
    io.write(out)
  end
end
io.write(failures == 0 and "all Lua test files passed\n" or (failures .. " test file(s) failed\n"))
os.exit(failures == 0 and 0 or 1)
