# Headless xrSound check: initializes xrCore against the real game data, brings up the OpenAL device, loads an .ogg
# from the archives and plays it 2D and 3D. Added from cmake/targets/xrSound.cmake, so it exists only when the module
# is enabled. docs/ios-port/SOUND.md.
add_executable(xrsound_smoke main.cpp)
target_link_libraries(xrsound_smoke PRIVATE Anomaly::xrSound)

# xrSound needs the collision database (occlusion geometry) and the console from xrEngine. The console is stubbed in
# main.cpp; the collision database comes from xrCDB once that module is built, from cdb_stubs.cpp until then.
if(NOT TARGET xrCDB)
    target_sources(xrsound_smoke PRIVATE cdb_stubs.cpp)
endif()

# The game data is gitignored and lives next to the main checkout, which is not necessarily the checkout (or git
# worktree) being built. Take the first candidate that exists; the tool also accepts --game-dir and $ANOMALY_GAME_DIR.
set(XRSOUND_SMOKE_GAME_DIR "" CACHE PATH "Anomaly game directory (contains fsgame.ltx and db/) for xrsound_smoke")
if(NOT XRSOUND_SMOKE_GAME_DIR)
    foreach(candidate
            "${PROJECT_SOURCE_DIR}/game/Anomaly"
            "${PROJECT_SOURCE_DIR}/../../../game/Anomaly") # <repo>/.claude/worktrees/<name>
        if(EXISTS "${candidate}/fsgame.ltx")
            get_filename_component(XRSOUND_SMOKE_GAME_DIR "${candidate}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()

target_compile_definitions(xrsound_smoke PRIVATE
    XRSOUND_SMOKE_DEFAULT_GAME_DIR="${XRSOUND_SMOKE_GAME_DIR}"
    # everything the tool writes (generated fsgame.ltx, appdata, logs) goes here, never into the game directory
    XRSOUND_SMOKE_RUN_DIR="${CMAKE_CURRENT_BINARY_DIR}/run"
)

if(ANOMALY_IOS)
    # On the iPad the game archives are not available. The app bundles one sound (written by the macOS run with
    # `xrsound_smoke --dump-ogg <file>`) and lays out a one-file game directory around it in its container.
    set(XRSOUND_SMOKE_BUNDLE_OGG "${CMAKE_CURRENT_BINARY_DIR}/sample.ogg" CACHE FILEPATH "Ogg file bundled into the iOS app")
    target_link_libraries(xrsound_smoke PRIVATE SDL3::SDL3-static)
    if(EXISTS "${XRSOUND_SMOKE_BUNDLE_OGG}")
        target_sources(xrsound_smoke PRIVATE "${XRSOUND_SMOKE_BUNDLE_OGG}")
        set_source_files_properties("${XRSOUND_SMOKE_BUNDLE_OGG}" PROPERTIES MACOSX_PACKAGE_LOCATION "")
        target_compile_definitions(xrsound_smoke PRIVATE XRSOUND_SMOKE_BUNDLED_OGG)
    endif()
    anomaly_configure_app(xrsound_smoke
        BUNDLE_SUFFIX ".soundlab"
        DISPLAY_NAME "Anomaly Sound Lab"
        INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-ios.plist.in"
    )
endif()
