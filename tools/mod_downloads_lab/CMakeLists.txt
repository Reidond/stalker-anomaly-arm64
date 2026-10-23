# Lab app (iPad): DownloadsView on its own, plus `--selftest <github|moddb|local>`, a scripted run of the browser flows that
# writes Documents/lab-report.txt. Not part of the game. Build the Debug configuration: it needs `@testable import ModDownloads`.
#   xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme mod_downloads_lab -configuration Debug -destination 'generic/platform=iOS' -allowProvisioningUpdates build
add_executable(mod_downloads_lab LabApp.swift SelfTest.swift)
set_target_properties(mod_downloads_lab PROPERTIES Swift_LANGUAGE_VERSION 5 Swift_COMPILATION_MODE wholemodule)
target_link_libraries(mod_downloads_lab PRIVATE ModDownloads)
anomaly_configure_app(mod_downloads_lab
    BUNDLE_SUFFIX ".dllab"
    DISPLAY_NAME "Anomaly Downloads Lab"
    INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-ios.plist.in"
)
