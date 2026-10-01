#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "dylib.hpp"
#include <memory>
#include <cstdlib>
// #include <iostream>
static std::shared_ptr<dylib> python_dll;
static const std::shared_ptr<dylib> & P();

#define DECL_F(name) \
    /*std::cout << #name " called" << std::endl;*/ \
    static_assert(std::is_same<decltype(Py_Lua_Bridge_##name), decltype(name)>::value, "CHECK DECLARATION "#name); \
    static auto c = P()->get_function<decltype(name)>(#name);
extern "C" {

PyObject * Py_Lua_Bridge_PyExc_Exception;
PyObject * Py_Lua_Bridge_PyExc_RuntimeError;
PyObject * Py_Lua_Bridge_PyExc_TypeError;
PyObject * Py_Lua_Bridge_PyExc_ValueError;

PyTypeObject * Py_Lua_Bridge_PyFloat_TypePTR;

struct _longobject * Py_Lua_Bridge__Py_FalseStructPTR; 
struct _longobject * Py_Lua_Bridge__Py_TrueStructPTR; 
PyObject * Py_Lua_Bridge__Py_NoneStructPTR;


int Py_Lua_Bridge__PyArg_VaParse_SizeT(PyObject *args, const char *format, va_list vargs) {
    DECL_F(_PyArg_VaParse_SizeT)
    return c(args, format, vargs);
}

int Py_Lua_Bridge_PyArg_ParseTuple(PyObject *args, const char *format, ...) {
    va_list myargs;
    va_start(myargs, format);
    int r = Py_Lua_Bridge__PyArg_VaParse_SizeT(args, format, myargs);
    va_end(myargs);
    return r;
}
int Py_Lua_Bridge_PyBytes_AsStringAndSize(PyObject *obj, char **buffer, Py_ssize_t *length) {
    DECL_F(PyBytes_AsStringAndSize)
    return c(obj, buffer, length);
}
int Py_Lua_Bridge_PyCallable_Check(PyObject *o) {
    DECL_F(PyCallable_Check)
    return c(o);
}
PyObject *Py_Lua_Bridge_PyDict_New() {
    DECL_F(PyDict_New)
    return c();
}
int Py_Lua_Bridge_PyDict_SetItemString(PyObject *p, const char *key, PyObject *val) {
    DECL_F(PyDict_SetItemString)
    return c(p, key, val);
}
void Py_Lua_Bridge_PyErr_Clear() {
    DECL_F(PyErr_Clear)
    c();
}

PyObject *Py_Lua_Bridge_PyErr_FormatV(PyObject *exception, const char *format, va_list vargs) {
    DECL_F(PyErr_FormatV)
    return c(exception, format, vargs);
}

PyObject *Py_Lua_Bridge_PyErr_Format(PyObject *exception, const char *format, ...)  {
    va_list myargs;
    va_start(myargs, format);
    PyObject * r = Py_Lua_Bridge_PyErr_FormatV(exception, format, myargs);
    va_end(myargs);
    return r;
}
void Py_Lua_Bridge_PyErr_Print() {
    DECL_F(PyErr_Print)
    c();
}
void Py_Lua_Bridge_PyErr_SetString(PyObject *type, const char *message) {
    DECL_F(PyErr_SetString)
    c(type, message);
}
PyObject *Py_Lua_Bridge_PyEval_GetBuiltins(void) {
    DECL_F(PyEval_GetBuiltins)
    return c();
}
PyObject *Py_Lua_Bridge_PyEval_GetGlobals(void) {
    DECL_F(PyEval_GetGlobals)
    return c();
}
PyObject *Py_Lua_Bridge_PyEval_GetLocals(void) {
    DECL_F(PyEval_GetLocals)
    return c();
}



double Py_Lua_Bridge_PyFloat_AsDouble(PyObject *pyfloat) {
    DECL_F(PyFloat_AsDouble)
    return c(pyfloat);
}
PyObject *Py_Lua_Bridge_PyFloat_FromDouble(double v) {
    DECL_F(PyFloat_FromDouble)
    return c(v);
}

PyObject *Py_Lua_Bridge_PyImport_AddModule(const char *name) {
    DECL_F(PyImport_AddModule)
    return c(name);
}
int Py_Lua_Bridge_PyImport_AppendInittab(const char *name, PyObject *(*initfunc)(void)) {
    DECL_F(PyImport_AppendInittab)
    return c(name, initfunc);
}
PyObject *Py_Lua_Bridge_PyImport_ImportModule(const char *name) {
    DECL_F(PyImport_ImportModule)
    return c(name);
}


long Py_Lua_Bridge_PyLong_AsLong(PyObject *obj) {
    DECL_F(PyLong_AsLong)
    return c(obj);
}
PyObject *Py_Lua_Bridge_PyLong_FromLong(long v) {
    DECL_F(PyLong_FromLong)
    return c(v);
}


PyObject *Py_Lua_Bridge_PyModule_Create2(PyModuleDef *def, int module_api_version) {
    DECL_F(PyModule_Create2)
    return c(def, module_api_version);
}

PyObject *Py_Lua_Bridge_PyModule_GetDict(PyObject *module) {
    DECL_F(PyModule_GetDict)
    return c(module);
}
PyObject *Py_Lua_Bridge_PyObject_Call(PyObject *callable, PyObject *args, PyObject *kwargs) {
    DECL_F(PyObject_Call)
    return c(callable, args, kwargs);
}
int Py_Lua_Bridge_PyObject_DelItem(PyObject *o, PyObject *key) {
    DECL_F(PyObject_DelItem)
    return c(o, key);
}
void Py_Lua_Bridge_PyObject_Free(void *p) {
    DECL_F(PyObject_Free)
    c(p);
}
PyObject *Py_Lua_Bridge_PyObject_GetAttr(PyObject *o, PyObject *attr_name) {
    DECL_F(PyObject_GetAttr)
    return c(o, attr_name);
}
PyObject *Py_Lua_Bridge_PyObject_GetAttrString(PyObject *o, const char *attr_name) {
    DECL_F(PyObject_GetAttrString)
    return c(o, attr_name);
}
PyObject *Py_Lua_Bridge_PyObject_GetItem(PyObject *o, PyObject *key) {
    DECL_F(PyObject_GetItem)
    return c(o, key);
}

PyObject * Py_Lua_Bridge__PyObject_New(PyTypeObject * o) {
    DECL_F(_PyObject_New)
    return c(o);
}

PyObject *Py_Lua_Bridge_PyObject_SelfIter(PyObject * o) {
    DECL_F(PyObject_SelfIter)
    return c(o);
}
int Py_Lua_Bridge_PyObject_SetAttrString(PyObject *o, const char *attr_name, PyObject *v) {
    DECL_F(PyObject_SetAttrString)
    return c(o, attr_name, v);
}
int Py_Lua_Bridge_PyObject_SetItem(PyObject *o, PyObject *key, PyObject *v) {
    DECL_F(PyObject_SetItem)
    return c(o, key, v);
}
PyObject *Py_Lua_Bridge_PyObject_Str(PyObject *o) {
    DECL_F(PyObject_Str)
    return c(o);
}
PyObject *Py_Lua_Bridge_PyRun_StringFlags(const char *str, int start, PyObject *globals, PyObject *locals, PyCompilerFlags *flags) {
    DECL_F(PyRun_StringFlags)
    return c(str, start, globals, locals, flags);
}
void Py_Lua_Bridge_PySys_SetArgv(int argc, wchar_t **argv) {
    DECL_F(PySys_SetArgv)
    c(argc, argv);
}
PyObject *Py_Lua_Bridge_PyTuple_GetItem(PyObject *p, Py_ssize_t pos) {
    DECL_F(PyTuple_GetItem)
    return c(p, pos);
}
PyObject *Py_Lua_Bridge_PyTuple_New(Py_ssize_t len) {
    DECL_F(PyTuple_New)
    return c(len);
}
int Py_Lua_Bridge_PyTuple_SetItem(PyObject *p, Py_ssize_t pos, PyObject *o) {
    DECL_F(PyTuple_SetItem)
    return c(p, pos, o);
}
Py_ssize_t Py_Lua_Bridge_PyTuple_Size(PyObject *p) {
    DECL_F(PyTuple_Size)
    return c(p);
}
PyObject *Py_Lua_Bridge_PyType_GenericAlloc(PyTypeObject *type, Py_ssize_t nitems) {
    DECL_F(PyType_GenericAlloc)
    return c(type, nitems);
}
PyObject *Py_Lua_Bridge_PyType_GenericNew(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    DECL_F(PyType_GenericNew)
    return c(type, args, kwds);
}
int Py_Lua_Bridge_PyType_IsSubtype(PyTypeObject *a, PyTypeObject *b) {
    DECL_F(PyType_IsSubtype)
    return c(a, b);
}
int Py_Lua_Bridge_PyType_Ready(PyTypeObject *type) {
    DECL_F(PyType_Ready)
    return c(type);
}

PyObject *Py_Lua_Bridge_PyUnicode_FromFormatV(const char *format, va_list vargs) {
    DECL_F(PyUnicode_FromFormatV)
    return c(format, vargs);
}


PyObject *Py_Lua_Bridge_PyUnicode_AsEncodedString(PyObject *unicode, const char *encoding, const char *errors) {
    DECL_F(PyUnicode_AsEncodedString)
    return c(unicode, encoding, errors);
}
PyObject *Py_Lua_Bridge_PyUnicode_FromFormat(const char *format, ...) {
    va_list myargs;
    va_start(myargs, format);
    PyObject * r = Py_Lua_Bridge_PyUnicode_FromFormatV(format, myargs);
    va_end(myargs);
    return r;
}
PyObject *Py_Lua_Bridge_PyUnicode_FromString(const char *u) {
    DECL_F(PyUnicode_FromString)
    return c(u);
}
PyObject *Py_Lua_Bridge_PyUnicode_FromStringAndSize(const char *u, Py_ssize_t size) {
    DECL_F(PyUnicode_FromStringAndSize)
    return c(u, size);
}

PyObject * Py_Lua_Bridge_PyBytes_FromStringAndSize(const char * u, Py_ssize_t size) {
    DECL_F(PyBytes_FromStringAndSize)
    return c(u, size);
}

void Py_Lua_Bridge_Py_Initialize() {
    DECL_F(Py_Initialize)
    c();
}
int Py_Lua_Bridge_Py_IsInitialized() {
    DECL_F(Py_IsInitialized)
    return c();
}
void Py_Lua_Bridge_Py_SetProgramName(const wchar_t *name) {
    DECL_F(Py_SetProgramName)
    c(name);
}
// TYPE *PyObject_New(TYPE, PyTypeObject *type) {
//     return 0;
// }
void Py_Lua_Bridge__Py_Dealloc(PyObject * o) {
    DECL_F(_Py_Dealloc)
    c(o);
}

void Py_Lua_Bridge_PyErr_Fetch(PyObject **ptype, PyObject **pvalue, PyObject **ptraceback) {
    DECL_F(PyErr_Fetch)
    c(ptype, pvalue, ptraceback);
}
}
#if (defined(_WIN32) || defined(_WIN64))
#include <psapi.h>
#include <vector>
#include <iostream>
#include <cctype>
std::string findPythonLibrary() {
    std::vector<HMODULE> hModules;
    const HMODULE hSelf = GetModuleHandleA(nullptr);
    {
        DWORD nModules;
        EnumProcessModules(GetCurrentProcess(), nullptr, 0, &nModules);
        hModules.resize(nModules);
        EnumProcessModules(GetCurrentProcess(), &hModules[0], nModules, &nModules);
    }

    // Find a module of interest
    for (auto hModule : hModules) {
        if (hModule == hSelf)
            continue;
        MODULEINFO modinfo;
        // GetModuleInformation(GetCurrentProcess(), hModule, &modinfo, sizeof(modinfo));
        char buf[1024];
        int r = GetModuleBaseName(GetCurrentProcess(), hModule, buf, 1024);
        if(r > 0) {
            buf[r] = 0;
            auto orig = std::string(buf, r);
            auto ret = orig;
            for(auto &c: ret) {
                c = tolower(c);
            }
            if(ret.size() >= 10 && ret.substr(0, 6) == "python" && ret.substr(ret.size() - 4, 4) == ".dll") {
                return orig;
            }
        }
        
        
        
    }
    return "";
}
#endif

const std::shared_ptr<dylib> & P() {
    static std::shared_ptr<dylib> python_dll;
    if(!python_dll) {
#ifndef BUILD_AS_PYTHON_EXTENSION
        auto path = std::getenv("LUA_PYTHON_BRIDGE_PYTHON_LIBRARY_PATH");
        auto name = std::getenv("LUA_PYTHON_BRIDGE_PYTHON_LIBRARY_NAME");
        if(!name || !path) {
            printf("Exact path and name of libpython must be passed via\nLUA_PYTHON_BRIDGE_PYTHON_LIBRARY_PATH\nLUA_PYTHON_BRIDGE_PYTHON_LIBRARY_NAME\nenv. variables\n");
            throw;
        }
        python_dll.reset(new dylib(path, name));
#else
        const char * name = NULL;
#if (defined(_WIN32) || defined(_WIN64))
        auto detected = findPythonLibrary();
        if(detected != "") {
            name = detected.c_str();
        } else
            name = std::getenv("LUA_PYTHON_BRIDGE_PYTHON_LIBRARY_NAME");
        
        if(!name) {
            printf("Exact name of libpython must be passed via\nLUA_PYTHON_BRIDGE_PYTHON_LIBRARY_NAME\nenv. variable\n");
            throw;
        }
#endif
        python_dll.reset(new dylib(nullptr, name));
#endif
        Py_Lua_Bridge_PyExc_Exception = python_dll->get_variable<PyObject *>("PyExc_Exception");
        Py_Lua_Bridge_PyExc_RuntimeError = python_dll->get_variable<PyObject *>("PyExc_RuntimeError");
        Py_Lua_Bridge_PyExc_TypeError = python_dll->get_variable<PyObject *>("PyExc_TypeError");
        Py_Lua_Bridge_PyExc_ValueError = python_dll->get_variable<PyObject *>("PyExc_ValueError");

        Py_Lua_Bridge_PyFloat_TypePTR =  reinterpret_cast<PyTypeObject *>(python_dll->get_symbol("PyFloat_Type"));
        Py_Lua_Bridge__Py_FalseStructPTR = reinterpret_cast<struct _longobject *>(python_dll->get_symbol("_Py_FalseStruct"));
        Py_Lua_Bridge__Py_TrueStructPTR = reinterpret_cast<struct _longobject *>(python_dll->get_symbol("_Py_TrueStruct"));
        Py_Lua_Bridge__Py_NoneStructPTR = reinterpret_cast<PyObject *>(python_dll->get_symbol("_Py_NoneStruct"));
    }
    return python_dll;
}