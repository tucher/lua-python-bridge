local T = require "harness"
local python = require "python"

T.test("module basics", function()
  T.truthy(python.version:find("lua-python-bridge", 1, true))
  T.eq(tostring(python.none), "None")
  T.eq(python.eval("None"), nil)
end)

T.test("integers stay integers, floats stay floats", function()
  local v = python.eval("1")
  T.eq(math.type(v), "integer")
  T.eq(tostring(v), "1")
  T.eq(math.type(python.eval("2.0")), "float")
  T.eq(python.eval("-(2**63)"), math.mininteger)
  T.eq(python.eval("2**63 - 1"), math.maxinteger)
end)

T.test("ints outside lua_Integer become floats", function()
  local v = python.eval("2**70")
  T.eq(math.type(v), "float")
  T.eq(v, 2.0^70)
  T.eq(python.eval("-(2**64)"), -(2.0^64))
  -- No pending OverflowError afterwards.
  T.eq(python.eval("1 + 1"), 2)
end)

T.test("numbers.Integral types convert by value, other numbers stay objects", function()
  python.execute("import numbers, fractions\nclass MyInt:\n    def __init__(self, v): self.v = v\n    def __index__(self): return self.v\nnumbers.Integral.register(MyInt)")
  local v = python.eval("MyInt(7)")
  T.eq(math.type(v), "integer")
  T.eq(v, 7)
  T.eq(type(python.eval("fractions.Fraction(1, 3)")), "userdata")
end)

T.test("Lua numbers become int or float", function()
  local f = python.eval("lambda x: type(x).__name__")
  T.eq(f(3), "int")
  T.eq(f(3.0), "float")
  T.eq(f(math.maxinteger), "int")
  T.eq(f(0/0), "float")
  T.eq(f(math.huge), "float")
  T.eq(python.eval("lambda x: x")(math.maxinteger), math.maxinteger)
end)

T.test("strings and bytes", function()
  local f = python.eval("lambda x: (type(x).__name__, x)")
  T.eq(tostring(f("héllo")), "('str', 'héllo')")
  T.eq(tostring(f("\xff\xfe")), "('bytes', b'\\xff\\xfe')")
  T.eq(python.eval("'h\\u00e9llo'"), "héllo")
  T.eq(python.eval("b'\\x00\\x01'"), "\0\1")
  T.eq(tostring(python.eval("type")(python.asbytes("abc"))), "<class 'bytes'>")
end)

T.test("lone surrogates do not crash", function()
  T.eq(python.eval("'a\\udc80b'"), "a\x80b")      -- surrogateescape round trip
  T.raises(function() return python.eval("'\\ud800'") end, "UnicodeEncodeError")
end)

T.test("booleans and nil", function()
  T.eq(python.eval("True"), true)
  T.eq(python.eval("False"), false)
  local f = python.eval("lambda *a: [type(x).__name__ for x in a]")
  T.eq(tostring(f(true, false, nil)), "['bool', 'bool', 'NoneType']")
end)

T.test("Lua values come back unchanged", function()
  local t, fn = {}, function() end
  local id = python.eval("lambda x: x")
  T.eq(id(t), t)
  T.eq(id(fn), fn)
  T.eq(id(coroutine.create(fn)) ~= nil, true)
end)

T.done()
