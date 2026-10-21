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

add_subdirectory(tools/platform_probe)
