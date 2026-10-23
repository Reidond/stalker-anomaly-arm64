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
