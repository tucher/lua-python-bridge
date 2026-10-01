import pytest

import lua


def test_positional_calls():
    f = lua.eval("function(...) return select('#', ...), ... end")
    assert f() == 0
    assert f(1, None, "x") == (3, 1, None, "x")


def test_many_arguments():
    f = lua.eval("function(...) return select('#', ...) end")
    assert f(*range(1000)) == 1000


def test_keyword_arguments_become_a_trailing_table():
    f = lua.eval("function(a, opts) return a, opts and opts.x, opts and opts.y end")
    assert f(1) == (1, None, None)
    assert f(1, x=2, y="z") == (1, 2, "z")


def test_tablecall():
    f = lua.tablecall(lua.eval("function(t) return #t, t.name end"))
    assert f(1, 2, 3, name="n") == (3, "n")
    assert f() == (0, None)
    with pytest.raises(TypeError):
        lua.tablecall(print)


def test_callbacks_in_both_directions():
    g = lua.globals()
    g.py_add = lambda a, b: a + b
    assert lua.eval("py_add(2, 3)") == 5
    twice = lua.eval("function(f, x) return f(f(x)) end")
    assert twice(lambda x: x * 3, 2) == 18


def test_lua_errors():
    with pytest.raises(lua.LuaError) as info:
        lua.execute("error('plain message')")
    assert "plain message" in str(info.value)
    assert "stack traceback" in info.value.traceback
    with pytest.raises(lua.LuaError) as info:
        lua.execute("error({code = 42})")
    assert info.value.value["code"] == 42
    with pytest.raises(lua.LuaError, match="syntax error|unexpected symbol|expected"):
        lua.execute("this is not lua")
    with pytest.raises(lua.LuaError) as info:
        lua.execute("error(setmetatable({}, {__tostring = function() return 'described' end}))")
    assert str(info.value) == "described"


def test_python_exceptions_keep_their_type():
    class MyErr(Exception):
        pass

    def boom():
        raise MyErr("details")

    lua.globals().boom = boom
    with pytest.raises(MyErr, match="details"):
        lua.execute("boom()")


def test_system_exit_propagates_through_lua():
    import sys
    lua.globals().ex = sys.exit
    with pytest.raises(SystemExit) as info:
        lua.execute("ex(5)")
    assert info.value.code == 5


def test_pcall_in_lua_catches_python_errors():
    lua.globals().raiser = lambda: int("x")
    ok, msg = lua.eval("pcall(raiser)")
    assert ok is False
    assert "ValueError" in lua.eval("function(e) return tostring(e) end")(msg)


def test_lua_error_values_round_trip():
    relay = lua.eval("function(f) return f() end")
    t = lua.eval("{code = 1}")
    fail = lua.eval("function(t) error(t) end")
    lua.globals().t = t
    with pytest.raises(lua.LuaError) as info:
        relay(lambda: fail(t))
    assert info.value.value == t


def test_require():
    assert lua.require("string").upper("x") == "X"
    with pytest.raises(lua.LuaError, match="no_such_module"):
        lua.require("no_such_module")


def test_python_module_inside_lua():
    assert lua.eval("require('python').eval('6 * 7')") == 42
    assert lua.eval("python.eval('[1, 2]')[2]") == 2
