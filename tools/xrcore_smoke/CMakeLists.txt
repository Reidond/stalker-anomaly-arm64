# Headless xrCore smoke test: mounts the real game archives, reads a config and a texture out of them.
if(ANOMALY_IOS)
    return() # a console tool; on iPadOS xrCore is exercised through the app targets
endif()

add_executable(xrcore_smoke main.cpp)
target_link_libraries(xrcore_smoke PRIVATE Anomaly::xrCore)

# The game data is gitignored and lives next to the main checkout, which is not necessarily the checkout (or git
# worktree) being built. Take the first candidate that exists; the tool also accepts --game-dir and $ANOMALY_GAME_DIR.
set(XRCORE_SMOKE_GAME_DIR "" CACHE PATH "Anomaly game directory (contains fsgame.ltx and db/) for xrcore_smoke")
if(NOT XRCORE_SMOKE_GAME_DIR)
    foreach(candidate
            "${PROJECT_SOURCE_DIR}/game/Anomaly"
            "${PROJECT_SOURCE_DIR}/../../../game/Anomaly") # <repo>/.claude/worktrees/<name>
        if(EXISTS "${candidate}/fsgame.ltx")
            get_filename_component(XRCORE_SMOKE_GAME_DIR "${candidate}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()

# --log-race against the log code before the log lock (log_lines.h) existed: for the "before" run of the TSan comparison
option(XRCORE_SMOKE_OLD_LOG_API "xrcore_smoke --log-race reads LogFile directly (the code before log_lines.h)" OFF)
if(XRCORE_SMOKE_OLD_LOG_API)
    target_compile_definitions(xrcore_smoke PRIVATE XRCORE_SMOKE_OLD_LOG_API=1)
endif()

target_compile_definitions(xrcore_smoke PRIVATE
    XRCORE_SMOKE_DEFAULT_GAME_DIR="${XRCORE_SMOKE_GAME_DIR}"
    # everything the tool writes (generated fsgame.ltx, appdata, logs) goes here, never into the game directory
    XRCORE_SMOKE_RUN_DIR="${CMAKE_CURRENT_BINARY_DIR}/run"
)

# WebAssembly (docs/web-port/WASM-BUILD.md): ctest runs the tool under Node, with NODERAWFS so it reads the real game archives.
if(ANOMALY_WEB)
    # the path layer on folders that differ only in case; needs no game data. The tool creates and removes <dir>/case_test.
    add_test(NAME xrcore_smoke_case_test
             COMMAND xrcore_smoke --case-test "${CMAKE_CURRENT_BINARY_DIR}/case_dir")
    # the real data: mounts every archive, reads configs\system.ltx and a texture through both archive read paths
    if(XRCORE_SMOKE_GAME_DIR)
        add_test(NAME xrcore_smoke_gamedata
                 COMMAND xrcore_smoke --game-dir "${XRCORE_SMOKE_GAME_DIR}" --run-dir "${CMAKE_CURRENT_BINARY_DIR}/run")
    else()
        message(STATUS "xrcore_smoke_gamedata not registered: no game directory (set XRCORE_SMOKE_GAME_DIR)")
    endif()
endif()
