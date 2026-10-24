# The game: one app that links every engine module statically (xrCore, xrEngine, the renderer, xrGame, ...). The
# Windows engine loads the renderer and the game with LoadLibrary; here EngineAPI.cpp calls DllMainXrRenderR4 and
# DllMainXrGame directly (STATIC_RENDERER_R4) and the factory entry points are plain symbols of xrGame.
#
# Registered from the root CMakeLists.txt, and only when the modules it needs are enabled
# (-DANOMALY_BUILD_ALL_MODULES=ON). Documented in docs/ios-port/INTEGRATION.md.
if(NOT (TARGET xrEngine AND TARGET xrGame AND TARGET xrRender_R4))
    message(STATUS "anomaly app: skipped (needs ANOMALY_BUILD_ALL_MODULES=ON)")
    return()
endif()

include(AnomalyGameApp)

add_executable(anomaly main.cpp Unattended.mm)
target_include_directories(anomaly PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(anomaly PRIVATE xrEngine xrGame xrRender_R4 metal_d3d11 anomaly_apple_shell anomaly_launcher "-framework Foundation" "$<$<NOT:$<BOOL:${ANOMALY_IOS}>>:-framework AppKit>")
anomaly_configure_game_app(anomaly DISPLAY_NAME "Anomaly")
# anomaly_launcher is Swift: CMake would link with swiftc, which does not take the C++ link flags (-fsanitize, ...); the C++ driver does,
# and the Swift runtime's paths come with anomaly_launcher's INTERFACE link options.
set_target_properties(anomaly PROPERTIES LINKER_LANGUAGE CXX)

# The Monolith gamedata overlay (the repository's gamedata/: scripts, configs and shaders the engine build needs next to the
# game data) rides in the bundle; the shell packs it into an archive at startup (src/Platform/Apple/Overlay.cpp).
if(ANOMALY_IOS)
    set(anomaly_resources_dir "$<TARGET_BUNDLE_CONTENT_DIR:anomaly>")
else()
    set(anomaly_resources_dir "$<TARGET_BUNDLE_CONTENT_DIR:anomaly>/Resources")
endif()
file(GLOB_RECURSE anomaly_overlay_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/gamedata/*")
# editing a file in gamedata/ relinks the app (LINK_DEPENDS), which reruns the copy below; CONFIGURE_DEPENDS alone only notices
# a changed list of files
set_property(TARGET anomaly APPEND PROPERTY LINK_DEPENDS ${anomaly_overlay_files})
add_custom_command(TARGET anomaly POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${anomaly_resources_dir}/overlay"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/gamedata" "${anomaly_resources_dir}/overlay"
    COMMENT "anomaly: copying the Monolith gamedata overlay into the bundle")

# The launcher's logo: the generated app icon (cmake/AnomalyAppIcon.cmake: the Anomaly launcher's logo when the game data was there at
# configure time, the repository's own fallback artwork otherwise), as launcher-logo.png in the bundle's resources. Never committed.
get_property(anomaly_app_icon_dir GLOBAL PROPERTY ANOMALY_APP_ICON_DIR)
if(anomaly_app_icon_dir AND EXISTS "${anomaly_app_icon_dir}/icon-1024.png")
    add_custom_command(TARGET anomaly POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${anomaly_app_icon_dir}/icon-1024.png" "${anomaly_resources_dir}/launcher-logo.png"
        COMMENT "anomaly: copying the launcher logo into the bundle")
endif()
