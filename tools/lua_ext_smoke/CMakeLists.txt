add_executable(lua_ext_smoke main.cpp)
target_link_libraries(lua_ext_smoke PRIVATE lua_extensions LuaJIT::luajit)
add_test(NAME lua_ext_smoke COMMAND lua_ext_smoke)
