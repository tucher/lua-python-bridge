import collections.abc
import copy

import pytest

import lua


@pytest.fixture
def t():
    return lua.eval("{10, 20, 30, name = 'x'}")


def test_item_access(t):
    assert t[1] == 10
    assert t["name"] == "x"
    assert t[99] is None
    t[4] = 40
    t["new"] = "v"
    assert lua.eval("function(t) return t[4], t.new end")(t) == (40, "v")
    del t["new"]
    assert t["new"] is None


def test_attribute_protocol(t):
    assert t.name == "x"
    assert t.missing is None
    assert not hasattr(t, "__missing_dunder__")
    with pytest.raises(AttributeError):
        t.__foo__
    t.attr = 5
    assert t["attr"] == 5
    del t.attr
    assert t.attr is None


def test_fields_shadow_methods():
    t = lua.eval("{keys = 'field'}")
    assert t.keys == "field"
    assert lua.eval("{a = 1}").keys() == ["a"]


def test_dict_and_list_conversions(t):
    assert dict(t) == {1: 10, 2: 20, 3: 30, "name": "x"}
    assert sorted(map(str, t)) == ["1", "2", "3", "name"]
    assert sorted(map(str, t.values())) == ["10", "20", "30", "x"]
    assert (1, 10) in t.items()


def test_functions_are_not_indexable():
    f = lua.eval("print")
    assert callable(f)
    assert not hasattr(f, "__name__")
    with pytest.raises(AttributeError):
        f.x = 1
    with pytest.raises(TypeError):
        f[1]
    with pytest.raises(TypeError):
        len(f)


def test_len_and_bool():
    assert len(lua.eval("{1, 2, 3}")) == 3
    assert len(lua.eval("setmetatable({}, {__len = function() return 7 end})")) == 7
    assert bool(lua.eval("{}"))
    assert bool(lua.eval("print"))


def test_equality_and_hash():
    g = lua.globals()
    assert g == lua.globals()
    assert g == g._G
    assert g != lua.eval("{}")
    assert g != 5 and not (g == 5)
    assert {g: 1}[lua.globals()] == 1
    assert len({lua.eval("{}"), lua.eval("{}")}) == 2


def test_ordering_uses_lua_metamethods():
    mk = lua.eval("""function(v)
        return setmetatable({v = v}, {__lt = function(a, b) return a.v < b.v end,
                                      __le = function(a, b) return a.v <= b.v end})
    end""")
    a, b = mk(1), mk(2)
    assert a < b and a <= b and b > a and b >= a
    assert not (b < a)
    with pytest.raises(lua.LuaError):
        lua.eval("{}") < lua.eval("{}")


def test_repr_and_tostring():
    assert repr(lua.eval("{}")).startswith("<Lua table at 0x")
    assert repr(lua.eval("print")).startswith("<Lua function")
    t = lua.eval("setmetatable({}, {__tostring = function() return 'custom' end})")
    assert str(t) == "custom"
    bad = lua.eval("setmetatable({}, {__tostring = function() return '\\xff' end})")
    assert str(bad) == "�"


def test_iteration_is_independent(t):
    for _ in t:
        break
    assert len(list(t)) == 4
    pairs = [(a, b) for a in lua.eval("{1, 2}") for b in lua.eval("{1, 2}")]
    assert len(pairs) == 4
    it = iter(t)
    assert len(list(it)) == 4
    assert list(it) == []


def test_large_iteration():
    lua.execute("big = {} for i = 1, 100000 do big[i] = i end")
    assert sum(1 for _ in lua.globals().big) == 100000


def test_iteration_respects_pairs():
    t = lua.eval("""setmetatable({}, {__pairs = function(t)
        local i = 0
        return function() i = i + 1 if i <= 3 then return i, i * i end end, t, nil
    end})""")
    assert list(t) == [1, 2, 3]
    assert t.items() == [(1, 1), (2, 4), (3, 9)]


def test_not_iterable():
    with pytest.raises(TypeError):
        list(lua.eval("print"))


def test_erroring_metamethods_raise_instead_of_aborting():
    lua.execute("bad = setmetatable({}, {__index = function() error('boom') end, "
                "__newindex = function() error('nope') end})")
    bad = lua.globals().bad
    with pytest.raises(lua.LuaError, match="boom"):
        bad.x
    with pytest.raises(lua.LuaError, match="nope"):
        bad.x = 1


def test_lua_objects_cannot_be_created_from_python():
    with pytest.raises(TypeError):
        type(lua.globals())()
    with pytest.raises(TypeError):
        class Sub(lua.LuaObject):
            pass


def test_copy_is_refused_cleanly():
    with pytest.raises(Exception):
        copy.copy(lua.eval("{}"))


def test_seq_view():
    t = lua.eval("{'a', 'b', 'c'}")
    s = lua.seq(t)
    assert isinstance(s, collections.abc.MutableSequence)
    assert len(s) == 3
    assert s[0] == "a" and s[-1] == "c"
    assert s[0:2] == ["a", "b"] and s[::-1] == ["c", "b", "a"] and s[5:] == []
    assert list(s) == ["a", "b", "c"]
    assert "b" in s
    with pytest.raises(IndexError):
        s[3]
    s.append("d")
    s.insert(0, "z")
    assert list(s) == ["z", "a", "b", "c", "d"]
    assert s.pop() == "d" and s.pop(0) == "z"
    s[1] = "B"
    del s[0]
    assert list(s) == ["B", "c"]
    s.extend(["x", "y"])
    assert t[4] == "y"
    s.clear()
    assert len(s) == 0 and len(t) == 0
    assert repr(lua.seq(lua.eval("{1, 2}"))) == "lua.seq([1, 2])"
    with pytest.raises(TypeError):
        lua.seq([1, 2])


def test_seq_passes_back_as_the_table():
    t = lua.eval("{1}")
    assert lua.eval("function(x) return x end")(lua.seq(t)) == t
