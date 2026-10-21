add_executable(platform_probe main.cpp)
target_link_libraries(platform_probe PRIVATE SDL3::SDL3-static OpenAL::OpenAL LuaJIT::luajit)
target_compile_definitions(platform_probe PRIVATE AL_LIBTYPE_STATIC)

anomaly_configure_app(platform_probe
    BUNDLE_SUFFIX ".probe"
    DISPLAY_NAME "Anomaly Probe"
    INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-${ANOMALY_PLATFORM_NAME}.plist.in"
    ENTITLEMENTS "${PROJECT_SOURCE_DIR}/platform/ios/Anomaly.entitlements"
)
