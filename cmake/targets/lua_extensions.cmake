# lua_extensions: Lua libraries and helpers built into the script engine (lfs, marshal, string/math/table libs,
# luasocket, keyboard, debugger glue). Source list: src/3rd party/lua-extensions/lua_extensions.vcxproj.
# Owner: xrgame-script-entities session. Opt-in: -DANOMALY_BUILD_LUA_EXTENSIONS=ON.
anomaly_module_begin(lua_extensions "Lua extension libraries")

anomaly_vcxproj_sources(lua_extensions_sources "${ANOMALY_SRC_DIR}/3rd party/lua-extensions/lua_extensions.vcxproj")

anomaly_add_module(lua_extensions
    SOURCES ${lua_extensions_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/3rd party/lua-extensions"
    DEFINES _CRT_SECURE_NO_WARNINGS
    LINK LuaJIT::luajit
)
