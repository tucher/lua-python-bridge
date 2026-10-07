import threading
import time

import lua


def spin_count(work):
    """Iterations a background Python thread manages while `work` runs."""
    counter = [0]
    stop = threading.Event()

    def spin():
        while not stop.is_set():
            counter[0] += 1
            time.sleep(0.0005)

    th = threading.Thread(target=spin)
    th.start()
    try:
        work()
    finally:
        stop.set()
        th.join()
    return counter[0]


def test_gil_is_released_while_lua_runs():
    # Relative to the same thread while the main thread sleeps, so that timer resolution
    # (about 1 ms on Windows) does not matter. A starved thread gets almost nothing.
    baseline = spin_count(lambda: time.sleep(0.5))
    during_lua = spin_count(lambda: lua.execute("local t = os.clock() while os.clock() - t < 0.5 do end"))
    assert during_lua > baseline / 4, (during_lua, baseline)


def test_threads_share_the_state_safely():
    lua.execute("shared_count = 0 function bump() shared_count = shared_count + 1 end")
    bump = lua.globals().bump

    def worker():
        for _ in range(500):
            bump()

    threads = [threading.Thread(target=worker) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert lua.eval("shared_count") == 4000


def test_lua_calling_python_that_waits_for_a_thread_calling_lua():
    lua.execute("function lua_square(x) return x * x end")
    square = lua.globals().lua_square
    results = []

    def run_in_thread():
        th = threading.Thread(target=lambda: results.append(square(7)))
        th.start()
        th.join()
        return results[0]

    lua.globals().run_in_thread = run_in_thread
    assert lua.eval("run_in_thread()") == 49


def test_objects_released_from_other_threads():
    def make():
        for _ in range(1000):
            lua.eval("{}")

    threads = [threading.Thread(target=make) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    lua.execute("collectgarbage()")
