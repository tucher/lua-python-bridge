/*
 * lua-python-bridge: finds and loads libpython at runtime (Lua module build only).
 *
 * Copyright (c) 2026 Aleks Tuchkov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The module is compiled against the CPython 3.10 limited API. Each CPython function it uses is
 * defined here under its real name as a trampoline into the loaded libpython, so the rest of the
 * bridge is ordinary CPython code.
 */
#include "bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <crt_externs.h>
#    define LPB_ENVIRON (*_NSGetEnviron())
#  else
extern char **environ;
#    define LPB_ENVIRON environ
#  endif
#endif

/* ------------------------------------------------------------------------------------------ */
/* Function table, trampolines and data pointers                                              */
/* ------------------------------------------------------------------------------------------ */

#define LPB_PYFUNC(ret, name, params, args) ret (*name) params;
#define LPB_PYPROC(name, params, args) void (*name) params;
#define LPB_PYDATA(type, name)
static struct {
#include "pyapi_list.h"
    int unused;
} lpb_py;
#undef LPB_PYFUNC
#undef LPB_PYPROC
#undef LPB_PYDATA

#define LPB_PYFUNC(ret, name, params, args) ret name params { return lpb_py.name args; }
#define LPB_PYPROC(name, params, args) void name params { lpb_py.name args; }
#define LPB_PYDATA(type, name) type *lpb_data_##name;
#include "pyapi_list.h"
#undef LPB_PYFUNC
#undef LPB_PYPROC
#undef LPB_PYDATA

#define LPB_PYFUNC(ret, name, params, args) {#name, (void *)&lpb_py.name},
#define LPB_PYPROC(name, params, args) {#name, (void *)&lpb_py.name},
#define LPB_PYDATA(type, name) {#name, (void *)&lpb_data_##name},
static const struct {
    const char *name;
    void *slot;
} lpb_symbols[] = {
#include "pyapi_list.h"
    {NULL, NULL}
};
#undef LPB_PYFUNC
#undef LPB_PYPROC
#undef LPB_PYDATA

/* ------------------------------------------------------------------------------------------ */
/* Helpers                                                                                    */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    char *s;
    size_t n, cap;
} strbuf;

static void sb_add(strbuf *b, const char *s) {
    size_t len = strlen(s);
    if (b->n + len + 1 > b->cap) {
        size_t cap = (b->n + len + 1) * 2;
        char *p = (char *)realloc(b->s, cap);
        if (!p)
            return;
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, len + 1);
    b->n += len;
}

static void sb_addf(strbuf *b, const char *a, const char *c, const char *d) {
    sb_add(b, a);
    if (c)
        sb_add(b, c);
    if (d)
        sb_add(b, d);
}

static int lpb_loaded;
static char *q_path_json, *q_executable, *q_prefix, *q_exec_prefix;

typedef void *lpb_lib;

static void *lib_sym(lpb_lib h, const char *name) {
#ifdef _WIN32
    FARPROC p = GetProcAddress((HMODULE)h, name);
    void *r;
    memcpy(&r, &p, sizeof r);
    return r;
#else
    return dlsym(h, name);
#endif
}

/* Keep this module loaded for the life of the process: Python keeps pointers into it (types,
   the `lua` module, the exit hook), while Lua's lua_close would otherwise unload it. */
static void pin_self(void) {
#ifdef _WIN32
    HMODULE h;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void *)&lpb_pyloader_load, &h);
#else
    Dl_info info;
    if (dladdr((void *)&lpb_pyloader_load, &info) && info.dli_fname)
        (void)dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
}

/* A libpython already present in the process (the host embeds Python itself). */
static lpb_lib probe_process(void) {
#ifdef _WIN32
    char name[32];
    int minor, dbg;
    for (minor = 10; minor < 40; minor++) {
        for (dbg = 0; dbg < 2; dbg++) {
            HMODULE h;
            snprintf(name, sizeof name, "python3%d%s.dll", minor, dbg ? "_d" : "");
            h = GetModuleHandleA(name);
            if (h)
                return (lpb_lib)h;
        }
    }
    return NULL;
#else
    Dl_info info;
    void *sym = dlsym(RTLD_DEFAULT, "Py_IsInitialized");
    if (!sym || sym == (void *)&Py_IsInitialized)
        return NULL;
    if (dladdr(sym, &info) && info.dli_fname) {
        void *h = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
        if (h)
            return h;
    }
    return dlopen(NULL, RTLD_NOW);
#endif
}

static lpb_lib open_lib(const char *path, strbuf *log) {
#ifdef _WIN32
    wchar_t wpath[4096];
    HMODULE h;
    char err[64];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096)) {
        sb_addf(log, "  ", path, ": invalid path\n");
        return NULL;
    }
    h = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        snprintf(err, sizeof err, ": LoadLibrary failed (error %lu)\n", (unsigned long)GetLastError());
        sb_addf(log, "  ", path, err);
    }
    return (lpb_lib)h;
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        sb_addf(log, "  ", dlerror(), "\n");
    return h;
#endif
}

/* ------------------------------------------------------------------------------------------ */
/* Asking an interpreter where its libpython is                                               */
/* ------------------------------------------------------------------------------------------ */

static const char query_script[] =
    "import sys, os, json, sysconfig\n"
    "v = sysconfig.get_config_var\n"
    "c = []\n"
    "if sys.platform == 'win32':\n"
    "    n = 'python%d%d%s.dll' % (sys.version_info[0], sys.version_info[1], '_d' if hasattr(sys, 'gettotalrefcount') else '')\n"
    "    for d in (sys.base_prefix, sys.base_exec_prefix, os.path.dirname(getattr(sys, '_base_executable', '') or sys.executable)):\n"
    "        c.append(os.path.join(d, n))\n"
    "else:\n"
    "    ld = v('LDLIBRARY') or ''\n"
    "    fw = v('PYTHONFRAMEWORKPREFIX')\n"
    "    if fw and ld:\n"
    "        c.append(os.path.join(fw, ld))\n"
    "    libdir = v('LIBDIR') or ''\n"
    "    for d in (libdir, os.path.join(libdir, v('MULTIARCH') or ''), os.path.join(sys.base_prefix, 'lib')):\n"
    "        for n in (v('INSTSONAME'), ld):\n"
    "            if d and n:\n"
    "                c.append(os.path.join(d, n))\n"
    "lib = next((p for p in c if os.path.isfile(p) and not p.endswith('.a')), '')\n"
    "ft = '1' if v('Py_GIL_DISABLED') else '0'\n"
    "out = [lib, '%d.%d' % sys.version_info[:2], ft, sys.executable, sys.prefix, sys.exec_prefix,\n"
    "       json.dumps(sys.path), json.dumps(c)]\n"
    "sys.stdout.buffer.write(('\\n'.join(out) + '\\n').encode('utf-8', 'surrogateescape'))\n";

/* -c argument that runs query_script without any quoting concerns. */
static char *query_arg(void) {
    static const char hex[] = "0123456789abcdef";
    static const char pre[] = "exec(bytes.fromhex('", post[] = "').decode())";
    size_t n = sizeof query_script - 1, i;
    char *a = (char *)malloc(sizeof pre + 2 * n + sizeof post), *p;
    if (!a)
        return NULL;
    memcpy(a, pre, sizeof pre - 1);
    p = a + sizeof pre - 1;
    for (i = 0; i < n; i++) {
        *p++ = hex[(unsigned char)query_script[i] >> 4];
        *p++ = hex[(unsigned char)query_script[i] & 15];
    }
    memcpy(p, post, sizeof post);
    return a;
}

#ifdef _WIN32
static char *run_capture(const char *exe, const char *arg) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE r = NULL, w = NULL, nul;
    size_t len = strlen(exe) + strlen(arg) + 16;
    char *cmd = (char *)malloc(len), *out = NULL;
    wchar_t *wcmd = NULL;
    size_t n = 0, cap = 0;
    DWORD got, code = 1;
    int wlen;
    if (!cmd)
        return NULL;
    /* `exe` is either a path (quoted) or "py -3" style command (unquoted, has a space and no
       path separator) */
    if (strchr(exe, '\\') || strchr(exe, '/'))
        snprintf(cmd, len, "\"%s\" -c \"%s\"", exe, arg);
    else
        snprintf(cmd, len, "%s -c \"%s\"", exe, arg);
    wlen = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
    wcmd = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wcmd || !MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, wlen) || !CreatePipe(&r, &w, &sa, 0))
        goto done;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = nul;
    si.hStdInput = NULL;
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        goto done;
    }
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    CloseHandle(w);
    w = NULL;
    for (;;) {
        if (n + 4096 > cap) {
            char *p = (char *)realloc(out, cap = n + 8192);
            if (!p)
                break;
            out = p;
        }
        if (!ReadFile(r, out + n, 4096, &got, NULL) || got == 0)
            break;
        n += got;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (out)
        out[n] = 0;
    if (code != 0 || n == 0) {
        free(out);
        out = NULL;
    }
done:
    if (r)
        CloseHandle(r);
    if (w)
        CloseHandle(w);
    free(cmd);
    free(wcmd);
    return out;
}
#else
static char *run_capture(const char *exe, const char *arg) {
    int fds[2], status = 0;
    pid_t pid;
    posix_spawn_file_actions_t fa;
    char *argv[4], *out = NULL;
    size_t n = 0, cap = 0;
    ssize_t got;
    if (pipe(fds) != 0)
        return NULL;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    argv[0] = (char *)exe;
    argv[1] = (char *)"-c";
    argv[2] = (char *)arg;
    argv[3] = NULL;
    if (posix_spawnp(&pid, exe, &fa, NULL, argv, LPB_ENVIRON) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    for (;;) {
        if (n + 4096 > cap) {
            char *p = (char *)realloc(out, cap = n + 8192);
            if (!p)
                break;
            out = p;
        }
        got = read(fds[0], out + n, 4096);
        if (got <= 0)
            break;
        n += (size_t)got;
    }
    close(fds[0]);
    if (waitpid(pid, &status, 0) == pid && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        free(out);
        return NULL;
    }
    if (out)
        out[n] = 0;
    if (n == 0) {
        free(out);
        return NULL;
    }
    return out;
}
#endif

/* Splits out into at most 8 lines (in place). */
static int split_lines(char *out, char **lines, int max) {
    int k = 0;
    char *p = out;
    while (k < max && p && *p) {
        char *nl = strchr(p, '\n');
        lines[k++] = p;
        if (nl) {
            *nl = 0;
            if (nl > p && nl[-1] == '\r')
                nl[-1] = 0;
            p = nl + 1;
        } else {
            p = NULL;
        }
    }
    return k;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

/* Asks `exe` for its libpython and opens it. */
static lpb_lib try_interpreter(const char *exe, strbuf *log) {
    char *arg = query_arg(), *out, *lines[8];
    lpb_lib h;
    int major = 0, minor = 0;
    if (!arg)
        return NULL;
    out = run_capture(exe, arg);
    free(arg);
    if (!out) {
        sb_addf(log, "  ", exe, ": not found, or it is not a working Python 3 interpreter\n");
        return NULL;
    }
    if (split_lines(out, lines, 8) < 8) {
        sb_addf(log, "  ", exe, ": unexpected output\n");
        free(out);
        return NULL;
    }
    if (sscanf(lines[1], "%d.%d", &major, &minor) != 2 || major != 3 || minor < 10) {
        sb_addf(log, "  ", exe, ": Python ");
        sb_addf(log, lines[1], " is too old (3.10 or newer is required)\n", NULL);
        free(out);
        return NULL;
    }
    if (lines[2][0] == '1') {
        sb_addf(log, "  ", exe, ": free-threaded Python builds are not supported\n");
        free(out);
        return NULL;
    }
    if (!lines[0][0]) {
        sb_addf(log, "  ", exe, ": this Python has no shared libpython (looked for: ");
        sb_addf(log, lines[7], "); install the package that provides it (e.g. libpython3.X) or "
                "use a Python built with --enable-shared\n", NULL);
        free(out);
        return NULL;
    }
    h = open_lib(lines[0], log);
    if (h) {
        q_executable = dup_str(lines[3]);
        q_prefix = dup_str(lines[4]);
        q_exec_prefix = dup_str(lines[5]);
        q_path_json = dup_str(lines[6]);
    }
    free(out);
    return h;
}

/* ------------------------------------------------------------------------------------------ */
/* Entry points                                                                               */
/* ------------------------------------------------------------------------------------------ */

static int fail(lua_State *L, strbuf *log, const char *msg) {
    lua_pushfstring(L, "python: %s\n%s", msg, log->s ? log->s : "");
    free(log->s);
    return -1;
}

int lpb_pyloader_load(lua_State *L) {
    strbuf log = {NULL, 0, 0};
    lpb_lib h;
    const char *env, *version;
    int i, major = 0, minor = 0;

    if (lpb_loaded)
        return 0;
    pin_self();

    h = probe_process();
    if (!h) {
        env = getenv("LUA_PYTHON_LIBPYTHON");
        if (env && *env) {
            h = open_lib(env, &log);
            if (!h)
                return fail(L, &log, "cannot load the library named by LUA_PYTHON_LIBPYTHON:");
        }
    }
    if (!h) {
        env = getenv("LUA_PYTHON_EXECUTABLE");
        if (env && *env) {
            h = try_interpreter(env, &log);
        } else {
            static const char *const defaults[] = {
                "python3", "python",
#ifdef _WIN32
                "py -3",
#endif
                NULL
            };
            for (i = 0; defaults[i] && !h; i++)
                h = try_interpreter(defaults[i], &log);
        }
    }
    if (!h)
        return fail(L, &log, "could not find a usable libpython (set LUA_PYTHON_EXECUTABLE to a "
                    "Python 3.10+ interpreter, or LUA_PYTHON_LIBPYTHON to the library):");

    if (lib_sym(h, "_Py_MergeZeroLocalRefcount"))
        return fail(L, &log, "free-threaded Python builds are not supported");
    for (i = 0; lpb_symbols[i].name; i++) {
        void *sym = lib_sym(h, lpb_symbols[i].name);
        if (!sym) {
            sb_addf(&log, "  missing symbol: ", lpb_symbols[i].name, "\n");
            return fail(L, &log, "the loaded libpython lacks required symbols (CPython 3.10 or newer "
                        "is required):");
        }
        memcpy(lpb_symbols[i].slot, &sym, sizeof sym);
    }
    version = Py_GetVersion();
    if (sscanf(version, "%d.%d", &major, &minor) != 2 || major != 3 || minor < 10)
        return fail(L, &log, "CPython 3.10 or newer is required");
    free(log.s);
    lpb_loaded = 1;
    return 0;
}

static int set_sys_str(const char *name, const char *value) {
    PyObject *s;
    int rc;
    if (!value)
        return 0;
    s = PyUnicode_DecodeUTF8(value, (Py_ssize_t)strlen(value), "surrogateescape");
    if (!s)
        return -1;
    rc = PySys_SetObject(name, s);
    Py_DECREF(s);
    return rc;
}

/* After Py_InitializeEx: adopt the queried interpreter's environment (virtualenvs etc.). */
int lpb_pyloader_after_init(void) {
    PyObject *json, *path, *text;
    if (!q_path_json)
        return 0;
    if (set_sys_str("executable", q_executable) < 0 || set_sys_str("prefix", q_prefix) < 0 ||
        set_sys_str("exec_prefix", q_exec_prefix) < 0)
        return -1;
    json = PyImport_ImportModule("json");
    if (!json)
        return -1;
    text = PyUnicode_DecodeUTF8(q_path_json, (Py_ssize_t)strlen(q_path_json), "surrogateescape");
    path = text ? lpb_call_method1(json, "loads", text) : NULL;
    Py_XDECREF(text);
    Py_DECREF(json);
    if (!path)
        return -1;
    if (PyList_Check(path) && PySys_SetObject("path", path) < 0) {
        Py_DECREF(path);
        return -1;
    }
    Py_DECREF(path);
    return 0;
}
