# Controller layer for the Apple port.
#
#   gameinput          engine-independent core (bindings, sticks, gyro, cursor, radial, feedback model)
#   gameinput_sdl      SDL3 backend + native GameController adaptive triggers (Objective-C++)
#   gameinput_selftest assertions over the core; run by gameinput_tests on macOS and by input_lab --selftest
#
# Nothing here includes engine headers; the engine reaches it through GameInput/Adapter.h.

add_library(gameinput STATIC
    src/Adapter.cpp
    src/Bindings.cpp
    src/Config.cpp
    src/Cursor.cpp
    src/Device.cpp
    src/Feedback.cpp
    src/Filters.cpp
    src/GameInput.cpp
    src/Gyro.cpp
    src/PadKeys.cpp
    src/Profile.cpp
    src/Radial.cpp
)
target_include_directories(gameinput PUBLIC include)
target_compile_features(gameinput PUBLIC cxx_std_17)
target_compile_options(gameinput PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(gameinput_sdl STATIC
    src/DualSenseTriggers.mm
    src/SdlPadSource.cpp
)
target_link_libraries(gameinput_sdl PUBLIC gameinput SDL3::SDL3-static)
target_link_libraries(gameinput_sdl PUBLIC "-framework Foundation" "-framework GameController")
target_compile_options(gameinput_sdl PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(gameinput_selftest STATIC tests/SelfTest.cpp)
target_link_libraries(gameinput_selftest PUBLIC gameinput)
target_include_directories(gameinput_selftest PUBLIC tests)

if(NOT ANOMALY_IOS)
    enable_testing()
    add_executable(gameinput_tests tests/main.cpp)
    target_link_libraries(gameinput_tests PRIVATE gameinput_selftest)
    add_test(NAME gameinput COMMAND gameinput_tests)
endif()
