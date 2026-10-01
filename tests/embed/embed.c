/*
 * Embedding test: a C host with several Lua states and threads, each loading the Lua module
 * "python" through require (LUA_CPATH must point at the built module).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE thread_t;
static DWORD WINAPI thread_main(LPVOID arg);
static void thread_start(thread_t *t, void *arg) { *t = CreateThread(NULL, 0, thread_main, arg, 0, NULL); }
static void thread_join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#  include <pthread.h>
typedef pthread_t thread_t;
static void *thread_main(void *arg);
static void thread_start(thread_t *t, void *arg) { pthread_create(t, NULL, thread_main, arg); }
static void thread_join(thread_t t) { pthread_join(t, NULL); }
#endif

static int failures;

static lua_State *new_state(void) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    return L;
}

static int run(lua_State *L, const char *what, const char *code) {
    if (luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "FAIL %s: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
        failures++;
        return 1;
    }
    return 0;
}

#define NTHREADS 4
static int thread_failed[NTHREADS];

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID arg)
#else
static void *thread_main(void *arg)
#endif
{
    int i = (int)(size_t)arg;
    lua_State *L = new_state();
    if (luaL_dostring(L,
            "local python = require 'python'\n"
            "local s = 0\n"
            "for i = 1, 200 do s = s + python.eval('sum(range(10))') end\n"
            "assert(s == 200 * 45, s)\n"
            "local t = python.eval('lambda f: f(2)')(function(x) return x * 10 end)\n"
            "assert(t == 20)\n") != LUA_OK) {
        fprintf(stderr, "FAIL thread %d: %s\n", i, lua_tostring(L, -1));
        thread_failed[i] = 1;
    }
    lua_close(L);
    return 0;
}

int main(void) {
    lua_State *L1 = new_state(), *L2 = new_state(), *L3;
    thread_t threads[NTHREADS];
    int i;

    run(L1, "state 1 setup",
        "python = require 'python'\n"
        "python.globals().f1 = function(x) return x * 2 end\n"
        "t1 = {}\n"
        "python.globals().t1 = t1\n");

    run(L2, "state 2 calls a function of state 1 (same thread)",
        "python = require 'python'\n"
        "assert(python.eval('f1(21)') == 42)\n");

    run(L2, "objects of another state are rejected",
        "local ok, err = pcall(python.eval, 't1')\n"
        "assert(not ok and tostring(err):find('different Lua state', 1, true), tostring(err))\n");

    for (i = 0; i < NTHREADS; i++)
        thread_start(&threads[i], (void *)(size_t)i);
    for (i = 0; i < NTHREADS; i++) {
        thread_join(threads[i]);
        failures += thread_failed[i];
    }

    run(L1, "a Python thread calls into state 1 while it serves",
        "python.execute([[\n"
        "import threading\n"
        "res = []\n"
        "th = threading.Thread(target=lambda: res.append(f1(5)))\n"
        "th.start()\n"
        "]])\n"
        "local deadline = os.time() + 10\n"
        "while python.eval('len(res)') == 0 and os.time() < deadline do python.serve(0.01) end\n"
        "python.execute('th.join()')\n"
        "assert(python.eval('res[0]') == 10)\n");

    lua_close(L1);

    run(L2, "objects of a closed state raise",
        "python.execute([[\n"
        "try:\n"
        "    f1(1)\n"
        "    r = 'no error'\n"
        "except Exception as e:\n"
        "    r = type(e).__name__ + ': ' + str(e)\n"
        "]])\n"
        "local r = python.eval('r')\n"
        "assert(r:find('closed', 1, true), r)\n"
        "assert(python.eval('repr(t1)'):find('state closed', 1, true))\n");

    L3 = new_state();
    run(L3, "a new state after closing one",
        "python = require 'python'\n"
        "assert(python.eval('1 + 1') == 2)\n");

    lua_close(L2);
    lua_close(L3);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("embed test passed\n");
    return 0;
}
