rockspec_format = "3.0"
package = "lua-python-bridge"
version = "scm-1"

source = {
   url = "git+https://github.com/tucher/lua-python-bridge.git",
}

description = {
   summary = "Python inside Lua (require 'python'), and Lua inside Python (import lua)",
   detailed = [[
      Call Python from Lua and Lua from Python. The Lua module loads libpython at runtime, so
      one build works with any CPython 3.10 or newer; it finds the interpreter on PATH (or
      LUA_PYTHON_EXECUTABLE / LUA_PYTHON_LIBPYTHON) and adopts its sys.path, virtualenvs included.
   ]],
   homepage = "https://github.com/tucher/lua-python-bridge",
   license = "LGPL-2.1-or-later",
}

dependencies = {
   "lua >= 5.3, < 5.6",
}

build = {
   type = "cmake",
   variables = {
      CMAKE_BUILD_TYPE = "Release",
      CMAKE_INSTALL_PREFIX = "$(PREFIX)",
      LPB_BUILD_LUA_MODULE = "ON",
      LPB_BUILD_PYTHON_MODULE = "OFF",
      LUA_INCLUDE_DIR = "$(LUA_INCDIR)",
      LUA_LIBRARIES = "$(LUA_LIBDIR)/$(LUALIB)",
      LPB_LUA_MODULE_INSTALL_DIR = "$(LIBDIR)",
   },
}

test = {
   type = "command",
   script = "tests/lua/run.lua",
}
