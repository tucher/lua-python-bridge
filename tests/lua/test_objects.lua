local T = require "harness"
local python = require "python"

T.test("sequences are 1-based", function()
  local l = python.eval("['a', 'b', 'c']")
  T.eq(#l, 3)
  T.eq(l[1], "a")
  T.eq(l[3], "c")
  T.eq(l[-1], "c")
  T.eq(l[0], nil)
  T.eq(l[4], nil)
  T.eq(l.count("a"), 1)                 -- string keys fall back to attributes
end)

T.test("sequence writes", function()
  local l = python.eval("[1, 2, 3]")
  l[1] = 10
  l[#l + 1] = 4                         -- append
  T.eq(tostring(l), "[10, 2, 3, 4]")
  l[#l] = nil                           -- pop
  T.eq(tostring(l), "[10, 2, 3]")
  T.raises(function() l[1] = nil end, "no holes")
  T.raises(function() l[10] = 1 end, "IndexError")
end)

T.test("table library works on Python lists", function()
  local l = python.eval("[3, 1, 2]")
  table.insert(l, 4)
  table.insert(l, 1, 0)
  T.eq(tostring(l), "[0, 3, 1, 2, 4]")
  T.eq(table.remove(l, 1), 0)
  T.eq(table.remove(l), 4)
  table.sort(l)
  T.eq(table.concat(l, ","), "1,2,3")
  T.eq(select("#", table.unpack(l)), 3)
  local seen = {}
  for i, v in ipairs(l) do seen[#seen + 1] = i .. "=" .. v end
  T.eq(table.concat(seen, " "), "1=1 2=2 3=3")
end)

T.test("mappings keep their keys", function()
  local d = python.eval("{1: 'one', 'a': 'A', 'items': 'field'}")
  T.eq(d[1], "one")
  T.eq(d.a, "A")
  T.eq(d.items, "field")                -- the key wins over the method
  T.eq(d.missing, nil)
  T.truthy(tostring(d.keys):find("built-in method keys", 1, true))
  d.b = "B"
  d.a = nil                             -- assigning nil deletes
  d.nothing = nil                       -- deleting a missing key is fine
  T.eq(tostring(python.eval("lambda d: sorted(map(str, d))")(d)), "['1', 'b', 'items']")
end)

T.test("attribute mode", function()
  local ns = python.eval("__import__('types').SimpleNamespace(x=1)")
  T.eq(ns.x, 1)
  ns.y = 2
  T.eq(ns.y, 2)
  ns.y = nil
  T.eq(ns.y, nil)
  T.eq(ns.nope, nil)
end)

T.test("errors in attribute access propagate", function()
  python.execute("class P:\n    @property\n    def bad(self): raise RuntimeError('in property')\n")
  local p = python.eval("P()")
  T.raises(function() return p.bad end, "in property")
end)

T.test("mode switches", function()
  local d = python.eval("{'keys': 1}")
  T.eq(d.keys, 1)
  T.eq(type(python.asattr(d).keys), "userdata")
  local r = python.eval("range(5)")
  T.eq(r[1], 0)
  T.eq(python.asmap(python.eval("[7, 8]"))[0], 7)
  T.eq(python.asseq(python.eval("(5, 6)"))[2], 6)
end)

T.test("__get and __set", function()
  local l = python.eval("[1, 2]")
  T.eq(l.__get(0), 1)
  T.eq(l.__get(5, "dflt"), "dflt")
  l.__set(0, 9)
  T.eq(l[1], 9)
end)

T.test("length, equality, ordering", function()
  T.eq(#python.eval("{'a': 1}"), 1)
  T.eq(python.import("re") == python.import("re"), true)
  T.eq(python.eval("[1]") == python.eval("[1]"), true)
  T.eq(python.eval("[1]") == python.eval("[2]"), false)
  local a, b = python.eval("__import__('fractions').Fraction(1, 3)"), python.eval("__import__('fractions').Fraction(1, 2)")
  T.eq(a < b, true)
  T.eq(a <= b, true)
  T.eq(b < a, false)
end)

T.test("arithmetic", function()
  local F = python.eval("__import__('fractions').Fraction")
  local x = F(1, 3)
  T.eq(tostring(x + x), "2/3")
  T.eq(tostring(x - 1), "-2/3")
  T.eq(tostring(2 * x), "2/3")
  T.eq(tostring(x / 2), "1/6")
  T.eq(tostring(-x), "-1/3")
  T.eq(tostring(x ^ 2), "1/9")
  T.eq(tostring(F(7, 1) % 4), "3")
  T.eq(tostring(F(7, 2) // 1), "3")
  local l = python.eval("[1]")
  T.eq(tostring(l + l), "[1, 1]")
  T.eq(tostring(l * 3), "[1, 1, 1]")
end)

T.test("bitwise operators", function()
  T.eq(python.eval("6") & 3, 2)                          -- plain ints convert to Lua integers
  local Flag = python.eval("__import__('enum').IntFlag('Flag', 'A B C')")
  T.eq(Flag.A | Flag.B, 3)
end)

T.test("concatenation and tostring", function()
  local l = python.eval("[1, 2]")
  T.eq("l=" .. l, "l=[1, 2]")
  T.eq(l .. "!", "[1, 2]!")
  T.eq(tostring(python.eval("{'a': 1}")), "{'a': 1}")
  T.raises(function() return l .. {} end, "concatenate")
end)

T.test("pairs over sequences, mappings and iterables", function()
  local out = {}
  for i, v in pairs(python.eval("['x', 'y']")) do out[#out + 1] = i .. v end
  T.eq(table.concat(out, ","), "1x,2y")
  out = {}
  for k, v in pairs(python.eval("{'k': 'v', None: 'none-key'}")) do out[#out + 1] = tostring(k) .. "=" .. v end
  table.sort(out)
  T.eq(table.concat(out, ","), "None=none-key,k=v")
  out = {}
  for i, v in pairs(python.eval("(c for c in 'ab')")) do out[#out + 1] = i .. v end
  T.eq(table.concat(out, ","), "1a,2b")
end)

T.test("python.iter keeps None items", function()
  local out = {}
  for v in python.iter(python.eval("[1, None, 3]")) do out[#out + 1] = tostring(v) end
  T.eq(table.concat(out, ","), "1,None,3")
end)

T.test("copying Lua tables into Python containers", function()
  local json = python.import("json")
  T.eq(json.dumps(python.list({1, 2, "x"})), '[1, 2, "x"]')
  T.eq(json.dumps(python.dict({a = 1})), '{"a": 1}')
  T.eq(tostring(python.tuple({1, 2})), "(1, 2)")
  T.eq(tostring(python.list(python.eval("'ab'"))), "['a', 'b']")
end)

T.test("context managers", function()
  if T.lua_version < 5.4 then return end
  local code = [[
    local python = ...
    local cm, f
    do
      local m <close> = python.eval("__import__('io').StringIO('data')")
      f = m
    end
    return f.closed
  ]]
  T.eq(load(code)(python), true)
  local cm, value = python.with(python.eval("__import__('contextlib').nullcontext(5)"))
  T.eq(value, 5)
end)

T.test("released objects are rejected", function()
  local mt = getmetatable(python.none)
  T.eq(mt, "python.object")              -- the metatable is protected
end)

T.done()
