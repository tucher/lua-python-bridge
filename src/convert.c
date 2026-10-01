/*
 * lua-python-bridge: value conversion and error bridging.
 *
 * Copyright (c) 2002-2005 Gustavo Niemeyer <gustavo@niemeyer.net> (Lunatic Python)
 * Copyright (c) 2026 Aleks Tuchkov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "bridge.h"

#include <stdio.h>
#include <string.h>

static PyObject *lpb_abc_Mapping;
static PyObject *lpb_abc_Sequence;
static PyObject *lpb_numbers_Integral;

int lpb_convert_init(void) {
    PyObject *m;
    if (lpb_abc_Mapping)
        return 0;
    m = PyImport_ImportModule("collections.abc");
    if (!m)
        return -1;
    lpb_abc_Mapping = PyObject_GetAttrString(m, "Mapping");
    lpb_abc_Sequence = PyObject_GetAttrString(m, "Sequence");
    Py_DECREF(m);
    m = PyImport_ImportModule("numbers");
    if (m) {
        lpb_numbers_Integral = PyObject_GetAttrString(m, "Integral");
        Py_DECREF(m);
    }
    if (!lpb_abc_Mapping || !lpb_abc_Sequence || !lpb_numbers_Integral) {
        Py_CLEAR(lpb_abc_Mapping);
        Py_CLEAR(lpb_abc_Sequence);
        Py_CLEAR(lpb_numbers_Integral);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Small Python helpers (the variadic call APIs are avoided on purpose)                       */
/* ------------------------------------------------------------------------------------------ */

PyObject *lpb_call1(PyObject *callable, PyObject *arg) {
    PyObject *args, *r;
    args = PyTuple_New(1);
    if (!args)
        return NULL;
    Py_INCREF(arg);
    PyTuple_SetItem(args, 0, arg);
    r = PyObject_CallObject(callable, args);
    Py_DECREF(args);
    return r;
}

PyObject *lpb_call_method0(PyObject *obj, const char *name) {
    PyObject *m = PyObject_GetAttrString(obj, name), *r;
    if (!m)
        return NULL;
    r = PyObject_CallNoArgs(m);
    Py_DECREF(m);
    return r;
}

PyObject *lpb_call_method1(PyObject *obj, const char *name, PyObject *arg) {
    PyObject *m = PyObject_GetAttrString(obj, name), *r;
    if (!m)
        return NULL;
    r = lpb_call1(m, arg);
    Py_DECREF(m);
    return r;
}

int lpb_is_dunder(PyObject *name) {
    Py_ssize_t n;
    const char *s;
    if (!PyUnicode_Check(name))
        return 0;
    s = PyUnicode_AsUTF8AndSize(name, &n);
    if (!s) {
        PyErr_Clear();
        return 0;
    }
    return n >= 5 && s[0] == '_' && s[1] == '_' && s[n - 1] == '_' && s[n - 2] == '_';
}

/* ------------------------------------------------------------------------------------------ */
/* Lua primitives that may run metamethods                                                    */
/* ------------------------------------------------------------------------------------------ */

int lpb_prim_gettable(lua_State *L) {
    lua_gettable(L, 1);
    return 1;
}

int lpb_prim_settable(lua_State *L) {
    lua_settable(L, 1);
    return 0;
}

int lpb_prim_len(lua_State *L) {
    lua_pushinteger(L, luaL_len(L, 1));
    return 1;
}

int lpb_prim_lt(lua_State *L) {
    lua_pushboolean(L, lua_compare(L, 1, 2, LUA_OPLT));
    return 1;
}

int lpb_prim_le(lua_State *L) {
    lua_pushboolean(L, lua_compare(L, 1, 2, LUA_OPLE));
    return 1;
}

int lpb_prim_tostring(lua_State *L) {
    luaL_tolstring(L, 1, NULL);
    return 1;
}

/* ------------------------------------------------------------------------------------------ */
/* Python -> Lua                                                                              */
/* ------------------------------------------------------------------------------------------ */

lpb_pyobj *lpb_test_pyobject(lua_State *L, int idx) {
    return (lpb_pyobj *)luaL_testudata(L, idx, LPB_PYOBJECT_MT);
}

void lpb_push_pyobject(lua_State *L, PyObject *o, int mode) {
    lpb_pyobj *u = (lpb_pyobj *)lpb_newuserdata(L, sizeof *u);
    u->o = NULL;
    u->mode = mode;
    luaL_setmetatable(L, LPB_PYOBJECT_MT);
    Py_INCREF(o);
    u->o = o;
}

int lpb_wrap_mode(PyObject *o) {
    int r;
    PyObject *tp;
    if (PyDict_Check(o))
        return LPB_MODE_MAP;
    if (PyList_Check(o) || PyTuple_Check(o))
        return LPB_MODE_SEQ;
    if (PyType_Check(o) || PyModule_Check(o))
        return LPB_MODE_ATTR;
    r = PyObject_IsInstance(o, lpb_abc_Mapping);
    if (r < 0)
        return -1;
    if (r)
        return LPB_MODE_MAP;
    r = PyObject_IsInstance(o, lpb_abc_Sequence);
    if (r < 0)
        return -1;
    if (r)
        return LPB_MODE_SEQ;
    /* numpy-style arrays: look at the type, not the instance, to avoid __getattr__ hooks. */
    tp = (PyObject *)Py_TYPE(o);
    if (PyObject_HasAttrString(tp, "__array_interface__") && PyObject_HasAttrString(tp, "__len__"))
        return LPB_MODE_SEQ;
    return LPB_MODE_ATTR;
}

static int lpb_push_long(lua_State *L, PyObject *o) {
    int overflow = 0;
    long long v = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (v == -1 && PyErr_Occurred())
        return -1;
    if (!overflow && v >= (long long)LUA_MININTEGER && v <= (long long)LUA_MAXINTEGER) {
        lua_pushinteger(L, (lua_Integer)v);
        return 0;
    }
    {
        /* Outside the lua_Integer range: a float, as Lua does with overflowing literals. */
        double d = PyLong_AsDouble(o);
        if (d == -1.0 && PyErr_Occurred())
            return -1;
        lua_pushnumber(L, (lua_Number)d);
    }
    return 0;
}

static int lpb_push_str(lua_State *L, PyObject *o) {
    Py_ssize_t n;
    char *bs;
    PyObject *b;
    const char *s = PyUnicode_AsUTF8AndSize(o, &n);
    if (s) {
        lua_pushlstring(L, s, (size_t)n);
        return 0;
    }
    if (!PyErr_ExceptionMatches(PyExc_UnicodeEncodeError))
        return -1;
    PyErr_Clear();
    /* Lone surrogates (e.g. from os.fsdecode): round-trip them as the original bytes. */
    b = PyUnicode_AsEncodedString(o, "utf-8", "surrogateescape");
    if (!b)
        return -1;
    if (PyBytes_AsStringAndSize(b, &bs, &n) < 0) {
        Py_DECREF(b);
        return -1;
    }
    lua_pushlstring(L, bs, (size_t)n);
    Py_DECREF(b);
    return 0;
}

int lpb_push_py(lpb_runtime *rt, lua_State *L, PyObject *o) {
    if (o == Py_None) {
        lua_pushnil(L);
    } else if (o == Py_True) {
        lua_pushboolean(L, 1);
    } else if (o == Py_False) {
        lua_pushboolean(L, 0);
    } else if (PyLong_Check(o)) {
        return lpb_push_long(L, o);
    } else if (PyFloat_Check(o)) {
        double d = PyFloat_AsDouble(o);
        if (d == -1.0 && PyErr_Occurred())
            return -1;
        lua_pushnumber(L, (lua_Number)d);
    } else if (PyUnicode_Check(o)) {
        return lpb_push_str(L, o);
    } else if (PyBytes_Check(o)) {
        char *s;
        Py_ssize_t n;
        if (PyBytes_AsStringAndSize(o, &s, &n) < 0)
            return -1;
        lua_pushlstring(L, s, (size_t)n);
    } else if (LuaObject_Check(o) || LuaSequence_Check(o)) {
        LuaObject *lo = LuaObject_Check(o) ? (LuaObject *)o : ((LuaSequence *)o)->table;
        if (lo->rt != rt) {
            PyErr_SetString(PyExc_TypeError, "this Lua object belongs to a different Lua state");
            return -1;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, lo->ref);
    } else {
        int mode, r = PyObject_IsInstance(o, lpb_numbers_Integral);
        if (r < 0)
            return -1;
        if (r) {
            /* Integers that are not int subclasses (numpy integers) convert by value too. */
            PyObject *i = PyNumber_Index(o);
            if (!i)
                return -1;
            r = lpb_push_long(L, i);
            Py_DECREF(i);
            return r;
        }
        mode = lpb_wrap_mode(o);
        if (mode < 0)
            return -1;
        lpb_push_pyobject(L, o, mode);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Lua -> Python                                                                              */
/* ------------------------------------------------------------------------------------------ */

PyObject *lpb_to_py(lpb_runtime *rt, lua_State *L, int idx) {
    switch (lua_type(L, idx)) {
    case LUA_TNONE:
    case LUA_TNIL:
        Py_RETURN_NONE;
    case LUA_TBOOLEAN:
        if (lua_toboolean(L, idx))
            Py_RETURN_TRUE;
        Py_RETURN_FALSE;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            return PyLong_FromLongLong((long long)lua_tointeger(L, idx));
        return PyFloat_FromDouble((double)lua_tonumber(L, idx));
    case LUA_TSTRING: {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        PyObject *r = PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, NULL);
        if (r || !PyErr_ExceptionMatches(PyExc_UnicodeDecodeError))
            return r;
        PyErr_Clear();
        return PyBytes_FromStringAndSize(s, (Py_ssize_t)n);
    }
    case LUA_TUSERDATA: {
        lpb_pyobj *u = lpb_test_pyobject(L, idx);
        if (u) {
            if (!u->o) {
                PyErr_SetString(PyExc_ValueError, "python.object has been released");
                return NULL;
            }
            Py_INCREF(u->o);
            return u->o;
        }
        break;
    }
    default:
        break;
    }
    return lpb_luaobject_new(rt, L, idx);
}

/* ------------------------------------------------------------------------------------------ */
/* Errors                                                                                     */
/* ------------------------------------------------------------------------------------------ */

void lpb_push_pyerr(lpb_runtime *rt, lua_State *L) {
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    if (!type) {
        lua_pushliteral(L, "python: unknown error");
        return;
    }
    PyErr_NormalizeException(&type, &value, &tb);
    if (value && tb)
        PyException_SetTraceback(value, tb);
    if (value && lpb_LuaError && PyErr_GivenExceptionMatches(value, lpb_LuaError)) {
        /* A Lua error on its way back to Lua: re-raise the original Lua value. */
        PyObject *d = PyObject_GetAttrString(value, "__dict__");
        PyObject *orig = d ? PyDict_GetItemString(d, "value") : NULL;   /* borrowed */
        if (orig && lpb_push_py(rt, L, orig) == 0) {
            Py_XDECREF(d);
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(tb);
            return;
        }
        Py_XDECREF(d);
        PyErr_Clear();
    }
    if (value)
        lpb_push_pyobject(L, value, LPB_MODE_ATTR);
    else
        lpb_push_pyobject(L, type, LPB_MODE_ATTR);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
}

int lpb_error(lpb_runtime *rt, lua_State *L) {
    lpb_push_pyerr(rt, L);
    return lua_error(L);
}

static void lpb_add_lua_note(PyObject *exc, PyObject *tb) {
    PyObject *note, *r;
    if (!tb || !PyObject_HasAttrString(exc, "add_note"))
        return;
    note = PyUnicode_FromString("Lua ");
    if (note) {
        PyObject *full = PyUnicode_Concat(note, tb);
        Py_DECREF(note);
        if (full) {
            r = lpb_call_method1(exc, "add_note", full);
            Py_XDECREF(r);
            Py_DECREF(full);
        }
    }
    PyErr_Clear();
}

static LPB_TLS int lpb_message_depth;

/* A Python str describing the Lua error value at idx. */
static PyObject *lpb_lua_message(lpb_runtime *rt, lua_State *L, int idx) {
    char buf[96];
    size_t n;
    const char *s;
    int t = lua_type(L, idx);
    if (t == LUA_TSTRING || t == LUA_TNUMBER) {
        PyObject *r;
        lua_pushvalue(L, idx);
        s = lua_tolstring(L, -1, &n);
        r = PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "replace");
        lua_pop(L, 1);
        return r;
    }
    if (lpb_message_depth < 2) {
        int nres;
        lpb_message_depth++;
        lua_pushcfunction(L, lpb_prim_tostring);
        lua_pushvalue(L, idx);
        nres = lpb_lcall(rt, L, 1, 1);
        lpb_message_depth--;
        if (nres == 1) {
            PyObject *r = NULL;
            s = lua_tolstring(L, -1, &n);
            if (s)
                r = PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "replace");
            lua_pop(L, 1);
            if (r)
                return r;
        }
        PyErr_Clear();
    }
    snprintf(buf, sizeof buf, "(error object is a %s value)", luaL_typename(L, idx));
    return PyUnicode_FromString(buf);
}

void lpb_set_pyerr_from_lua(lpb_runtime *rt, lua_State *L, int idx, int status) {
    PyObject *tb = NULL, *msg = NULL, *val = NULL, *exc = NULL;
    const char *s;
    size_t n;
    lpb_pyobj *u;

    idx = lua_absindex(L, idx);
    if (!lua_checkstack(L, 4)) {
        PyErr_NoMemory();
        return;
    }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &lpb_traceback_key);
    s = lua_tolstring(L, -1, &n);
    if (s) {
        tb = PyUnicode_DecodeUTF8(s, (Py_ssize_t)n, "replace");
        if (!tb)
            PyErr_Clear();
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &lpb_traceback_key);

    if (status == LUA_ERRMEM) {
        PyErr_NoMemory();
        goto done;
    }

    u = lpb_test_pyobject(L, idx);
    if (u && u->o && PyExceptionInstance_Check(u->o)) {
        /* A Python exception that travelled through Lua: re-raise it as it was. */
        PyObject *e = u->o;
        lpb_add_lua_note(e, tb);
        Py_INCREF(Py_TYPE(e));
        Py_INCREF(e);
        PyErr_Restore((PyObject *)Py_TYPE(e), e, PyException_GetTraceback(e));
        goto done;
    }

    msg = lpb_lua_message(rt, L, idx);
    if (!msg)
        goto done;
    val = lpb_to_py(rt, L, idx);
    if (!val) {
        PyErr_Clear();
        Py_INCREF(Py_None);
        val = Py_None;
    }
    exc = lpb_call1(lpb_LuaError, msg);
    if (!exc)
        goto done;
    if (PyObject_SetAttrString(exc, "value", val) < 0 ||
        PyObject_SetAttrString(exc, "traceback", tb ? tb : Py_None) < 0)
        goto done;
    lpb_add_lua_note(exc, tb);
    PyErr_SetObject(lpb_LuaError, exc);
done:
    Py_XDECREF(tb);
    Py_XDECREF(msg);
    Py_XDECREF(val);
    Py_XDECREF(exc);
}
