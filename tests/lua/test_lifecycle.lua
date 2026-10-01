local T = require "harness"

local function env(extra)
  local e = {
    LUA_CPATH = os.getenv("LUA_CPATH") or "",
    LUA_PATH = os.getenv("LUA_PATH") or "",
    LUA_PYTHON_EXECUTABLE = os.getenv("LUA_PYTHON_EXECUTABLE") or "",
  }
  for k, v in pairs(extra or {}) do e[k] = v end
  return e
end

T.test("a missing Python is a Lua error, not an abort", function()
  local ok, _, out = T.run_lua([[
    local ok, err = pcall(require, "python")
    print(ok, err)
    os.exit(ok and 1 or 0)
  ]], env{LUA_PYTHON_EXECUTABLE = "/nonexistent/python3"})
  T.eq(ok, true, out)
  T.truthy(out:find("could not find a usable libpython", 1, true), out)
end)

T.test("a bad LUA_PYTHON_LIBPYTHON is a Lua error", function()
  local ok, _, out = T.run_lua([[
    local ok, err = pcall(require, "python")
    print(ok, err)
    os.exit(ok and 1 or 0)
  ]], env{LUA_PYTHON_LIBPYTHON = "/nonexistent/libpython3.so"})
  T.eq(ok, true, out)
  T.truthy(out:find("LUA_PYTHON_LIBPYTHON", 1, true), out)
end)

T.test("Python output is flushed and atexit runs when the host exits", function()
  local marker = os.tmpname()
  os.remove(marker)
  local ok, _, out = T.run_lua(([[
    local python = require "python"
    python.execute("print('from python', end='')")
    python.execute("import atexit\natexit.register(lambda: open(r'%s', 'w').close())")
  ]]):format(marker), env())
  T.eq(ok, true, out)
  T.eq(out, "from python")
  if T.windows then return end           -- Python is not finalized at exit on Windows
  local f = io.open(marker)
  T.truthy(f, "atexit handler did not run")
  if f then f:close() end
  os.remove(marker)
end)

T.test("require inside a coroutine that is later collected", function()
  local ok, _, out = T.run_lua([[
    local co = coroutine.create(function() python = require "python" end)
    assert(coroutine.resume(co))
    co = nil
    for i = 1, 5 do collectgarbage("collect") end
    local f = python.eval("lambda g: g(41)")
    io.write(tostring(f(function(x) return x + 1 end)))
  ]], env())
  T.eq(ok, true, out)
  T.eq(out, "42")
end)

T.test("requiring twice gives a working module", function()
  local p1 = require "python"
  package.loaded.python = nil
  local p2 = require "python"
  T.eq(p2.eval("1 + 1"), 2)
  T.eq(p1.eval("2 + 2"), 4)
end)

T.test("Python objects are released when Lua collects them", function()
  local python = require "python"
  python.execute([[
import weakref
class Thing: pass
freed = []
def make():
    t = Thing()
    weakref.finalize(t, freed.append, 1)
    return t
]])
  local t = python.eval("make()")
  t = nil
  collectgarbage("collect")
  collectgarbage("collect")
  T.eq(python.eval("len(freed)"), 1)       -- released at this crossing at the latest
end)

T.test("Lua objects are released when Python drops them", function()
  local python = require "python"
  local before = collectgarbage("count")
  for i = 1, 2000 do python.eval("lambda t: t")({i}) end
  collectgarbage("collect")
  python.eval("None")
  collectgarbage("collect")
  T.truthy(collectgarbage("count") < before + 200, "registry references leaked")
end)

T.test("a virtualenv's environment is adopted", function()
  local exe = os.getenv("LUA_PYTHON_EXECUTABLE")
  if not exe or exe == "" then return end
  local dir = os.tmpname()
  os.remove(dir)
  assert(os.execute(('"%s" -m venv --without-pip "%s"'):format(exe, dir)))
  local venv_python = T.windows and (dir .. "\\Scripts\\python.exe") or (dir .. "/bin/python")
  local ok, _, out = T.run_lua(([[
    local python = require "python"
    local os_path = python.import("os").path
    local sys = python.import("sys")
    io.write(tostring(os_path.samefile(sys.prefix, %q)), " ", tostring(sys.prefix ~= sys.base_prefix))
  ]]):format(dir), env{LUA_PYTHON_EXECUTABLE = venv_python})
  T.eq(ok, true, out)
  T.eq(out, "true true")
end)

T.done()
