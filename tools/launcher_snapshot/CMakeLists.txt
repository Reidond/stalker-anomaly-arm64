# Renders every launcher screen to a PNG with ImageRenderer, offscreen (no window): the PR screenshots. It compiles the launcher's
# own Swift sources next to its main.swift, so it sees the views the app uses.
#   cmake --build build/macos-arm64 --target launcher_snapshot
#   build/macos-arm64/tools/launcher_snapshot/launcher_snapshot <out-dir> [--logo <png>] [--only <name>]
#   build/macos-arm64/tools/launcher_snapshot/launcher_snapshot --selftest <work-dir>   (pad navigation, reordering, Play; no window)
#   build/macos-arm64/tools/launcher_snapshot/launcher_snapshot --fixtures <mods-root>   (four invented mods, for ANOMALY_MODS_ROOT)
add_executable(launcher_snapshot main.swift Fixtures.swift SelfTest.swift ${ANOMALY_LAUNCHER_SOURCES})
# the sanitizer flags of the engine build are not for swiftc (see src/Platform/Launcher/CMakeLists.txt)
set_property(TARGET launcher_snapshot PROPERTY COMPILE_OPTIONS "")
set_property(TARGET launcher_snapshot PROPERTY LINK_OPTIONS "")
set_target_properties(launcher_snapshot PROPERTIES Swift_LANGUAGE_VERSION 5 Swift_COMPILATION_MODE wholemodule)
if(ANOMALY_LAUNCHER_SWIFT_TARGET_OPTION)
    target_compile_options(launcher_snapshot PRIVATE "${ANOMALY_LAUNCHER_SWIFT_TARGET_OPTION}")
endif()
target_link_libraries(launcher_snapshot PRIVATE ModManagerCore ModDownloads)
