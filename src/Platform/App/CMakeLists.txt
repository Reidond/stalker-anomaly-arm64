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

add_executable(anomaly main.cpp)
target_include_directories(anomaly PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(anomaly PRIVATE xrEngine xrGame xrRender_R4 anomaly_apple_shell)
anomaly_configure_game_app(anomaly DISPLAY_NAME "Anomaly")
