# The SwiftUI launcher: the app's first screen (home, mods, add mod). A static library of Swift files; the app shell reaches it through
# the C functions of AnomalyLauncher.h. The mod manager and downloads modules (ModManagerCore, ModDownloads) are linked when they are in
# the build; Stubs.swift stands in for whichever is not, and goes away once both are on main. Documented in docs/ios-port/MOD-MANAGER.md.
file(GLOB anomaly_launcher_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.swift")
set(ANOMALY_LAUNCHER_SOURCES ${anomaly_launcher_sources} CACHE INTERNAL "SwiftUI launcher sources (also compiled into tools/launcher_snapshot)")

# CMake does not pass the deployment target to swiftc, which would then build for the SDK's newest OS.
if(ANOMALY_IOS)
    set(ANOMALY_LAUNCHER_SWIFT_TRIPLE "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE INTERNAL "swiftc -target of the launcher")
else()
    set(ANOMALY_LAUNCHER_SWIFT_TRIPLE "arm64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE INTERNAL "swiftc -target of the launcher")
endif()

add_library(anomaly_launcher STATIC ${anomaly_launcher_sources} AnomalyLauncher.h)
set_target_properties(anomaly_launcher PROPERTIES
    Swift_LANGUAGE_VERSION 5
    Swift_MODULE_NAME AnomalyLauncher)
target_compile_options(anomaly_launcher PRIVATE "SHELL:-target ${ANOMALY_LAUNCHER_SWIFT_TRIPLE}")
foreach(anomaly_launcher_dep ModManagerCore ModDownloads)
    if(TARGET ${anomaly_launcher_dep})
        target_link_libraries(anomaly_launcher PUBLIC ${anomaly_launcher_dep})
    endif()
endforeach()

# A C++ executable that links Swift code needs the Swift runtime's search paths (the Swift driver would add them itself).
if(ANOMALY_IOS)
    set(anomaly_launcher_sdk "iphoneos")
else()
    set(anomaly_launcher_sdk "macosx")
endif()
execute_process(COMMAND xcrun --sdk ${anomaly_launcher_sdk} --show-sdk-path
    OUTPUT_VARIABLE anomaly_launcher_sdk_path OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND xcrun --find swiftc
    OUTPUT_VARIABLE anomaly_launcher_swiftc OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(anomaly_launcher_toolchain "${anomaly_launcher_swiftc}" DIRECTORY)
get_filename_component(anomaly_launcher_toolchain "${anomaly_launcher_toolchain}/.." ABSOLUTE)
target_link_options(anomaly_launcher INTERFACE
    "-L${anomaly_launcher_sdk_path}/usr/lib/swift"
    "-L${anomaly_launcher_toolchain}/lib/swift/${anomaly_launcher_sdk}"
    "-Wl,-rpath,/usr/lib/swift")
