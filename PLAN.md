# lua-python-bridge: restructure and fix plan

> Temporary working document. **Status: P0–P6 implemented and tested on macOS; P7 prepared (CI, cibuildwheel, rockspec), not yet published.** Last updated 2026-10-01.
>
> - ✔ = reproduced in a scratch build: macOS arm64, Lua 5.4.7 built with `LUA_USE_APICHECK`, Homebrew CPython 3.10–3.14, and a stand-in for `dylib.hpp`.
> - ◇ = from reading the code. This includes the Linux- and Windows-only paths, which could not be run here.
> - File:line references point at the **baseline layout** (before P1). They stay valid via the P0 baseline commit.

---

## 1. Goals

- One source tree ships two standard packages:
  - **Python package** (pip): `import lua` embeds Lua in Python.
  - **Lua rock** (luarocks): `require "python"` embeds Python in Lua.
- Stay embeddable into a host application's own CMake build.
- No crashes or undefined behaviour reachable from script code.
- Semantics that feel natural from both languages, and are documented.
- Self-contained tests for both directions, with every known bug as a regression test.

**Non-goals:** LuaJIT / Lua 5.1–5.2, free-threaded CPython, Python sub-interpreters, PyPy.

---

## 2. Key decisions

| Topic | Decision |
|---|---|
| libpython for the Lua rock | **Loaded at runtime.** One binary per platform works with any CPython ≥ 3.10 (GIL builds). Compiled against the vendored 3.10 headers with `Py_LIMITED_API=0x030A0000`. |
| Lua versions | The rock supports host Lua **5.3, 5.4, 5.5**. The wheel bundles **Lua 5.5** (vendored source). |
| Naming | The product-specific prefix is removed **everywhere** (code, env vars, messages, docs), and `grep -ri` must come back clean. Names follow §3.2. |
| Call conventions | Plain calls are positional in both directions. Named arguments go through two mirrored Lua idioms: the options table and the argument table (§5.7). |
| Index bases | Each side sees the other side's **sequences** with its own index base: 1-based in Lua, 0-based in Python. Mapping keys are never translated (§5.6). |
| Missing keys on Lua objects, seen from Python | Attribute lookup order: (1) the Lua value, if not nil; (2) a Python-side attribute of the type (methods such as `keys()/values()/items()`, so `dict(t)` works, plus dunders); (3) a dunder name raises `AttributeError`; (4) any other name returns `None`. Item access `t[k]` returns `None` for a missing key. |
| Python `int` outside the `lua_Integer` range | Converted to a Lua float, as Lua does for its own overflowing integer literals. |
| GIL | **The GIL is never held while Lua code runs**, in either host (§5.8). |
| Lua binary modules inside a Python host | Supported (§5.9). |

---

## 3. Target shape

### 3.1 Repository layout

```
CMakeLists.txt                      single project; builds either or both artifacts
pyproject.toml                      scikit-build-core → wheel (cp310-abi3); cibuildwheel settings
rockspec/lua-python-bridge-scm-1.rockspec   luarocks, build.type = "cmake"
src/
  bridge.h                          shared internal declarations
  runtime.c                         runtimes, ownership protocol, crossings in both directions (§5.3, §5.8)
  convert.c                         value conversion and error bridging (§5.4, §5.5)
  pythoninlua.c                     Python objects inside Lua   (Lua module "python")
  luainpython.c                     Lua objects inside Python   (Python module "lua")
  pyloader.c                        libpython discovery, loading and trampolines (rock build only, §5.1, §5.2)
  pyapi_list.h, pyapi.h             generated list of every CPython symbol used (§5.1)
  luacompat.h, lpb_threads.h        Lua 5.3 / 5.4 / 5.5 differences; mutex, condition variable, TLS
tools/gen_pyapi.py                  regenerates pyapi_list.h / pyapi.h from the vendored headers
third_party/
  lua-5.5.1/                        Lua sources compiled into the wheel
  python3.10-headers/               one header tree + pyconfig/{macos,linux,windows}/pyconfig.h
python/lua.pyi                      type stubs
tests/python/                       pytest (Python host)
tests/lua/                          plain-Lua runner (Lua host); each test file in its own process
tests/embed/                        small C host: several lua_States, threads, close and reopen
tests/check_symbols.py              checks imported and exported symbols of both binaries
.github/workflows/ci.yml
LICENSE (LGPL-2.1, inherited from lunatic-python), NOTICE (Lua: MIT, Python headers: PSF)
README.md
```

**Removed:**
- `lua_python_binding/` (merged into the top-level CMake).
- The `dylib` dependency, `hacks.h`, and `dynamic_python_interface.cpp`, replaced by `pyloader.c`. The bridge becomes pure C.
- The network-dependent `python/test*.py` and `test.lua`.
- The upstream tests and README.

### 3.2 Names

| Thing | Current | New |
|---|---|---|
| pip distribution / rock | — | `lua-python-bridge`: free on PyPI and LuaRocks (checked 2026-10-01) |
| Python module | `lua_python_binding` | `lua`: lunatic-python's canonical name, symmetric with `require "python"`; no PyPI distribution uses it |
| Lua module | `python` | `python` |
| Python types | `lua_python_binding.lua_python_binding_custom` | `lua.LuaObject`, `lua.LuaSequence` (view, §5.6), `lua.LuaError`; the iterator type stays internal |
| Lua metatable | `POBJECT` | `python.object` (also its `__name`) |
| Lua mode switches | `python.asindx`, `python.asattr` | `python.asmap`, `python.asseq`, `python.asattr` (§5.6) |
| libpython override | product-prefixed `…_PYTHON_LIBRARY_PATH` + `…_PYTHON_LIBRARY_NAME` | `LUA_PYTHON_LIBPYTHON` (full path to the library) |
| Interpreter to query | — | `LUA_PYTHON_EXECUTABLE` (default `python3`, then `python`) |
| C symbols | `LuaState`, `LuaConvert`, `Lua_run`, `Py_Lua_Bridge_*`, … all exported | `lpb_*`, all hidden |
| CMake options | — | `LPB_BUILD_PYTHON_MODULE`, `LPB_BUILD_LUA_MODULE`, `LPB_LUA_TARGET`, `LPB_PYTHON_HEADERS=vendored\|system` |

The existing LuaRocks package `lunatic-python` (1.0-1, by leso-kn) is a build of the same upstream, so it most likely also provides `require "python"`. Installing both would conflict; document it.

### 3.3 Build matrix

| Artifact | Lua | Python | Exports | Output |
|---|---|---|---|---|
| Python wheel | vendored Lua 5.5 compiled in (as `lua55.dll` on Windows, §5.9) | linked normally (`Python_add_library(... USE_SABI 3.10 WITH_SOABI)`); no runtime loading | `PyInit_lua` + the Lua C API (§5.9) | `lua.abi3.so` / `lua.pyd` |
| Lua rock | the host's Lua: not linked on Linux; `-undefined dynamic_lookup` on macOS; import lib on Windows | vendored 3.10 headers + runtime loading (`pyloader.c`) | `luaopen_python` only | `python.so` / `python.dll` |
| Host-embedded | `LPB_LUA_TARGET=<target>` (e.g. the host's `lua` target); `LUA_INCLUDE_DIR`/`LUA_LIBRARIES` as fallback | as the rock | as the rock | same as the rock |

**Common to all:**
- `Py_LIMITED_API=0x030A0000`, so one compiled artifact per platform serves every CPython ≥ 3.10.
- Compiled with `-fvisibility=hidden`. Exports are controlled by a version script (Linux) or `-exported_symbols_list` (macOS), plus `-Wl,--exclude-libs,ALL` on Linux.

**Windows:**
- The rock defines `Py_NO_ENABLE_SHARED`. That avoids `dllimport` declarations and the `#pragma comment(lib, …)` auto-link.
- Lua is built with `LUA_BUILD_AS_DLL`; `LUA_LIB` is set only when compiling Lua's own sources.

**Warnings:** must be clean with `-Wall -Wextra -Wcast-function-type-strict`.

### 3.4 Supported platforms and versions

- **Python:** CPython 3.10–3.14 (3.15 once released), GIL builds only.
- **Lua:** host Lua 5.3/5.4/5.5 for the rock; Lua 5.5 inside the wheel.
- **OS:** macOS arm64/x86_64, Linux x86_64/aarch64 (32-bit best-effort), Windows x64.
- **Direction:** both Lua-hosted and Python-hosted.

---

## 4. Problems and actions

Format: **ID: problem.** Evidence. → Action. *(phase, see §6)*

### 4.A Crashes, undefined behaviour, memory safety

- **A1: Comparing Lua objects from Python steals references and gives wrong results.**
  - `LuaObject_richcmp` returns `Py_True`/`Py_False` without `Py_INCREF`, and returns `False` for any non-Lua operand ([luainpython.c:337](python/lunatic-python/src/luainpython.c#L337), [:348](python/lunatic-python/src/luainpython.c#L348)).
  - ✔ The process aborts after about 165 comparisons on 3.10. 3.12+ hides the refcount damage because bools are immortal there.
  - ✔ `t != 5` evaluates to `False`.
  - → Return new references. Return `NotImplemented` for non-Lua operands. Implement `==`/`!=` with `lua_rawequal`, which stays consistent with the hash in B9. Implement `<`/`<=` with a protected `lua_compare`. *(P3)*
- **A2: The Lua stack leaks and overflows when driven from Python.**
  - `LuaObject_iternext` never pops the table, so every `next()` leaks one slot ([luainpython.c:358-384](python/lunatic-python/src/luainpython.c#L358-L384)).
  - `richcmp` leaves its result on the stack.
  - `LuaCall` pushes arguments without `lua_checkstack` ([:124-139](python/lunatic-python/src/luainpython.c#L124-L139)).
  - Error messages are left on the stack.
  - ✔ Iterating 200 keys, or calling `f(*range(200))`, trips Lua's API-check assertion. ✔ With a normal Lua build, iterating 100k keys segfaults.
  - → Stack discipline (§5.3). *(P3)*
- **A3: Lua API calls made from Python are not protected.**
  - `lua_gettable`/`lua_settable` (which can run metamethods), `luaL_len`, `luaL_callmeta("__tostring")`, `lua_next`, `luaL_ref` and `lua_newuserdata` all run outside `lua_pcall`.
  - ✔ An erroring `__index`, or `len(lua_function)`, gives `PANIC: unprotected error` and aborts a Python host. In a Lua host the same error long-jumps over live Python frames.
  - → Protected calls (§5.3); Lua errors become `LuaError` (§5.4). *(P3)*
- **A4: Building arguments from a table is broken** ([pythoninlua.c:116-160](python/lunatic-python/src/pythoninlua.c#L116-L160)).
  - ✔ A nil gap (`{a, nil, c}`) leaves a NULL in the argument tuple, and the call segfaults.
  - ✔ Key `0`, a negative key or `1.5` makes `PyTuple_SetItem` fail. The error stays pending and the call proceeds anyway.
  - ✔ Numeric-string keys (`["1"]`) count as positional, because `lua_isnumber` accepts them.
  - Other key types are silently ignored, and `%d` is given a `lua_Integer`.
  - → The argument-table rules in §5.7, as one parser shared by `python.tablecall` and `python.kw`. *(P3)*
- **A5: Converting a Python `str` to a Lua string ignores failures** ([pythoninlua.c:74-83](python/lunatic-python/src/pythoninlua.c#L74-L83)).
  - When UTF-8 encoding fails (a lone surrogate), `PyBytes_AsStringAndSize` is called on the `str`. That also fails, and an uninitialized pointer is passed to `lua_pushlstring`.
  - ✔ SIGBUS on `python.eval[['a\udc80b']]`.
  - → Use `PyUnicode_AsUTF8AndSize`. On failure, encode with `surrogateescape`; if that fails too, raise a Lua error. Check every conversion result. *(P3)*
- **A6: One global `LuaState` serves every Lua state.**
  - It is set by whichever thread first runs `luaopen_python` ([pythoninlua.c:636](python/lunatic-python/src/pythoninlua.c#L636)) and used for every Python→Lua operation.
  - ✔ `require` inside a coroutine, let the coroutine be collected, and the next callback segfaults.
  - ◇ With several `lua_State`s, references from state B are resolved in state A. Passing a Lua object into another state pushes a reference from the wrong registry.
  - ◇ After the host's `lua_close` the pointer dangles, and `LuaObject_dealloc` later calls `luaL_unref` on freed memory.
  - → A runtime handle per state (§5.3). Mixing objects across runtimes raises an error. *(P3)*
- **A7: `lua_settop(LuaState, 0)` throughout the Python side** ([luainpython.c:182](python/lunatic-python/src/luainpython.c#L182), 223, 261, 353, 391, 496, 524). Also `lua_pop(L, -1)`, which is really `settop(0)`, at [:238](python/lunatic-python/src/luainpython.c#L238).
  - ◇ In a Lua host this wipes the stack frame of whichever C function called into Python. If that is `coroutine.resume`, its coroutine can lose its only reference while still running.
  - → Stack discipline (§5.3). *(P3)*
- **A8: The Lua-object type is a static `PyTypeObject` with the 3.10 layout** ([luainpython.c:416-457](python/lunatic-python/src/luainpython.c#L416-L457)).
  - It is 408 bytes, versus 416 in 3.12+. Newer CPython reads and writes `tp_watched`/`tp_versions_used` past its end.
  - ✔ In this build those bytes are `lua_module`'s refcount (3.14 reads `tp_versions_used = 0xC000`). That is harmless only because of link order.
  - → Heap types via `PyType_FromSpec`. *(P2)*
- **A9: Load problems terminate the process.**
  - `throw;` with no active exception ([dynamic_python_interface.cpp:331](python/dynamic_python_interface.cpp#L331), 345) calls `std::terminate`. ✔ A missing env var kills the host even under `pcall`.
  - A failed symbol lookup throws a C++ exception through `extern "C"` and C frames.
  - → Loader written in C that returns errors. `require "python"` fails with a normal Lua error that lists what was tried. *(P1)*
- **A10: `PyErr_Print()` is the error handling everywhere** (about 15 call sites).
  - On `SystemExit` it calls `exit()`. ✔ `sys.exit(7)` inside a Python function called under Lua `pcall` exits the Lua host with code 7.
  - ✔ In a Python host, `sys.exit` raised from a Lua→Python callback also exits instead of propagating.
  - It also writes to stderr and sets `sys.last_*`.
  - → Never call `PyErr_Print`; error bridging per §5.4. *(P3: stop exiting; P4: full bridging)*
- **A11: `LuaCall` calls `Py_DECREF(NULL)`** when converting a single return value fails ([luainpython.c:155](python/lunatic-python/src/luainpython.c#L155)). *(P3)*
- **A12: A Python object can be released twice.**
  - The metatable is reachable, so `getmetatable(o).__gc(o)` releases the object a second time.
  - `py_object_gc` doesn't clear the pointer.
  - → Set `__metatable`, clear the pointer after release, and have every method check for NULL. *(P3)*
- **A13: The Lua-object type can be instantiated and subclassed.**
  - `tp_new = PyType_GenericNew` plus `Py_TPFLAGS_BASETYPE` means `type(t)()` creates an object with `ref = 0`, and dealloc then calls `luaL_unref(…, 0)`.
  - → No `tp_new` (`Py_TPFLAGS_DISALLOW_INSTANTIATION`); not a base type. *(P2)*
- **A14: Function-pointer casts with mismatched signatures.**
  - `LuaObject_length` returns `int` but is used as `lenfunc`, so negative values and errors are misreported.
  - `LuaObject_call` takes 2 arguments but is cast to `ternaryfunc`. The dealloc and iternext functions are cast the same way.
  - ✔ Reported by `-Wcast-function-type-strict`.
  - → Correct signatures. *(P2)*
- **A15: `Lua_run` mishandles its code buffer** ([luainpython.c:471-475](python/lunatic-python/src/luainpython.c#L471-L475)).
  - `strncat` stops at an embedded NUL in the `s#` input, but `len` counts the full length, so `luaL_loadbuffer` reads uninitialized bytes.
  - `malloc` is unchecked.
  - → Build the buffer with `luaL_Buffer`. *(P3)*
- **A16: Free-threaded CPython would corrupt memory.**
  - ◇ The inline refcounting assumes the normal object header; on 3.13t/3.14t it corrupts memory.
  - → The rock refuses at load: discovery rejects `libpython3.Xt`, and `sys.abiflags` is checked right after init. pip already refuses to install `abi3` wheels on free-threaded builds. *(P1)*
- **A17: Two copies of libpython in one process.**
  - ◇ If the process already contains Python (for example, the Lua host is itself embedded in a Python app), the loader still opens the configured libpython.
  - → Probe the process first and reuse what is already loaded (§5.2). *(P1)*
- **A18: Windows `findPythonLibrary` bugs** ([dynamic_python_interface.cpp:286-320](python/dynamic_python_interface.cpp#L286-L320)).
  - ◇ It also matches `python3.dll` (the stable-ABI forwarder) and `pythoncom3xx.dll`.
  - It treats `EnumProcessModules`' byte count as an element count.
  - It writes `buf[r]` one past the buffer when a name is 1024 characters long.
  - → Replaced by the process probe in §5.2, with an exact `python3\d+(_d)?\.dll` match. *(P1)*
- **A19: libpython is unloaded at process exit.**
  - ◇ The `dylib` instance lives in a function-local `static std::shared_ptr`. Its destructor calls `dlclose`/`FreeLibrary` during static destruction, while Python threads may still be running code inside libpython.
  - → The loader never unloads libpython. *(P1)*
- **A20: Lua operators are `eval`'d lambdas cached in a process-wide `static` registry reference per operator** ([pythoninlua.c:374-402](python/lunatic-python/src/pythoninlua.c#L374-L402)).
  - That reference is invalid in any other Lua state, and after the state closes.
  - The script buffer is pushed including its trailing NULs.
  - → Use `PyNumber_*` directly. *(P3)*

### 4.B Wrong results and silent failures

- **B1: Python `int` → Lua** ([pythoninlua.c:85-86](python/lunatic-python/src/pythoninlua.c#L85-L86)).
  - ✔ `1` arrives as float `1.0` (`math.type` is `float`, `tostring` is `"1.0"`).
  - ✔ `2**70` arrives as `-1.0`, leaving an `OverflowError` pending.
  - ◇ On Windows `long` is 32-bit, so anything ≥ 2³¹ overflows.
  - → Conversion rules in §5.5. *(P4)*
- **B2: Lua number → Python** ([luainpython.c:76-84](python/lunatic-python/src/luainpython.c#L76-L84)).
  - `(long)num` is undefined behaviour for NaN, ±inf and out-of-range values.
  - Integral floats (`2.0`) become `int`.
  - Lua 5.3+ integers above 2⁵³ lose precision through `lua_tonumber`.
  - → Conversion rules in §5.5. *(P4)*
- **B3: `python.execute` / `python.eval` swallow errors** ([pythoninlua.c:443-447](python/lunatic-python/src/pythoninlua.c#L443-L447)).
  - The traceback goes to stderr, nothing is returned, and no Lua error is raised. ✔ `pcall(python.execute, "raise ValueError")` returns `true, nil`.
  - → Raise a Lua error (§5.4). Compile with `Py_CompileString` + `PyEval_EvalCode`. *(P4)*
- **B4: Python errors reach Lua as fixed strings.**
  - The messages are "error calling python function", "failed to set item", "failed importing '%s'", and so on. The real exception only goes to stderr.
  - ✔ An import failure shows in Lua only as `failed importing '_struct'`.
  - → §5.4. *(P4)*
- **B5: Lua errors reach Python badly.**
  - Calls raise a bare `Exception`; `execute`/`eval` raise `RuntimeError`.
  - ✔ Non-string error values become `"<NO ERROR CONTENT>"`.
  - There is no Lua traceback.
  - → `lua.LuaError` (§5.4). *(P4)*
- **B6: Python exception types are lost on a round trip.**
  - ✔ Python → Lua → Python: `MyErr("details")` comes back as `RuntimeError('… error calling python function')`.
  - → §5.4. *(P4)*
- **B7: The single-table call convention is ambiguous.**
  - `f{...}` always means positional + keyword arguments. ✔ `len({1,2,3})` becomes `len(1,2,3)`. A lone Lua table can never be passed positionally.
  - → The call conventions in §5.7. *(P4)*
- **B8: Keyword arguments to Lua functions are silently dropped.**
  - ✔ `f(1, 2, x=3, y=4)` reaches Lua with 2 arguments.
  - → The call conventions in §5.7. *(P4)*
- **B9: The attribute protocol of `LuaObject` misleads Python code.**
  - Missing keys return `None`, so `hasattr()` is always True. ✔ `dict(t)` fails with `'NoneType' object is not callable`.
  - ✔ On non-indexable values such as functions, attribute access raises `RuntimeError` instead of `AttributeError`, so `hasattr(luafunc, "__name__")` raises.
  - ✔ The object is unhashable (`{t: 1}` fails).
  - setattr accepts only tables, while getattr also accepts userdata and strings.
  - → The lookup order from §2. `AttributeError` on non-indexable values. `__hash__` from `lua_topointer` (identity, consistent with A1's raw `==`). setattr allowed wherever Lua allows it. *(P4)*
- **B10: Iteration state lives on the object itself** (`tp_iter = PyObject_SelfIter` plus a `refiter` field).
  - ✔ After `for k in t: break`, `list(t)` returns `[2, 3]`.
  - ✔ Nested loops over the same table are broken.
  - → A separate iterator object (§5.6). *(P4)*
- **B11: Indexing Python objects from Lua swallows errors.**
  - On *any* exception, `_p_object_index_get` falls back from `GetItem` to `GetAttr`, and it returns nil on any failure ([pythoninlua.c:274-290](python/lunatic-python/src/pythoninlua.c#L274-L290)).
  - Attribute reads return nil on any exception, including a failing `@property` ([:330-338](python/lunatic-python/src/pythoninlua.c#L330-L338)).
  - `d["keys"]` returns the bound method when the key is missing.
  - → Fall back, or return nil, only for `LookupError`/`AttributeError` (plus `TypeError` for the item→attribute fallback). Everything else propagates as a Lua error. *(P4)*
- **B12: `LuaObject_str` can return with an exception set.**
  - When `__tostring` yields invalid UTF-8, `PyUnicode_FromString` fails, the fallback path continues anyway, and Python reports `SystemError`.
  - It also reads the string after popping it.
  - → Decode with `errors="replace"`, inside a protected call. *(P4)*
- **B13: `lua.globals()` looks up the global named `_G`** ([luainpython.c:513](python/lunatic-python/src/luainpython.c#L513)), which is wrong if `_G` was reassigned or removed. → `lua_pushglobaltable`. *(P4)*
- **B14: `lua.eval` returns only the first value**, while calls return a tuple for multiple values. → `LUA_MULTRET`, with the same rule as calls. *(P4)*
- **B15: Python objects compare unequal in Lua.**
  - ✔ `python.import 're' == python.import 're'` is `false` (two distinct userdata).
  - → An `__eq` metamethod using `PyObject_RichCompare`, with an identity fast path. *(P4)*
- **B16: Index bases are unnatural on both sides.**
  - Lua code indexes Python lists 0-based (`pylist[0]`); `#pylist`, `ipairs` and `table.*` don't work.
  - Python code has no 0-based view of a Lua array.
  - → §5.6. *(P4)*
- **B17: str/bytes asymmetry.**
  - ◇ Python `bytes` → Lua string → Python `str` whenever the bytes happen to be valid UTF-8.
  - → Document it. Keep `python.asbytes`. *(P6 docs)*
- **B18: Lua tables are only usable as wrappers on the Python side.**
  - `json.dumps(t)` fails, and there is no way to build a Lua table from Python values except by running Lua code.
  - → The converters and constructors in §5.6. *(P6)*

### 4.C Lifecycle, threading, embedding

- **C1: The GIL is never released in a Lua host.**
  - Python is initialized and the main thread keeps the GIL forever.
  - ✔ A Python thread got 2 iterations during 1 s of Lua work, and 240 once Lua called `time.sleep`.
  - ◇ Nothing calls `PyGILState_Ensure`, so calling into Python from any other host thread crashes.
  - → §5.8. *(P3: `PyGILState` at every entry; P5: the full model)*
- **C2: Python is never shut down in a Lua host.**
  - ✔ Piped `print()` output from Python is lost at exit.
  - ✔ `atexit` handlers never run.
  - → Flush `sys.stdout`/`sys.stderr` when each runtime closes. Register a process-exit hook that calls `Py_FinalizeEx` if the bridge initialized Python (not on Windows: a DLL's exit hooks run under the loader lock, where finalizing could deadlock). *(P5)*
- **C3: The Python module's free function closes the Lua state unconditionally.**
  - `globalCloseFunc` ([luainpython.c:548-555](python/lunatic-python/src/luainpython.c#L548-L555)) runs at Python finalization or module dealloc. In a Lua host it would `lua_close` the **host's own** state; C2's fix makes that path reachable.
  - → Close only states the module created (`owns_state`, §5.3). *(P3)*
- **C4: `Py_Initialize` installs Python's signal handlers in the host** (SIGINT sets the KeyboardInterrupt flag; SIGPIPE is ignored). ◇ → `Py_InitializeEx(0)`, and document how to opt in. *(P5)*
- **C5: Deprecated init APIs:** `Py_SetProgramName` and `PySys_SetArgv` (deprecated in 3.11) at [pythoninlua.c:644](python/lunatic-python/src/pythoninlua.c#L644) and 661.
  - → Drop them and set `sys.argv` via `PySys_SetObject`.
  - Then check on Linux (pyenv, conda, Debian) and Windows that Python still finds its prefix without a program name.
  - If it doesn't: use `PyInitConfig_*` (3.14+, PEP 741) where the library exports it, and the deprecated setters on 3.10–3.13, chosen at runtime by which symbols are present. *(P1; checked in P5)*
- **C6: The interpreter environment is ignored.** Embedded Python sees the base installation's `sys.path` and ignores an active venv, pyenv, etc. → Discovery asks the selected interpreter (§5.2). *(P5)*
- **C7: Linux `PYTHON_LIBRT` is defined without a value** ([python/CMakeLists.txt:36](python/CMakeLists.txt#L36)).
  - ◇ So the code calls `dlopen("1", …)`. Debug builds abort on `assert(ok)`; release builds skip the preload.
  - → Removed; the loader opens libpython `RTLD_GLOBAL` itself. *(P1)*
- **C8: A hidden dependency on a locally patched `dylib` 2.1.0.**
  - The code works only with a patched copy:
    - Given a NULL directory, it opens the current process (`dlopen(NULL)`, or `GetModuleHandleA(name)` on Windows).
    - It opens libraries with `RTLD_NOW | RTLD_GLOBAL`.
  - Stock `dylib` throws on a NULL directory and opens libraries `RTLD_LOCAL`. ✔ With `RTLD_LOCAL`, every compiled extension import (`math`, `_struct`, numpy) fails.
  - → Our own loader (§5.2) keeps the `RTLD_GLOBAL` behaviour. The wheel doesn't load libpython at runtime at all. *(P1)*
- **C9: Python may already be initialized when `require "python"` runs.** `PyImport_AppendInittab` is ignored after init, so `import lua` is unavailable in that Python. ◇ → Create the module and put it into `sys.modules["lua"]` directly. *(P5)*
- **C10: In a Python host the GIL is held while Lua runs**, so other Python threads stall during long Lua work. → §5.8. *(P5)*
- **C11: Reference cycles between Python and Lua are never collected**, because neither garbage collector sees across the boundary. ◇ → Document it; tests check there are no leaks in the acyclic cases. *(P6 docs)*
- **C12: Binary Lua modules can't load inside a Python host.**
  - ◇ The extension is loaded `RTLD_LOCAL`, so a module loaded by `require` can't resolve `lua_*`.
  - → §5.9. *(P5)*
- **C13: Sub-interpreters are unsupported** (single-phase init, process-global state). → Document it. *(P6 docs)*

### 4.D Lua-side API (`require "python"`)

- **D1: Missing metamethods.**
  - ✔ `#pylist` fails with "attempt to get length of a POBJECT value". ✔ `pairs(pylist)` errors. ✔ Unary minus errors.
  - Missing: `__len`, `__lt`, `__le`, `__unm`, `__mod`, `__idiv`, the bitwise operators, `__concat`, `__pairs`, `__close`.
  - → Add them via `PyNumber_*`, `PyObject_RichCompare` and `PyObject_Size`. `__len`/`__pairs` follow §5.6. `__close` (5.4+) calls `__exit__`, and `python.with(cm)` calls `__enter__`. *(P4: `__len`, `__pairs`, `__eq`; P6: the rest)*
- **D2: The metatable is named `POBJECT`.** Errors read "POBJECT value", and the name could clash with other libraries. → `python.object`. *(P1)*
- **D3: The Lua 5.1 shims are dead code;** the code uses 5.2+ API anyway. ✔ Lua 5.4 and 5.5 compile. → Remove them, add 5.3 to CI, and add `luacompat.h`. *(P1)*
- **D4: Dead and legacy bits.**
  - `registry.Py_None` is never read ([pythoninlua.c:684-685](python/lunatic-python/src/pythoninlua.c#L684-L685)).
  - The `LUA_COMPAT_MODULE`/`LUA_COMPAT_ALL` defines.
  - The magic `__get`/`__set` attribute names shadow real Python attributes with those names.
  - → Remove the dead code. Keep `__get`/`__set` but document them. *(P1/P6)*
- **D5: Missing helpers:**
  - `python.iter(o)`;
  - `python.list(t)`, `python.tuple(t)`, `python.dict(t)`;
  - `python.kw`, `python.tablecall` (§5.7);
  - `python.asseq`, `python.asmap` (§5.6);
  - `python.with`, `python.serve` (§5.8), `python.version`.
  - *(P4–P6, together with the features they belong to)*

### 4.E Python-side API (`import lua`)

- **E1:** Rename the module and types (§3.2). *(P1)*
- **E2:** `lua.LuaError`, and export `lua.LuaObject` for `isinstance`. *(P4)*
- **E3:** `lua.tablecall` (§5.7), `lua.seq` (§5.6), `lua.table(...)`/`lua.table_from(...)` (§5.6). *(P4/P6)*
- **E4:** `lua.__version__`, `lua.LUA_VERSION`. *(P6)*
- **E5:** Type stubs (`lua.pyi`) and docstrings; every method currently has `NULL` docs. *(P6)*
- **E6:** Define which Lua state the module-level functions (`eval`, `execute`, `globals`, `require`) use. In a Python host it's the module's own state. In a Lua host it's the state currently calling into Python, otherwise the most recently opened live one. *(P3)*

### 4.F Build, packaging, repository

- **F1: The build is not self-contained.**
  - The CMake files reference `../../../3party/{lua_wrapper,dylib,python-headers}` and `../../../common`.
  - `dylib.hpp` and the Lua build are not in the repo, and `python-headers/` is here but at a different path.
  - → §3.1 and §3.3. *(P1)*
- **F2: Two near-duplicate CMakeLists.**
  - One has a dead include path ([lua_python_binding/CMakeLists.txt:64](lua_python_binding/CMakeLists.txt#L64)).
  - Lua is consumed through the host's `LUA_INCLUDE` variable convention.
  - → One top-level project. Consume Lua through a target (`LPB_LUA_TARGET`), or through `LUA_INCLUDE_DIR`/`LUA_LIBRARIES`, which is what luarocks passes. *(P1)*
- **F3: The Lua module links `lua`.** On Linux/macOS, with a static Lua, this puts two Lua VMs in one process. → No link on POSIX, the import lib only on Windows. Embed mode links the host's target as given. *(P1)*
- **F4: Windows hacks.**
  - A PUBLIC `Py_BUILD_CORE` suppresses `#pragma comment(lib, "python310.lib")`. It also turns `PyAPI_FUNC` into dllexport and enables the internal headers.
  - `LUA_LIB` makes every Lua API declaration dllexport.
  - `Py_EXPORTED_SYMBOL` is added by hand to `PyInit_*` to undo the `Py_BUILD_CORE` side effect.
  - → §3.3. *(P2)*
- **F5: No control over exported symbols.**
  - ✔ `python.so` exports `LuaState`, `LuaConvert`, `Lua_run`, `LuaObject_Type` and every `Py_Lua_Bridge_*` wrapper.
  - ✔ The Python extension exports all 343 `lua_*`/`luaL_*` symbols, unintentionally. §5.9 later exports the Lua C API on purpose.
  - On Linux these can be interposed by same-named symbols in the host.
  - → §3.3. *(P1)*
- **F6: Irrelevant RPATH settings** (`@executable_path`, `$ORIGIN`) on a plugin with no shared dependencies. → Remove. *(P1)*
- **F7: Three copies of the vendored headers.**
  - They differ only in `pyconfig.h`/`patchlevel.h`; the Windows copy also lacks `pydtrace_probes.h`.
  - They come from three patch releases: 3.10.8 (the "unix" 64-bit config, generated on macOS, with no `HAVE_EPOLL`), 3.10.12 (Linux 32-bit) and 3.10.9 (Windows).
  - → One tree (3.10.8) plus `pyconfig/{macos,linux,windows}/pyconfig.h` chosen by CMake. The Linux one takes its word-size dependent values from the compiler (`__SIZEOF_*__`), so it serves 32- and 64-bit. Provenance is in `third_party/README.md`. `LPB_PYTHON_HEADERS=system` uses an installed Python's headers instead, still with `Py_LIMITED_API=0x030A0000`. *(P1)*
- **F8: No packaging metadata.**
  - → `pyproject.toml` (scikit-build-core, `wheel.py-api = "cp310"`).
  - → A rockspec (`build.type = "cmake"`, depends on `lua >= 5.3, < 5.6`).
  - → cibuildwheel configuration. *(P1; publishing in P7)*
- **F9: Hand-written `Py_Lua_Bridge_*` wrappers plus `hacks.h` macro overrides.**
  - Every call site uses the prefixed name, and the wrappers are maintained by hand.
  - A stray plain `Py_DECREF`/`PyErr_Occurred` compiles and silently binds straight to libpython on Linux.
  - → §5.1. *(P1)*
- **F10: The tests aren't tests.**
  - `python/test.py`/`test.lua` need network access and assert nothing.
  - The upstream `lunatic-python/tests` target the old module name and layout.
  - → §7. *(harness in P1; each phase adds its regression tests)*
- **F11: Repository hygiene.** It is not a git repository; there are `.DS_Store` files in the tree; the README describes upstream lunatic-python. → `git init` and a baseline commit before any change, a `.gitignore`, and a new README. *(P0, README in P6)*
- **F12: Licensing.** lunatic-python is LGPL-2.1-or-later. The package ships that license, plus notices for the bundled Lua (MIT) and Python headers (PSF). *(P1)*
- **F13: Product-specific env var names and messages** ([dynamic_python_interface.cpp:327-344](python/dynamic_python_interface.cpp#L327-L344)). → Removed with the loader rewrite. *(P1)*

---

## 5. Design notes

### 5.1 CPython API layer

- **`src/pyapi_list.h`** lists every symbol used, once.
  - For functions it gives the signature and argument names, e.g. `PYFUNC(PyObject *, PyObject_Call, (PyObject *a, PyObject *b, PyObject *c), (a, b, c))`.
  - It also lists data symbols (`_Py_NoneStruct`, `_Py_TrueStruct`, `_Py_FalseStruct`, the `PyExc_*` used, and the type objects used).
- **Wheel build:** the list is unused, and the sources call CPython directly.
- **Rock build:** `pyloader.c` expands the list into three things:
  - a table of function pointers, filled from libpython at load time;
  - **trampolines with the real CPython names**, e.g. `PyObject *PyObject_Call(PyObject *a, …) { return lpb_py.PyObject_Call(a, b, c); }`;
  - macro overrides for the handful of data symbols (`Py_None`, `Py_True`, `Py_False`, `PyExc_*`).
- **Why trampolines:**
  - The static inline functions and macros in `Python.h` (`Py_DECREF` → `_Py_Dealloc`, …) keep working unmodified, with no copies of CPython internals.
  - The sources read as plain CPython code.
  - The compiler checks every trampoline's signature against the header declaration.
- **No variadic CPython functions are used** (no `PyErr_Format`, `PyArg_ParseTuple`, `PyObject_CallFunction`…), so every trampoline is a plain forward.
- **The list is generated:** `tools/gen_pyapi.py` reads the exact prototypes from the vendored 3.10 limited-API headers and writes `pyapi_list.h` (functions and data) and `pyapi.h` (data macros). `tests/check_symbols.py` fails if the Lua module references any CPython symbol directly, which is how a missing entry shows up.
- **Export control** keeps the trampolines private: a version script / `-exported_symbols_list` on POSIX. On Windows, `Py_NO_ENABLE_SHARED` turns the declarations into plain externs, so defining the trampolines is legal.
- **The API set is all in the 3.10 limited API** (the generator only finds declarations there). `PyGILState_Check` is not available and not used.
  - `PyRun_StringFlags` is not in the limited API; use `Py_CompileString` + `PyEval_EvalCode` instead.
- **A missing symbol at load** fails `require` with: "libpython at <path> lacks <symbol> (needs CPython ≥ 3.10)".

### 5.2 Finding libpython (rock build)

The loader tries these in order and stops at the first success:

1. **Already in the process?** Probe `Py_IsInitialized` (`dlsym(RTLD_DEFAULT, …)`, or the Windows module scan with an exact name match), then reuse that library.
2. **`LUA_PYTHON_LIBPYTHON`** gives a full path to load.
3. **Ask an interpreter:** `LUA_PYTHON_EXECUTABLE`, otherwise `python3`, then `python` (and `py -3` on Windows). It runs a short `-c` script that prints:
   - **the libpython path, with per-OS rules:**
     - macOS framework builds: `PYTHONFRAMEWORKPREFIX/LDLIBRARY`.
     - Linux: `LIBDIR/INSTSONAME`, then `LIBDIR/LDLIBRARY`. Check that the file exists rather than trusting `Py_ENABLE_SHARED`: Debian's `python3` is statically linked but ships `libpython3.X.so.1.0`.
     - Windows: `pythonXY.dll` in `sys.base_prefix`.
   - its `sys.path`, `sys.executable`, version and abiflags.
4. **Otherwise**, raise a Lua error listing every attempt. For a static-only interpreter, say which package or configure flag provides a shared libpython.

**Loading:**
- POSIX: `RTLD_NOW | RTLD_GLOBAL`, so compiled extension modules resolve libpython's symbols.
- Windows: `LoadLibraryExW(…, LOAD_WITH_ALTERED_SEARCH_PATH)`.
- Then check the version (`Py_GetVersion`) and reject free-threaded builds.
- The library is never unloaded.

**Initialization:**
- `Py_InitializeEx(0)`.
- Set `sys.argv`, and apply the interpreter's `sys.path`/`sys.executable` (venv support).
- Register the `lua` module through `PyImport_AppendInittab` before init, or directly in `sys.modules` if Python was already running.
- Finally `PyEval_SaveThread()` (§5.8).

### 5.3 Runtime handle, stack discipline, protected calls

```c
typedef struct lpb_runtime {
    lua_State *main;      /* main thread of the state */
    int refcount;         /* LuaObjects + the state's sentinel */
    int closed;           /* set by the sentinel's __gc during lua_close */
    int owns_state;       /* created by `import lua` (Python host) → the module may close it */
    /* lock and ownership bookkeeping, §5.8 */
} lpb_runtime;

typedef struct { PyObject_HEAD lpb_runtime *rt; int ref; const void *ptr; const char *tname; } LuaObject;
/* ptr (lua_topointer) and tname are taken at creation: ==, hash and repr need no Lua access.
   The iterator and the sequence view are separate types. */
```

- **Lua side.** `luaopen_python` creates the runtime and anchors a sentinel userdata in the registry. Its `__gc` sets `closed` and drops its reference.
  - Lua 5.4+ runs finalizers in reverse order of marking at `lua_close`, so the sentinel created first runs **after** every `python.object` finalizer.
  - Objects whose runtime is closed raise `LuaError("Lua state is closed")`, and their dealloc skips `luaL_unref`.
- **Current state.** A thread-local stack of `{lua_State *L; lpb_runtime *rt}` frames gets a frame pushed whenever Lua calls into Python, popped on return.
  - A Python→Lua operation on object `o` uses the innermost frame's `L` if that frame belongs to `o->rt`. That is the running coroutine, which behaves exactly like a normal C function calling Lua.
  - Otherwise it takes a Lua thread from the runtime's **pool** (created with `lua_newthread`, anchored in the registry) and returns it afterwards. Using the main thread instead would let two Python threads, each suspended inside a Lua call, interleave frames on one Lua stack.
  - Module-level functions follow E6.
- **Deferred releases.** Lua finalizers of `python.object` do not take the GIL: they queue the reference, and the next crossing drains the queue (or the finalizer itself, past 4096 pending). Python's `LuaObject` deallocation queues its `luaL_unref`, which the next thread owning the runtime performs.
- **Stack discipline.** Every Python→Lua operation records `top = lua_gettop(L)`, checks space with `lua_checkstack` (raising `MemoryError` on failure), and restores `lua_settop(L, top)` on every path. `settop(0)` is never used.
- **Protected calls.** Every Lua operation started from Python (index, newindex, len, tostring, next, compare, call, ref) runs inside `lua_pcall` through a small C trampoline. Inputs and outputs travel in a struct passed as light userdata, and a message handler collects the traceback (§5.4).
  - The only things done outside `pcall` are pushes that cannot raise.

### 5.4 Error bridging

- **Python → Lua.**
  1. When a Python operation fails inside a Lua-called entry point, fetch and normalize the exception (with its traceback attached).
  2. Release every Python reference, then raise a Lua error whose value is a `python.object` wrapping the **exception instance**.
  - `tostring(err)` gives `"ValueError: boom"`; the Python traceback is reachable from the error object.
  - Lua code can inspect `err.args`, etc.
- **Lua → Python.**
  - `lua_pcall` runs with a message handler that keeps the original error value and records `luaL_traceback` alongside it.
  - If the value wraps a Python exception, the original exception is re-raised. On 3.11+ the Lua traceback is attached via `add_note`.
  - Otherwise raise `lua.LuaError(message)`, with `.value` (the converted original) and `.traceback`. `__tostring` on error objects is respected.
- **No special cases.** `SystemExit`, `KeyboardInterrupt` and the rest propagate like any other exception.
  - In a Python host they come back out as themselves.
  - In a Lua host they are ordinary, `pcall`-able Lua errors.
- **`PyErr_Print` is never used.**

### 5.5 Value conversion rules

| Lua → Python | | Python → Lua | |
|---|---|---|---|
| `nil` | `None` | `None` | `nil` |
| boolean | `bool` | `bool` (checked before `int`) | boolean |
| integer | `int` | `int` | integer; float if outside the `lua_Integer` range |
| float | `float` | `float` | float |
| string | `str` if valid UTF-8, else `bytes` | `str` | UTF-8 string (`surrogateescape`, else error) |
| `python.object` userdata | the original object | `bytes` | string |
| table / function / userdata / thread / light userdata | `lua.LuaObject` | `lua.LuaObject` | the original Lua value (same runtime only) |
| | | anything else | `python.object` (mode per §5.6) |

Subclasses of `int`/`float`/`str` (`IntEnum`, `numpy.float64`, …) convert by value, and so do other `numbers.Integral` types (numpy integers, via `__index__`). Other numbers (`Fraction`, `Decimal`) stay Python objects, so they stay exact.

### 5.6 Indexing and iteration

**Principle:** each side sees the other side's **sequences** with its own index base. **Mappings** keep their keys unchanged, because there the keys are data.

**Python objects in Lua (`python.object`).** The mode is chosen when the object is wrapped:

| Mode | Chosen for | `o[k]` | `#o` | `pairs(o)` |
|---|---|---|---|---|
| sequence | `collections.abc.Sequence` (list, tuple, range, deque, array, bytearray, memoryview, …) and array-likes with `__array_interface__` (numpy) | integer `i ≥ 1` → `o[i-1]`; `i ≤ -1` → `o[i]` (from the end, like `string.sub`); `0` or out of range → `nil` | `len(o)` | `1, v1`, `2, v2`, … |
| mapping | `dict`, `collections.abc.Mapping` | the key as is | `len(o)` | `key, value` |
| attribute | everything else | attributes | `len(o)` if defined | `1, item1`, … if iterable |

- **Fallback for other keys.** Non-integer keys on sequences, and missing keys on mappings, fall back to attributes (`l.append`, `d.items`), but only on `LookupError`/`TypeError` (B11).
- **Sequence writes:**
  - `o[i] = v` sets `o[i-1]`.
  - `o[#o + 1] = v` appends.
  - `o[#o] = nil` pops the last element.
  - `nil` anywhere else is an error, because Python sequences have no holes.
  - As a result, `ipairs`, `table.insert`, `table.remove`, `table.sort`, `table.concat` and `table.unpack` work on Python lists; the Lua 5.3+ table library honours metamethods.
- **Mapping writes:** `o[k] = nil` deletes the key.
- **Forcing a mode:** `python.asseq(o)`, `python.asmap(o)`, `python.asattr(o)`.
- **Plain iteration:** `python.iter(o)` gives Python's `iter(o)` as a Lua iterator yielding values (generators, sets, files, …).
- **Copying into Python containers:** `python.list(t)`, `python.tuple(t)` and `python.dict(t)` build real Python containers from a Lua table (shallow), for APIs that need them (`json.dumps`, …).

**Lua tables in Python (`lua.LuaObject`).** Lua tables are mappings:
- `t[k]` uses the key as is, so `t[1]` is the first array element.
- `len(t)` is `#t`.
- Iteration yields keys from a separate iterator object, respecting `__pairs`.
- `keys()/values()/items()` are available.
- **Sequence view:** `lua.seq(t)` returns a `lua.LuaSequence`, registered as `collections.abc.MutableSequence`:
  - 0-based: `s[0]` is `t[1]`; negative indices count from the end.
  - `len(s)` is `#t`, and iteration yields values.
  - `append`/`insert`/`pop` are supported, and slicing returns a `list`.
- **Building tables from Python:** `lua.table(*items, **fields)` and `lua.table_from(iterable_or_mapping)`.

### 5.7 Call conventions

**Plain calls are positional in both directions.**
- `pyf(a, b, t)` passes the Lua table `t` as one argument (a `lua.LuaObject`).
- `luaf(a, b, d)` passes the dict `d` as one argument (a `python.object`).

**Named arguments use the two Lua idioms for them, mirrored exactly:**

| Lua idiom | Lua calls Python | Python calls Lua |
|---|---|---|
| Options table: `function f(a, b, opts)` | `pyf(a, b, python.kw{k=v})` → `pyf(a, b, k=v)` | `luaf(a, b, k=v)` → `luaf(a, b, {k=v})` |
| Argument table: `f{a, b, k=v}`, received as `function f(t)` or `function f(...)` + `{...}` | `python.tablecall(pyf){a, b, k=v}` → `pyf(a, b, k=v)` | `lua.tablecall(luaf)(a, b, k=v)` → `luaf{a, b, k=v}` |

- **Round trips are lossless.**
  - A Lua function that received `(a, b, opts)` from Python forwards them with `pyf(a, b, python.kw(opts))`.
  - One that received an argument table forwards it with `python.tablecall(pyf)(t)`.
- **`python.kw(t)`** must be the last argument, and its table may contain only string keys. It wraps the table; it neither copies it nor touches its metatable.
- **The options table is created only when keyword arguments are present.** Without them, Lua receives exactly the positional arguments. This mirrors the Lua side, where a plain call never produces keyword arguments.
- **The `tablecall` wrappers:**
  - `python.tablecall(f)` returns a Lua function that takes exactly one table.
  - `lua.tablecall(f)` returns a Python callable.
  - Both are meant to be cached: `local plot = python.tablecall(plt.plot)`.
- **Argument-table rules (both directions):**
  - Integer keys `1..n` are positional, where `n` is the largest integer key; gaps become `None`.
  - String keys are named arguments.
  - Any other key (`0`, negative, non-integer, other types) is an error.
  - Trailing `nil` positionals cannot be represented in a Lua table; document it.

### 5.8 Threading and the GIL

**Rule: the GIL is never held while Lua code runs.** Each runtime has a lock that grants the right to run Lua on it.

1. **Entering Python.** Every Lua→Python entry point (calls, index/newindex, metamethods, `__gc`, `python.*` functions) calls `PyGILState_Ensure` on entry and `PyGILState_Release` on exit, so any host thread can call into Python.
2. **Lua host.** Right after init, call `PyEval_SaveThread()`.
3. **Runtime ownership.**
   - A Lua-host runtime is held by the host, except while its thread is inside Python through the bridge ("parked").
   - The host thread itself may use its own runtime whenever it is in Python, even outside a call from that state ("borrowing"). This is what lets one thread run several Lua states that call each other through Python.
   - A Python-host runtime is free unless a Python→Lua call is running.
   - A crossing from Lua into Python on a thread that already holds the GIL does not park: no other thread can run Python at that moment, and parking would make this thread wait for the runtime while holding the GIL.
4. **A Python→Lua call, from any thread:**
   1. Release the GIL.
   2. Acquire the runtime: wait until it is parked or free. Re-acquiring on the same thread is allowed.
   3. Run Lua without the GIL. Arguments and results are converted in short sections that hold the GIL.
   4. Release the runtime.
   5. Re-acquire the GIL.
   - The lock order is always runtime → GIL, which rules out deadlocks.
5. **Returning from Python to Lua.** Wait until no other thread holds the runtime, then continue.
6. **What this means for Python threads.** In a Lua host they run freely. When they call a Lua callback, it runs whenever the Lua side is inside Python (`join()`, `time.sleep`, an asyncio loop) or calls `python.serve(seconds)`, which parks the runtime for that long.
7. **Finalizers.** The `__gc` of Python objects does not take the GIL: releases are queued and batched (§5.3).
8. **Exactly what runs with the GIL held:** value conversion (which may trigger Lua garbage collection, and so finalizers) and Python code. Calls into Lua functions and Lua operations that may run metamethods (`gettable`, `settable`, `len`, comparisons, `tostring`, `__pairs`) run without it.

**Consequences:**
- The GIL is acquired and released on every crossing; measure it and batch where possible.
- Tests must cover deadlock scenarios, `join()` from Lua on a thread that calls Lua, an asyncio server calling Lua handlers, and a multi-threaded host with two states.

### 5.9 Binary Lua modules inside a Python host

- **What's exported.** The wheel exports the Lua C API (`lua_*`, `luaL_*`, `luaopen_*`); the bridge internals stay hidden.
- **POSIX.** At import, the extension reopens itself with `RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL` (its own path comes from `dladdr`). That puts the Lua API into the global symbol scope, so binary modules loaded by `require` resolve against the embedded Lua. It is the same `RTLD_GLOBAL` mechanism the rock uses for libpython, applied in the other direction.
- **Windows.** Binary modules import a Lua DLL by name. So the wheel builds Lua 5.5 as `lua55.dll`, ships it beside the extension and links the extension to it; modules linked against `lua55.dll` load into the same VM. Modules built against a differently named Lua DLL can't be supported; document it.
- **Module requirements.** Modules must be built for Lua 5.5 (`luarocks --lua-version 5.5`). `package.path`/`package.cpath` use the standard 5.5 locations and the `LUA_PATH_5_5`/`LUA_CPATH_5_5` env vars.
- **Risk.** On Linux, another Lua-embedding extension loaded later into the same process can bind to our global Lua API if it wasn't built with hidden visibility. Document it.

---

## 6. Phases

Each phase ends with green tests, and each fixed item gets a regression test.

| Phase | Content | Exit criteria | Status |
|---|---|---|---|
| **P0 Repo hygiene** | F11: `git init`, baseline commit of the current tree, `.gitignore`, remove `.DS_Store` | Baseline commit exists | done |
| **P1 Build, packaging, loader** | §3.1 layout; one CMake; wheel (links Python, bundles Lua 5.5) + rock (§5.1–5.2); renames (E1, D2); A9, A16–A19, C5, C7, C8, D3, D4, F1–F3, F5–F10, F12, F13; test harness porting today's working behaviour | Wheel builds and smoke tests pass on 3.10–3.14 (macOS locally, Linux/Windows in CI). `luarocks make` works against Lua 5.3/5.4/5.5, and Lua-host smoke tests pass with libpython 3.10–3.14 found automatically. `grep -ri` for the old prefix is clean. Only the intended symbols are exported | done on macOS (Lua 5.3/5.4/5.5 × CPython 3.10–3.14, `luarocks make` and `luarocks test`); Linux/Windows in CI, not yet run |
| **P2 Limited API + type rewrite** | `Py_LIMITED_API=0x030A0000` everywhere; heap types via `PyType_FromSpec`; A8, A13, A14, F4 | The same rock and wheel binaries pass on 3.10–3.14; `-Wcast-function-type-strict` clean; Windows builds in CI | done on macOS; Windows in CI, not yet run |
| **P3 Memory safety** | §5.3; A1–A7, A10 (stop exiting), A11, A12, A15, A20, C1 (`PyGILState` at every entry), C3, E6 | Every ✔ crash has a regression test; suites pass under ASan/UBSan and Lua `LUA_USE_APICHECK` | done (both hosts under ASan + UBSan on macOS) |
| **P4 Semantics and errors** | §5.4–5.7; A10 (full), B1–B16, D1 (`__len`, `__pairs`, `__eq`), E2, E3 (`lua.tablecall`, `lua.seq`) | Conversion, indexing, call-convention and error round-trip tests in both hosts | done |
| **P5 Lifecycle, threading, embedding** | §5.8, §5.9; C1, C2, C4, C5 (check on Linux/Windows), C6, C9, C10, C12 | Thread, deadlock, flush/atexit, venv and binary-module tests; the embed test with 2 states + threads passes | done on macOS; the prefix check without a program name (C5) on Linux/Windows awaits CI; C9 (Python started by the host itself) is implemented but has no test |
| **P6 API completion and docs** | B17, B18, C11, C13, D1 (rest), D5, E3 (rest), E4, E5; README, stubs | Documented API matches the tests | done |
| **P7 Release** | cibuildwheel wheels (manylinux/musllinux x86_64+aarch64, macOS arm64+x86_64, Windows amd64), rock upload, versioning, CHANGELOG | Install from the index works in a clean environment | prepared: workflow, cibuildwheel and rockspec written, repository at github.com/tucher/lua-python-bridge; needs publishing accounts |

---

## 7. Test plan

- **`tests/python/` (pytest, Python host):**
  - conversions, errors, indexing views;
  - call conventions;
  - refcount stability: `sys.getrefcount(True)` unchanged over a loop of comparisons;
  - Lua stack balance: every operation restores the stack top on all paths, and the suites drive 100k-key iterations and 1000-argument calls with `LUA_USE_APICHECK`;
  - threads.
- **`tests/lua/` (Lua host):** `lua tests/lua/run.lua` runs each test file in a **separate process**, so crashes and `exit` codes are caught as failures rather than taking the runner down.
- **`tests/embed/`:** a C host with two `lua_State`s, `require "python"` in both, cross-state objects (must error, not corrupt), closing one state while the other is in use, and calls from several threads.
- **CI matrix:** OS × CPython 3.10–3.14 × host Lua 5.3/5.4/5.5. The Lua-host tests run **the same rock binary** against every Python version.
- **Debug job:** Lua built with `LUA_USE_APICHECK`, plus ASan + UBSan.
- **Binary-module job:** build a minimal Lua C module for 5.5 and `require` it from the Python host.
- **Regression tests seeded from the known reproductions:**
  - comparison refcounts and `!=`;
  - iterating 200 / 100k keys; `f(*range(200))`;
  - erroring `__index`; `len(lua_function)`;
  - argument tables `{1, nil, 3}`, `{[0]=…}`, `{[1.5]=…}`, `{["1"]=…}`;
  - a surrogate string;
  - `require` inside a collected coroutine;
  - `sys.exit` under `pcall` (Lua host) and from a callback (Python host);
  - `python.execute` raising; the Python exception type surviving a round trip;
  - keyword arguments to a Lua function;
  - `hasattr`/`dict()`/`hash()`; iteration after `break` and nested iteration;
  - int/float/bigint conversions;
  - `#pyobj`, `pairs(pyobj)`, `-pyobj`, `pyobj == pyobj`, `table.insert/remove` on a Python list;
  - Python thread starvation; piped stdout flush; `atexit`;
  - a missing libpython must give a Lua error, not an abort;
  - extension imports (`math`, `_struct`) from a Lua host.
