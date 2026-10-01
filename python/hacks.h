extern PyObject * Py_Lua_Bridge__Py_NoneStructPTR;
#undef Py_None
#define Py_None (Py_Lua_Bridge__Py_NoneStructPTR)

#undef Py_False
extern struct _longobject * Py_Lua_Bridge__Py_FalseStructPTR;
#define Py_False ((PyObject *) Py_Lua_Bridge__Py_FalseStructPTR)
#undef Py_True
extern struct _longobject * Py_Lua_Bridge__Py_TrueStructPTR;
#define Py_True ((PyObject *) Py_Lua_Bridge__Py_TrueStructPTR)

#undef PyFloat_Check
extern PyTypeObject * Py_Lua_Bridge_PyFloat_TypePTR;
#define PyFloat_Check(op) PyObject_TypeCheck(op, Py_Lua_Bridge_PyFloat_TypePTR)


extern PyObject * Py_Lua_Bridge_PyExc_Exception;
extern PyObject * Py_Lua_Bridge_PyExc_RuntimeError;
extern PyObject * Py_Lua_Bridge_PyExc_TypeError;
extern PyObject * Py_Lua_Bridge_PyExc_ValueError;

#define Py_Lua_Bridge_PyModule_Create(module) \
        Py_Lua_Bridge_PyModule_Create2(module, PYTHON_API_VERSION)

#define Py_Lua_Bridge_PyRun_String(str, s, g, l) Py_Lua_Bridge_PyRun_StringFlags(str, s, g, l, NULL)
#define Py_Lua_Bridge_PyObject_New(type, typeobj) ((type *)Py_Lua_Bridge__PyObject_New(typeobj))




int Py_Lua_Bridge__PyArg_VaParse_SizeT(PyObject *args, const char *format, va_list vargs);
int Py_Lua_Bridge_PyArg_ParseTuple(PyObject *args, const char *format, ...);
int Py_Lua_Bridge_PyBytes_AsStringAndSize(PyObject *obj, char **buffer, Py_ssize_t *length);
int Py_Lua_Bridge_PyCallable_Check(PyObject *o);
PyObject *Py_Lua_Bridge_PyDict_New();
int Py_Lua_Bridge_PyDict_SetItemString(PyObject *p, const char *key, PyObject *val);
void Py_Lua_Bridge_PyErr_Clear();
PyObject *Py_Lua_Bridge_PyErr_FormatV(PyObject *exception, const char *format, va_list vargs);
PyObject *Py_Lua_Bridge_PyErr_Format(PyObject *exception, const char *format, ...);
void Py_Lua_Bridge_PyErr_Print();
void Py_Lua_Bridge_PyErr_SetString(PyObject *type, const char *message);
PyObject *Py_Lua_Bridge_PyEval_GetBuiltins(void);
PyObject *Py_Lua_Bridge_PyEval_GetGlobals(void);
PyObject *Py_Lua_Bridge_PyEval_GetLocals(void);
double Py_Lua_Bridge_PyFloat_AsDouble(PyObject *pyfloat);
PyObject *Py_Lua_Bridge_PyFloat_FromDouble(double v);
PyObject *Py_Lua_Bridge_PyImport_AddModule(const char *name);
int Py_Lua_Bridge_PyImport_AppendInittab(const char *name, PyObject *(*initfunc)(void)) ;
PyObject *Py_Lua_Bridge_PyImport_ImportModule(const char *name);
long Py_Lua_Bridge_PyLong_AsLong(PyObject *obj);
PyObject *Py_Lua_Bridge_PyLong_FromLong(long v);
PyObject *Py_Lua_Bridge_PyModule_Create2(PyModuleDef *def, int module_api_version);
PyObject *Py_Lua_Bridge_PyModule_GetDict(PyObject *module);
PyObject *Py_Lua_Bridge_PyObject_Call(PyObject *callable, PyObject *args, PyObject *kwargs);
int Py_Lua_Bridge_PyObject_DelItem(PyObject *o, PyObject *key);
void Py_Lua_Bridge_PyObject_Free(void *p);
PyObject *Py_Lua_Bridge_PyObject_GetAttr(PyObject *o, PyObject *attr_name);
PyObject *Py_Lua_Bridge_PyObject_GetAttrString(PyObject *o, const char *attr_name);
PyObject *Py_Lua_Bridge_PyObject_GetItem(PyObject *o, PyObject *key);
PyObject * Py_Lua_Bridge__PyObject_New(PyTypeObject * o);
PyObject *Py_Lua_Bridge_PyObject_SelfIter(PyObject * o);
int Py_Lua_Bridge_PyObject_SetAttrString(PyObject *o, const char *attr_name, PyObject *v);
int Py_Lua_Bridge_PyObject_SetItem(PyObject *o, PyObject *key, PyObject *v);
PyObject *Py_Lua_Bridge_PyObject_Str(PyObject *o);
PyObject *Py_Lua_Bridge_PyRun_StringFlags(const char *str, int start, PyObject *globals, PyObject *locals, PyCompilerFlags *flags);
void Py_Lua_Bridge_PySys_SetArgv(int argc, wchar_t **argv);
PyObject *Py_Lua_Bridge_PyTuple_GetItem(PyObject *p, Py_ssize_t pos);
PyObject *Py_Lua_Bridge_PyTuple_New(Py_ssize_t len);
int Py_Lua_Bridge_PyTuple_SetItem(PyObject *p, Py_ssize_t pos, PyObject *o);
Py_ssize_t Py_Lua_Bridge_PyTuple_Size(PyObject *p);
PyObject *Py_Lua_Bridge_PyType_GenericAlloc(PyTypeObject *type, Py_ssize_t nitems);
PyObject *Py_Lua_Bridge_PyType_GenericNew(PyTypeObject *type, PyObject *args, PyObject *kwds);
int Py_Lua_Bridge_PyType_IsSubtype(PyTypeObject *a, PyTypeObject *b);
int Py_Lua_Bridge_PyType_Ready(PyTypeObject *type);
PyObject *Py_Lua_Bridge_PyUnicode_FromFormatV(const char *format, va_list vargs);
PyObject *Py_Lua_Bridge_PyUnicode_AsEncodedString(PyObject *unicode, const char *encoding, const char *errors);
PyObject *Py_Lua_Bridge_PyUnicode_FromFormat(const char *format, ...);
PyObject *Py_Lua_Bridge_PyUnicode_FromString(const char *u);
PyObject *Py_Lua_Bridge_PyUnicode_FromStringAndSize(const char *u, Py_ssize_t size);
PyObject * Py_Lua_Bridge_PyBytes_FromStringAndSize(const char * u, Py_ssize_t size);
void Py_Lua_Bridge_Py_Initialize();
int Py_Lua_Bridge_Py_IsInitialized();
void Py_Lua_Bridge_Py_SetProgramName(const wchar_t *name);

void Py_Lua_Bridge__Py_Dealloc(PyObject * o);
void Py_Lua_Bridge_PyErr_Fetch(PyObject **ptype, PyObject **pvalue, PyObject **ptraceback);
static inline void Py_Lua_Bridge__Py_DECREF(
#if defined(Py_REF_DEBUG) && !(defined(Py_LIMITED_API) && Py_LIMITED_API+0 >= 0x030A0000)
    const char *filename, int lineno,
#endif
    PyObject *op)
{
#if defined(Py_REF_DEBUG) && defined(Py_LIMITED_API) && Py_LIMITED_API+0 >= 0x030A0000
    // Stable ABI for Python 3.10 built in debug mode.
    _Py_DecRef(op);
#else
    // Non-limited C API and limited C API for Python 3.9 and older access
    // directly PyObject.ob_refcnt.
#ifdef Py_REF_DEBUG
    _Py_RefTotal--;
#endif
    if (--op->ob_refcnt != 0) {
#ifdef Py_REF_DEBUG
        if (op->ob_refcnt < 0) {
            _Py_NegativeRefcount(filename, lineno, op);
        }
#endif
    }
    else {
        Py_Lua_Bridge__Py_Dealloc(op);
    }
#endif
}

#define Py_Lua_Bridge_Py_DECREF(op) Py_Lua_Bridge__Py_DECREF(_PyObject_CAST(op))

static inline int Py_Lua_Bridge__PyObject_TypeCheck(PyObject *ob, PyTypeObject *type) {
    return Py_IS_TYPE(ob, type) || Py_Lua_Bridge_PyType_IsSubtype(Py_TYPE(ob), type);
}
#define Py_Lua_Bridge_PyObject_TypeCheck(ob, type) Py_Lua_Bridge__PyObject_TypeCheck(_PyObject_CAST(ob), type)


#define Py_Lua_Bridge_PyFloat_Check(op) Py_Lua_Bridge_PyObject_TypeCheck(op, Py_Lua_Bridge_PyFloat_TypePTR)
