# Renders DownloadsView offscreen (SwiftUI ImageRenderer) to a PNG: no window is created.
#   mod_downloads_snapshot --out view.png [--width 1180 --height 760] [--dark] [--narrow]
add_executable(mod_downloads_snapshot main.swift)
moddownloads_swift_target(mod_downloads_snapshot)
set_target_properties(mod_downloads_snapshot PROPERTIES Swift_LANGUAGE_VERSION 5 Swift_COMPILATION_MODE wholemodule)
target_link_libraries(mod_downloads_snapshot PRIVATE ModDownloads)
