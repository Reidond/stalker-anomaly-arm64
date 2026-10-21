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

include(AnomalyPlatform)
include(Dependencies)

add_subdirectory("src/3rd party/luabind")
add_subdirectory("src/3rd party/ode")

# Host-side smoke tests of the engine libraries; the iOS build only checks that the libraries compile.
enable_testing()
if(NOT ANOMALY_IOS)
    add_subdirectory(tools/ode_smoke)
    add_subdirectory(tools/luabind_smoke)
endif()

add_subdirectory(tools/platform_probe)
add_subdirectory(src/xrCore)
add_subdirectory(tools/xrcore_smoke)

# App shell (paths, game-data validation, lifecycle, launcher) and the lab app that exercises it.
add_subdirectory(src/Platform/Apple)
add_subdirectory(tools/shell_lab)

add_subdirectory(src/Platform/MetalD3D11)
add_subdirectory(tools/metal_d3d11_test)
