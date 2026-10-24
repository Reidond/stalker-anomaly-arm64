# The SwiftUI launcher: the app's first screen (home, mods, add mod). A static library of Swift files; the app shell reaches it through
# the C functions of AnomalyLauncher.h. Documented in docs/ios-port/MOD-MANAGER.md.
# The launcher needs the mod library and importer (ModManagerCore, an opt-in module: ANOMALY_BUILD_MODMANAGERCORE or
# ANOMALY_BUILD_ALL_MODULES) and the browser and downloader (ModDownloads, always built).
if(NOT (TARGET ModManagerCore AND TARGET ModDownloads))
    message(STATUS "launcher: skipped (needs ANOMALY_BUILD_ALL_MODULES=ON or ANOMALY_BUILD_MODMANAGERCORE=ON)")
    return()
endif()

file(GLOB anomaly_launcher_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.swift")
set(ANOMALY_LAUNCHER_SOURCES ${anomaly_launcher_sources} CACHE INTERNAL "SwiftUI launcher sources (also compiled into tools/launcher_snapshot)")

# CMake does not pass the deployment target to swiftc with the Ninja generator, which would then build for the SDK's newest OS (the Xcode
# generator passes it itself, and a second -target there only draws a warning).
set(ANOMALY_LAUNCHER_SWIFT_TARGET_OPTION "" CACHE INTERNAL "swiftc -target of the launcher (Ninja)")
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    if(ANOMALY_IOS)
        set(ANOMALY_LAUNCHER_SWIFT_TARGET_OPTION "SHELL:-target arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE INTERNAL "" FORCE)
    else()
        set(ANOMALY_LAUNCHER_SWIFT_TARGET_OPTION "SHELL:-target arm64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE INTERNAL "" FORCE)
    endif()
endif()

add_library(anomaly_launcher STATIC ${anomaly_launcher_sources} AnomalyLauncher.h)
# The sanitizer flags (-fsanitize=..., cmake/AnomalyPlatform.cmake) are directory-wide options and swiftc rejects them: this target drops
# what it inherited. Whole-module: the driver's -incremental mode intermittently fails to link with `cannotResolveTempPath`.
set_property(TARGET anomaly_launcher PROPERTY COMPILE_OPTIONS "")
set_property(TARGET anomaly_launcher PROPERTY LINK_OPTIONS "")
set_target_properties(anomaly_launcher PROPERTIES
    Swift_LANGUAGE_VERSION 5
    Swift_COMPILATION_MODE wholemodule
    Swift_MODULE_NAME AnomalyLauncher)
if(ANOMALY_LAUNCHER_SWIFT_TARGET_OPTION)
    target_compile_options(anomaly_launcher PRIVATE "${ANOMALY_LAUNCHER_SWIFT_TARGET_OPTION}")
endif()
target_link_libraries(anomaly_launcher PUBLIC ModManagerCore ModDownloads)
if(ANOMALY_IOS)
    # The Xcode generator does not hand a static Swift library's .swiftmodule (in the library's products folder) to the targets that
    # link it, whose products folders are their own subdirectories
    target_compile_options(anomaly_launcher PRIVATE "SHELL:-I $<TARGET_FILE_DIR:ModManagerCore>")
endif()

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
