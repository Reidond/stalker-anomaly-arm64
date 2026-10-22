# Host test of the device graphics profiles (src/Platform/Apple/GraphicsProfile.cpp has no engine or platform dependency).
#   graphics_profile_test            checks the tables and the lock rules
#   graphics_profile_test --dump     prints every profile's resolved settings as markdown (docs/ios-port/SETTINGS.md)
if(NOT ANOMALY_IOS)
    add_executable(graphics_profile_test main.cpp "${PROJECT_SOURCE_DIR}/src/Platform/Apple/GraphicsProfile.cpp")
    target_include_directories(graphics_profile_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(graphics_profile_test PRIVATE cxx_std_17)
    add_test(NAME graphics_profile_test COMMAND graphics_profile_test)
endif()
