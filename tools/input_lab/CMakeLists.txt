# Input lab: SDL3 app that shows the controller layer working, live, on macOS and iPadOS.
add_executable(input_lab main.cpp)
target_link_libraries(input_lab PRIVATE gameinput_sdl gameinput_selftest SDL3::SDL3-static)

# No memory entitlements: the lab is tiny, and a smaller entitlement set signs more reliably with a
# free Personal Team.
anomaly_configure_app(input_lab
    BUNDLE_SUFFIX ".inputlab"
    DISPLAY_NAME "Anomaly Input Lab"
    INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-${ANOMALY_PLATFORM_NAME}.plist.in"
)
