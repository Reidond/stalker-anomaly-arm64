# Headless check of xrEngine's SDL3 input and Theora decoding on macOS, without the engine around them.
#
# The tool compiles the real sources (Xr_input.cpp + xr_input_sdl.cpp + IInputReceiver.cpp, line_edit_control.cpp,
# xrTheora_Stream.cpp) into its own translation units, with test_env.h standing in for stdafx.h (a stub Device and
# Engine). Events go in through SDL_PushEvent and a virtual gamepad; what comes out is what a receiver sees.
if(ANOMALY_IOS)
    return() # a console tool
endif()

add_executable(xrinput_smoke main.cpp input_unit.cpp theora_unit.cpp)
target_link_libraries(xrinput_smoke PRIVATE Anomaly::EngineConfig gameinput_sdl SDL3::SDL3-static Theora::theora)
# the SDL scancode table is a source of the engine target; the tool compiles it itself
target_sources(xrinput_smoke PRIVATE "${ANOMALY_SRC_DIR}/xrEngine/xr_input_keymap.cpp")

# Same lookup as xrcore_smoke: the game data is gitignored and lives next to the main checkout.
set(XRINPUT_SMOKE_GAME_DIR "" CACHE PATH "Anomaly game directory (contains fsgame.ltx and db/) for xrinput_smoke")
if(NOT XRINPUT_SMOKE_GAME_DIR)
    foreach(candidate
            "${PROJECT_SOURCE_DIR}/game/Anomaly"
            "${PROJECT_SOURCE_DIR}/../../../game/Anomaly") # <repo>/.claude/worktrees/<name>
        if(EXISTS "${candidate}/fsgame.ltx")
            get_filename_component(XRINPUT_SMOKE_GAME_DIR "${candidate}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()
target_compile_definitions(xrinput_smoke PRIVATE
    XRINPUT_SMOKE_DEFAULT_GAME_DIR="${XRINPUT_SMOKE_GAME_DIR}"
    XRINPUT_SMOKE_RUN_DIR="${CMAKE_CURRENT_BINARY_DIR}/run"
)

add_test(NAME xrinput_smoke COMMAND xrinput_smoke)
