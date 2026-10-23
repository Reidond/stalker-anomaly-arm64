# ModDownloads: the in-app browser (WKWebView) and the link downloader of the mod manager. Swift, SwiftUI + WebKit,
# static library for iOS 17 and macOS 14, linked by the launcher. Documented in docs/ios-port/MOD-MANAGER.md ("Downloads").
#
#   mod_downloads_test        macOS: LinkDownloader against a local HTTP server, the download gate, file naming, bookmarks
#   mod_downloads_snapshot    macOS: renders DownloadsView offscreen with ImageRenderer to a PNG
#   mod_downloads_lab         iPad: DownloadsView on its own, with scripted browser flows (--selftest)
# CMP0157 NEW: the Swift_COMPILATION_MODE target property decides -incremental / -wmo (the root requires only CMake 3.28, where the old
# behaviour smashes -incremental into the Debug flags)
cmake_policy(SET CMP0157 NEW)
enable_language(Swift)

# The sanitizer flags (-fsanitize=..., cmake/AnomalyPlatform.cmake) are directory-wide compile and link options and swiftc rejects them.
# The Swift targets drop what they inherited; the engine's sanitizers have nothing to say about this code anyway.
function(moddownloads_swift_target target)
    set_property(TARGET ${target} PROPERTY COMPILE_OPTIONS "")
    set_property(TARGET ${target} PROPERTY LINK_OPTIONS "")
endfunction()

file(GLOB anomaly_moddownloads_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/Platform/ModDownloads/*.swift")
add_library(ModDownloads STATIC ${anomaly_moddownloads_sources})
moddownloads_swift_target(ModDownloads)
# whole-module: the driver's -incremental mode (the Debug default) intermittently fails to link with `cannotResolveTempPath`
set_target_properties(ModDownloads PROPERTIES Swift_LANGUAGE_VERSION 6 Swift_COMPILATION_MODE wholemodule XCODE_ATTRIBUTE_SWIFT_VERSION 6.0)
target_link_libraries(ModDownloads PUBLIC "-framework WebKit" "-framework SwiftUI" "-framework Foundation")
# the tools below use `@testable import ModDownloads`; the iPad lab builds the Debug configuration
if(ANOMALY_IOS)
    # The Xcode generator does not hand a static Swift library's .swiftmodule (in the library's products folder) to the targets
    # that link it, whose products folders are their own subdirectories
    target_compile_options(ModDownloads INTERFACE "SHELL:-I $<TARGET_FILE_DIR:ModDownloads>")
    target_compile_options(ModDownloads PRIVATE "$<$<CONFIG:Debug>:-enable-testing>")
    add_subdirectory(tools/mod_downloads_lab)
else()
    target_compile_options(ModDownloads PRIVATE -enable-testing)
    add_subdirectory(tools/mod_downloads_test)
    add_subdirectory(tools/mod_downloads_snapshot)
endif()
