# Host test of the script bytecode cache (src/xrServerEntities/script_bytecode_cache.cpp has no engine dependency).
#   lua_cache_test                     checks: cold/warm, stale source, addon override, corrupt entries, other VM, unusable directory
#   lua_cache_test --bench <dir> [n]   times the compile of every *.script under <dir> from the text and through a warm cache
if(TARGET LuaJIT::luajit AND NOT ANOMALY_IOS)
    add_executable(lua_cache_test main.cpp "${PROJECT_SOURCE_DIR}/src/xrServerEntities/script_bytecode_cache.cpp")
    target_include_directories(lua_cache_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(lua_cache_test PRIVATE cxx_std_17)
    target_link_libraries(lua_cache_test PRIVATE LuaJIT::luajit)
    add_test(NAME lua_cache_test COMMAND lua_cache_test)
endif()
