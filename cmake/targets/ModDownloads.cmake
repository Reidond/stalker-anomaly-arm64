# ModDownloads: the in-app browser (WKWebView) and the link downloader of the mod manager. Swift, SwiftUI + WebKit,
# static library for iOS 17 and macOS 14, linked by the launcher. Documented in docs/ios-port/MOD-MANAGER.md ("Downloads").
#
#   mod_downloads_test        macOS: LinkDownloader against a local HTTP server, the download gate, file naming, bookmarks
#   mod_downloads_snapshot    macOS: renders DownloadsView offscreen with ImageRenderer to a PNG
enable_language(Swift)

file(GLOB anomaly_moddownloads_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/Platform/ModDownloads/*.swift")
add_library(ModDownloads STATIC ${anomaly_moddownloads_sources})
set_target_properties(ModDownloads PROPERTIES Swift_LANGUAGE_VERSION 6)
target_link_libraries(ModDownloads PUBLIC "-framework WebKit" "-framework SwiftUI" "-framework Foundation")
if(NOT ANOMALY_IOS)
    # the host tools below use `@testable import ModDownloads`
    target_compile_options(ModDownloads PRIVATE -enable-testing)
    add_subdirectory(tools/mod_downloads_test)
endif()
