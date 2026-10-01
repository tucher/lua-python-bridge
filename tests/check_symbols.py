#!/usr/bin/env python3
"""Checks the dynamic symbols of the built modules (POSIX only).

    python3 tests/check_symbols.py lua-module path/to/python.so
    python3 tests/check_symbols.py python-module path/to/lua.abi3.so

Lua module: must not reference any CPython symbol directly (everything goes through the
trampolines in pyloader.c), and must export only luaopen_python.
Python module: must export PyInit_lua and the Lua C API, and nothing else.
"""
import re
import subprocess
import sys


def nm(path, *flags):
    out = subprocess.run(["nm", *flags, path], check=True, capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if parts:
            name = parts[-1]
            if sys.platform == "darwin" and name.startswith("_"):
                name = name[1:]
            names.add(name)
    return names


def exported(path):
    if sys.platform == "darwin":
        return nm(path, "-gU")
    return nm(path, "-D", "--defined-only")


def undefined(path):
    if sys.platform == "darwin":
        return nm(path, "-u")
    return nm(path, "-D", "-u")


def main():
    kind, path = sys.argv[1], sys.argv[2]
    errors = []
    exp = {s for s in exported(path) if not s.startswith(("_init", "_fini", "__bss", "_edata", "_end"))}
    if kind == "lua-module":
        py = sorted(s for s in undefined(path) if re.match(r"_?Py|_Py", s))
        if py:
            errors.append("references CPython symbols directly: %s" % ", ".join(py))
        if exp != {"luaopen_python"}:
            errors.append("exports %s, expected only luaopen_python" % sorted(exp))
    elif kind == "python-module":
        extra = sorted(s for s in exp if s != "PyInit_lua" and not re.match(r"lua(L)?_|luaopen_", s))
        if extra:
            errors.append("unexpected exports: %s" % ", ".join(extra))
        for needed in ("PyInit_lua", "lua_pcallk", "luaL_newstate", "luaopen_base"):
            if needed not in exp:
                errors.append("missing export %s" % needed)
    else:
        sys.exit("unknown kind %r" % kind)
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    print("%s: ok (%d exported symbols)" % (path, len(exp)))


if __name__ == "__main__":
    main()
