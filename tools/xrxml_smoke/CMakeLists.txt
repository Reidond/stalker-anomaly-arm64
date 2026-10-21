# Headless xrXMLParser check: parses XML files out of the real game archives through xrCore's file system.
# Registered from cmake/targets/xrXMLParser.cmake when ANOMALY_BUILD_XRXMLPARSER is on.
add_executable(xrxml_smoke main.cpp)
target_link_libraries(xrxml_smoke PRIVATE Anomaly::xrXMLParser)

# The game data is gitignored and lives next to the main checkout, which is not necessarily the checkout (or git
# worktree) being built; same lookup as tools/xrcore_smoke. The tool also takes --game-dir and $ANOMALY_GAME_DIR.
set(XRXML_SMOKE_GAME_DIR "" CACHE PATH "Anomaly game directory (contains fsgame.ltx and db/) for xrxml_smoke")
if(NOT XRXML_SMOKE_GAME_DIR)
    foreach(candidate
            "${PROJECT_SOURCE_DIR}/game/Anomaly"
            "${PROJECT_SOURCE_DIR}/../../../game/Anomaly") # <repo>/.claude/worktrees/<name>
        if(EXISTS "${candidate}/fsgame.ltx")
            get_filename_component(XRXML_SMOKE_GAME_DIR "${candidate}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()

target_compile_definitions(xrxml_smoke PRIVATE
    XRXML_SMOKE_DEFAULT_GAME_DIR="${XRXML_SMOKE_GAME_DIR}"
    # everything the tool writes (generated fsgame.ltx, appdata, logs) goes here, never into the game directory
    XRXML_SMOKE_RUN_DIR="${CMAKE_CURRENT_BINARY_DIR}/run"
)

# Without game data the test is skipped (exit code 77), not failed
add_test(NAME xrxml_smoke COMMAND xrxml_smoke)
set_tests_properties(xrxml_smoke PROPERTIES SKIP_RETURN_CODE 77)
