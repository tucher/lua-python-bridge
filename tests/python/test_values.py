import math
import sys

import pytest

import lua


def test_module_metadata():
    assert lua.__version__.count(".") == 2
    assert lua.LUA_VERSION.startswith("Lua 5.")
    assert issubclass(lua.LuaError, Exception)


def test_numbers():
    assert lua.eval("1") == 1 and type(lua.eval("1")) is int
    assert type(lua.eval("1.0")) is float
    assert lua.eval("math.maxinteger") == 2**63 - 1
    assert lua.eval("1e308 * 10") == math.inf
    f = lua.eval("function(x) return math.type(x) end")
    assert f(3) == "integer"
    assert f(3.0) == "float"
    assert f(2**63 - 1) == "integer"
    assert f(2**70) == "float"          # outside lua_Integer: a float, like Lua's own literals
    assert f(True) is None


def test_strings_and_bytes():
    assert lua.eval("'héllo'") == "héllo"
    assert lua.eval("'\\xff\\xfe'") == b"\xff\xfe"      # not UTF-8: bytes
    echo = lua.eval("function(x) return x end")
    assert echo("abc") == "abc"
    assert echo(b"abc") == "abc"
    assert echo("a\udc80b") == b"a\x80b"          # surrogateescape out, bytes back
    assert echo(b"\x00\x01") == "\x00\x01"


def test_surrogates_round_trip():
    echo = lua.eval("function(x) return #x end")
    assert echo("a\udc80b") == 3                          # surrogateescape


def test_none_and_bools():
    assert lua.eval("nil") is None
    assert lua.eval("true") is True and lua.eval("false") is False
    assert lua.eval("function(x) return x == nil end")(None) is True


def test_multiple_results():
    assert lua.eval("1, 2, 3") == (1, 2, 3)
    assert lua.execute("return") is None
    assert lua.execute("return 'a', nil") == ("a", None)


def test_python_values_come_back_unchanged():
    obj = object()
    echo = lua.eval("function(x) return x end")
    assert echo(obj) is obj
    d = {}
    assert echo(d) is d


def test_table_constructors():
    t = lua.table(1, 2, x=3)
    assert t[1] == 1 and t[2] == 2 and t.x == 3
    assert len(t) == 2
    t2 = lua.table_from({"a": 1, 2: "b"})
    assert t2.a == 1 and t2[2] == "b"
    t3 = lua.table_from(x * x for x in range(3))
    assert list(lua.seq(t3)) == [0, 1, 4]
    with pytest.raises(ValueError):
        lua.table_from({None: 1})


def test_lua_eval_requires_text():
    with pytest.raises(TypeError):
        lua.eval(42)
    assert lua.eval(b"1 + 1") == 2
    with pytest.raises(lua.LuaError):
        lua.execute("\x1bLua")          # binary chunks are refused


@pytest.mark.skipif(sys.version_info >= (3, 12), reason="bools are immortal on 3.12+")
def test_comparisons_do_not_steal_references():
    g = lua.globals()
    before = sys.getrefcount(True), sys.getrefcount(False)
    for _ in range(10000):
        g == g
        g != g
        g == 5
    assert (sys.getrefcount(True), sys.getrefcount(False)) == before
