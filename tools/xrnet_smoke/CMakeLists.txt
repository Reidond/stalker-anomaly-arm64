# Headless check of the single-player message path through xrNetServer (see docs/ios-port/NETSERVER.md). Added from
# cmake/targets/xrNetServer.cmake, so it exists only when ANOMALY_BUILD_XRNETSERVER (or ANOMALY_BUILD_ALL_MODULES) is on.
if(ANOMALY_IOS)
    return() # a console tool; on iPadOS xrNetServer is exercised through the app targets
endif()

add_executable(xrnet_smoke main.cpp)
target_link_libraries(xrnet_smoke PRIVATE Anomaly::xrNetServer)
# the tool needs no game data; it writes a minimal fsgame.ltx, its log and appdata here (override with --run-dir)
target_compile_definitions(xrnet_smoke PRIVATE XRNET_SMOKE_RUN_DIR="${CMAKE_CURRENT_BINARY_DIR}/run")

add_test(NAME xrnet_smoke COMMAND xrnet_smoke)
