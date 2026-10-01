local T = require "harness"
local python = require "python"

local show = python.eval("lambda *a, **k: repr((a, k))")

T.test("plain calls are positional", function()
  T.eq(show(1, "x"), "((1, 'x'), {})")
  T.eq(python.eval("len")({1, 2, 3}), 3)              -- a single table is one argument
  T.eq(show(1, nil), "((1, None), {})")
  T.eq(python.import("json").dumps(python.list({1})), "[1]")
end)

T.test("python.kw supplies keyword arguments", function()
  T.eq(show(1, python.kw{x = 2}), "((1,), {'x': 2})")
  T.eq(show(python.kw{}), "((), {})")
  T.raises(function() return show(python.kw{x = 1}, 2) end, "must be the last argument")
  T.raises(function() return show(python.kw{1}) end, "invalid key 1")
end)

T.test("python.tablecall takes one argument table", function()
  local f = python.tablecall(show)
  T.eq(f{1, 2, x = 3}, "((1, 2), {'x': 3})")
  T.eq(f{1, nil, 3}, "((1, None, 3), {})")            -- gaps become None
  T.eq(f{}, "((), {})")
  T.raises(function() return f{[0] = 1} end, "invalid key 0")
  T.raises(function() return f{[1.5] = 1} end, "invalid key 1.5")
  T.raises(function() return f{["1"] = 1, [true] = 2} end, "invalid boolean key")
  T.raises(function() return f(1) end, "exactly one argument table")
  T.raises(function() return f({}, {}) end, "exactly one argument table")
end)

T.test("numeric string keys are named arguments", function()
  local f = python.tablecall(python.eval("lambda **k: sorted(k)"))
  T.eq(tostring(f{["1"] = "x"}), "['1']")
end)

T.test("python.asfunc", function()
  local f = python.asfunc(python.eval("max"))
  T.eq(f(3, 9, 4), 9)
end)

T.test("Python calls Lua with keyword arguments as a trailing table", function()
  python.globals().lua_f = function(a, b, opts) return a + b + (opts and opts.extra or 0) end
  T.eq(python.eval("lua_f(1, 2)"), 3)
  T.eq(python.eval("lua_f(1, 2, extra=10)"), 13)
end)

T.test("lua.tablecall mirrors python.tablecall", function()
  python.globals().lua_g = function(t) return #t .. ":" .. tostring(t.name) end
  python.execute("import lua\ng2 = lua.tablecall(lua_g)")
  T.eq(python.eval("g2(1, 2, 3, name='n')"), "3:n")
end)

T.test("round trips are lossless", function()
  python.execute("def target(*a, **k): return repr((a, k))")
  python.globals().forward = function(a, b, opts) return python.eval("target")(a, b, python.kw(opts)) end
  T.eq(python.eval("forward(1, 2, x=3)"), "((1, 2), {'x': 3})")
end)

T.test("many arguments and results", function()
  local many = {}
  for i = 1, 300 do many[i] = i end
  T.eq(python.eval("lambda *a: len(a)")(table.unpack(many)), 300)
  python.globals().lua_many = function(...) return select("#", ...) end
  T.eq(python.eval("lua_many(*range(300))"), 300)
  python.globals().lua_multi = function() return 1, "two", nil end
  T.eq(tostring(python.eval("lua_multi()")), "(1, 'two', None)")
end)

T.test("callbacks nest", function()
  local depth = python.eval("lambda f, n: f(f, n)")
  local function rec(self, n) if n == 0 then return "bottom" end return depth(self, n - 1) end
  T.eq(depth(rec, 20), "bottom")
end)

T.test("coroutines", function()
  local co = coroutine.wrap(function()
    local r = python.eval("lambda f: f() * 2")(function() return 21 end)
    coroutine.yield(r)
    return "done"
  end)
  T.eq(co(), 42)
  T.eq(co(), "done")
  local c2 = coroutine.create(function() return python.eval("lambda f: f()")(coroutine.yield) end)
  local ok, err = coroutine.resume(c2)
  T.eq(ok, false)
  T.truthy(tostring(err):find("yield", 1, true))
end)

T.done()
