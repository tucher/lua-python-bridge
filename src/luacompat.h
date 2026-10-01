/*
 * lua-python-bridge: differences between Lua 5.3, 5.4 and 5.5.
 */
#ifndef LPB_LUACOMPAT_H
#define LPB_LUACOMPAT_H

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 503
#  error "lua-python-bridge requires Lua 5.3, 5.4 or 5.5"
#endif

#if LUA_VERSION_NUM == 503
#  define lpb_newuserdata(L, sz) lua_newuserdata((L), (sz))
#else
#  define lpb_newuserdata(L, sz) lua_newuserdatauv((L), (sz), 1)
#endif

/* One user value per userdata is enough for the bridge. */
#if LUA_VERSION_NUM == 503
#  define lpb_setuservalue(L, idx) lua_setuservalue((L), (idx))
#  define lpb_getuservalue(L, idx) lua_getuservalue((L), (idx))
#else
#  define lpb_setuservalue(L, idx) lua_setiuservalue((L), (idx), 1)
#  define lpb_getuservalue(L, idx) lua_getiuservalue((L), (idx), 1)
#endif

#endif
