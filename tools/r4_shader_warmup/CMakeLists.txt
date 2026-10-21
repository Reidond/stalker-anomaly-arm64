# r4_shader_warmup: runs every shader the R4 renderer requests at start-up through the D3DCompile path of the Metal
# port (macro list of CRender::shader_compile with default settings -> ShaderXlat -> shader container -> D3DReflect ->
# CreateXShader) and reports pass/fail and timings. See docs/ios-port/RENDERER.md.
if(ANOMALY_IOS)
    return() # a console tool for the Mac
endif()

add_executable(r4_shader_warmup main.cpp r4_macros.cpp)
target_include_directories(r4_shader_warmup PRIVATE "${ANOMALY_SRC_DIR}" "${ANOMALY_SRC_DIR}/Layers/xrRenderPC_R4")
target_link_libraries(r4_shader_warmup PRIVATE Anomaly::EngineConfig ShaderXlatMetal Threads::Threads)
find_package(Threads REQUIRED)

# game data lookup, like xrcore_smoke
set(R4_WARMUP_GAME_DIR "" CACHE PATH "Anomaly game directory (contains fsgame.ltx and db/) for r4_shader_warmup")
if(NOT R4_WARMUP_GAME_DIR)
    foreach(candidate
            "${PROJECT_SOURCE_DIR}/game/Anomaly"
            "${PROJECT_SOURCE_DIR}/../../../game/Anomaly") # <repo>/.claude/worktrees/<name>
        if(EXISTS "${candidate}/fsgame.ltx")
            get_filename_component(R4_WARMUP_GAME_DIR "${candidate}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()
target_compile_definitions(r4_shader_warmup PRIVATE
    R4_WARMUP_DEFAULT_GAME_DIR="${R4_WARMUP_GAME_DIR}"
    R4_WARMUP_RUN_DIR="${CMAKE_CURRENT_BINARY_DIR}/run"
    # engine sources that request shaders (blenders), scanned for r_Pass("vs", "ps") like the engine does at run time
    R4_WARMUP_SOURCE_DIRS="${ANOMALY_SRC_DIR}/Layers/xrRender|${ANOMALY_SRC_DIR}/Layers/xrRenderDX10|${ANOMALY_SRC_DIR}/Layers/xrRenderPC_R4"
)
