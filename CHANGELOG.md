# Changelog

## 0.1.0 (unreleased)

First release as standalone packages (`pip install lua-python-bridge`, `luarocks install
lua-python-bridge`), rewritten from Lunatic Python. Compared with Lunatic Python:

- One build per platform: the wheel uses the CPython stable ABI (3.10+), and the Lua module finds
  and loads libpython at runtime (any CPython 3.10+, virtualenvs included).
- Lua 5.3, 5.4 and 5.5 hosts; Lua 5.5 bundled in the wheel.
- No crashes reachable from scripts: protected Lua calls, balanced stacks, per-state runtimes, and
  correct reference counting.
- Errors carry their information in both directions and keep their identity on round trips;
  `sys.exit` no longer ends a Lua host.
- Integers stay integers; Python sequences are 1-based in Lua and Lua arrays get a 0-based view in
  Python; explicit keyword-argument conventions (`python.kw`, `python.tablecall`, `lua.tablecall`).
- The GIL is released while Lua runs; any thread may call into Python, and Python threads can call
  Lua callbacks safely.
