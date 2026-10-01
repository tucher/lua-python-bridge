#!/usr/bin/env python3
"""Generate src/pyapi_list.h and src/pyapi.h from the vendored CPython 3.10 headers.

The Lua module loads libpython at runtime. Every CPython function it uses gets a trampoline with
the real name (defined in pyloader.c from pyapi_list.h), and every data symbol gets a macro that
redirects it to a pointer filled at load time (pyapi.h).

When the bridge starts using a new CPython API, add its name below and rerun:

    python3 tools/gen_pyapi.py

The build checks that the Lua module references no CPython symbol directly
(tests/check_symbols.py), so a missing name shows up as a test failure.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS = os.path.join(ROOT, "third_party", "python3.10-headers")

FUNCTIONS = """
PyBytes_AsStringAndSize PyBytes_FromStringAndSize PyCMethod_New PyCallable_Check
PyDict_GetItemString PyDict_New PyDict_Next PyDict_SetItem PyDict_SetItemString PyDict_Size
PyErr_Clear PyErr_ExceptionMatches PyErr_Fetch PyErr_GivenExceptionMatches PyErr_NewException
PyErr_NoMemory PyErr_NormalizeException PyErr_Occurred PyErr_Restore PyErr_SetObject PyErr_SetString
PyEval_EvalCode PyEval_GetBuiltins PyEval_GetGlobals PyEval_GetLocals PyEval_RestoreThread
PyEval_SaveThread PyException_GetTraceback PyException_SetTraceback PyFloat_AsDouble
PyFloat_FromDouble PyGILState_Ensure PyGILState_Release PyImport_AddModule PyImport_AppendInittab
PyImport_ImportModule PyIndex_Check PyIter_Next PyList_Append PyList_GetItem PyList_New
PyList_SetItem PyList_Size PyLong_AsDouble PyLong_AsLongLongAndOverflow PyLong_FromLongLong
PyLong_FromSsize_t PyMapping_Items PyModule_AddObjectRef PyModule_AddStringConstant
PyModule_Create2 PyModule_GetDict PyNumber_Add PyNumber_And PyNumber_AsSsize_t PyNumber_FloorDivide
PyNumber_Index PyNumber_Invert PyNumber_Lshift PyNumber_Multiply PyNumber_Negative PyNumber_Or PyNumber_Power
PyNumber_Remainder PyNumber_Rshift PyNumber_Subtract PyNumber_TrueDivide PyNumber_Xor PyObject_Call
PyObject_CallNoArgs PyObject_CallObject PyObject_DelItem PyObject_GenericGetAttr PyObject_GetAttr
PyObject_GetAttrString PyObject_GetItem PyObject_GetIter PyObject_HasAttrString PyObject_IsInstance
PyObject_Repr PyObject_RichCompareBool PyObject_SetAttr PyObject_SetAttrString PyObject_SetItem
PyObject_Size PyObject_Str PySeqIter_New PySequence_List PySlice_AdjustIndices PySlice_Unpack
PySys_GetObject PySys_SetObject PyTuple_GetItem PyTuple_New PyTuple_SetItem PyTuple_Size
PyType_FromSpec PyType_GetFlags PyType_GetSlot PyType_IsSubtype PyUnicode_AsEncodedString
PyUnicode_AsUTF8AndSize PyUnicode_Concat PyUnicode_DecodeUTF8 PyUnicode_FromString
PyUnicode_GetLength PyUnicode_Join PyUnicode_ReadChar PyUnicode_Substring Py_CompileString
Py_FinalizeEx Py_GetVersion Py_InitializeEx Py_IsInitialized _PyObject_New _Py_Dealloc
""".split()

DATA = """
PyExc_AttributeError PyExc_IndexError PyExc_KeyError PyExc_LookupError PyExc_MemoryError
PyExc_RuntimeError PyExc_TypeError PyExc_UnicodeDecodeError PyExc_UnicodeEncodeError
PyExc_ValueError PyFloat_Type PyModule_Type PySlice_Type _Py_FalseStruct _Py_NoneStruct
_Py_NotImplementedStruct _Py_TrueStruct
""".split()

C_TYPE_WORDS = {"int", "long", "short", "char", "double", "float", "void", "unsigned", "signed",
                "const", "volatile", "struct", "union", "enum"}


def preprocess():
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-E", "-P", "-DPy_LIMITED_API=0x030A0000", "-DPY_SSIZE_T_CLEAN",
           "-I", os.path.join(HEADERS, "pyconfig", "macos"), "-I", os.path.join(HEADERS, "include"),
           os.path.join(HEADERS, "include", "Python.h")]
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def strip_attributes(s):
    out, i = [], 0
    while True:
        j = s.find("__attribute__", i)
        if j < 0:
            out.append(s[i:])
            return "".join(out)
        out.append(s[i:j])
        k = s.index("(", j)
        depth = 0
        while True:
            if s[k] == "(":
                depth += 1
            elif s[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        i = k + 1


def split_params(params):
    parts, depth, cur = [], 0, ""
    for ch in params:
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
            continue
        depth += ch == "("
        depth -= ch == ")"
        cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def param_type(p, n):
    """Return the parameter declaration renamed to a<n>."""
    name = "a%d" % n
    if "(" in p:                                     # function pointer: T (*name)(args)
        return re.sub(r"\(\s*\*\s*\w*\s*\)", "(*%s)" % name, p, count=1)
    tokens = re.findall(r"\w+|\*|\[|\]", p)
    last = tokens[-1]
    words = [t for t in tokens if re.match(r"\w", t)]
    named = (re.match(r"\w", last) and last not in C_TYPE_WORDS and
             (tokens[-2:-1] == ["*"] or len(words) >= (3 if words[0] in ("struct", "union", "enum") else 2)))
    if named:
        p = p[: p.rindex(last)].rstrip()
    sep = "" if p.endswith("*") else " "
    return p + sep + name


def main():
    src = re.sub(r"\s+", " ", strip_attributes(preprocess()))
    statements = src.split(";")
    func_lines, proc_lines = [], []
    for name in FUNCTIONS:
        decl = None
        for st in statements:
            st = st.strip()
            m = re.match(r"(?:extern\s+)?(.*?)\(?\b%s\b\)?\s*\((.*)\)$" % re.escape(name), st)
            if m and "{" not in st and "typedef" not in st:
                decl = m
                break
        if not decl:
            sys.exit("declaration of %s not found in the 3.10 limited API headers" % name)
        ret = decl.group(1).strip()
        params = decl.group(2).strip()
        plist = [] if params in ("", "void") else split_params(params)
        typed = ", ".join(param_type(p, i) for i, p in enumerate(plist)) or "void"
        call = ", ".join("a%d" % i for i in range(len(plist)))
        if ret == "void":
            proc_lines.append("LPB_PYPROC(%s, (%s), (%s))" % (name, typed, call))
        else:
            func_lines.append("LPB_PYFUNC(%s, %s, (%s), (%s))" % (ret, name, typed, call))
    data_lines = []
    for name in DATA:
        m = re.search(r"extern\s+([^;]*?)\b%s\s*$" % re.escape(name),
                      next(s for s in statements if re.search(r"\b%s\s*$" % re.escape(name), s.strip())))
        data_lines.append("LPB_PYDATA(%s, %s)" % (m.group(1).strip(), name))

    header = "/* Generated by tools/gen_pyapi.py from the CPython 3.10 limited API headers. Do not edit. */\n"
    with open(os.path.join(ROOT, "src", "pyapi_list.h"), "w") as f:
        f.write(header)
        f.write("/* LPB_PYFUNC(return type, name, (parameters), (arguments)) */\n")
        f.write("\n".join(func_lines) + "\n\n")
        f.write("/* LPB_PYPROC(name, (parameters), (arguments)): functions returning void */\n")
        f.write("\n".join(proc_lines) + "\n\n")
        f.write("/* LPB_PYDATA(type, name): data symbols, accessed through pointers (pyapi.h) */\n")
        f.write("\n".join(data_lines) + "\n")
    with open(os.path.join(ROOT, "src", "pyapi.h"), "w") as f:
        f.write(header)
        f.write("/* Included after <Python.h> in the Lua module build: CPython data symbols resolve\n"
                "   through pointers that pyloader.c fills when it loads libpython. */\n")
        f.write("#ifndef LPB_PYAPI_H\n#define LPB_PYAPI_H\n\n")
        for line in data_lines:
            m = re.match(r"LPB_PYDATA\((.*), (\w+)\)$", line)
            typ, name = m.group(1), m.group(2)
            f.write("extern %s *lpb_data_%s;\n#define %s (*lpb_data_%s)\n" % (typ, name, name, name))
        f.write("\n#endif\n")
    print("wrote %d functions, %d procedures, %d data symbols" % (len(func_lines), len(proc_lines), len(data_lines)))


if __name__ == "__main__":
    main()
