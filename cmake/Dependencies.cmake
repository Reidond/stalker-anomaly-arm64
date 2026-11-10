# Third-party libraries for the Apple port, built from the pinned submodules in external/.

set(ANOMALY_EXTERNAL_DIR "${PROJECT_SOURCE_DIR}/external")

# The scripts' VM: LuaJIT 2.1 (the default), or PUC Lua 5.1.5 + LuaBitOp compiled as C++ (in-tree; what the web build uses,
# docs/web-port/LUA51.md). Code that differs tests ANOMALY_LUA51; targets link Anomaly::Lua.
set(ANOMALY_LUA "luajit" CACHE STRING "Lua VM for the scripts: luajit (LuaJIT 2.1) or lua51 (PUC Lua 5.1.5 + LuaBitOp)")
set_property(CACHE ANOMALY_LUA PROPERTY STRINGS luajit lua51)
if(NOT ANOMALY_LUA MATCHES "^(luajit|lua51)$")
    message(FATAL_ERROR "ANOMALY_LUA must be luajit or lua51 (got '${ANOMALY_LUA}')")
endif()
set(anomaly_luajit_submodule LuaJIT)
if(ANOMALY_LUA STREQUAL "lua51")
    set(anomaly_luajit_submodule "")
endif()

foreach(dep SDL openal-soft ${anomaly_luajit_submodule} ogg vorbis theora SPIRV-Cross vkd3d sse2neon)
    if(NOT EXISTS "${ANOMALY_EXTERNAL_DIR}/${dep}/.git")
        message(FATAL_ERROR "external/${dep} is missing; run: git submodule update --init --depth 1 external/${dep}")
    endif()
endforeach()

# Third-party code is not ours to fix, so keep its warnings out of the build log.
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

# SDL3: window, Metal view, input, gamepads (DualSense via GameController), app lifecycle.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ANOMALY_EXTERNAL_DIR}/SDL" "${CMAKE_BINARY_DIR}/external/SDL" EXCLUDE_FROM_ALL SYSTEM)

# OpenAL Soft: the engine's sound backend (xrSound already speaks OpenAL).
set(LIBTYPE STATIC CACHE STRING "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EMBED_HRTF_DATA ON CACHE BOOL "" FORCE)
add_subdirectory("${ANOMALY_EXTERNAL_DIR}/openal-soft" "${CMAKE_BINARY_DIR}/external/openal-soft" EXCLUDE_FROM_ALL SYSTEM)
# 1.25.2 builds with -Werror=function-effects, which newer Apple clang trips in the CoreAudio backend.
target_compile_options(OpenAL PRIVATE -Wno-error=function-effects -w)
if(ANOMALY_IOS)
    # AvailabilityMacros.h sets MAC_OS_X_VERSION_MIN_REQUIRED to 10.5 on iOS, so OpenAL Soft takes its
    # "old macOS" fallbacks. One of them, a posix_memalign-based aligned operator new, rejects 4-byte
    # alignment and makes alGenEffects fail, which breaks EAX reverb. The header only sets the macro
    # when it is unset.
    target_compile_definitions(OpenAL PRIVATE MAC_OS_X_VERSION_MIN_REQUIRED=140000)
endif()

# Ogg / Vorbis / Theora: sound (xrSound) and video (xrEngine/xrTheora_*) decoding.
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${ANOMALY_EXTERNAL_DIR}/ogg" "${CMAKE_BINARY_DIR}/external/ogg" EXCLUDE_FROM_ALL SYSTEM)
include(deps/Vorbis)
include(deps/Theora)

# LuaJIT 2.1: built with its own Makefile; JIT is on for macOS, interpreter-only on iOS by default.
# With ANOMALY_LUA=lua51, Lua 5.1.5 instead (cmake/deps/Lua51.cmake).
add_library(anomaly_lua INTERFACE)
if(ANOMALY_LUA STREQUAL "lua51")
    include(deps/Lua51)
    target_link_libraries(anomaly_lua INTERFACE Lua51::lua51)
else()
    include(deps/LuaJIT)
    target_link_libraries(anomaly_lua INTERFACE LuaJIT::luajit)
endif()
add_library(Anomaly::Lua ALIAS anomaly_lua)
message(STATUS "Lua VM: ${ANOMALY_LUA}")

# Shader translation (src/Platform/ShaderXlat): HLSL -> SPIR-V (vkd3d-shader), SPIR-V -> MSL (SPIRV-Cross).
include(deps/SPIRV-Cross)
include(deps/Vkd3dShader)

# SSE intrinsics -> NEON for the engine's SSE code paths (single header).
include(deps/sse2neon)
