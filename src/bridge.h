/*
 * lua-python-bridge: shared internal declarations.
 *
 * Copyright (c) 2002-2005 Gustavo Niemeyer <gustavo@niemeyer.net> (Lunatic Python)
 * Copyright (c) 2026 Aleks Tuchkov
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of
 * the GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#ifndef LPB_BRIDGE_H
#define LPB_BRIDGE_H

#define PY_SSIZE_T_CLEAN
#ifndef Py_LIMITED_API
#  define Py_LIMITED_API 0x030A0000
#endif

/* MSVC debug builds would otherwise switch the Python headers to Py_DEBUG. */
#if defined(_MSC_VER) && defined(_DEBUG)
#  undef _DEBUG
#  include <Python.h>
#  define _DEBUG 1
#else
#  include <Python.h>
#endif

#ifdef LPB_DYNAMIC_PYTHON
#  include "pyapi.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "luacompat.h"
#include "lpb_threads.h"

#ifndef LPB_VERSION
#  define LPB_VERSION "0.0.0"
#endif

#if defined(_WIN32)
#  define LPB_EXPORT __declspec(dllexport)
#else
#  define LPB_EXPORT __attribute__((visibility("default")))
#endif

/* METH_KEYWORDS functions are stored as PyCFunction by CPython's design. */
#if defined(__clang__)
#  define LPB_METH_CAST_BEGIN _Pragma("clang diagnostic push") \
      _Pragma("clang diagnostic ignored \"-Wcast-function-type-strict\"")
#  define LPB_METH_CAST_END _Pragma("clang diagnostic pop")
#else
#  define LPB_METH_CAST_BEGIN
#  define LPB_METH_CAST_END
#endif
#define LPB_METH(f) ((PyCFunction)(void (*)(void))(f))

#define LPB_PYOBJECT_MT "python.object"
#define LPB_KWARGS_MT   "python.kw"
#define LPB_SENTINEL_MT "python.runtime"

/* ------------------------------------------------------------------------------------------ */
/* Runtime: one per lua_State that uses the bridge (runtime.c)                                */
/* ------------------------------------------------------------------------------------------ */

typedef struct lpb_pooled {
    lua_State *L;
    int ref;
} lpb_pooled;

typedef struct lpb_runtime {
    lua_State *main;          /* main thread of the Lua state */
    int refcount;             /* LuaObjects + sentinel; guarded by the GIL */
    int closed;               /* the Lua state is closed (or closing) */
    int owns_state;           /* created by `import lua`; the module closes it */
    struct lpb_runtime *next; /* list of live runtimes; guarded by the GIL */

    /* Right to run Lua on this state (see lpb_acquire / lpb_enter). Guarded by mu. */
    lpb_mutex mu;
    lpb_cond cv;
    void *owner;              /* NULL: free, LPB_OWNER_HOST, or a thread token */
    int depth;                /* re-acquisitions by the owning thread */
    int borrowed;             /* owner thread is the host thread borrowing LPB_OWNER_HOST */
    void *host_thread;        /* thread token of the Lua host thread (Lua host only) */

    int *unrefs;              /* registry refs released by Python while not owning; guarded by mu */
    size_t n_unrefs, cap_unrefs;

    lpb_pooled *pool;         /* idle Lua threads for calls that start in Python; guarded by ownership */
    int n_pool, cap_pool;
} lpb_runtime;

extern char lpb_owner_host_marker;
#define LPB_OWNER_HOST ((void *)&lpb_owner_host_marker)

/* A Lua->Python crossing in progress on the current OS thread. */
typedef struct lpb_frame {
    lua_State *L;
    lpb_runtime *rt;
    struct lpb_frame *prev;
    PyGILState_STATE gil;
    int parked;
    void *saved_owner;
    int saved_depth;
    int saved_borrowed;
} lpb_frame;

lpb_runtime *lpb_runtime_get(lua_State *L);
lpb_runtime *lpb_runtime_create(lua_State *L, int owns_state, int lua_host);   /* GIL held */
lpb_runtime *lpb_runtime_default(void);                                        /* GIL held */
void lpb_runtime_incref(lpb_runtime *rt);                                      /* GIL held */
void lpb_runtime_decref(lpb_runtime *rt);                                      /* GIL held */
void lpb_runtime_close_owned(lpb_runtime *rt);                                 /* GIL held */
void *lpb_thread_token(void);
lpb_frame *lpb_frame_for(lpb_runtime *rt);
lpb_runtime *lpb_current_runtime(void);                /* runtime of the innermost crossing */
void lpb_serve(lpb_runtime *rt, unsigned long ms);

/* Lua -> Python. */
void lpb_enter(lpb_runtime *rt, lua_State *L, lpb_frame *f);
void lpb_leave(lpb_frame *f);
/* Pushes a Lua function running `impl` as a Lua->Python entry point; `nup` upvalues for impl are
   popped from the stack. */
void lpb_push_entry(lua_State *L, lpb_runtime *rt, lua_CFunction impl, int nup);

/* Python -> Lua. All with the GIL held. */
int lpb_run(lpb_runtime *rt, lua_CFunction op, void *ud);    /* 0, or -1 with an exception set */
int lpb_lcall(lpb_runtime *rt, lua_State *L, int nargs, int nres);
int lpb_msgh(lua_State *L);
extern char lpb_traceback_key;

/* Lua operations that may run metamethods; called through lpb_lcall (without the GIL). */
int lpb_prim_gettable(lua_State *L);    /* (t, k) -> t[k] */
int lpb_prim_settable(lua_State *L);    /* (t, k, v) */
int lpb_prim_len(lua_State *L);         /* (v) -> #v */
int lpb_prim_lt(lua_State *L);          /* (a, b) -> a < b */
int lpb_prim_le(lua_State *L);          /* (a, b) -> a <= b */
int lpb_prim_tostring(lua_State *L);    /* (v) -> tostring(v) */

void lpb_defer_decref(PyObject *o);                         /* any thread, no GIL needed */
void lpb_drain_decrefs(void);                               /* GIL held */
size_t lpb_pending_decrefs(void);
void lpb_defer_unref(lpb_runtime *rt, int ref);             /* GIL held */

/* ------------------------------------------------------------------------------------------ */
/* Conversions and errors (convert.c). GIL held; may raise Lua errors (protected context).    */
/* ------------------------------------------------------------------------------------------ */

enum { LPB_MODE_ATTR = 0, LPB_MODE_MAP = 1, LPB_MODE_SEQ = 2 };

typedef struct lpb_pyobj {
    PyObject *o;
    int mode;
} lpb_pyobj;

int lpb_convert_init(void);
int lpb_push_py(lpb_runtime *rt, lua_State *L, PyObject *o);
void lpb_push_pyobject(lua_State *L, PyObject *o, int mode);
int lpb_wrap_mode(PyObject *o);
PyObject *lpb_to_py(lpb_runtime *rt, lua_State *L, int idx);
lpb_pyobj *lpb_test_pyobject(lua_State *L, int idx);
void lpb_push_pyerr(lpb_runtime *rt, lua_State *L);
int lpb_error(lpb_runtime *rt, lua_State *L);
void lpb_set_pyerr_from_lua(lpb_runtime *rt, lua_State *L, int idx, int status);
PyObject *lpb_call1(PyObject *callable, PyObject *arg);
PyObject *lpb_call_method0(PyObject *obj, const char *name);
PyObject *lpb_call_method1(PyObject *obj, const char *name, PyObject *arg);
int lpb_is_dunder(PyObject *name);

/* ------------------------------------------------------------------------------------------ */
/* Python side (luainpython.c)                                                                */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    lpb_runtime *rt;
    int ref;                  /* registry reference to the Lua value */
    const void *ptr;          /* lua_topointer: identity for ==, hash and repr */
    const char *tname;        /* Lua type name */
} LuaObject;

typedef struct {
    PyObject_HEAD
    LuaObject *table;
} LuaSequence;

extern PyObject *lpb_LuaError;
extern PyTypeObject *lpb_LuaObject_Type;
extern PyTypeObject *lpb_LuaSequence_Type;
extern PyTypeObject *lpb_LuaIterator_Type;

int lpb_types_init(void);
PyObject *lpb_luaobject_new(lpb_runtime *rt, lua_State *L, int idx);
PyObject *lpb_pyinit_lua(void);

#define LuaObject_Check(o)   PyObject_TypeCheck((o), lpb_LuaObject_Type)
#define LuaSequence_Check(o) PyObject_TypeCheck((o), lpb_LuaSequence_Type)

/* ------------------------------------------------------------------------------------------ */
/* Lua side (pythoninlua.c)                                                                   */
/* ------------------------------------------------------------------------------------------ */

int lpb_luaopen(lua_State *L);

/* ------------------------------------------------------------------------------------------ */
/* libpython loader (pyloader.c, Lua module build only)                                        */
/* ------------------------------------------------------------------------------------------ */

#ifdef LPB_DYNAMIC_PYTHON
int lpb_pyloader_load(lua_State *L);       /* 0, or -1 with an error message pushed */
int lpb_pyloader_after_init(void);         /* GIL held; applies the queried interpreter's paths */
#endif

#endif
