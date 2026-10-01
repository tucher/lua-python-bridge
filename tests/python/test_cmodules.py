"""Binary Lua modules loaded by `require` inside the Python host (PLAN.md 5.9)."""
import os
import shutil
import subprocess
import sys

import pytest

import lua

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LUA_INCLUDE = os.path.join(ROOT, "third_party", "lua-5.5.1", "src")

MODULE_SOURCE = r"""
#include "lua.h"
#include "lauxlib.h"

static int answer(lua_State *L) {
    lua_pushinteger(L, 42 + luaL_optinteger(L, 1, 0));
    return 1;
}

int luaopen_lpbtestmod(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, answer);
    lua_setfield(L, -2, "answer");
    return 1;
}
"""


@pytest.mark.skipif(sys.platform == "win32", reason="needs a C compiler and a lua55.dll import library")
@pytest.mark.skipif(not shutil.which("cc"), reason="no C compiler")
@pytest.mark.skipif(not os.path.isdir(LUA_INCLUDE), reason="Lua headers not available")
def test_binary_lua_module(tmp_path):
    src = tmp_path / "lpbtestmod.c"
    src.write_text(MODULE_SOURCE)
    out = tmp_path / "lpbtestmod.so"
    flags = ["-bundle", "-undefined", "dynamic_lookup"] if sys.platform == "darwin" else ["-shared", "-fPIC"]
    subprocess.run(["cc", *flags, "-I", LUA_INCLUDE, str(src), "-o", str(out)], check=True)
    lua.execute("package.cpath = %r .. ';' .. package.cpath" % str(tmp_path / "?.so"))
    assert lua.eval("require('lpbtestmod').answer(1)") == 43
