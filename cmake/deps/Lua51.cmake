# PUC Lua 5.1.5 with LuaBitOp 1.0.3, in-tree at src/3rd party/lua-5.1.5 (both MIT): the script VM when ANOMALY_LUA=lua51,
# and the one the web port uses, since LuaJIT has no wasm target (docs/web-port/LUA51.md).
#
# Compiled as C++, so lua_error unwinds as a C++ exception (luaconf.h LUAI_THROW) instead of longjmp: wasm has no cheap setjmp,
# and C++ unwinding runs the destructors of the engine frames it crosses. The API keeps C linkage (luaconf.h LUA_API).
# The changes to the upstream sources are marked ANOMALY and listed in src/3rd party/lua-5.1.5/README.anomaly.md.
#
# Targets: lua51 (static), Lua51::lua51. Consumers see ANOMALY_LUA51 defined.

set(lua51_dir "${PROJECT_SOURCE_DIR}/src/3rd party/lua-5.1.5")

set(lua51_sources
    lapi.c lauxlib.c lbaselib.c lcode.c ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c
    loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lvm.c lzio.c
    lanomaly.c)
list(TRANSFORM lua51_sources PREPEND "${lua51_dir}/src/")
list(APPEND lua51_sources "${lua51_dir}/bitop/bit.c")
set_source_files_properties(${lua51_sources} PROPERTIES LANGUAGE CXX)

add_library(lua51 STATIC ${lua51_sources})
target_include_directories(lua51 SYSTEM PUBLIC "${lua51_dir}/include")
target_include_directories(lua51 PRIVATE "${lua51_dir}/src")
# LUA_USE_POSIX: mkstemp for os.tmpname, popen, the POSIX error messages. LUA_USE_DLOPEN: C modules for require (unused by the
# game; the engine registers its libraries itself). LUA_USELONGLONG: string.format's %d and %x take 64-bit values on every
# target, wasm32 included (long is 32-bit there).
target_compile_definitions(lua51 PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN LUA_USELONGLONG)
target_compile_definitions(lua51 PUBLIC ANOMALY_LUA51)
# Compiled as C++: C++ exceptions for lua_error, and C++17 like the engine. Third-party code: no warnings in the build log.
set_target_properties(lua51 PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(lua51 PRIVATE -x c++ -w)
if(NOT ANOMALY_IOS)
    target_link_libraries(lua51 PUBLIC ${CMAKE_DL_LIBS})
endif()
add_library(Lua51::lua51 ALIAS lua51)
