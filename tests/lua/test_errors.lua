local T = require "harness"
local python = require "python"

T.test("execute and eval raise Lua errors", function()
  local err = T.raises(function() python.execute("raise ValueError('boom')") end, "ValueError: boom")
  T.truthy(tostring(err):find("Traceback", 1, true))
  T.raises(function() python.eval("1/0") end, "ZeroDivisionError")
  T.raises(function() python.execute("def (") end, "SyntaxError")
end)

T.test("error values are the exception objects", function()
  local ok, err = pcall(python.eval("lambda: int('x')"))
  T.eq(ok, false)
  T.eq(tostring(python.eval("type")(err)), "<class 'ValueError'>")
  T.truthy(tostring(err.args[1]):find("invalid literal", 1, true))
end)

T.test("import errors explain themselves", function()
  T.raises(function() python.import("no_such_module_xyz") end, "No module named 'no_such_module_xyz'")
end)

T.test("sys.exit does not terminate the Lua host", function()
  local ok, err = pcall(python.eval("lambda: __import__('sys').exit(7)"))
  T.eq(ok, false)
  T.truthy(tostring(err):find("SystemExit: 7", 1, true))
end)

T.test("Python exceptions keep their type through Lua", function()
  python.execute([[
class MyErr(Exception): pass
def boom(): raise MyErr('details')
def call(f):
    try:
        f()
    except MyErr as e:
        return 'caught ' + str(e)
]])
  local boom = python.eval("boom")
  T.eq(python.eval("call")(function() boom() end), "caught details")
end)

T.test("Lua error values survive a trip through Python", function()
  local e = {code = 42}
  local relay = python.eval("lambda f: f()")
  local ok, err = pcall(relay, function() error(e) end)
  T.eq(ok, false)
  T.eq(err, e)
end)

T.test("Lua errors seen from Python", function()
  python.globals().lua_fail = function() error("lua side failure") end
  python.execute([[
import lua
try:
    lua_fail()
except lua.LuaError as e:
    msg, tb = str(e), e.traceback
]])
  T.truthy(python.eval("msg"):find("lua side failure", 1, true))
  T.truthy(python.eval("tb"):find("stack traceback", 1, true))
end)

T.done()
