/*
 * lua-python-bridge: Lua objects inside Python (the Python module "lua").
 *
 * Copyright (c) 2002-2005 Gustavo Niemeyer <gustavo@niemeyer.net> (Lunatic Python)
 * Copyright (c) 2026 Aleks Tuchkov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Every operation on a Lua value runs through lpb_run(): it owns the Lua state for the duration,
 * works on a Lua thread that is safe for the calling thread, and is protected. Lua code itself
 * (calls, metamethods) runs through lpb_lcall(), without the GIL.
 */
#include "bridge.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32) && !defined(LPB_LUA_MODULE)
#  include <dlfcn.h>
#endif

PyObject *lpb_LuaError;
PyTypeObject *lpb_LuaObject_Type;
PyTypeObject *lpb_LuaSequence_Type;
PyTypeObject *lpb_LuaIterator_Type;

/* ------------------------------------------------------------------------------------------ */
/* Operation context                                                                          */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    lpb_runtime *rt;
    LuaObject *self;
    PyObject *a, *b;          /* inputs */
    PyObject *result;         /* output, new reference */
    const char *s;            /* input buffer */
    size_t len;
    lua_Integer i, j, k;      /* integer inputs / outputs */
    int flag;
} op_ctx;

#define CTX(L) ((op_ctx *)lua_touserdata((L), 1))

static void push_self(lua_State *L, op_ctx *c) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->self->ref);
}

static int run_on(LuaObject *self, lua_CFunction op, op_ctx *c) {
    c->self = self;
    c->rt = self->rt;
    return lpb_run(self->rt, op, c);
}

static int state_open(LuaObject *self) {
    return self->rt && !self->rt->closed;
}

/* nres results on top of the stack: none -> None, one -> value, several -> tuple. */
static PyObject *collect_results(lpb_runtime *rt, lua_State *L, int nres) {
    PyObject *t;
    int i;
    if (nres == 0)
        Py_RETURN_NONE;
    if (nres == 1)
        return lpb_to_py(rt, L, -1);
    t = PyTuple_New(nres);
    if (!t)
        return NULL;
    for (i = 0; i < nres; i++) {
        PyObject *v = lpb_to_py(rt, L, -nres + i);
        if (!v) {
            Py_DECREF(t);
            return NULL;
        }
        PyTuple_SetItem(t, i, v);
    }
    return t;
}

static int has_metafield(lua_State *L, int idx, const char *name) {
    if (luaL_getmetafield(L, idx, name) == LUA_TNIL)
        return 0;
    lua_pop(L, 1);
    return 1;
}

static int is_indexable(lua_State *L, int idx, const char *mm) {
    int t = lua_type(L, idx);
    return t == LUA_TTABLE || (t == LUA_TUSERDATA && has_metafield(L, idx, mm));
}

/* ------------------------------------------------------------------------------------------ */
/* LuaObject                                                                                  */
/* ------------------------------------------------------------------------------------------ */

PyObject *lpb_luaobject_new(lpb_runtime *rt, lua_State *L, int idx) {
    LuaObject *o = PyObject_New(LuaObject, lpb_LuaObject_Type);
    if (!o)
        return NULL;
    o->rt = NULL;
    o->ref = LUA_NOREF;
    o->ptr = lua_topointer(L, idx);
    o->tname = luaL_typename(L, idx);
    lua_pushvalue(L, idx);
    o->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    o->rt = rt;
    lpb_runtime_incref(rt);
    return (PyObject *)o;
}

static void free_instance(PyObject *self) {
    PyTypeObject *tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    f(self);
    Py_DECREF(tp);
}

static void LuaObject_dealloc(PyObject *self) {
    LuaObject *o = (LuaObject *)self;
    if (o->rt) {
        lpb_defer_unref(o->rt, o->ref);
        lpb_runtime_decref(o->rt);
    }
    free_instance(self);
}

/* get: t[key] -> result (NULL for nil); flag: value is indexable */
static int op_get(lua_State *L) {
    op_ctx *c = CTX(L);
    push_self(L, c);
    c->flag = is_indexable(L, 2, "__index");
    if (!c->flag)
        return 0;
    lua_pushcfunction(L, lpb_prim_gettable);
    lua_pushvalue(L, 2);
    if (lpb_push_py(c->rt, L, c->a) < 0 || lpb_lcall(c->rt, L, 2, 1) < 0)
        return 0;
    if (!lua_isnil(L, -1))
        c->result = lpb_to_py(c->rt, L, -1);
    return 0;
}

/* set: t[key] = value (b == NULL: nil); flag: value is indexable */
static int op_set(lua_State *L) {
    op_ctx *c = CTX(L);
    push_self(L, c);
    c->flag = is_indexable(L, 2, "__newindex");
    if (!c->flag)
        return 0;
    lua_pushcfunction(L, lpb_prim_settable);
    lua_pushvalue(L, 2);
    if (lpb_push_py(c->rt, L, c->a) < 0)
        return 0;
    if (c->b) {
        if (lpb_push_py(c->rt, L, c->b) < 0)
            return 0;
    } else {
        lua_pushnil(L);
    }
    lpb_lcall(c->rt, L, 3, 0);
    return 0;
}

static PyObject *LuaObject_getattro(PyObject *self, PyObject *name) {
    LuaObject *o = (LuaObject *)self;
    op_ctx c;
    PyObject *r;
    memset(&c, 0, sizeof c);
    if (state_open(o)) {
        c.a = name;
        if (run_on(o, op_get, &c) < 0)
            return NULL;
        if (c.result)
            return c.result;
    }
    r = PyObject_GenericGetAttr(self, name);
    if (r || !PyErr_ExceptionMatches(PyExc_AttributeError))
        return r;
    if (!c.flag || lpb_is_dunder(name))
        return NULL;
    PyErr_Clear();
    Py_RETURN_NONE;
}

static int LuaObject_setattro(PyObject *self, PyObject *name, PyObject *value) {
    LuaObject *o = (LuaObject *)self;
    op_ctx c;
    char buf[96];
    memset(&c, 0, sizeof c);
    c.a = name;
    c.b = value;
    if (run_on(o, op_set, &c) < 0)
        return -1;
    if (!c.flag) {
        snprintf(buf, sizeof buf, "Lua %s values have no fields", o->tname);
        PyErr_SetString(PyExc_AttributeError, buf);
        return -1;
    }
    return 0;
}

static PyObject *LuaObject_subscript(PyObject *self, PyObject *key) {
    LuaObject *o = (LuaObject *)self;
    op_ctx c;
    char buf[96];
    memset(&c, 0, sizeof c);
    c.a = key;
    if (run_on(o, op_get, &c) < 0)
        return NULL;
    if (!c.flag) {
        snprintf(buf, sizeof buf, "Lua %s is not subscriptable", o->tname);
        PyErr_SetString(PyExc_TypeError, buf);
        return NULL;
    }
    if (c.result)
        return c.result;
    Py_RETURN_NONE;
}

static int LuaObject_ass_subscript(PyObject *self, PyObject *key, PyObject *value) {
    LuaObject *o = (LuaObject *)self;
    op_ctx c;
    char buf[96];
    memset(&c, 0, sizeof c);
    c.a = key;
    c.b = value;
    if (run_on(o, op_set, &c) < 0)
        return -1;
    if (!c.flag) {
        snprintf(buf, sizeof buf, "Lua %s does not support item assignment", o->tname);
        PyErr_SetString(PyExc_TypeError, buf);
        return -1;
    }
    return 0;
}

/* len: #t -> i; flag: has a length */
static int op_len(lua_State *L) {
    op_ctx *c = CTX(L);
    int t;
    push_self(L, c);
    t = lua_type(L, 2);
    c->flag = t == LUA_TTABLE || t == LUA_TSTRING || (t == LUA_TUSERDATA && has_metafield(L, 2, "__len"));
    if (!c->flag)
        return 0;
    lua_pushcfunction(L, lpb_prim_len);
    lua_pushvalue(L, 2);
    if (lpb_lcall(c->rt, L, 1, 1) < 0)
        return 0;
    c->i = lua_tointeger(L, -1);
    return 0;
}

static Py_ssize_t luaobject_len(LuaObject *o) {
    op_ctx c;
    char buf[96];
    memset(&c, 0, sizeof c);
    if (run_on(o, op_len, &c) < 0)
        return -1;
    if (!c.flag) {
        snprintf(buf, sizeof buf, "Lua %s has no length", o->tname);
        PyErr_SetString(PyExc_TypeError, buf);
        return -1;
    }
    return (Py_ssize_t)c.i;
}

static Py_ssize_t LuaObject_length(PyObject *self) {
    return luaobject_len((LuaObject *)self);
}

static int LuaObject_bool(PyObject *self) {
    (void)self;
    return 1;     /* nil and false never become LuaObjects */
}

/* call: f(*a, **b) -> result. Keyword arguments arrive as a trailing table (README: Calls). */
static int op_call(lua_State *L) {
    op_ctx *c = CTX(L);
    Py_ssize_t n = PyTuple_Size(c->a), i, pos = 0;
    int kw = c->b && PyDict_Size(c->b) > 0, nres;
    PyObject *k, *v;
    if (n > 1000000) {
        PyErr_SetString(PyExc_ValueError, "too many arguments for a Lua call");
        return 0;
    }
    luaL_checkstack(L, (int)n + 8, "too many arguments");
    push_self(L, c);
    for (i = 0; i < n; i++)
        if (lpb_push_py(c->rt, L, PyTuple_GetItem(c->a, i)) < 0)
            return 0;
    if (kw) {
        lua_createtable(L, 0, (int)PyDict_Size(c->b));
        while (PyDict_Next(c->b, &pos, &k, &v)) {
            if (lpb_push_py(c->rt, L, k) < 0 || lpb_push_py(c->rt, L, v) < 0)
                return 0;
            lua_rawset(L, -3);
        }
    }
    nres = lpb_lcall(c->rt, L, (int)n + kw, LUA_MULTRET);
    if (nres >= 0)
        c->result = collect_results(c->rt, L, nres);
    return 0;
}

static PyObject *LuaObject_call(PyObject *self, PyObject *args, PyObject *kwargs) {
    op_ctx c;
    memset(&c, 0, sizeof c);
    c.a = args;
    c.b = kwargs;
    if (run_on((LuaObject *)self, op_call, &c) < 0)
        return NULL;
    return c.result;
}

/* compare self with other (a) using i = Py_LT / Py_LE / Py_GT / Py_GE -> flag */
static int op_order(lua_State *L) {
    op_ctx *c = CTX(L);
    int swap = c->i == Py_GT || c->i == Py_GE;
    lua_pushcfunction(L, (c->i == Py_LT || c->i == Py_GT) ? lpb_prim_lt : lpb_prim_le);
    if (swap) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ((LuaObject *)c->a)->ref);
        push_self(L, c);
    } else {
        push_self(L, c);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ((LuaObject *)c->a)->ref);
    }
    if (lpb_lcall(c->rt, L, 2, 1) < 0)
        return 0;
    c->flag = lua_toboolean(L, -1);
    return 0;
}

static PyObject *LuaObject_richcompare(PyObject *self, PyObject *other, int op) {
    LuaObject *a = (LuaObject *)self, *b;
    op_ctx c;
    if (!LuaObject_Check(other) || ((LuaObject *)other)->rt != a->rt)
        Py_RETURN_NOTIMPLEMENTED;
    b = (LuaObject *)other;
    if (op == Py_EQ || op == Py_NE) {
        /* Raw equality: consistent with __hash__. */
        int eq = a->ptr == b->ptr;
        if ((op == Py_EQ) == eq)
            Py_RETURN_TRUE;
        Py_RETURN_FALSE;
    }
    memset(&c, 0, sizeof c);
    c.a = other;
    c.i = op;
    if (run_on(a, op_order, &c) < 0)
        return NULL;
    if (c.flag)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static Py_hash_t LuaObject_hash(PyObject *self) {
    Py_hash_t h = (Py_hash_t)((uintptr_t)((LuaObject *)self)->ptr >> 4);
    return h == -1 ? -2 : h;
}

/* tostring: result if the value has __tostring / __name, else NULL */
static int op_tostring(lua_State *L) {
    op_ctx *c = CTX(L);
    size_t n;
    const char *s;
    push_self(L, c);
    if (!has_metafield(L, 2, "__tostring") && !has_metafield(L, 2, "__name"))
        return 0;
    lua_pushcfunction(L, lpb_prim_tostring);
    lua_pushvalue(L, 2);
    if (lpb_lcall(c->rt, L, 1, 1) < 0)
        return 0;
    s = lua_tolstring(L, -1, &n);
    if (s)
        c->result = PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "replace");
    return 0;
}

static PyObject *LuaObject_repr(PyObject *self) {
    LuaObject *o = (LuaObject *)self;
    char buf[128];
    if (state_open(o)) {
        op_ctx c;
        memset(&c, 0, sizeof c);
        if (run_on(o, op_tostring, &c) < 0)
            return NULL;
        if (c.result)
            return c.result;
        snprintf(buf, sizeof buf, "<Lua %s at 0x%" PRIxPTR ">", o->tname, (uintptr_t)o->ptr);
    } else {
        snprintf(buf, sizeof buf, "<Lua %s at 0x%" PRIxPTR " (state closed)>", o->tname,
                 (uintptr_t)o->ptr);
    }
    return PyUnicode_FromString(buf);
}

/* ------------------------------------------------------------------------------------------ */
/* Iterator over a Lua table (keys, values or items), respecting __pairs                      */
/* ------------------------------------------------------------------------------------------ */

enum { ITER_KEYS, ITER_VALUES, ITER_ITEMS };
enum { IT_START, IT_RAW, IT_PAIRS, IT_DONE };

typedef struct {
    PyObject_HEAD
    LuaObject *table;
    int kind, state;
    int kref;                 /* raw: current key */
    int fref, sref, cref;     /* __pairs: function, state, control */
} LuaIterator;

static PyObject *iterator_new(LuaObject *table, int kind) {
    LuaIterator *it = PyObject_New(LuaIterator, lpb_LuaIterator_Type);
    if (!it)
        return NULL;
    Py_INCREF(table);
    it->table = table;
    it->kind = kind;
    it->state = IT_START;
    it->kref = it->fref = it->sref = it->cref = LUA_NOREF;
    return (PyObject *)it;
}

static void iterator_release(LuaIterator *it) {
    lpb_runtime *rt = it->table->rt;
    lpb_defer_unref(rt, it->kref);
    lpb_defer_unref(rt, it->fref);
    lpb_defer_unref(rt, it->sref);
    lpb_defer_unref(rt, it->cref);
    it->kref = it->fref = it->sref = it->cref = LUA_NOREF;
    it->state = IT_DONE;
}

static void LuaIterator_dealloc(PyObject *self) {
    LuaIterator *it = (LuaIterator *)self;
    iterator_release(it);
    Py_DECREF(it->table);
    free_instance(self);
}

static void replace_ref(lua_State *L, int *ref, int idx) {
    lua_pushvalue(L, idx);
    int nref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (*ref != LUA_NOREF && *ref != LUA_REFNIL)
        luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    *ref = nref;
}

static int op_iter_next(lua_State *L) {
    op_ctx *c = CTX(L);
    LuaIterator *it = (LuaIterator *)c->a;
    int top;
    char buf[96];

    if (it->state == IT_START) {
        push_self(L, c);                                    /* 2 */
        if (luaL_getmetafield(L, 2, "__pairs") != LUA_TNIL) {
            lua_pushvalue(L, 2);
            if (lpb_lcall(c->rt, L, 1, 3) < 0) {
                it->state = IT_DONE;
                return 0;
            }
            replace_ref(L, &it->fref, -3);
            replace_ref(L, &it->sref, -2);
            replace_ref(L, &it->cref, -1);
            it->state = IT_PAIRS;
        } else if (lua_type(L, 2) == LUA_TTABLE) {
            it->kref = LUA_REFNIL;
            it->state = IT_RAW;
        } else {
            it->state = IT_DONE;
            snprintf(buf, sizeof buf, "Lua %s is not iterable", it->table->tname);
            PyErr_SetString(PyExc_TypeError, buf);
            return 0;
        }
        lua_settop(L, 1);
    }

    if (it->state == IT_RAW) {
        push_self(L, c);                                    /* 2 */
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->kref);        /* 3: previous key */
        if (!lua_next(L, 2)) {
            iterator_release(it);
            return 0;
        }
        replace_ref(L, &it->kref, -2);
    } else if (it->state == IT_PAIRS) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->fref);
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->sref);
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->cref);
        if (lpb_lcall(c->rt, L, 2, 2) < 0) {
            iterator_release(it);
            return 0;
        }
        if (lua_isnil(L, -2)) {
            iterator_release(it);
            return 0;
        }
        replace_ref(L, &it->cref, -2);
    } else {
        return 0;
    }

    top = lua_gettop(L);                                    /* key at top-1, value at top */
    if (it->kind == ITER_KEYS) {
        c->result = lpb_to_py(c->rt, L, top - 1);
    } else if (it->kind == ITER_VALUES) {
        c->result = lpb_to_py(c->rt, L, top);
    } else {
        PyObject *k = lpb_to_py(c->rt, L, top - 1), *v = k ? lpb_to_py(c->rt, L, top) : NULL;
        PyObject *t = v ? PyTuple_New(2) : NULL;
        if (t) {
            PyTuple_SetItem(t, 0, k);
            PyTuple_SetItem(t, 1, v);
            c->result = t;
        } else {
            Py_XDECREF(k);
            Py_XDECREF(v);
        }
    }
    return 0;
}

static PyObject *LuaIterator_next(PyObject *self) {
    LuaIterator *it = (LuaIterator *)self;
    op_ctx c;
    if (it->state == IT_DONE)
        return NULL;
    memset(&c, 0, sizeof c);
    c.a = self;
    if (run_on(it->table, op_iter_next, &c) < 0) {
        iterator_release(it);
        return NULL;
    }
    return c.result;       /* NULL without an exception: exhausted */
}

static PyObject *LuaIterator_iter(PyObject *self) {
    Py_INCREF(self);
    return self;
}

static PyObject *LuaObject_iter(PyObject *self) {
    return iterator_new((LuaObject *)self, ITER_KEYS);
}

static PyObject *iter_list(PyObject *self, int kind) {
    PyObject *it = iterator_new((LuaObject *)self, kind), *l;
    if (!it)
        return NULL;
    l = PySequence_List(it);
    Py_DECREF(it);
    return l;
}

static PyObject *LuaObject_keys(PyObject *self, PyObject *unused) {
    (void)unused;
    return iter_list(self, ITER_KEYS);
}

static PyObject *LuaObject_values(PyObject *self, PyObject *unused) {
    (void)unused;
    return iter_list(self, ITER_VALUES);
}

static PyObject *LuaObject_items(PyObject *self, PyObject *unused) {
    (void)unused;
    return iter_list(self, ITER_ITEMS);
}

static PyMethodDef LuaObject_methods[] = {
    {"keys", LuaObject_keys, METH_NOARGS, "List of the table's keys (used when the table has no 'keys' field)."},
    {"values", LuaObject_values, METH_NOARGS, "List of the table's values (used when the table has no 'values' field)."},
    {"items", LuaObject_items, METH_NOARGS, "List of (key, value) pairs (used when the table has no 'items' field)."},
    {NULL, NULL, 0, NULL}
};

/* ------------------------------------------------------------------------------------------ */
/* LuaSequence: 0-based view of a Lua array                                                   */
/* ------------------------------------------------------------------------------------------ */

static int prim_append(lua_State *L) {               /* (t, v) */
    lua_Integer n = luaL_len(L, 1);
    lua_seti(L, 1, n + 1);
    return 0;
}

static int prim_insert(lua_State *L) {               /* (t, pos, v) */
    lua_Integer n = luaL_len(L, 1), pos = lua_tointeger(L, 2), i;
    for (i = n; i >= pos; i--) {
        lua_geti(L, 1, i);
        lua_seti(L, 1, i + 1);
    }
    lua_pushvalue(L, 3);
    lua_seti(L, 1, pos);
    return 0;
}

static int prim_remove(lua_State *L) {               /* (t, pos) -> removed value */
    lua_Integer n = luaL_len(L, 1), pos = lua_tointeger(L, 2), i;
    lua_geti(L, 1, pos);
    for (i = pos; i < n; i++) {
        lua_geti(L, 1, i + 1);
        lua_seti(L, 1, i);
    }
    lua_pushnil(L);
    lua_seti(L, 1, n);
    return 1;
}

static int prim_clear(lua_State *L) {                /* (t) */
    lua_Integer n = luaL_len(L, 1), i;
    for (i = n; i >= 1; i--) {
        lua_pushnil(L);
        lua_seti(L, 1, i);
    }
    return 0;
}

static int prim_range(lua_State *L) {                /* (t, start, step, count) -> {values...} */
    lua_Integer start = lua_tointeger(L, 2), step = lua_tointeger(L, 3), count = lua_tointeger(L, 4), i;
    lua_createtable(L, (int)count, 0);
    for (i = 0; i < count; i++) {
        lua_geti(L, 1, start + i * step);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* seq ops: k selects the primitive; i, j integer arguments; b optional value */
enum { SEQ_GET, SEQ_SET, SEQ_APPEND, SEQ_INSERT, SEQ_REMOVE, SEQ_CLEAR, SEQ_RANGE };

static int op_seq(lua_State *L) {
    op_ctx *c = CTX(L);
    int nargs = 1, nres = 0, n;
    switch (c->k) {
    case SEQ_GET: lua_pushcfunction(L, lpb_prim_gettable); break;
    case SEQ_SET: lua_pushcfunction(L, lpb_prim_settable); break;
    case SEQ_APPEND: lua_pushcfunction(L, prim_append); break;
    case SEQ_INSERT: lua_pushcfunction(L, prim_insert); break;
    case SEQ_REMOVE: lua_pushcfunction(L, prim_remove); break;
    case SEQ_CLEAR: lua_pushcfunction(L, prim_clear); break;
    case SEQ_RANGE: lua_pushcfunction(L, prim_range); break;
    }
    push_self(L, c);
    switch (c->k) {
    case SEQ_GET: lua_pushinteger(L, c->i); nargs = 2; nres = 1; break;
    case SEQ_SET: lua_pushinteger(L, c->i); nargs = 3; break;
    case SEQ_APPEND: nargs = 2; break;
    case SEQ_INSERT: lua_pushinteger(L, c->i); nargs = 3; break;
    case SEQ_REMOVE: lua_pushinteger(L, c->i); nargs = 2; nres = 1; break;
    case SEQ_CLEAR: break;
    case SEQ_RANGE:
        lua_pushinteger(L, c->i);
        lua_pushinteger(L, c->j);
        lua_pushinteger(L, (lua_Integer)c->len);
        nargs = 4;
        nres = 1;
        break;
    }
    if (c->k == SEQ_SET || c->k == SEQ_APPEND || c->k == SEQ_INSERT) {
        if (lpb_push_py(c->rt, L, c->b) < 0)
            return 0;
    }
    n = lpb_lcall(c->rt, L, nargs, nres);
    if (n < 0)
        return 0;
    if (c->k == SEQ_GET || c->k == SEQ_REMOVE) {
        c->result = lpb_to_py(c->rt, L, -1);
    } else if (c->k == SEQ_RANGE) {
        Py_ssize_t count = (Py_ssize_t)c->len, i;
        PyObject *l = PyList_New(count);
        if (!l)
            return 0;
        for (i = 0; i < count; i++) {
            PyObject *v;
            lua_rawgeti(L, -1, (lua_Integer)i + 1);
            v = lpb_to_py(c->rt, L, -1);
            lua_pop(L, 1);
            if (!v) {
                Py_DECREF(l);
                return 0;
            }
            PyList_SetItem(l, i, v);
        }
        c->result = l;
    }
    return 0;
}

static PyObject *seq_op(LuaSequence *s, int k, lua_Integer i, lua_Integer j, size_t len, PyObject *v) {
    op_ctx c;
    memset(&c, 0, sizeof c);
    c.k = k;
    c.i = i;
    c.j = j;
    c.len = len;
    c.b = v;
    if (run_on(s->table, op_seq, &c) < 0)
        return NULL;
    if (c.result)
        return c.result;
    Py_RETURN_NONE;
}

static Py_ssize_t LuaSequence_length(PyObject *self) {
    return luaobject_len(((LuaSequence *)self)->table);
}

/* Normalizes a Python index against length n; -1 with IndexError if out of range. */
static int seq_index(PyObject *key, Py_ssize_t n, Py_ssize_t *out) {
    Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred())
        return -1;
    if (i < 0)
        i += n;
    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "lua.seq index out of range");
        return -1;
    }
    *out = i;
    return 0;
}

static PyObject *LuaSequence_subscript(PyObject *self, PyObject *key) {
    LuaSequence *s = (LuaSequence *)self;
    Py_ssize_t n = LuaSequence_length(self), i, start, stop, step, count;
    if (n < 0)
        return NULL;
    if (PySlice_Check(key)) {
        if (PySlice_Unpack(key, &start, &stop, &step) < 0)
            return NULL;
        count = PySlice_AdjustIndices(n, &start, &stop, step);
        if (count == 0)
            return PyList_New(0);
        return seq_op(s, SEQ_RANGE, start + 1, step, (size_t)count, NULL);
    }
    if (!PyIndex_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "lua.seq indices must be integers or slices");
        return NULL;
    }
    if (seq_index(key, n, &i) < 0)
        return NULL;
    return seq_op(s, SEQ_GET, i + 1, 0, 0, NULL);
}

static PyObject *LuaSequence_item(PyObject *self, Py_ssize_t i) {
    Py_ssize_t n = LuaSequence_length(self);
    if (n < 0)
        return NULL;
    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "lua.seq index out of range");
        return NULL;
    }
    return seq_op((LuaSequence *)self, SEQ_GET, i + 1, 0, 0, NULL);
}

static int LuaSequence_ass_subscript(PyObject *self, PyObject *key, PyObject *value) {
    LuaSequence *s = (LuaSequence *)self;
    Py_ssize_t n = LuaSequence_length(self), i;
    PyObject *r;
    if (n < 0)
        return -1;
    if (!PyIndex_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "lua.seq supports integer indices only for assignment");
        return -1;
    }
    if (seq_index(key, n, &i) < 0)
        return -1;
    r = value ? seq_op(s, SEQ_SET, i + 1, 0, 0, value) : seq_op(s, SEQ_REMOVE, i + 1, 0, 0, NULL);
    Py_XDECREF(r);
    return r ? 0 : -1;
}

static PyObject *LuaSequence_iter(PyObject *self) {
    return PySeqIter_New(self);
}

static PyObject *LuaSequence_repr(PyObject *self) {
    PyObject *l = PySequence_List(self), *r, *inner;
    if (!l)
        return NULL;
    inner = PyObject_Repr(l);
    Py_DECREF(l);
    if (!inner)
        return NULL;
    r = PyUnicode_FromString("lua.seq(");
    if (r) {
        PyObject *t = PyUnicode_Concat(r, inner), *close = PyUnicode_FromString(")");
        Py_DECREF(r);
        r = (t && close) ? PyUnicode_Concat(t, close) : NULL;
        Py_XDECREF(t);
        Py_XDECREF(close);
    }
    Py_DECREF(inner);
    return r;
}

static PyObject *LuaSequence_append(PyObject *self, PyObject *v) {
    return seq_op((LuaSequence *)self, SEQ_APPEND, 0, 0, 0, v);
}

static PyObject *LuaSequence_insert(PyObject *self, PyObject *args) {
    PyObject *idx, *v;
    Py_ssize_t n, i;
    if (PyTuple_Size(args) != 2) {
        PyErr_SetString(PyExc_TypeError, "insert expected 2 arguments");
        return NULL;
    }
    idx = PyTuple_GetItem(args, 0);
    v = PyTuple_GetItem(args, 1);
    n = LuaSequence_length(self);
    if (n < 0)
        return NULL;
    i = PyNumber_AsSsize_t(idx, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred())
        return NULL;
    if (i < 0)                              /* list.insert semantics: clamp */
        i = i + n < 0 ? 0 : i + n;
    if (i > n)
        i = n;
    return seq_op((LuaSequence *)self, SEQ_INSERT, i + 1, 0, 0, v);
}

static PyObject *LuaSequence_pop(PyObject *self, PyObject *args) {
    Py_ssize_t n = LuaSequence_length(self), i;
    PyObject *idx;
    if (n < 0)
        return NULL;
    if (n == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from empty lua.seq");
        return NULL;
    }
    if (PyTuple_Size(args) > 1) {
        PyErr_SetString(PyExc_TypeError, "pop expected at most 1 argument");
        return NULL;
    }
    if (PyTuple_Size(args) == 1) {
        idx = PyTuple_GetItem(args, 0);
        if (seq_index(idx, n, &i) < 0)
            return NULL;
    } else {
        i = n - 1;
    }
    return seq_op((LuaSequence *)self, SEQ_REMOVE, i + 1, 0, 0, NULL);
}

static PyObject *LuaSequence_extend(PyObject *self, PyObject *iterable) {
    PyObject *it = PyObject_GetIter(iterable), *v;
    if (!it)
        return NULL;
    while ((v = PyIter_Next(it))) {
        PyObject *r = LuaSequence_append(self, v);
        Py_DECREF(v);
        if (!r) {
            Py_DECREF(it);
            return NULL;
        }
        Py_DECREF(r);
    }
    Py_DECREF(it);
    if (PyErr_Occurred())
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *LuaSequence_clear(PyObject *self, PyObject *unused) {
    (void)unused;
    return seq_op((LuaSequence *)self, SEQ_CLEAR, 0, 0, 0, NULL);
}

static void LuaSequence_dealloc(PyObject *self) {
    Py_XDECREF(((LuaSequence *)self)->table);
    free_instance(self);
}

static PyMethodDef LuaSequence_methods[] = {
    {"append", LuaSequence_append, METH_O, "Append a value (t[#t + 1] = v)."},
    {"insert", LuaSequence_insert, METH_VARARGS, "Insert a value before index i, shifting later elements."},
    {"pop", LuaSequence_pop, METH_VARARGS, "Remove and return the element at index i (default: last)."},
    {"extend", LuaSequence_extend, METH_O, "Append all values from an iterable."},
    {"clear", LuaSequence_clear, METH_NOARGS, "Remove all elements."},
    {NULL, NULL, 0, NULL}
};

/* ------------------------------------------------------------------------------------------ */
/* Type objects                                                                               */
/* ------------------------------------------------------------------------------------------ */

static PyType_Slot LuaObject_slots[] = {
    {Py_tp_dealloc, (void *)LuaObject_dealloc},
    {Py_tp_repr, (void *)LuaObject_repr},
    {Py_tp_str, (void *)LuaObject_repr},
    {Py_tp_call, (void *)LuaObject_call},
    {Py_tp_getattro, (void *)LuaObject_getattro},
    {Py_tp_setattro, (void *)LuaObject_setattro},
    {Py_tp_richcompare, (void *)LuaObject_richcompare},
    {Py_tp_hash, (void *)LuaObject_hash},
    {Py_tp_iter, (void *)LuaObject_iter},
    {Py_mp_length, (void *)LuaObject_length},
    {Py_mp_subscript, (void *)LuaObject_subscript},
    {Py_mp_ass_subscript, (void *)LuaObject_ass_subscript},
    {Py_nb_bool, (void *)LuaObject_bool},
    {Py_tp_methods, (void *)LuaObject_methods},
    {Py_tp_doc, (void *)"A Lua value (table, function, userdata or thread) owned by a Lua state."},
    {0, NULL}
};

static PyType_Spec LuaObject_spec = {
    "lua.LuaObject", sizeof(LuaObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, LuaObject_slots
};

static PyType_Slot LuaIterator_slots[] = {
    {Py_tp_dealloc, (void *)LuaIterator_dealloc},
    {Py_tp_iter, (void *)LuaIterator_iter},
    {Py_tp_iternext, (void *)LuaIterator_next},
    {0, NULL}
};

static PyType_Spec LuaIterator_spec = {
    "lua._LuaIterator", sizeof(LuaIterator), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, LuaIterator_slots
};

static PyType_Slot LuaSequence_slots[] = {
    {Py_tp_dealloc, (void *)LuaSequence_dealloc},
    {Py_tp_repr, (void *)LuaSequence_repr},
    {Py_tp_iter, (void *)LuaSequence_iter},
    {Py_mp_length, (void *)LuaSequence_length},
    {Py_sq_length, (void *)LuaSequence_length},
    {Py_sq_item, (void *)LuaSequence_item},
    {Py_mp_subscript, (void *)LuaSequence_subscript},
    {Py_mp_ass_subscript, (void *)LuaSequence_ass_subscript},
    {Py_tp_methods, (void *)LuaSequence_methods},
    {Py_tp_doc, (void *)"A 0-based list view of a Lua array: lua.seq(t)[0] is t[1]."},
    {0, NULL}
};

static PyType_Spec LuaSequence_spec = {
    "lua.LuaSequence", sizeof(LuaSequence), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, LuaSequence_slots
};

int lpb_types_init(void) {
    PyObject *abc, *mseq, *r;
    if (lpb_LuaObject_Type)
        return 0;
    lpb_LuaError = PyErr_NewException("lua.LuaError", NULL, NULL);
    if (!lpb_LuaError)
        return -1;
    lpb_LuaObject_Type = (PyTypeObject *)PyType_FromSpec(&LuaObject_spec);
    lpb_LuaIterator_Type = (PyTypeObject *)PyType_FromSpec(&LuaIterator_spec);
    lpb_LuaSequence_Type = (PyTypeObject *)PyType_FromSpec(&LuaSequence_spec);
    if (!lpb_LuaObject_Type || !lpb_LuaIterator_Type || !lpb_LuaSequence_Type)
        goto fail;
    abc = PyImport_ImportModule("collections.abc");
    mseq = abc ? PyObject_GetAttrString(abc, "MutableSequence") : NULL;
    r = mseq ? lpb_call_method1(mseq, "register", (PyObject *)lpb_LuaSequence_Type) : NULL;
    Py_XDECREF(abc);
    Py_XDECREF(mseq);
    if (!r)
        goto fail;
    Py_DECREF(r);
    return 0;
fail:
    Py_CLEAR(lpb_LuaError);
    Py_CLEAR(lpb_LuaObject_Type);
    Py_CLEAR(lpb_LuaIterator_Type);
    Py_CLEAR(lpb_LuaSequence_Type);
    return -1;
}

/* ------------------------------------------------------------------------------------------ */
/* Module functions                                                                           */
/* ------------------------------------------------------------------------------------------ */

static lpb_runtime *default_runtime(void) {
    lpb_runtime *rt = lpb_runtime_default();
    if (!rt)
        PyErr_SetString(lpb_LuaError, "no Lua state is available");
    return rt;
}

static int run_default(lua_CFunction op, op_ctx *c) {
    c->rt = default_runtime();
    if (!c->rt)
        return -1;
    return lpb_run(c->rt, op, c);
}

/* code -> results; flag: evaluate as an expression */
static int op_exec(lua_State *L) {
    op_ctx *c = CTX(L);
    int st, nres;
    if (c->flag) {
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        luaL_addstring(&b, "return ");
        luaL_addlstring(&b, c->s, c->len);
        luaL_pushresult(&b);
        st = luaL_loadbufferx(L, lua_tostring(L, -1), lua_rawlen(L, -1), "=<python>", "t");
        lua_remove(L, -2);
    } else {
        st = luaL_loadbufferx(L, c->s, c->len, "=<python>", "t");
    }
    if (st != LUA_OK) {
        lpb_set_pyerr_from_lua(c->rt, L, -1, st);
        return 0;
    }
    nres = lpb_lcall(c->rt, L, 0, LUA_MULTRET);
    if (nres >= 0)
        c->result = collect_results(c->rt, L, nres);
    return 0;
}

static int code_arg(PyObject *code, op_ctx *c) {
    Py_ssize_t n;
    if (PyUnicode_Check(code)) {
        c->s = PyUnicode_AsUTF8AndSize(code, &n);
        if (!c->s)
            return -1;
    } else if (PyBytes_Check(code)) {
        char *s;
        if (PyBytes_AsStringAndSize(code, &s, &n) < 0)
            return -1;
        c->s = s;
    } else {
        PyErr_SetString(PyExc_TypeError, "Lua code must be str or bytes");
        return -1;
    }
    c->len = (size_t)n;
    return 0;
}

static PyObject *run_code(PyObject *code, int expr) {
    op_ctx c;
    memset(&c, 0, sizeof c);
    if (code_arg(code, &c) < 0)
        return NULL;
    c.flag = expr;
    if (run_default(op_exec, &c) < 0)
        return NULL;
    return c.result;
}

static PyObject *lua_execute(PyObject *mod, PyObject *code) {
    (void)mod;
    return run_code(code, 0);
}

static PyObject *lua_eval(PyObject *mod, PyObject *code) {
    (void)mod;
    return run_code(code, 1);
}

static int op_globals(lua_State *L) {
    op_ctx *c = CTX(L);
    lua_pushglobaltable(L);
    c->result = lpb_to_py(c->rt, L, -1);
    return 0;
}

static PyObject *lua_globals(PyObject *mod, PyObject *unused) {
    op_ctx c;
    (void)mod;
    (void)unused;
    memset(&c, 0, sizeof c);
    if (run_default(op_globals, &c) < 0)
        return NULL;
    return c.result;
}

static int op_require(lua_State *L) {
    op_ctx *c = CTX(L);
    int nres;
    lua_pushglobaltable(L);
    lua_pushliteral(L, "require");
    lua_rawget(L, -2);
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        PyErr_SetString(lpb_LuaError, "require is not available in this Lua state");
        return 0;
    }
    if (lpb_push_py(c->rt, L, c->a) < 0)
        return 0;
    nres = lpb_lcall(c->rt, L, 1, LUA_MULTRET);
    if (nres >= 0)
        c->result = collect_results(c->rt, L, nres);
    return 0;
}

static PyObject *lua_require(PyObject *mod, PyObject *name) {
    op_ctx c;
    (void)mod;
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "module name must be a str");
        return NULL;
    }
    memset(&c, 0, sizeof c);
    c.a = name;
    if (run_default(op_require, &c) < 0)
        return NULL;
    return c.result;
}

/* table(a = items list or NULL, b = fields dict / pairs list or NULL) -> new table */
static int op_table(lua_State *L) {
    op_ctx *c = CTX(L);
    Py_ssize_t n = c->a ? PyList_Size(c->a) : 0, i, pos = 0;
    PyObject *k, *v;
    lua_createtable(L, (int)n, 0);
    for (i = 0; i < n; i++) {
        if (lpb_push_py(c->rt, L, PyList_GetItem(c->a, i)) < 0)
            return 0;
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    if (c->b && PyDict_Check(c->b)) {
        while (PyDict_Next(c->b, &pos, &k, &v)) {
            if (lpb_push_py(c->rt, L, k) < 0 || lpb_push_py(c->rt, L, v) < 0)
                return 0;
            lua_rawset(L, -3);
        }
    } else if (c->b) {                           /* list of (key, value) pairs */
        n = PyList_Size(c->b);
        for (i = 0; i < n; i++) {
            PyObject *pair = PyList_GetItem(c->b, i);
            if (!PyTuple_Check(pair) || PyTuple_Size(pair) != 2) {
                PyErr_SetString(PyExc_TypeError, "expected (key, value) pairs");
                return 0;
            }
            k = PyTuple_GetItem(pair, 0);
            if (k == Py_None) {
                PyErr_SetString(PyExc_ValueError, "None cannot be a Lua table key");
                return 0;
            }
            if (lpb_push_py(c->rt, L, k) < 0 || lpb_push_py(c->rt, L, PyTuple_GetItem(pair, 1)) < 0)
                return 0;
            lua_rawset(L, -3);
        }
    }
    c->result = lpb_to_py(c->rt, L, -1);
    return 0;
}

static PyObject *lua_table(PyObject *mod, PyObject *args, PyObject *kwargs) {
    op_ctx c;
    PyObject *items;
    (void)mod;
    memset(&c, 0, sizeof c);
    items = PySequence_List(args);
    if (!items)
        return NULL;
    c.a = items;
    c.b = kwargs;
    if (run_default(op_table, &c) < 0)
        c.result = NULL;
    Py_DECREF(items);
    return c.result;
}

static PyObject *lua_table_from(PyObject *mod, PyObject *obj) {
    op_ctx c;
    PyObject *data;
    int mode;
    (void)mod;
    memset(&c, 0, sizeof c);
    mode = lpb_wrap_mode(obj);
    if (mode < 0)
        return NULL;
    if (mode == LPB_MODE_MAP) {
        data = PyMapping_Items(obj);
        c.b = data;
    } else {
        data = PySequence_List(obj);
        c.a = data;
    }
    if (!data)
        return NULL;
    if (run_default(op_table, &c) < 0)
        c.result = NULL;
    Py_DECREF(data);
    return c.result;
}

static PyObject *lua_seq(PyObject *mod, PyObject *t) {
    LuaSequence *s;
    (void)mod;
    if (LuaSequence_Check(t)) {
        Py_INCREF(t);
        return t;
    }
    if (!LuaObject_Check(t)) {
        PyErr_SetString(PyExc_TypeError, "lua.seq() expects a Lua table");
        return NULL;
    }
    s = PyObject_New(LuaSequence, lpb_LuaSequence_Type);
    if (!s)
        return NULL;
    Py_INCREF(t);
    s->table = (LuaObject *)t;
    return (PyObject *)s;
}

/* lua.tablecall(f)(*args, **kwargs) == f{args..., k=v} */
static int op_tablecall(lua_State *L) {
    op_ctx *c = CTX(L);
    Py_ssize_t n = PyTuple_Size(c->a), i, pos = 0;
    PyObject *k, *v;
    int nres;
    push_self(L, c);
    lua_createtable(L, (int)n, c->b ? (int)PyDict_Size(c->b) : 0);
    for (i = 0; i < n; i++) {
        if (lpb_push_py(c->rt, L, PyTuple_GetItem(c->a, i)) < 0)
            return 0;
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    if (c->b) {
        while (PyDict_Next(c->b, &pos, &k, &v)) {
            if (lpb_push_py(c->rt, L, k) < 0 || lpb_push_py(c->rt, L, v) < 0)
                return 0;
            lua_rawset(L, -3);
        }
    }
    nres = lpb_lcall(c->rt, L, 1, LUA_MULTRET);
    if (nres >= 0)
        c->result = collect_results(c->rt, L, nres);
    return 0;
}

static PyObject *tablecall_impl(PyObject *f, PyObject *args, PyObject *kwargs) {
    op_ctx c;
    memset(&c, 0, sizeof c);
    c.a = args;
    c.b = kwargs;
    if (run_on((LuaObject *)f, op_tablecall, &c) < 0)
        return NULL;
    return c.result;
}

LPB_METH_CAST_BEGIN
static PyMethodDef tablecall_def = {
    "tablecall", LPB_METH(tablecall_impl), METH_VARARGS | METH_KEYWORDS,
    "Calls the wrapped Lua function with one argument table {args..., name=value...}."
};
LPB_METH_CAST_END

static PyObject *lua_tablecall(PyObject *mod, PyObject *f) {
    (void)mod;
    if (!LuaObject_Check(f)) {
        PyErr_SetString(PyExc_TypeError, "lua.tablecall() expects a Lua function");
        return NULL;
    }
    return PyCFunction_NewEx(&tablecall_def, f, NULL);
}

LPB_METH_CAST_BEGIN
static PyMethodDef lua_methods[] = {
    {"execute", lua_execute, METH_O, "execute(code) -> result\n\nRun a Lua chunk and return its results (None, a value, or a tuple)."},
    {"eval", lua_eval, METH_O, "eval(expr) -> result\n\nEvaluate a Lua expression."},
    {"globals", lua_globals, METH_NOARGS, "globals() -> LuaObject\n\nThe Lua globals table."},
    {"require", lua_require, METH_O, "require(name) -> result\n\nCall Lua's require."},
    {"table", LPB_METH(lua_table), METH_VARARGS | METH_KEYWORDS,
     "table(*items, **fields) -> LuaObject\n\nA new Lua table {items..., field=value...}."},
    {"table_from", lua_table_from, METH_O,
     "table_from(obj) -> LuaObject\n\nA new Lua table from a mapping (keys kept) or an iterable (1-based)."},
    {"seq", lua_seq, METH_O, "seq(t) -> LuaSequence\n\nA 0-based list view of a Lua array."},
    {"tablecall", lua_tablecall, METH_O,
     "tablecall(f) -> callable\n\nf2(*args, **kwargs) calls the Lua function f with one table {args..., k=v}."},
    {NULL, NULL, 0, NULL}
};
LPB_METH_CAST_END

/* ------------------------------------------------------------------------------------------ */
/* Module init                                                                                */
/* ------------------------------------------------------------------------------------------ */

#ifndef LPB_LUA_MODULE
/* Python host: the module owns one Lua state. */
static lpb_runtime *lpb_owned_rt;

static int setup_state(lua_State *L) {
    lpb_runtime **out = (lpb_runtime **)lua_touserdata(L, 1);
    luaL_openlibs(L);
    *out = lpb_runtime_create(L, 1, 0);
    if (!*out)
        return luaL_error(L, "out of memory");
    luaL_requiref(L, "python", lpb_luaopen, 1);
    return 0;
}

static void lpb_module_free(void *m) {
    (void)m;
    if (lpb_owned_rt) {
        lpb_runtime *rt = lpb_owned_rt;
        lpb_owned_rt = NULL;
        lpb_runtime_close_owned(rt);
    }
}

#  ifndef _WIN32
/* Let binary Lua modules loaded by `require` resolve the Lua API against this extension. */
static void promote_lua_symbols(void) {
    Dl_info info;
    if (dladdr((void *)&lpb_pyinit_lua, &info) && info.dli_fname)
        (void)dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
}
#  endif
#endif

static struct PyModuleDef lua_module = {
    PyModuleDef_HEAD_INIT, "lua",
    "Lua inside Python, and Python inside Lua.",
    -1, lua_methods, NULL, NULL, NULL,
#ifndef LPB_LUA_MODULE
    lpb_module_free
#else
    NULL
#endif
};

static int add_object(PyObject *m, const char *name, PyObject *o) {
    return PyModule_AddObjectRef(m, name, o);
}

PyObject *lpb_pyinit_lua(void) {
    PyObject *m;
    if (lpb_types_init() < 0 || lpb_convert_init() < 0)
        return NULL;
    m = PyModule_Create(&lua_module);
    if (!m)
        return NULL;
    if (add_object(m, "LuaError", lpb_LuaError) < 0 ||
        add_object(m, "LuaObject", (PyObject *)lpb_LuaObject_Type) < 0 ||
        add_object(m, "LuaSequence", (PyObject *)lpb_LuaSequence_Type) < 0 ||
        PyModule_AddStringConstant(m, "__version__", LPB_VERSION) < 0 ||
        PyModule_AddStringConstant(m, "LUA_VERSION", LUA_RELEASE) < 0) {
        Py_DECREF(m);
        return NULL;
    }
#ifndef LPB_LUA_MODULE
    if (!lpb_owned_rt) {
        lpb_runtime *rt = NULL;
        lua_State *L = luaL_newstate();
        int st;
        if (!L) {
            Py_DECREF(m);
            return PyErr_NoMemory();
        }
        lua_pushcfunction(L, setup_state);
        lua_pushlightuserdata(L, &rt);
        st = lua_pcall(L, 1, 0, 0);
        if (st != LUA_OK) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_RuntimeError, lua_tostring(L, -1));
            if (rt) {
                lpb_runtime_close_owned(rt);
            } else {
                lua_close(L);
            }
            Py_DECREF(m);
            return NULL;
        }
        lpb_owned_rt = rt;
    }
#  ifndef _WIN32
    promote_lua_symbols();
#  endif
#endif
    return m;
}

#ifndef LPB_LUA_MODULE
PyMODINIT_FUNC PyInit_lua(void) {
    return lpb_pyinit_lua();
}
#endif
