/*
 * lua-python-bridge: runtimes, the ownership protocol between Lua and Python, and crossings.
 *
 * Copyright (c) 2026 Aleks Tuchkov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Rules (see PLAN.md, section 5.8):
 *  - The GIL is not held while the bridge runs Lua code.
 *  - A thread may run Lua on a state only while it "owns" the state's runtime. A Lua host owns
 *    its runtime implicitly (LPB_OWNER_HOST) except while it is inside a call into Python
 *    ("parked"). Python threads acquire the runtime for each operation.
 *  - Waiting for a runtime always happens without the GIL (lock order: runtime, then GIL).
 */
#include "bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char lpb_owner_host_marker;
char lpb_traceback_key;

static char lpb_runtime_key;
static char lpb_sentinel_key;

static LPB_TLS char lpb_tls_token;
static LPB_TLS lpb_frame *lpb_frames;

static lpb_runtime *lpb_runtimes;   /* guarded by the GIL */

void *lpb_thread_token(void) {
    return &lpb_tls_token;
}

/* ------------------------------------------------------------------------------------------ */
/* Deferred Py_DECREF (Lua finalizers run without the GIL)                                    */
/* ------------------------------------------------------------------------------------------ */

static lpb_mutex lpb_pend_mu = LPB_MUTEX_INIT;
static PyObject **lpb_pend;
static size_t lpb_pend_n, lpb_pend_cap;

void lpb_defer_decref(PyObject *o) {
    lpb_mutex_lock(&lpb_pend_mu);
    if (lpb_pend_n == lpb_pend_cap) {
        size_t cap = lpb_pend_cap ? lpb_pend_cap * 2 : 64;
        PyObject **p = (PyObject **)realloc(lpb_pend, cap * sizeof *p);
        if (!p) {                       /* out of memory: leak the reference */
            lpb_mutex_unlock(&lpb_pend_mu);
            return;
        }
        lpb_pend = p;
        lpb_pend_cap = cap;
    }
    lpb_pend[lpb_pend_n++] = o;
    lpb_mutex_unlock(&lpb_pend_mu);
}

size_t lpb_pending_decrefs(void) {
    size_t n;
    lpb_mutex_lock(&lpb_pend_mu);
    n = lpb_pend_n;
    lpb_mutex_unlock(&lpb_pend_mu);
    return n;
}

void lpb_drain_decrefs(void) {
    PyObject **v;
    size_t n, i;
    lpb_mutex_lock(&lpb_pend_mu);
    v = lpb_pend;
    n = lpb_pend_n;
    lpb_pend = NULL;
    lpb_pend_n = lpb_pend_cap = 0;
    lpb_mutex_unlock(&lpb_pend_mu);
    for (i = 0; i < n; i++)
        Py_DECREF(v[i]);
    free(v);
}

/* ------------------------------------------------------------------------------------------ */
/* Deferred luaL_unref (Python may drop Lua objects without owning the runtime)               */
/* ------------------------------------------------------------------------------------------ */

void lpb_defer_unref(lpb_runtime *rt, int ref) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL)
        return;
    lpb_mutex_lock(&rt->mu);
    if (!rt->closed) {
        if (rt->n_unrefs == rt->cap_unrefs) {
            size_t cap = rt->cap_unrefs ? rt->cap_unrefs * 2 : 32;
            int *p = (int *)realloc(rt->unrefs, cap * sizeof *p);
            if (p) {
                rt->unrefs = p;
                rt->cap_unrefs = cap;
            }
        }
        if (rt->n_unrefs < rt->cap_unrefs)
            rt->unrefs[rt->n_unrefs++] = ref;
    }
    lpb_mutex_unlock(&rt->mu);
}

/* Called by a thread that is allowed to run Lua on L. */
static void lpb_flush_unrefs(lpb_runtime *rt, lua_State *L) {
    int *refs;
    size_t n, i;
    lpb_mutex_lock(&rt->mu);
    refs = rt->unrefs;
    n = rt->n_unrefs;
    rt->unrefs = NULL;
    rt->n_unrefs = rt->cap_unrefs = 0;
    lpb_mutex_unlock(&rt->mu);
    if (!refs)
        return;
    if (lua_checkstack(L, 2)) {         /* luaL_unref does not allocate */
        for (i = 0; i < n; i++)
            luaL_unref(L, LUA_REGISTRYINDEX, refs[i]);
    }
    free(refs);
}

/* ------------------------------------------------------------------------------------------ */
/* Runtime lifecycle                                                                          */
/* ------------------------------------------------------------------------------------------ */

lpb_runtime *lpb_runtime_get(lua_State *L) {
    lpb_runtime *rt;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &lpb_runtime_key);
    rt = (lpb_runtime *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return rt;
}

static void lpb_runtime_unlink(lpb_runtime *rt) {
    lpb_runtime **p;
    for (p = &lpb_runtimes; *p; p = &(*p)->next) {
        if (*p == rt) {
            *p = rt->next;
            break;
        }
    }
    rt->next = NULL;
}

void lpb_runtime_incref(lpb_runtime *rt) {
    rt->refcount++;
}

void lpb_runtime_decref(lpb_runtime *rt) {
    if (--rt->refcount > 0)
        return;
    lpb_runtime_unlink(rt);
    lpb_mutex_destroy(&rt->mu);
    lpb_cond_destroy(&rt->cv);
    free(rt->unrefs);
    free(rt->pool);
    free(rt);
}

static void lpb_flush_stdio(void) {
    static const char *const names[] = {"stdout", "stderr"};
    size_t i;
    for (i = 0; i < 2; i++) {
        PyObject *f = PySys_GetObject(names[i]);   /* borrowed */
        if (f && f != Py_None) {
            PyObject *r = lpb_call_method0(f, "flush");
            Py_XDECREF(r);
        }
        PyErr_Clear();
    }
}

/* __gc of the sentinel: runs when the Lua state is closed. */
static int lpb_sentinel_gc(lua_State *L) {
    lpb_runtime **p = (lpb_runtime **)lua_touserdata(L, 1);
    lpb_runtime *rt = p ? *p : NULL;
    if (!rt)
        return 0;
    *p = NULL;
    lpb_mutex_lock(&rt->mu);
    rt->closed = 1;
    rt->n_pool = 0;                     /* pooled threads die with the state */
    lpb_cond_broadcast(&rt->cv);
    lpb_mutex_unlock(&rt->mu);
    if (Py_IsInitialized()) {
        PyGILState_STATE g = PyGILState_Ensure();
        lpb_drain_decrefs();
        lpb_flush_stdio();
        lpb_runtime_unlink(rt);
        lpb_runtime_decref(rt);
        PyGILState_Release(g);
    }
    return 0;
}

lpb_runtime *lpb_runtime_create(lua_State *L, int owns_state, int lua_host) {
    lpb_runtime **p;
    lpb_runtime *rt = (lpb_runtime *)calloc(1, sizeof *rt);
    if (!rt) {
        PyErr_NoMemory();
        return NULL;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    rt->main = lua_tothread(L, -1);
    lua_pop(L, 1);
    lpb_mutex_init(&rt->mu);
    lpb_cond_init(&rt->cv);
    rt->owns_state = owns_state;
    rt->owner = lua_host ? LPB_OWNER_HOST : NULL;
    rt->host_thread = lua_host ? lpb_thread_token() : NULL;
    rt->refcount = 1;                   /* held by the sentinel */

    /* The sentinel is created before any python.object, so at lua_close its finalizer runs
       after theirs. */
    p = (lpb_runtime **)lpb_newuserdata(L, sizeof *p);
    *p = rt;
    if (luaL_newmetatable(L, LPB_SENTINEL_MT)) {
        lua_pushcfunction(L, lpb_sentinel_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &lpb_sentinel_key);
    lua_pushlightuserdata(L, rt);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &lpb_runtime_key);

    rt->next = lpb_runtimes;
    lpb_runtimes = rt;
    return rt;
}

lpb_runtime *lpb_current_runtime(void) {
    return lpb_frames ? lpb_frames->rt : NULL;
}

lpb_frame *lpb_frame_for(lpb_runtime *rt) {
    lpb_frame *f;
    for (f = lpb_frames; f; f = f->prev)
        if (f->rt == rt)
            return f;
    return NULL;
}

lpb_runtime *lpb_runtime_default(void) {
    lpb_runtime *rt;
    lpb_frame *f;
    for (f = lpb_frames; f; f = f->prev)
        if (!f->rt->closed)
            return f->rt;
    for (rt = lpb_runtimes; rt; rt = rt->next)
        if (rt->owns_state && !rt->closed)
            return rt;
    for (rt = lpb_runtimes; rt; rt = rt->next)
        if (!rt->closed)
            return rt;
    return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Ownership                                                                                  */
/* ------------------------------------------------------------------------------------------ */

/* mu held */
static int lpb_try_take(lpb_runtime *rt, void *me) {
    if (rt->owner == me) {
        rt->depth++;
        return 1;
    }
    if (rt->owner == NULL) {
        rt->owner = me;
        rt->depth = 1;
        rt->borrowed = 0;
        return 1;
    }
    if (rt->owner == LPB_OWNER_HOST && rt->host_thread == me) {
        /* The host thread itself is in Python (outside any call from Lua), so it is not running
           Lua: borrow the runtime from the host. */
        rt->owner = me;
        rt->depth = 1;
        rt->borrowed = 1;
        return 1;
    }
    return 0;
}

/* GIL held on entry and on return. */
static int lpb_acquire(lpb_runtime *rt) {
    void *me = lpb_thread_token();
    int ok, closed;
    lpb_mutex_lock(&rt->mu);
    closed = rt->closed;
    ok = !closed && lpb_try_take(rt, me);
    lpb_mutex_unlock(&rt->mu);
    if (!ok && !closed) {
        PyThreadState *ts = PyEval_SaveThread();
        lpb_mutex_lock(&rt->mu);
        while (!(closed = rt->closed) && !(ok = lpb_try_take(rt, me)))
            lpb_cond_wait(&rt->cv, &rt->mu);
        lpb_mutex_unlock(&rt->mu);
        PyEval_RestoreThread(ts);
    }
    if (!ok) {
        PyErr_SetString(lpb_LuaError ? lpb_LuaError : PyExc_RuntimeError,
                        "the Lua state is closed");
        return -1;
    }
    return 0;
}

static void lpb_release(lpb_runtime *rt) {
    lpb_mutex_lock(&rt->mu);
    if (--rt->depth == 0) {
        rt->owner = rt->borrowed ? LPB_OWNER_HOST : NULL;
        rt->borrowed = 0;
        lpb_cond_broadcast(&rt->cv);
    }
    lpb_mutex_unlock(&rt->mu);
}

static void lpb_park(lpb_runtime *rt, lpb_frame *f) {
    lpb_mutex_lock(&rt->mu);
    f->saved_owner = rt->owner;
    f->saved_depth = rt->depth;
    f->saved_borrowed = rt->borrowed;
    if (rt->owner == LPB_OWNER_HOST)
        rt->host_thread = lpb_thread_token();
    rt->owner = NULL;
    rt->depth = 0;
    rt->borrowed = 0;
    lpb_cond_broadcast(&rt->cv);
    lpb_mutex_unlock(&rt->mu);
}

static void lpb_unpark(lpb_frame *f) {
    lpb_runtime *rt = f->rt;
    lpb_mutex_lock(&rt->mu);
    while (rt->owner != NULL && !rt->closed)
        lpb_cond_wait(&rt->cv, &rt->mu);
    if (rt->closed) {
        lpb_mutex_unlock(&rt->mu);
        fprintf(stderr, "lua-python-bridge: fatal: a Lua state was closed while one of its "
                        "calls into Python was still running\n");
        abort();
    }
    rt->owner = f->saved_owner;
    rt->depth = f->saved_depth;
    rt->borrowed = f->saved_borrowed;
    lpb_mutex_unlock(&rt->mu);
}

/* ------------------------------------------------------------------------------------------ */
/* Lua -> Python                                                                              */
/* ------------------------------------------------------------------------------------------ */

void lpb_enter(lpb_runtime *rt, lua_State *L, lpb_frame *f) {
    f->L = L;
    f->rt = rt;
    f->parked = 0;
    lpb_flush_unrefs(rt, L);
    f->gil = PyGILState_Ensure();
    /* If this thread already held the GIL, other threads cannot be running Python, so keeping the
       runtime is safe; parking would let this thread wait for the runtime while holding the GIL. */
    if (f->gil == PyGILState_UNLOCKED) {
        lpb_park(rt, f);
        f->parked = 1;
    }
    f->prev = lpb_frames;
    lpb_frames = f;
    lpb_drain_decrefs();
}

/* python.serve: let Python threads run Lua on this state for a while. */
void lpb_serve(lpb_runtime *rt, unsigned long ms) {
    lpb_frame f;
    f.rt = rt;
    lpb_park(rt, &f);
    lpb_sleep_ms(ms);
    lpb_unpark(&f);
}

void lpb_leave(lpb_frame *f) {
    lpb_frames = f->prev;
    PyGILState_Release(f->gil);
    if (f->parked)
        lpb_unpark(f);
}

static int lpb_entry_fn(lua_State *L) {
    lpb_runtime *rt = (lpb_runtime *)lua_touserdata(L, lua_upvalueindex(2));
    lpb_frame f;
    int st;
    if (rt->closed)
        return luaL_error(L, "python: the Lua state is being closed");
    if (!Py_IsInitialized())
        return luaL_error(L, "python: the Python interpreter has been finalized");
    lpb_enter(rt, L, &f);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    st = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
    lpb_leave(&f);
    if (st != LUA_OK)
        return lua_error(L);
    return lua_gettop(L);
}

void lpb_push_entry(lua_State *L, lpb_runtime *rt, lua_CFunction impl, int nup) {
    lua_pushcclosure(L, impl, nup);
    lua_pushlightuserdata(L, rt);
    lua_pushcclosure(L, lpb_entry_fn, 2);
}

/* ------------------------------------------------------------------------------------------ */
/* Python -> Lua                                                                              */
/* ------------------------------------------------------------------------------------------ */

static int lpb_create_thread(lua_State *L) {
    lua_newthread(L);
    lua_pushvalue(L, -1);
    lua_pushinteger(L, luaL_ref(L, LUA_REGISTRYINDEX));
    return 2;
}

/* The caller owns the runtime. */
static lua_State *lpb_pool_take(lpb_runtime *rt, int *ref) {
    lua_State *M = rt->main, *T;
    if (rt->n_pool > 0) {
        rt->n_pool--;
        *ref = rt->pool[rt->n_pool].ref;
        return rt->pool[rt->n_pool].L;
    }
    if (!lua_checkstack(M, 3))
        return NULL;
    lua_pushcfunction(M, lpb_create_thread);
    if (lua_pcall(M, 0, 2, 0) != LUA_OK) {
        lua_pop(M, 1);
        return NULL;
    }
    T = lua_tothread(M, -2);
    *ref = (int)lua_tointeger(M, -1);
    lua_pop(M, 2);
    return T;
}

static void lpb_pool_give(lpb_runtime *rt, lua_State *T, int ref) {
    lua_settop(T, 0);
    if (rt->n_pool == rt->cap_pool) {
        int cap = rt->cap_pool ? rt->cap_pool * 2 : 4;
        lpb_pooled *p = (lpb_pooled *)realloc(rt->pool, (size_t)cap * sizeof *p);
        if (!p) {
            luaL_unref(T, LUA_REGISTRYINDEX, ref);
            return;
        }
        rt->pool = p;
        rt->cap_pool = cap;
    }
    rt->pool[rt->n_pool].L = T;
    rt->pool[rt->n_pool].ref = ref;
    rt->n_pool++;
}

int lpb_run(lpb_runtime *rt, lua_CFunction op, void *ud) {
    lua_State *L;
    lpb_frame *f;
    int ref = LUA_NOREF, top, st, rc = -1;

    lpb_drain_decrefs();
    if (lpb_acquire(rt) < 0)
        return -1;
    /* Inside a call from Lua on this thread: continue on that Lua thread, like any C function
       would. Otherwise use a Lua thread of our own, so that Python threads never interleave
       frames on a shared Lua stack. */
    f = lpb_frame_for(rt);
    if (f) {
        L = f->L;
    } else if (!(L = lpb_pool_take(rt, &ref))) {
        lpb_release(rt);
        PyErr_NoMemory();
        return -1;
    }
    lpb_flush_unrefs(rt, L);
    top = lua_gettop(L);
    if (!lua_checkstack(L, LUA_MINSTACK)) {
        PyErr_NoMemory();
    } else {
        lua_pushcfunction(L, op);
        lua_pushlightuserdata(L, ud);
        st = lua_pcall(L, 1, 0, 0);
        if (st != LUA_OK) {
            if (!PyErr_Occurred()) {
                const char *msg = lua_tostring(L, -1);
                PyErr_SetString(st == LUA_ERRMEM ? PyExc_MemoryError : lpb_LuaError,
                                msg ? msg : "Lua error");
            }
        } else if (!PyErr_Occurred()) {
            rc = 0;
        }
    }
    lua_settop(L, top);
    if (ref != LUA_NOREF)
        lpb_pool_give(rt, L, ref);
    lpb_release(rt);
    return rc;
}

int lpb_msgh(lua_State *L) {
    luaL_traceback(L, L, NULL, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &lpb_traceback_key);
    lua_settop(L, 1);
    return 1;
}

/* Calls the function below the nargs arguments on top of the stack, without the GIL.
   Returns the number of results, or -1 with a Python exception set and the function and
   arguments removed. */
int lpb_lcall(lpb_runtime *rt, lua_State *L, int nargs, int nres) {
    int fidx = lua_gettop(L) - nargs;
    PyThreadState *ts;
    int st;
    lua_pushcfunction(L, lpb_msgh);
    lua_insert(L, fidx);
    ts = PyEval_SaveThread();
    st = lua_pcall(L, nargs, nres, fidx);
    PyEval_RestoreThread(ts);
    if (st != LUA_OK) {
        lpb_set_pyerr_from_lua(rt, L, -1, st);
        lua_settop(L, fidx - 1);
        return -1;
    }
    lua_remove(L, fidx);
    return lua_gettop(L) - (fidx - 1);
}

void lpb_runtime_close_owned(lpb_runtime *rt) {
    lua_State *L;
    if (!rt->owns_state || rt->closed)
        return;
    if (lpb_acquire(rt) < 0) {
        PyErr_Clear();
        return;
    }
    L = rt->main;
    lpb_runtime_incref(rt);             /* keep the struct alive across lua_close */
    lua_close(L);
    lpb_release(rt);
    lpb_runtime_decref(rt);
}
