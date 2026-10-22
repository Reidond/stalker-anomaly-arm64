# lua_extensions: Lua libraries and helpers built into the script engine (lfs, marshal, string/math/table libs,
# luasocket, keyboard, debugger glue). Source list: src/3rd party/lua-extensions/lua_extensions.vcxproj.
# Owner: xrgame-script-entities session. Opt-in: -DANOMALY_BUILD_LUA_EXTENSIONS=ON.
anomaly_module_begin(lua_extensions "Lua extension libraries")

anomaly_vcxproj_sources(lua_extensions_sources "${ANOMALY_SRC_DIR}/3rd party/lua-extensions/lua_extensions.vcxproj")

# luasocket: Winsock (wsocket.c) on Windows, BSD sockets (usocket.c, already in the tree) on Apple.
if(NOT WIN32)
    list(FILTER lua_extensions_sources EXCLUDE REGEX "/luasocket/wsocket\\.c$")
    list(APPEND lua_extensions_sources "${ANOMALY_SRC_DIR}/3rd party/lua-extensions/luasocket/usocket.c")
endif()

# The Windows build compiles against src/3rd party/luajit-2 (LuaJIT 2.0.4, whose lauxlib.h keeps the Lua 5.0 names);
# the Apple port links LuaJIT 2.1 from external/LuaJIT, which dropped `luaL_reg`.
anomaly_add_module(lua_extensions
    SOURCES ${lua_extensions_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/3rd party/lua-extensions"
    DEFINES _CRT_SECURE_NO_WARNINGS luaL_reg=luaL_Reg
    LINK LuaJIT::luajit
)
