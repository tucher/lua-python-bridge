/*
 * lua-python-bridge: Python objects inside Lua (the Lua module "python").
 *
 * Copyright (c) 2002-2005 Gustavo Niemeyer <gustavo@niemeyer.net> (Lunatic Python)
 * Copyright (c) 2026 Aleks Tuchkov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Every function here that touches Python runs as an "entry" (lpb_push_entry): the GIL is held,
 * the call is protected, and Python exceptions become Lua errors through lpb_error().
 */
#include "bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT() lpb_current_runtime()

static lpb_pyobj *check_pyobj(lua_State *L, int idx) {
    lpb_pyobj *u = (lpb_pyobj *)luaL_checkudata(L, idx, LPB_PYOBJECT_MT);
    if (!u->o)
        luaL_error(L, "python.object has been released");
    return u;
}

static int is_kw(lua_State *L, int idx) {
    return luaL_testudata(L, idx, LPB_KWARGS_MT) != NULL;
}

/* Pushes a Python object and returns 1, or raises the current Python exception. Steals r. */
static int push_result(lpb_runtime *rt, lua_State *L, PyObject *r) {
    int rc;
    if (!r)
        return lpb_error(rt, L);
    rc = lpb_push_py(rt, L, r);
    Py_DECREF(r);
    if (rc < 0)
        return lpb_error(rt, L);
    return 1;
}

static PyObject *lua_string_to_py(lua_State *L, int idx) {
    size_t n;
    const char *s = lua_tolstring(L, idx, &n);
    return PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "surrogateescape");
}

/* ------------------------------------------------------------------------------------------ */
/* Argument tables (PLAN.md 5.7)                                                              */
/* ------------------------------------------------------------------------------------------ */

#define LPB_MAX_POSITIONAL (1 << 20)

static int argtable_error(lua_State *L, const char *what) {
    char buf[200];
    if (lua_type(L, -2) == LUA_TNUMBER) {
        lua_pushvalue(L, -2);           /* convert a copy, not the key lua_next is using */
        snprintf(buf, sizeof buf, "%s: invalid key %s in argument table (positional keys are "
                 "integers 1..n, named keys are strings)", what, lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        snprintf(buf, sizeof buf, "%s: invalid %s key in argument table (positional keys are "
                 "integers 1..n, named keys are strings)", what, luaL_typename(L, -2));
    }
    lua_pop(L, 2);
    PyErr_SetString(PyExc_TypeError, buf);
    return -1;
}

/* Parses the table at idx into positional arguments (*pargs, may be NULL) and keyword arguments
   (*pkwargs, NULL if none). Returns -1 with a Python exception set. */
static int parse_argtable(lpb_runtime *rt, lua_State *L, int idx, int allow_positional,
                          const char *what, PyObject **pargs, PyObject **pkwargs) {
    lua_Integer n = 0, i;
    int named = 0;
    PyObject *args = NULL, *kwargs = NULL;

    idx = lua_absindex(L, idx);
    luaL_checkstack(L, 4, NULL);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        int kt = lua_type(L, -2);
        if (kt == LUA_TNUMBER && allow_positional) {
            int isint = 0;
            i = lua_tointegerx(L, -2, &isint);
            if (!isint || i < 1)
                return argtable_error(L, what);
            if (i > n)
                n = i;
        } else if (kt == LUA_TSTRING) {
            named = 1;
        } else {
            return argtable_error(L, what);
        }
        lua_pop(L, 1);
    }
    if (n > LPB_MAX_POSITIONAL) {
        PyErr_SetString(PyExc_ValueError, "argument table has too many positional entries");
        return -1;
    }
    if (pargs) {
        args = PyTuple_New((Py_ssize_t)n);
        if (!args)
            return -1;
        for (i = 0; i < n; i++) {
            Py_INCREF(Py_None);
            PyTuple_SetItem(args, (Py_ssize_t)i, Py_None);
        }
    }
    if (named && !(kwargs = PyDict_New()))
        goto fail;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        PyObject *v = lpb_to_py(rt, L, -1);
        if (!v) {
            lua_pop(L, 2);
            goto fail;
        }
        if (lua_type(L, -2) == LUA_TNUMBER) {
            i = lua_tointeger(L, -2);
            PyTuple_SetItem(args, (Py_ssize_t)(i - 1), v);
        } else {
            PyObject *k = lua_string_to_py(L, -2);
            int rc = k ? PyDict_SetItem(kwargs, k, v) : -1;
            Py_XDECREF(k);
            Py_DECREF(v);
            if (rc < 0) {
                lua_pop(L, 2);
                goto fail;
            }
        }
        lua_pop(L, 1);
    }
    if (pargs)
        *pargs = args;
    *pkwargs = kwargs;
    return 0;
fail:
    Py_XDECREF(args);
    Py_XDECREF(kwargs);
    return -1;
}

/* Calls `callable` with the Lua values first..top as arguments; a trailing python.kw(t) supplies
   keyword arguments. */
static int call_with_stack(lpb_runtime *rt, lua_State *L, PyObject *callable, int first) {
    int top = lua_gettop(L), nargs = top - first + 1, i;
    PyObject *args, *kwargs = NULL, *r;

    if (nargs > 0 && is_kw(L, top)) {
        lpb_getuservalue(L, top);
        if (parse_argtable(rt, L, -1, 0, "python.kw", NULL, &kwargs) < 0)
            return lpb_error(rt, L);
        lua_pop(L, 1);
        nargs--;
    }
    args = PyTuple_New(nargs);
    if (!args) {
        Py_XDECREF(kwargs);
        return lpb_error(rt, L);
    }
    for (i = 0; i < nargs; i++) {
        PyObject *v;
        if (is_kw(L, first + i)) {
            Py_DECREF(args);
            Py_XDECREF(kwargs);
            return luaL_error(L, "python.kw(...) must be the last argument");
        }
        v = lpb_to_py(rt, L, first + i);
        if (!v) {
            Py_DECREF(args);
            Py_XDECREF(kwargs);
            return lpb_error(rt, L);
        }
        PyTuple_SetItem(args, i, v);
    }
    r = PyObject_Call(callable, args, kwargs);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return push_result(rt, L, r);
}

/* ------------------------------------------------------------------------------------------ */
/* python.object metamethods                                                                  */
/* ------------------------------------------------------------------------------------------ */

static int py_call(lua_State *L) {
    lpb_pyobj *u = check_pyobj(L, 1);
    return call_with_stack(RT(), L, u->o, 2);
}

/* Integer key at idx (an integral number), or 0 if it is not one. */
static int integer_key(lua_State *L, int idx, lua_Integer *out) {
    int isint = 0;
    if (lua_type(L, idx) != LUA_TNUMBER)
        return 0;
    *out = lua_tointegerx(L, idx, &isint);
    return isint;
}

static int push_attr(lpb_runtime *rt, lua_State *L, PyObject *o, int kidx) {
    PyObject *k = lua_string_to_py(L, kidx), *r;
    if (!k)
        return lpb_error(rt, L);
    r = PyObject_GetAttr(o, k);
    Py_DECREF(k);
    if (!r) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return lpb_error(rt, L);
        PyErr_Clear();
        lua_pushnil(L);
        return 1;
    }
    return push_result(rt, L, r);
}

/* o[key] where missing keys (LookupError) give nil; `attr_fallback`: string keys then try
   attributes. */
static int push_item(lpb_runtime *rt, lua_State *L, PyObject *o, PyObject *key, int kidx,
                     int attr_fallback) {
    PyObject *r = PyObject_GetItem(o, key);
    if (r)
        return push_result(rt, L, r);
    if (!PyErr_ExceptionMatches(PyExc_LookupError) &&
        !(attr_fallback && PyErr_ExceptionMatches(PyExc_TypeError)))
        return lpb_error(rt, L);
    PyErr_Clear();
    if (attr_fallback && lua_type(L, kidx) == LUA_TSTRING)
        return push_attr(rt, L, o, kidx);
    lua_pushnil(L);
    return 1;
}

static int py_get_fn(lua_State *L);
static int py_set_fn(lua_State *L);

static int py_index(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *u = check_pyobj(L, 1);
    PyObject *o = u->o, *key;
    int kt = lua_type(L, 2), rc;
    lua_Integer i;

    if (kt == LUA_TSTRING) {
        const char *k = lua_tostring(L, 2);
        if (strcmp(k, "__get") == 0 || strcmp(k, "__set") == 0) {
            lua_pushvalue(L, 1);
            lpb_push_entry(L, rt, k[2] == 'g' ? py_get_fn : py_set_fn, 1);
            return 1;
        }
    }
    if (u->mode == LPB_MODE_SEQ && integer_key(L, 2, &i)) {
        PyObject *r;
        if (i == 0) {
            lua_pushnil(L);
            return 1;
        }
        key = PyLong_FromLongLong((long long)(i > 0 ? i - 1 : i));
        if (!key)
            return lpb_error(rt, L);
        r = PyObject_GetItem(o, key);
        Py_DECREF(key);
        if (!r) {
            if (!PyErr_ExceptionMatches(PyExc_IndexError))
                return lpb_error(rt, L);
            PyErr_Clear();
            lua_pushnil(L);
            return 1;
        }
        return push_result(rt, L, r);
    }
    if (kt == LUA_TSTRING && u->mode != LPB_MODE_MAP)
        return push_attr(rt, L, o, 2);
    key = lpb_to_py(rt, L, 2);
    if (!key)
        return lpb_error(rt, L);
    rc = push_item(rt, L, o, key, 2, u->mode == LPB_MODE_MAP);
    Py_DECREF(key);
    return rc;
}

static int set_item(lpb_runtime *rt, lua_State *L, PyObject *o, int kidx, int vidx) {
    PyObject *key = lpb_to_py(rt, L, kidx), *v;
    int rc;
    if (!key)
        return lpb_error(rt, L);
    if (lua_isnil(L, vidx)) {
        rc = PyObject_DelItem(o, key);
        if (rc < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {   /* deleting a missing key */
            PyErr_Clear();
            rc = 0;
        }
    } else {
        v = lpb_to_py(rt, L, vidx);
        rc = v ? PyObject_SetItem(o, key, v) : -1;
        Py_XDECREF(v);
    }
    Py_DECREF(key);
    return rc < 0 ? lpb_error(rt, L) : 0;
}

static int set_attr(lpb_runtime *rt, lua_State *L, PyObject *o, int kidx, int vidx) {
    PyObject *k = lua_string_to_py(L, kidx), *v;
    int rc;
    if (!k)
        return lpb_error(rt, L);
    if (lua_isnil(L, vidx)) {
        rc = PyObject_SetAttr(o, k, NULL);
        if (rc < 0 && PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            rc = 0;
        }
    } else {
        v = lpb_to_py(rt, L, vidx);
        rc = v ? PyObject_SetAttr(o, k, v) : -1;
        Py_XDECREF(v);
    }
    Py_DECREF(k);
    return rc < 0 ? lpb_error(rt, L) : 0;
}

static int set_seq(lpb_runtime *rt, lua_State *L, PyObject *o, lua_Integer i, int vidx) {
    Py_ssize_t n = PyObject_Size(o);
    PyObject *key = NULL, *v = NULL, *r;
    int rc = -1;
    if (n < 0)
        return lpb_error(rt, L);
    if (lua_isnil(L, vidx)) {
        if (n > 0 && (i == n || i == -1)) {           /* o[#o] = nil pops the last element */
            key = PyLong_FromSsize_t(n - 1);
            rc = key ? PyObject_DelItem(o, key) : -1;
        } else if (i == n + 1) {
            rc = 0;                                    /* o[#o + 1] = nil: nothing to do */
        } else {
            return luaL_error(L, "cannot assign nil inside a Python sequence (index %d of %d): "
                              "Python sequences have no holes", (int)i, (int)n);
        }
    } else {
        v = lpb_to_py(rt, L, vidx);
        if (!v)
            return lpb_error(rt, L);
        if (i == n + 1) {                               /* o[#o + 1] = v appends */
            r = lpb_call_method1(o, "append", v);
            rc = r ? 0 : -1;
            Py_XDECREF(r);
        } else if ((i >= 1 && i <= n) || (i <= -1 && i >= -n)) {
            key = PyLong_FromLongLong((long long)(i > 0 ? i - 1 : i));
            rc = key ? PyObject_SetItem(o, key, v) : -1;
        } else {
            PyErr_SetString(PyExc_IndexError, "sequence index out of range");
        }
    }
    Py_XDECREF(key);
    Py_XDECREF(v);
    return rc < 0 ? lpb_error(rt, L) : 0;
}

static int py_newindex(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *u = check_pyobj(L, 1);
    lua_Integer i;
    if (u->mode == LPB_MODE_SEQ && integer_key(L, 2, &i))
        return set_seq(rt, L, u->o, i, 3);
    if (lua_type(L, 2) == LUA_TSTRING && u->mode != LPB_MODE_MAP)
        return set_attr(rt, L, u->o, 2, 3);
    return set_item(rt, L, u->o, 2, 3);
}

/* o.__get(key [, default]) and o.__set(key, value): raw Python item access. */
static int py_get_fn(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *u = (lpb_pyobj *)lua_touserdata(L, lua_upvalueindex(1));
    PyObject *key, *r;
    if (!u || !u->o)
        return luaL_error(L, "python.object has been released");
    key = lpb_to_py(rt, L, 1);
    if (!key)
        return lpb_error(rt, L);
    r = PyObject_GetItem(u->o, key);
    Py_DECREF(key);
    if (r)
        return push_result(rt, L, r);
    if (!PyErr_ExceptionMatches(PyExc_LookupError))
        return lpb_error(rt, L);
    PyErr_Clear();
    lua_settop(L, 2);
    return 1;
}

static int py_set_fn(lua_State *L) {
    lpb_pyobj *u = (lpb_pyobj *)lua_touserdata(L, lua_upvalueindex(1));
    if (!u || !u->o)
        return luaL_error(L, "python.object has been released");
    lua_settop(L, 2);
    return set_item(RT(), L, u->o, 1, 2);
}

static int py_len(lua_State *L) {
    lpb_pyobj *u = check_pyobj(L, 1);
    Py_ssize_t n = PyObject_Size(u->o);
    if (n < 0)
        return lpb_error(RT(), L);
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int py_eq(lua_State *L) {
    lpb_pyobj *a = lpb_test_pyobject(L, 1), *b = lpb_test_pyobject(L, 2);
    int r;
    if (!a || !b || !a->o || !b->o) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (a->o == b->o) {
        lua_pushboolean(L, 1);
        return 1;
    }
    r = PyObject_RichCompareBool(a->o, b->o, Py_EQ);
    if (r < 0)
        return lpb_error(RT(), L);
    lua_pushboolean(L, r);
    return 1;
}

static int compare(lua_State *L, int op) {
    lpb_runtime *rt = RT();
    PyObject *a = lpb_to_py(rt, L, 1), *b;
    int r;
    if (!a)
        return lpb_error(rt, L);
    b = lpb_to_py(rt, L, 2);
    if (!b) {
        Py_DECREF(a);
        return lpb_error(rt, L);
    }
    r = PyObject_RichCompareBool(a, b, op);
    Py_DECREF(a);
    Py_DECREF(b);
    if (r < 0)
        return lpb_error(rt, L);
    lua_pushboolean(L, r);
    return 1;
}

static int py_lt(lua_State *L) { return compare(L, Py_LT); }
static int py_le(lua_State *L) { return compare(L, Py_LE); }

enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_IDIV, OP_UNM,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_BNOT
};

static const struct { const char *name; int op; } arith_ops[] = {
    {"__add", OP_ADD}, {"__sub", OP_SUB}, {"__mul", OP_MUL}, {"__div", OP_DIV},
    {"__mod", OP_MOD}, {"__pow", OP_POW}, {"__idiv", OP_IDIV}, {"__unm", OP_UNM},
    {"__band", OP_BAND}, {"__bor", OP_BOR}, {"__bxor", OP_BXOR}, {"__shl", OP_SHL},
    {"__shr", OP_SHR}, {"__bnot", OP_BNOT}, {NULL, 0}
};

static int py_arith(lua_State *L) {
    lpb_runtime *rt = RT();
    int op = (int)lua_tointeger(L, lua_upvalueindex(1));
    int unary = op == OP_UNM || op == OP_BNOT;
    PyObject *a, *b = NULL, *r = NULL;
    a = lpb_to_py(rt, L, 1);
    if (!a)
        return lpb_error(rt, L);
    if (!unary && !(b = lpb_to_py(rt, L, 2))) {
        Py_DECREF(a);
        return lpb_error(rt, L);
    }
    switch (op) {
    case OP_ADD: r = PyNumber_Add(a, b); break;
    case OP_SUB: r = PyNumber_Subtract(a, b); break;
    case OP_MUL: r = PyNumber_Multiply(a, b); break;
    case OP_DIV: r = PyNumber_TrueDivide(a, b); break;
    case OP_MOD: r = PyNumber_Remainder(a, b); break;
    case OP_POW: r = PyNumber_Power(a, b, Py_None); break;
    case OP_IDIV: r = PyNumber_FloorDivide(a, b); break;
    case OP_UNM: r = PyNumber_Negative(a); break;
    case OP_BAND: r = PyNumber_And(a, b); break;
    case OP_BOR: r = PyNumber_Or(a, b); break;
    case OP_BXOR: r = PyNumber_Xor(a, b); break;
    case OP_SHL: r = PyNumber_Lshift(a, b); break;
    case OP_SHR: r = PyNumber_Rshift(a, b); break;
    case OP_BNOT: r = PyNumber_Invert(a); break;
    }
    Py_DECREF(a);
    Py_XDECREF(b);
    return push_result(rt, L, r);
}

/* str(o), with exceptions rendered like Python prints them. */
static PyObject *py_text(PyObject *o) {
    if (PyExceptionInstance_Check(o)) {
        PyObject *tbmod = PyImport_ImportModule("traceback"), *lines = NULL, *sep = NULL, *s = NULL;
        if (tbmod) {
            lines = lpb_call_method1(tbmod, "format_exception", o);
            sep = PyUnicode_FromString("");
            if (lines && sep)
                s = PyUnicode_Join(sep, lines);
        }
        Py_XDECREF(tbmod);
        Py_XDECREF(lines);
        Py_XDECREF(sep);
        if (s) {
            Py_ssize_t n = PyUnicode_GetLength(s);
            if (n > 0 && PyUnicode_ReadChar(s, n - 1) == '\n') {
                PyObject *t = PyUnicode_Substring(s, 0, n - 1);
                Py_DECREF(s);
                s = t;
            }
            return s;
        }
        PyErr_Clear();
    }
    return PyObject_Str(o);
}

static int push_text(lpb_runtime *rt, lua_State *L, PyObject *o) {
    PyObject *s = py_text(o), *b;
    char *p;
    Py_ssize_t n;
    if (!s)
        return lpb_error(rt, L);
    b = PyUnicode_AsEncodedString(s, "utf-8", "surrogateescape");
    Py_DECREF(s);
    if (!b || PyBytes_AsStringAndSize(b, &p, &n) < 0) {
        Py_XDECREF(b);
        return lpb_error(rt, L);
    }
    lua_pushlstring(L, p, (size_t)n);
    Py_DECREF(b);
    return 1;
}

static int py_tostring(lua_State *L) {
    lpb_pyobj *u = check_pyobj(L, 1);
    return push_text(RT(), L, u->o);
}

/* a .. b: Python objects as str(o); strings and numbers as themselves. */
static int py_concat(lua_State *L) {
    lpb_runtime *rt = RT();
    int i;
    for (i = 1; i <= 2; i++) {
        lpb_pyobj *u = lpb_test_pyobject(L, i);
        if (u && u->o) {
            push_text(rt, L, u->o);
        } else if (lua_type(L, i) == LUA_TSTRING || lua_type(L, i) == LUA_TNUMBER) {
            lua_pushvalue(L, i);
            lua_tostring(L, -1);
        } else {
            return luaL_error(L, "attempt to concatenate a %s value", luaL_typename(L, i));
        }
    }
    lua_concat(L, 2);
    return 1;
}

static void push_none(lua_State *L) {
    lpb_push_pyobject(L, Py_None, LPB_MODE_ATTR);
}

static int py_pairs_next(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *it = (lpb_pyobj *)lua_touserdata(L, lua_upvalueindex(1));
    int mode = (int)lua_tointeger(L, lua_upvalueindex(2));
    lua_Integer n = lua_tointeger(L, lua_upvalueindex(3));
    PyObject *item;
    if (!it || !it->o) {
        lua_pushnil(L);
        return 1;
    }
    item = PyIter_Next(it->o);
    if (!item) {
        if (PyErr_Occurred())
            return lpb_error(rt, L);
        lua_pushnil(L);
        return 1;
    }
    if (mode == LPB_MODE_MAP) {
        PyObject *k, *v;
        if (!PyTuple_Check(item) || PyTuple_Size(item) != 2) {
            Py_DECREF(item);
            return luaL_error(L, "items() did not yield (key, value) pairs");
        }
        k = PyTuple_GetItem(item, 0);
        v = PyTuple_GetItem(item, 1);
        if (k == Py_None)
            push_none(L);         /* nil would end the loop */
        else if (lpb_push_py(rt, L, k) < 0) {
            Py_DECREF(item);
            return lpb_error(rt, L);
        }
        if (lpb_push_py(rt, L, v) < 0) {
            Py_DECREF(item);
            return lpb_error(rt, L);
        }
        Py_DECREF(item);
        return 2;
    }
    n++;
    lua_pushinteger(L, n);
    lua_replace(L, lua_upvalueindex(3));
    lua_pushinteger(L, n);
    if (lpb_push_py(rt, L, item) < 0) {
        Py_DECREF(item);
        return lpb_error(rt, L);
    }
    Py_DECREF(item);
    return 2;
}

static int py_pairs(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *u = check_pyobj(L, 1);
    PyObject *src, *it;
    if (u->mode == LPB_MODE_MAP) {
        src = lpb_call_method0(u->o, "items");
        if (!src)
            return lpb_error(rt, L);
        it = PyObject_GetIter(src);
        Py_DECREF(src);
    } else {
        it = PyObject_GetIter(u->o);
    }
    if (!it)
        return lpb_error(rt, L);
    lpb_push_pyobject(L, it, LPB_MODE_ATTR);
    Py_DECREF(it);
    lua_pushinteger(L, u->mode);
    lua_pushinteger(L, 0);
    lpb_push_entry(L, rt, py_pairs_next, 3);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

static int py_close(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *u = check_pyobj(L, 1);
    lpb_pyobj *err = lpb_test_pyobject(L, 2);
    PyObject *exit_fn, *args, *r;
    int i;
    exit_fn = PyObject_GetAttrString(u->o, "__exit__");
    if (!exit_fn) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return lpb_error(rt, L);
        PyErr_Clear();
        return 0;
    }
    args = PyTuple_New(3);
    if (!args) {
        Py_DECREF(exit_fn);
        return lpb_error(rt, L);
    }
    if (err && err->o && PyExceptionInstance_Check(err->o)) {
        PyObject *tb = PyException_GetTraceback(err->o);
        Py_INCREF(Py_TYPE(err->o));
        PyTuple_SetItem(args, 0, (PyObject *)Py_TYPE(err->o));
        Py_INCREF(err->o);
        PyTuple_SetItem(args, 1, err->o);
        if (!tb) {
            Py_INCREF(Py_None);
            tb = Py_None;
        }
        PyTuple_SetItem(args, 2, tb);
    } else {
        for (i = 0; i < 3; i++) {
            Py_INCREF(Py_None);
            PyTuple_SetItem(args, i, Py_None);
        }
    }
    r = PyObject_CallObject(exit_fn, args);
    Py_DECREF(exit_fn);
    Py_DECREF(args);
    if (!r)
        return lpb_error(rt, L);
    Py_DECREF(r);
    return 0;
}

/* Not an entry: finalizers must not take the GIL each time, so references are released in
   batches by the next crossing. */
static int py_gc(lua_State *L) {
    lpb_pyobj *u = (lpb_pyobj *)lua_touserdata(L, 1);
    lpb_runtime *rt = (lpb_runtime *)lua_touserdata(L, lua_upvalueindex(1));
    if (u && u->o) {
        PyObject *o = u->o;
        u->o = NULL;
        lpb_defer_decref(o);
    }
    if (lpb_pending_decrefs() >= 4096 && rt && !rt->closed && Py_IsInitialized()) {
        lpb_frame f;
        lpb_enter(rt, L, &f);           /* drains the pending releases */
        lpb_leave(&f);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* python.* functions                                                                         */
/* ------------------------------------------------------------------------------------------ */

static int py_run(lua_State *L, int start) {
    lpb_runtime *rt = RT();
    size_t n;
    const char *code = luaL_checklstring(L, 1, &n);
    PyObject *g = NULL, *l = NULL, *co, *r;
    if (memchr(code, 0, n))
        return luaL_argerror(L, 1, "code contains a NUL byte");
    if (!lua_isnoneornil(L, 2)) {
        g = lpb_to_py(rt, L, 2);
        if (!g)
            return lpb_error(rt, L);
        if (!PyDict_Check(g)) {
            Py_DECREF(g);
            return luaL_argerror(L, 2, "globals must be a Python dict");
        }
    } else {
        PyObject *m = PyImport_AddModule("__main__");   /* borrowed */
        if (!m)
            return lpb_error(rt, L);
        g = PyModule_GetDict(m);
        Py_INCREF(g);
    }
    if (!lua_isnoneornil(L, 3)) {
        l = lpb_to_py(rt, L, 3);
        if (!l) {
            Py_DECREF(g);
            return lpb_error(rt, L);
        }
    } else {
        Py_INCREF(g);
        l = g;
    }
    co = Py_CompileString(code, "<lua>", start);
    r = co ? PyEval_EvalCode(co, g, l) : NULL;
    Py_XDECREF(co);
    Py_DECREF(g);
    Py_DECREF(l);
    if (start == Py_file_input) {
        if (!r)
            return lpb_error(rt, L);
        Py_DECREF(r);
        return 0;
    }
    return push_result(rt, L, r);
}

static int py_execute(lua_State *L) { return py_run(L, Py_file_input); }
static int py_eval(lua_State *L) { return py_run(L, Py_eval_input); }

static int py_import(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    return push_result(RT(), L, PyImport_ImportModule(name));
}

static int py_builtins(lua_State *L) {
    PyObject *b = PyEval_GetBuiltins();   /* borrowed */
    if (!b)
        return lpb_error(RT(), L);
    Py_INCREF(b);
    return push_result(RT(), L, b);
}

static int py_globals(lua_State *L) {
    PyObject *g = PyEval_GetGlobals();    /* borrowed; NULL outside any Python frame */
    if (!g) {
        PyObject *m = PyImport_AddModule("__main__");
        if (!m)
            return lpb_error(RT(), L);
        g = PyModule_GetDict(m);
    }
    Py_INCREF(g);
    return push_result(RT(), L, g);
}

static int py_locals(lua_State *L) {
    PyObject *l = PyEval_GetLocals();     /* borrowed */
    if (!l)
        return py_globals(L);
    Py_INCREF(l);
    return push_result(RT(), L, l);
}

static int as_mode(lua_State *L, int mode) {
    lpb_pyobj *u = check_pyobj(L, 1);
    lpb_push_pyobject(L, u->o, mode);
    return 1;
}

static int py_asattr(lua_State *L) { return as_mode(L, LPB_MODE_ATTR); }
static int py_asmap(lua_State *L) { return as_mode(L, LPB_MODE_MAP); }
static int py_asseq(lua_State *L) { return as_mode(L, LPB_MODE_SEQ); }

static int py_asbytes(lua_State *L) {
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    PyObject *b = PyBytes_FromStringAndSize(s, (Py_ssize_t)n);
    if (!b)
        return lpb_error(RT(), L);
    lpb_push_pyobject(L, b, LPB_MODE_ATTR);
    Py_DECREF(b);
    return 1;
}

static int py_asfunc_call(lua_State *L) {
    lpb_pyobj *u = (lpb_pyobj *)lua_touserdata(L, lua_upvalueindex(1));
    if (!u || !u->o)
        return luaL_error(L, "python.object has been released");
    return call_with_stack(RT(), L, u->o, 1);
}

static int py_asfunc(lua_State *L) {
    lpb_pyobj *u = check_pyobj(L, 1);
    if (!PyCallable_Check(u->o))
        return luaL_argerror(L, 1, "object is not callable");
    lua_settop(L, 1);
    lpb_push_entry(L, RT(), py_asfunc_call, 1);
    return 1;
}

static int py_iter_next(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *it = (lpb_pyobj *)lua_touserdata(L, lua_upvalueindex(1));
    PyObject *item;
    if (!it || !it->o)
        return 0;
    item = PyIter_Next(it->o);
    if (!item) {
        if (PyErr_Occurred())
            return lpb_error(rt, L);
        lua_pushnil(L);
        return 1;
    }
    if (item == Py_None) {          /* nil would end a generic for loop */
        Py_DECREF(item);
        push_none(L);
        return 1;
    }
    return push_result(rt, L, item);
}

static int py_iter(lua_State *L) {
    lpb_runtime *rt = RT();
    PyObject *o, *it;
    luaL_checkany(L, 1);
    o = lpb_to_py(rt, L, 1);
    if (!o)
        return lpb_error(rt, L);
    it = PyObject_GetIter(o);
    Py_DECREF(o);
    if (!it)
        return lpb_error(rt, L);
    lpb_push_pyobject(L, it, LPB_MODE_ATTR);
    Py_DECREF(it);
    lpb_push_entry(L, rt, py_iter_next, 1);
    return 1;
}

/* python.list / python.tuple / python.dict: copy a Lua table (raw contents) into a Python
   container; Python objects are passed to the Python constructor. */
static int copy_container(lua_State *L, const char *kind) {
    lpb_runtime *rt = RT();
    PyObject *r = NULL;
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TTABLE) {
        PyObject *o = lpb_to_py(rt, L, 1), *ctor;
        if (!o)
            return lpb_error(rt, L);
        ctor = PyDict_GetItemString(PyEval_GetBuiltins(), kind);   /* borrowed */
        r = ctor ? lpb_call1(ctor, o) : NULL;
        Py_DECREF(o);
        if (!ctor && !PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "builtins are not available");
        return push_result(rt, L, r);
    }
    if (kind[0] == 'd') {
        r = PyDict_New();
        if (!r)
            return lpb_error(rt, L);
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            PyObject *k = lpb_to_py(rt, L, -2), *v = k ? lpb_to_py(rt, L, -1) : NULL;
            int rc = v ? PyDict_SetItem(r, k, v) : -1;
            Py_XDECREF(k);
            Py_XDECREF(v);
            lua_pop(L, 1);
            if (rc < 0) {
                Py_DECREF(r);
                return lpb_error(rt, L);
            }
        }
    } else {
        lua_Integer n = (lua_Integer)lua_rawlen(L, 1), i;
        r = kind[0] == 'l' ? PyList_New((Py_ssize_t)n) : PyTuple_New((Py_ssize_t)n);
        if (!r)
            return lpb_error(rt, L);
        for (i = 1; i <= n; i++) {
            PyObject *v;
            lua_rawgeti(L, 1, i);
            v = lpb_to_py(rt, L, -1);
            lua_pop(L, 1);
            if (!v) {
                Py_DECREF(r);
                return lpb_error(rt, L);
            }
            if (kind[0] == 'l')
                PyList_SetItem(r, (Py_ssize_t)(i - 1), v);
            else
                PyTuple_SetItem(r, (Py_ssize_t)(i - 1), v);
        }
    }
    return push_result(rt, L, r);
}

static int py_list(lua_State *L) { return copy_container(L, "list"); }
static int py_tuple(lua_State *L) { return copy_container(L, "tuple"); }
static int py_dict(lua_State *L) { return copy_container(L, "dict"); }

static int py_kw(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    lpb_newuserdata(L, 1);
    lua_pushvalue(L, 1);
    lpb_setuservalue(L, -2);
    luaL_setmetatable(L, LPB_KWARGS_MT);
    return 1;
}

static int py_tablecall_call(lua_State *L) {
    lpb_runtime *rt = RT();
    lpb_pyobj *u = (lpb_pyobj *)lua_touserdata(L, lua_upvalueindex(1));
    PyObject *args = NULL, *kwargs = NULL, *r;
    if (!u || !u->o)
        return luaL_error(L, "python.object has been released");
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TTABLE)
        return luaL_error(L, "python.tablecall: expected exactly one argument table");
    if (parse_argtable(rt, L, 1, 1, "python.tablecall", &args, &kwargs) < 0)
        return lpb_error(rt, L);
    r = PyObject_Call(u->o, args, kwargs);
    Py_DECREF(args);
    Py_XDECREF(kwargs);
    return push_result(rt, L, r);
}

static int py_tablecall(lua_State *L) {
    lpb_pyobj *u = check_pyobj(L, 1);
    if (!PyCallable_Check(u->o))
        return luaL_argerror(L, 1, "object is not callable");
    lua_settop(L, 1);
    lpb_push_entry(L, RT(), py_tablecall_call, 1);
    return 1;
}

static int py_with(lua_State *L) {
    lpb_pyobj *u = check_pyobj(L, 1);
    lua_settop(L, 1);
    push_result(RT(), L, lpb_call_method0(u->o, "__enter__"));
    return 2;
}

/* Not an entry: does not touch Python. */
static int py_serve(lua_State *L) {
    lpb_runtime *rt = (lpb_runtime *)lua_touserdata(L, lua_upvalueindex(1));
    lua_Number s = luaL_optnumber(L, 1, 0);
    if (s < 0)
        s = 0;
    lpb_serve(rt, (unsigned long)(s * 1000.0));
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Module                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static const struct { const char *name; lua_CFunction fn; } object_methods[] = {
    {"__index", py_index}, {"__newindex", py_newindex}, {"__call", py_call},
    {"__len", py_len}, {"__eq", py_eq}, {"__lt", py_lt}, {"__le", py_le},
    {"__concat", py_concat}, {"__tostring", py_tostring}, {"__pairs", py_pairs},
    {"__close", py_close}, {NULL, NULL}
};

static const struct { const char *name; lua_CFunction fn; } module_functions[] = {
    {"execute", py_execute}, {"eval", py_eval}, {"import", py_import},
    {"builtins", py_builtins}, {"globals", py_globals}, {"locals", py_locals},
    {"asattr", py_asattr}, {"asmap", py_asmap}, {"asseq", py_asseq},
    {"asbytes", py_asbytes}, {"asfunc", py_asfunc}, {"iter", py_iter},
    {"list", py_list}, {"tuple", py_tuple}, {"dict", py_dict}, {"kw", py_kw},
    {"tablecall", py_tablecall}, {"with", py_with}, {NULL, NULL}
};

/* Runs as an entry (GIL held). */
static int build_module(lua_State *L) {
    lpb_runtime *rt = RT();
    int i;

    luaL_newmetatable(L, LPB_PYOBJECT_MT);
    for (i = 0; object_methods[i].name; i++) {
        lpb_push_entry(L, rt, object_methods[i].fn, 0);
        lua_setfield(L, -2, object_methods[i].name);
    }
    for (i = 0; arith_ops[i].name; i++) {
        lua_pushinteger(L, arith_ops[i].op);
        lpb_push_entry(L, rt, py_arith, 1);
        lua_setfield(L, -2, arith_ops[i].name);
    }
    lua_pushlightuserdata(L, rt);
    lua_pushcclosure(L, py_gc, 1);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, LPB_PYOBJECT_MT);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    luaL_newmetatable(L, LPB_KWARGS_MT);
    lua_pushliteral(L, LPB_KWARGS_MT);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    lua_newtable(L);
    for (i = 0; module_functions[i].name; i++) {
        lpb_push_entry(L, rt, module_functions[i].fn, 0);
        lua_setfield(L, -2, module_functions[i].name);
    }
    lua_pushlightuserdata(L, rt);
    lua_pushcclosure(L, py_serve, 1);
    lua_setfield(L, -2, "serve");
    lua_pushliteral(L, "lua-python-bridge " LPB_VERSION);
    lua_setfield(L, -2, "version");
    push_none(L);
    lua_setfield(L, -2, "none");
    return 1;
}

#ifdef LPB_LUA_MODULE

#ifndef _WIN32
static void lpb_atexit_finalize(void) {
    if (Py_IsInitialized()) {
        PyGILState_Ensure();
        Py_FinalizeEx();
    }
}
#endif

/* Pushes the current Python exception as a message string (GIL held). */
static void push_pyerr_message(lua_State *L, const char *prefix) {
    PyObject *t, *v, *tb, *s = NULL;
    const char *msg = NULL;
    PyErr_Fetch(&t, &v, &tb);
    if (v)
        s = PyObject_Str(v);
    if (s)
        msg = PyUnicode_AsUTF8AndSize(s, NULL);
    lua_pushfstring(L, "%s: %s", prefix, msg ? msg : "unknown error");
    Py_XDECREF(s);
    Py_XDECREF(t);
    Py_XDECREF(v);
    Py_XDECREF(tb);
    PyErr_Clear();
}

static int set_argv(void) {
    PyObject *argv = PyList_New(0), *empty;
    int rc = -1;
    if (!argv)
        return -1;
    empty = PyUnicode_FromString("");
    if (empty && PyList_Append(argv, empty) == 0)
        rc = PySys_SetObject("argv", argv);
    Py_XDECREF(empty);
    Py_DECREF(argv);
    return rc;
}

static int is_free_threaded(void) {
    PyObject *flags = PySys_GetObject("abiflags");   /* borrowed; absent on Windows */
    const char *s;
    if (!flags || !PyUnicode_Check(flags))
        return 0;
    s = PyUnicode_AsUTF8AndSize(flags, NULL);
    return s && strchr(s, 't') != NULL;
}

/* First `require "python"` in a Lua state of a Lua host. Returns the runtime, or NULL with an
   error message pushed. */
static lpb_runtime *init_lua_host(lua_State *L) {
    lpb_runtime *rt = NULL;
    if (!Py_IsInitialized()) {
        PyImport_AppendInittab("lua", lpb_pyinit_lua);
        Py_InitializeEx(0);
        if (!Py_IsInitialized()) {
            lua_pushliteral(L, "python: Py_InitializeEx failed");
            return NULL;
        }
        if (is_free_threaded()) {
            lua_pushliteral(L, "python: free-threaded Python builds are not supported");
            PyEval_SaveThread();
            return NULL;
        }
        if (set_argv() < 0 || lpb_pyloader_after_init() < 0 || lpb_types_init() < 0 ||
            lpb_convert_init() < 0 || !(rt = lpb_runtime_create(L, 0, 1))) {
            push_pyerr_message(L, "python: initialization failed");
            PyEval_SaveThread();
            return NULL;
        }
#ifndef _WIN32
        /* A DLL's atexit handlers run under the loader lock on Windows, where finalizing Python
           could deadlock; there, output is only flushed when the Lua state closes. */
        atexit(lpb_atexit_finalize);
#endif
        PyEval_SaveThread();
    } else {
        /* Python is already running in this process (e.g. the Lua host is embedded in a Python
           application): `import lua` cannot be registered as a builtin any more. */
        PyGILState_STATE g = PyGILState_Ensure();
        PyObject *modules = PySys_GetObject("modules");   /* borrowed */
        if (lpb_types_init() < 0 || lpb_convert_init() < 0 ||
            !(rt = lpb_runtime_create(L, 0, 1))) {
            push_pyerr_message(L, "python: initialization failed");
            PyGILState_Release(g);
            return NULL;
        }
        if (modules && PyDict_Check(modules) && !PyDict_GetItemString(modules, "lua")) {
            PyObject *m = lpb_pyinit_lua();
            if (!m || PyDict_SetItemString(modules, "lua", m) < 0)
                PyErr_Clear();
            Py_XDECREF(m);
        }
        PyGILState_Release(g);
    }
    return rt;
}

#endif /* LPB_LUA_MODULE */

int lpb_luaopen(lua_State *L) {
    lpb_runtime *rt;
#ifdef LPB_DYNAMIC_PYTHON
    if (lpb_pyloader_load(L) < 0)
        return lua_error(L);
#endif
    rt = lpb_runtime_get(L);
    if (!rt) {
#ifdef LPB_LUA_MODULE
        rt = init_lua_host(L);
        if (!rt)
            return lua_error(L);
#else
        return luaL_error(L, "python: this Lua state was not created by the Python module 'lua'");
#endif
    }
    lpb_push_entry(L, rt, build_module, 0);
    lua_call(L, 0, 1);
    return 1;
}

#ifdef LPB_LUA_MODULE
LPB_EXPORT int luaopen_python(lua_State *L) {
    return lpb_luaopen(L);
}
#endif
