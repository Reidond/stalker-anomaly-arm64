# Lab app (iPad): DownloadsView on its own, plus `--selftest <github|moddb|local>`, a scripted run of the browser flows that
# writes Documents/lab-report.txt. Not part of the game. Build the Debug configuration: it needs `@testable import ModDownloads`.
#   xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme mod_downloads_lab -configuration Debug -destination 'generic/platform=iOS' -allowProvisioningUpdates build
add_executable(mod_downloads_lab LabApp.swift SelfTest.swift)
moddownloads_swift_target(mod_downloads_lab)
set_target_properties(mod_downloads_lab PROPERTIES Swift_LANGUAGE_VERSION 5 Swift_COMPILATION_MODE wholemodule XCODE_ATTRIBUTE_SWIFT_VERSION 5.0)
target_link_libraries(mod_downloads_lab PRIVATE ModDownloads)
# A free Personal Team cannot register a new App ID from the command line (xcodebuild: "No Accounts"), so the lab takes the id of
# an existing lab; install it, run it, uninstall it. Override with -DANOMALY_DOWNLOADS_LAB_SUFFIX=.dllab where Xcode can register one.
set(ANOMALY_DOWNLOADS_LAB_SUFFIX ".probe" CACHE STRING "Bundle id suffix of mod_downloads_lab")
anomaly_configure_app(mod_downloads_lab
    BUNDLE_SUFFIX "${ANOMALY_DOWNLOADS_LAB_SUFFIX}"
    DISPLAY_NAME "Anomaly Downloads Lab"
    INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-ios.plist.in"
)
