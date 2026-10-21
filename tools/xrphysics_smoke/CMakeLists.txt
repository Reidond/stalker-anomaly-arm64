# Headless xrPhysics smoke test: physics world, shells and characters on a static mesh, stepped through xrPhysics's own
# API. Added from cmake/targets/xrPhysics.cmake, so it exists only when xrPhysics and xrCDB are enabled:
#   cmake --preset macos-arm64 -DANOMALY_BUILD_XRPHYSICS=ON -DANOMALY_BUILD_XRCDB=ON
#   cmake --build --preset macos-arm64 --target xrphysics_smoke && ctest --test-dir build/macos-arm64 -R xrphysics_smoke
add_executable(xrphysics_smoke main.cpp)
target_link_libraries(xrphysics_smoke PRIVATE xrPhysics xrCDB ode)
# main.cpp includes the module's own "stdafx.h" and headers by their short names, like the module's sources
target_include_directories(xrphysics_smoke PRIVATE "${ANOMALY_SRC_DIR}/xrPhysics" "${ANOMALY_SRC_DIR}/3rd party/ode")
target_compile_options(xrphysics_smoke PRIVATE
    "SHELL:-iquote \"${ANOMALY_SRC_DIR}/xrPhysics\""
    "SHELL:-iquote \"${ANOMALY_SRC_DIR}/3rd party/ode\"")
add_test(NAME xrphysics_smoke COMMAND xrphysics_smoke)
