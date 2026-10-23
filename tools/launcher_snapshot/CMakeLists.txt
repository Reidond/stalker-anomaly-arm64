# Renders every launcher screen to a PNG with ImageRenderer, offscreen (no window): the PR screenshots. It compiles the launcher's
# own Swift sources next to its main.swift, so it sees the views the app uses.
#   cmake --build build/macos-arm64 --target launcher_snapshot
#   build/macos-arm64/tools/launcher_snapshot/launcher_snapshot <out-dir> [--logo <png>] [--only <name>]
#   build/macos-arm64/tools/launcher_snapshot/launcher_snapshot --selftest <work-dir>   (pad navigation, reordering, Play; no window)
#   build/macos-arm64/tools/launcher_snapshot/launcher_snapshot --fixtures <mods-root>   (four invented mods, for ANOMALY_MODS_ROOT)
add_executable(launcher_snapshot main.swift Fixtures.swift SelfTest.swift ${ANOMALY_LAUNCHER_SOURCES})
set_target_properties(launcher_snapshot PROPERTIES Swift_LANGUAGE_VERSION 5)
target_compile_options(launcher_snapshot PRIVATE "SHELL:-target ${ANOMALY_LAUNCHER_SWIFT_TRIPLE}")
foreach(launcher_snapshot_dep ModManagerCore ModDownloads)
    if(TARGET ${launcher_snapshot_dep})
        target_link_libraries(launcher_snapshot PRIVATE ${launcher_snapshot_dep})
    endif()
endforeach()
