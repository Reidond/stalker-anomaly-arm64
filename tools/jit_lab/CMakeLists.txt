# jit_lab: LuaJIT benchmark and stability app for macOS and iPadOS. See docs/ios-port/IPAD-JIT.md.

# Embed the Lua workloads so the app has no resource lookup to get wrong on a device.
set(bench_lua_file "${CMAKE_CURRENT_SOURCE_DIR}/lua/bench.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bench_lua_file}")
file(READ "${bench_lua_file}" bench_lua_text)
set(bench_lua_header "${CMAKE_CURRENT_BINARY_DIR}/bench_lua.h")
set(bench_lua_new "// Generated from lua/bench.lua by CMake.\n#pragma once\nstatic const char kBenchLua[] = R\"JITLABLUA(${bench_lua_text})JITLABLUA\";\n")
if(EXISTS "${bench_lua_header}")
    file(READ "${bench_lua_header}" bench_lua_old)
else()
    set(bench_lua_old "")
endif()
if(NOT bench_lua_old STREQUAL bench_lua_new)
    file(WRITE "${bench_lua_header}" "${bench_lua_new}")
endif()

add_executable(jit_lab main.cpp lab.cpp lab.h)
target_include_directories(jit_lab PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# The LuaJIT variant is a compile definition so that lab.cpp is rebuilt when it changes
# (it includes luajit_txm.h only in the patched variants).
target_compile_definitions(jit_lab PRIVATE
    JITLAB_BUNDLE_ID="${ANOMALY_BUNDLE_ID_PREFIX}.jitlab"
    JITLAB_LUAJIT_VARIANT="${luajit_variant}")
target_link_libraries(jit_lab PRIVATE SDL3::SDL3-static LuaJIT::luajit)
add_dependencies(jit_lab luajit_build)  # the library is rebuilt from scratch when the LuaJIT variant changes

anomaly_configure_app(jit_lab
    BUNDLE_SUFFIX ".jitlab"
    DISPLAY_NAME "JIT Lab"
    INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-${ANOMALY_PLATFORM_NAME}.plist.in"
    ENTITLEMENTS "${ANOMALY_IOS_ENTITLEMENTS}"
)
