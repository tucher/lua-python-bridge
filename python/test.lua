python = require("python")
re = python.import("re")
pattern = re.compile("^Hel(lo) world!")
match = pattern.match("Hello world!")
print(match.group(1))
myscript = python.import("test")
print(myscript.fuuu())

print(myscript.call_funct(function(a, b, c) 
    return a+b+c
end, 1, 2, 3))

print(myscript.test_requests())

myscript.test_threading()
myscript.test_async_io()
myscript.test_server()