# The Apple app shell: paths, game-data validation, lifecycle, logging/crash capture, launcher.
include(AnomalyGameApp)

add_library(anomaly_apple_shell STATIC
    DeviceInfo.mm
    GameData.cpp
    GameDataManifest.inc
    Launcher.cpp
    Launcher.h
    Lifecycle.mm
    Overlay.cpp
    Shell.h
    Shell.mm
    ShellInternal.h
    ShellLog.cpp
)
# Engine code includes the public headers as <Platform/Apple/Shell.h>.
target_include_directories(anomaly_apple_shell PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_options(anomaly_apple_shell PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(DeviceInfo.mm Lifecycle.mm Shell.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
set_source_files_properties(GameDataManifest.inc PROPERTIES HEADER_FILE_ONLY TRUE)
target_link_libraries(anomaly_apple_shell PUBLIC SDL3::SDL3-static "-framework Foundation")
if(ANOMALY_IOS)
    target_link_libraries(anomaly_apple_shell PUBLIC "-framework UIKit")
else()
    target_link_libraries(anomaly_apple_shell PUBLIC "-framework AppKit")
endif()
