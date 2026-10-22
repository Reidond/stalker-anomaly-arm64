# CMake build for the macOS / iPadOS port. The Windows build still uses src/engine-vs2022.sln.
cmake_minimum_required(VERSION 3.28)

project(AnomalyPort
    VERSION 0.1.0
    DESCRIPTION "STALKER Anomaly (xray-monolith) for macOS and iPadOS"
    LANGUAGES C CXX OBJC OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "This CMake build targets macOS and iOS only; use src/engine-vs2022.sln on Windows.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

enable_testing()

include(AnomalyPlatform)
include(Dependencies)
include(AnomalyModules)

# Engine libraries.
add_subdirectory("src/3rd party/luabind")
add_subdirectory("src/3rd party/ode")
add_subdirectory(src/xrCore)

# Engine modules (xrCDB ... xrGame): one file per module in cmake/targets, owned by the session porting it. Each is
# off by default (ANOMALY_BUILD_<MODULE>, or ANOMALY_BUILD_ALL_MODULES) until it compiles. New modules need no edit here.
file(GLOB anomaly_module_files "${PROJECT_SOURCE_DIR}/cmake/targets/*.cmake")
list(SORT anomaly_module_files)
foreach(anomaly_module_file IN LISTS anomaly_module_files)
    include("${anomaly_module_file}")
endforeach()
anomaly_finalize_modules()

# Port platform layer: app shell, controller input, shader translation, D3D11 subset on Metal.
add_subdirectory(src/Platform/Apple)
add_subdirectory(src/Platform/GameInput)
add_subdirectory(src/Platform/ShaderXlat)
add_subdirectory(src/Platform/MetalD3D11)

# xrEngine smoke test (macOS). After the module files, because it links xrGame and the renderer when they are built and
# takes stubs for the modules that are not.
if(NOT ANOMALY_IOS AND TARGET xrEngine)
    add_subdirectory(tools/xrengine_smoke)
endif()

# The game: every module linked statically into one app (needs ANOMALY_BUILD_ALL_MODULES).
add_subdirectory(src/Platform/App)

# Host-side smoke tests of the engine libraries; the iOS build only checks that the libraries compile.
if(NOT ANOMALY_IOS)
    add_subdirectory(tools/ode_smoke)
    add_subdirectory(tools/luabind_smoke)
endif()
add_subdirectory(tools/xrcore_smoke)

# Device and tooling apps.
add_subdirectory(tools/platform_probe)
add_subdirectory(tools/shell_lab)
add_subdirectory(tools/input_lab)
add_subdirectory(tools/jit_lab)
add_subdirectory(tools/metal_d3d11_test)
add_subdirectory(tools/xrdb_unpack)
add_subdirectory(tools/shader_batch)
