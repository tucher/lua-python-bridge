/*
 * Embedding test: a C host with several Lua states and threads, each loading the Lua module
 * "python" through require (LUA_CPATH must point at the built module).
 *
 * Covers the ownership rules of PLAN.md 5.8: states on one thread calling each other through
 * Python ("borrowing"), threads waiting for a state that its host is running, and states moving
 * between threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* ------------------------------------------------------------------------------------------ */
/* Threads                                                                                    */
/* ------------------------------------------------------------------------------------------ */

typedef struct job {
    void (*fn)(struct job *);
    lua_State *L;             /* input: a state to use (may be NULL) */
    int index;
    int failed;               /* output */
} job;

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE thread_t;
static DWORD WINAPI trampoline(LPVOID p) { ((job *)p)->fn((job *)p); return 0; }
static void thread_start(thread_t *t, job *j) { *t = CreateThread(NULL, 0, trampoline, j, 0, NULL); }
static void thread_join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#  include <pthread.h>
#  include <time.h>
typedef pthread_t thread_t;
static void *trampoline(void *p) { ((job *)p)->fn((job *)p); return NULL; }
static void thread_start(thread_t *t, job *j) { pthread_create(t, NULL, trampoline, j); }
static void thread_join(thread_t t) { pthread_join(t, NULL); }
#endif

/* ------------------------------------------------------------------------------------------ */
/* Helpers                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static int failures;          /* main thread only */
static const char *volatile current_step = "startup";

/* Deadlocks are the failure mode of the ownership rules: report them instead of hanging. */
#define WATCHDOG_SECONDS 120
static void watchdog(job *j) {
    (void)j;
#ifdef _WIN32
    Sleep(WATCHDOG_SECONDS * 1000);
#else
    {
        struct timespec ts = {WATCHDOG_SECONDS, 0};
        while (nanosleep(&ts, &ts) != 0) {}
    }
#endif
    fprintf(stderr, "FAIL timeout after %d s (deadlock?) in: %s\n", WATCHDOG_SECONDS, current_step);
    fflush(stderr);
    _Exit(3);
}

static lua_State *new_state(void) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    return L;
}

/* Runs a chunk; returns 1 and reports on failure. */
static int run_chunk(lua_State *L, const char *what, const char *code) {
    if (luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "FAIL %s: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }
    return 0;
}

static void run(lua_State *L, const char *what, const char *code) {
    current_step = what;
    failures += run_chunk(L, what, code);
}

static void join_job(thread_t t, job *j) {
    thread_join(t);
    failures += j->failed;
}

/* ------------------------------------------------------------------------------------------ */
/* Thread bodies                                                                              */
/* ------------------------------------------------------------------------------------------ */

/* Each worker has its own state and uses Python concurrently with the others. */
static void worker(job *j) {
    lua_State *L = new_state();
    j->failed = run_chunk(L, "worker thread",
        "local python = require 'python'\n"
        "local s = 0\n"
        "for i = 1, 200 do s = s + python.eval('sum(range(10))') end\n"
        "assert(s == 200 * 45, s)\n"
        "local t = python.eval('lambda f: f(2)')(function(x) return x * 10 end)\n"
        "assert(t == 20)\n");
    lua_close(L);
}

/* A state on this thread calls f1, a function of the main thread's state 1. */
static void cross_caller(job *j) {
    lua_State *L = new_state();
    j->failed = run_chunk(L, "state on another thread calls state 1",
        "local python = require 'python'\n"
        "python.execute('cross_result = f1(7)')\n");
    lua_close(L);
}

/* State 1 moves to this thread (the main thread waits in join meanwhile). After its first call
   into Python here, another state on this thread can borrow it. */
static void migrate(job *j) {
    lua_State *L5;
    j->failed = run_chunk(j->L, "state 1 runs on another thread",
        "assert(python.eval('1 + 1') == 2)\n");
    L5 = new_state();
    j->failed += run_chunk(L5, "a state on the new thread borrows state 1",
        "local python = require 'python'\n"
        "assert(python.eval('f1(3)') == 6)\n");
    lua_close(L5);
}

/* ------------------------------------------------------------------------------------------ */

#define NWORKERS 4

int main(void) {
    lua_State *L1 = new_state(), *L2 = new_state(), *L3;
    thread_t threads[NWORKERS], t, wd;
    job jobs[NWORKERS], j, wdjob;
    int i;

    memset(&wdjob, 0, sizeof wdjob);
    wdjob.fn = watchdog;
    thread_start(&wd, &wdjob);              /* never joined: the process exits */

    run(L1, "state 1 setup",
        "python = require 'python'\n"
        "python.globals().f1 = function(x) return x * 2 end\n"
        "python.globals().a_fn = function(n)\n"
        "  if n == 0 then return 'a' end\n"
        "  return python.eval('b_fn(' .. (n - 1) .. ')')\n"
        "end\n"
        "t1 = {}\n"
        "python.globals().t1 = t1\n");

    run(L2, "state 2 calls a function of state 1 (same thread)",
        "python = require 'python'\n"
        "assert(python.eval('f1(21)') == 42)\n");

    run(L2, "states on one thread call each other through Python, nested",
        "python.globals().b_fn = function(n)\n"
        "  if n == 0 then return 'b' end\n"
        "  return python.eval('a_fn(' .. (n - 1) .. ')')\n"
        "end\n"
        "assert(python.eval('a_fn(6)') == 'a')\n"
        "assert(python.eval('a_fn(5)') == 'b')\n"
        "assert(python.eval('b_fn(9)') == 'a')\n");

    run(L2, "objects of another state are rejected",
        "local ok, err = pcall(python.eval, 't1')\n"
        "assert(not ok and tostring(err):find('different Lua state', 1, true), tostring(err))\n");

    for (i = 0; i < NWORKERS; i++) {
        memset(&jobs[i], 0, sizeof jobs[i]);
        jobs[i].fn = worker;
        jobs[i].index = i;
        thread_start(&threads[i], &jobs[i]);
    }
    current_step = "worker threads";
    for (i = 0; i < NWORKERS; i++)
        join_job(threads[i], &jobs[i]);

    /* A borrow must hand the state back to the host: if it were left free, the Python thread
       below could run state 1's Lua while the host is running it. */
    run(L2, "borrow state 1 once more",
        "assert(python.eval('f1(1)') == 2)\n");
    run(L1, "after a borrow, the host owns its state again",
        "ran_at = nil\n"
        "python.globals().mark = function() ran_at = os.clock() return 1 end\n"
        "python.execute([[\n"
        "import threading, time\n"
        "def later():\n"
        "    time.sleep(0.1)\n"
        "    mark()\n"
        "th2 = threading.Thread(target=later)\n"
        "th2.start()\n"
        "]])\n"
        "local t0 = os.clock()\n"
        "while os.clock() - t0 < 0.5 do end\n"
        "local busy_end = os.clock()\n"
        "assert(ran_at == nil, 'a Python thread ran Lua while the host was running this state')\n"
        "local deadline = os.time() + 10\n"
        "while ran_at == nil and os.time() < deadline do python.serve(0.01) end\n"
        "python.execute('th2.join()')\n"
        "assert(ran_at and ran_at >= busy_end, 'the waiting thread never ran')\n");

    /* A state on another thread must wait while this thread runs state 1. */
    run(L1, "reset", "python.execute('cross_result = None')\n");
    memset(&j, 0, sizeof j);
    j.fn = cross_caller;
    thread_start(&t, &j);
    run(L1, "a state on another thread waits for the host, then runs",
        "local t0 = os.clock()\n"
        "while os.clock() - t0 < 0.3 do end\n"
        "assert(python.eval('cross_result') == nil, 'ran while the host was running its state')\n"
        "local deadline = os.time() + 10\n"
        "while python.eval('cross_result') == nil and os.time() < deadline do python.serve(0.01) end\n"
        "assert(python.eval('cross_result') == 14)\n");
    join_job(t, &j);

    /* The host moves state 1 to another thread. */
    current_step = "state 1 moves to another thread";
    memset(&j, 0, sizeof j);
    j.fn = migrate;
    j.L = L1;
    thread_start(&t, &j);
    join_job(t, &j);

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
