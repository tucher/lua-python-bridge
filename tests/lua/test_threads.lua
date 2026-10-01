local T = require "harness"
local python = require "python"

python.execute([[
import threading, time
counter = [0]
stop = [False]
def spin():
    while not stop[0]:
        counter[0] += 1
        time.sleep(0.0005)
]])

local function busy(seconds)
  local t = os.clock()
  while os.clock() - t < seconds do end
end

T.test("Python threads run while Lua code runs", function()
  python.execute("counter[0] = 0; stop[0] = False\nth = threading.Thread(target=spin); th.start()")
  busy(0.5)
  local n = python.eval("counter[0]")
  python.execute("stop[0] = True; th.join()")
  T.truthy(n > 50, "background Python thread starved: " .. n .. " iterations")
end)

T.test("a Python thread can call Lua while Lua waits in Python", function()
  python.globals().lua_cb = function(x) return x * 2 end
  python.execute([[
results = []
def worker():
    for i in range(50):
        results.append(lua_cb(i))
th = threading.Thread(target=worker); th.start(); th.join()
]])
  T.eq(python.eval("sum(results)"), 2 * (49 * 50 // 2))
end)

T.test("python.serve lets waiting threads in", function()
  python.globals().lua_cb2 = function() return "served" end
  python.execute([[
served = []
th = threading.Thread(target=lambda: served.append(lua_cb2())); th.start()
]])
  local deadline = os.time() + 5
  while python.eval("len(served)") == 0 and os.time() < deadline do python.serve(0.05) end
  python.execute("th.join()")
  T.eq(python.eval("served[0]"), "served")
end)

T.test("many threads calling Lua concurrently", function()
  local count = 0
  python.globals().lua_inc = function() count = count + 1 return count end
  python.execute([[
def hammer():
    for _ in range(200):
        lua_inc()
ths = [threading.Thread(target=hammer) for _ in range(8)]
for t in ths: t.start()
for t in ths: t.join()
]])
  T.eq(count, 1600)
end)

T.test("asyncio with Lua callbacks", function()
  python.globals().lua_handler = function(x) return "handled " .. x end
  python.execute([[
import asyncio
async def main():
    loop = asyncio.get_running_loop()
    a = await loop.run_in_executor(None, lua_handler, 1)
    b = lua_handler(2)
    return a + ', ' + b
async_result = asyncio.run(main())
]])
  T.eq(python.eval("async_result"), "handled 1, handled 2")
end)

T.done()
