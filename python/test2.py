import lua_python_binding

lg = lua_python_binding.globals()
print(lg.string.lower("Hello world!"))

d = {}
lg.d = d
lua_python_binding.execute("d['key'] = 'value'")
print(d)
d2 = lua_python_binding.eval("d")
print(d is d2)
