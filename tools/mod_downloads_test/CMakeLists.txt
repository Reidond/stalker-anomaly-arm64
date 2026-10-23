# Host tests of ModDownloads (see cmake/targets/ModDownloads.cmake). No window, no UI: LinkDownloader runs against a
# local HTTP server on 127.0.0.1, and the pure logic (file naming, the download gate, bookmarks, address parsing) is checked directly.
add_executable(mod_downloads_test main.swift TestServer.swift)
set_target_properties(mod_downloads_test PROPERTIES Swift_LANGUAGE_VERSION 5)
target_link_libraries(mod_downloads_test PRIVATE ModDownloads)
add_test(NAME mod_downloads_test COMMAND mod_downloads_test)
