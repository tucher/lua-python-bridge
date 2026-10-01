# lua-python-bridge

Call Python from Lua and Lua from Python, in one process.

- **Python package** `lua-python-bridge` → `import lua` embeds Lua 5.5 in Python.
- **Lua rock** `lua-python-bridge` → `require "python"` embeds Python in Lua 5.3, 5.4 or 5.5.

The Lua module loads libpython at runtime, so one build works with any CPython 3.10 or newer.
The Python wheel uses the stable ABI, so one wheel per platform covers CPython 3.10 and newer.
Based on [Lunatic Python](https://labix.org/lunatic-python) by Gustavo Niemeyer.

## Install

```sh
pip install lua-python-bridge        # Python side: import lua
luarocks install lua-python-bridge   # Lua side: require "python"
```

From a source checkout: `pip install .` and `luarocks make rockspec/lua-python-bridge-scm-1.rockspec`.

## Quick start

```lua
local python = require "python"

local re = python.import "re"
print(re.match("(\\w+) (\\w+)", "hello world").group(2))   --> world

local np = python.import "numpy"
local a = np.arange(5)
print(a[1], #a, a * 2)                                      --> 0   5   [0 2 4 6 8]

local json = python.import "json"
print(json.dumps(python.dict{answer = 42}, python.kw{indent = 2}))
```

```python
import lua

lua.execute("function greet(name) return 'hello ' .. name end")
print(lua.globals().greet("world"))                         # hello world

t = lua.eval("{10, 20, 30, name = 'x'}")
print(t[1], t.name, len(t), dict(t))                        # 10 x 3 {1: 10, 2: 20, 3: 30, 'name': 'x'}
print(list(lua.seq(t)))                                     # [10, 20, 30]
```

## Finding Python (Lua host)

`require "python"` looks for libpython in this order:

1. A Python already loaded in the process (the host embeds Python itself).
2. `LUA_PYTHON_LIBPYTHON`: the full path of the library to load.
3. The interpreter named by `LUA_PYTHON_EXECUTABLE`, or else `python3`, `python` (and `py -3` on
   Windows) from `PATH`. The bridge asks it where its libpython is, and adopts its `sys.path`,
   `sys.prefix` and `sys.executable`. Pointing `LUA_PYTHON_EXECUTABLE` at a virtualenv's Python gives
   you that virtualenv.

If nothing works, `require` fails with a Lua error that lists what was tried. A Python built without
a shared libpython (for example a default pyenv build) cannot be embedded; use one built with
`--enable-shared` or install your distribution's `libpython3.X` package.

Python is started without its signal handlers, so Ctrl-C keeps working as the host intends. Its
buffered output is flushed when the Lua state closes, and on Linux and macOS Python is shut down
when the process exits, so `atexit` handlers run (not on Windows, where that is unsafe from a DLL).

## Values

| Lua → Python | | Python → Lua | |
|---|---|---|---|
| `nil` | `None` | `None` | `nil` |
| boolean | `bool` | `bool` | boolean |
| integer | `int` | `int` | integer (a float if it does not fit in a Lua integer) |
| float | `float` | `float` | float |
| string | `str` if valid UTF-8, else `bytes` | `str`, `bytes` | string |
| a wrapped Python object | the original object | a wrapped Lua value | the original Lua value |
| table, function, userdata, thread | `lua.LuaObject` | anything else | a Python object (userdata) |

Subclasses of `int`, `float` and `str` convert by value, and so do other `numbers.Integral` types
such as numpy integers. `python.asbytes(s)` passes a Lua string to
Python as `bytes`. `python.none` is Python's `None` as a Lua value, for places where `nil` cannot
go (table values, varargs).

## Indexing and iteration

Each side sees the other side's **sequences** with its own index base; **mapping keys** are never
changed.

In Lua, a Python object is accessed in one of three modes, chosen by its type:

| Mode | Used for | `o[k]` | `#o` | `pairs(o)` |
|---|---|---|---|---|
| sequence | lists, tuples, ranges, other `collections.abc.Sequence`, numpy arrays | `o[1]` is the first element; `o[-1]` the last; out of range is `nil` | `len(o)` | index, value (1-based) |
| mapping | dicts and other `collections.abc.Mapping` | the key as is | `len(o)` | key, value |
| attribute | everything else | attributes | `len(o)` | counter, item (if iterable) |

- `o[#o + 1] = v` appends and `o[#o] = nil` removes the last element, so `ipairs`, `table.insert`,
  `table.remove`, `table.sort`, `table.concat` and `table.unpack` work on Python lists.
- On a mapping, `d.k = nil` deletes the key. String keys that a mapping or sequence does not have fall
  back to attributes: `l.append(4)`, `d.items()`.
- `python.asseq(o)`, `python.asmap(o)` and `python.asattr(o)` choose the mode explicitly.
  `o.__get(k [, default])` and `o.__set(k, v)` always use plain Python item access.
- `python.iter(o)` iterates over any Python iterable (`None` items arrive as `python.none`, so they
  do not end the loop).
- `python.list(t)`, `python.tuple(t)` and `python.dict(t)` copy a Lua table into a real Python
  container, for APIs such as `json.dumps` that need one.

In Python, a Lua table is a mapping: `t[1]` is the first array element, `len(t)` is `#t`, iterating
yields keys (honouring `__pairs`), and `keys()`, `values()` and `items()` are available (a table field
of the same name takes precedence). `lua.seq(t)` gives a 0-based `MutableSequence` view of a Lua
array. `lua.table(*items, **fields)` and `lua.table_from(iterable_or_mapping)` build new tables.

Attribute access on a Lua table returns `None` for missing fields, except dunder names, which raise
`AttributeError`. Lua values compare and hash by identity (Lua's raw equality); `<` and `<=` use
Lua's `__lt` and `__le`.

## Calls

Plain calls are positional in both directions: a Lua table passed to a Python function arrives as
one argument, and a dict passed to a Lua function arrives as one argument. Keyword arguments use the
two Lua idioms for named arguments, mirrored exactly:

| Lua idiom | Lua calls Python | Python calls Lua |
|---|---|---|
| options table: `f(a, b, opts)` | `pyf(a, b, python.kw{k = v})` → `pyf(a, b, k=v)` | `luaf(a, b, k=v)` → `luaf(a, b, {k = v})` |
| argument table: `f{a, b, k = v}` | `python.tablecall(pyf){a, b, k = v}` → `pyf(a, b, k=v)` | `lua.tablecall(luaf)(a, b, k=v)` → `luaf{a, b, k = v}` |

In an argument table, integer keys `1..n` are positional (gaps become `None`), string keys are named,
and anything else is an error. A Lua table cannot hold trailing `nil`s, so trailing `None` positional
arguments are lost in that form.

Lua functions return `None`, their single result, or a tuple of results. Python functions return
one value to Lua.

## Errors

- A Python exception raised into Lua is a Lua error whose value is the exception object:
  `tostring(err)` gives Python's formatted traceback, and `err.args` etc. are accessible.
  `SystemExit` and `KeyboardInterrupt` are ordinary errors that `pcall` can catch; they do not end the
  Lua host.
- A Lua error raised into Python is `lua.LuaError`, with the original error value in `.value` and the
  Lua traceback in `.traceback` (also added as an exception note on Python 3.11+).
- Errors keep their identity across round trips: a Python exception that travels through Lua comes
  back as the same exception; a Lua error value that travels through Python comes back as the same
  value.

## Threads

The GIL is never held while the bridge runs Lua code, so Python threads keep running while Lua
works, and Lua code can run on several OS threads (each with its own Lua state) in parallel.

A Lua state is still single-threaded. The bridge serializes access to it:

- Any thread may call into Python from Lua.
- A Python thread may call a Lua function while the Lua side is inside a call to Python (for example
  `t.join()`, `time.sleep`, an asyncio loop), when the state is idle in a Python host, or while the
  Lua host calls `python.serve(seconds)`, which lets waiting threads in for that long. Otherwise the
  Python thread waits.

## API

**Lua (`local python = require "python"`):** `execute(code [, globals [, locals]])`,
`eval(expr [, globals [, locals]])`, `import(name)`, `builtins()`, `globals()`, `locals()`,
`asattr(o)`, `asmap(o)`, `asseq(o)`, `asbytes(s)`, `asfunc(o)`, `iter(o)`, `list(t)`, `tuple(t)`,
`dict(t)`, `kw(t)`, `tablecall(f)`, `with(cm)`, `serve(seconds)`, `none`, `version`.

`python.with(cm)` calls `cm.__enter__()` and returns `cm` and its result; with Lua 5.4's
`local x <close> = ...`, Python objects call `__exit__` when the variable goes out of scope.

**Python (`import lua`):** `execute(code)`, `eval(expr)`, `globals()`, `require(name)`,
`table(*items, **fields)`, `table_from(obj)`, `seq(t)`, `tablecall(f)`, `LuaError`, `LuaObject`,
`LuaSequence`, `__version__`, `LUA_VERSION`. See `lua.pyi` for details.

## Binary Lua modules in Python

Inside Python, `require` can load compiled Lua modules (LuaRocks C modules) built for Lua 5.5:
`luarocks --lua-version 5.5 install ...`. On Windows such modules must be linked against
`lua55.dll`, which the wheel ships next to the extension.

## Building and embedding

One CMake project builds either artifact:

```sh
# Lua module: point it at the host's Lua headers
cmake -S . -B build -DLUA_INCLUDE_DIR=/usr/include/lua5.4 && cmake --build build
# Python extension
pip install .
```

To embed the Lua module in an application's CMake build, add this directory and set
`LPB_LUA_TARGET` to the application's Lua target; the host then calls `luaopen_python` (for example
through `luaL_requiref`). Options: `LPB_BUILD_LUA_MODULE`, `LPB_BUILD_PYTHON_MODULE`,
`LPB_LUA_TARGET`, `LUA_INCLUDE_DIR`, `LUA_LIBRARIES` (Windows), `LPB_PYTHON_HEADERS=vendored|system`,
`LPB_BUILD_TESTS`.

Tests: `pytest tests/python` (Python host), `lua tests/lua/run.lua` with the module on `LUA_CPATH`
(Lua host), and `build/lpb_embed_test` (C host with several states and threads, `-DLPB_BUILD_TESTS=ON`).

## Limitations

- CPython only, 3.10 or newer, with the GIL (free-threaded builds are refused).
- Python sub-interpreters are not supported.
- Reference cycles that pass through both languages are not collected: neither garbage collector
  can see across the bridge.
- `bytes` that happen to be valid UTF-8 come back from Lua as `str`.
- A Lua object can only be passed back into the Lua state it came from.
- The `lunatic-python` rock also provides `require "python"`; do not install both.

## License

LGPL-2.1-or-later, see `LICENSE` and `NOTICE`. The bundled Lua is MIT-licensed.
