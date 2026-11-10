# ShaderXlat: HLSL -> SPIR-V -> MSL translation library with D3D-style reflection.
add_library(ShaderXlat STATIC
    src/ShaderXlat.cpp
    src/SpirvToMsl.cpp
    src/Rdef.cpp
    src/Cache.cpp
    src/FxcCompat.cpp
    src/FrontendVkd3d.cpp
)
target_include_directories(ShaderXlat PUBLIC include PRIVATE src)
target_compile_features(ShaderXlat PUBLIC cxx_std_17)
target_link_libraries(ShaderXlat PRIVATE spirv-cross-msl spirv-cross-glsl spirv-cross-core)

target_link_libraries(ShaderXlat PUBLIC vkd3d-shader)

add_executable(shaderxlat_selftest tests/selftest.cpp)
target_link_libraries(shaderxlat_selftest PRIVATE ShaderXlat)
add_test(NAME shaderxlat_selftest COMMAND shaderxlat_selftest)

# Bridge to the D3DCompile hook of the Metal D3D11 shim: translates with ShaderXlat and returns the shim's shader
# container (see include/ShaderXlat/MetalBridge.h). Used by the R4 renderer and tools/r4_shader_warmup.
add_library(ShaderXlatMetal STATIC src/MetalBridge.cpp)
target_include_directories(ShaderXlatMetal PUBLIC include)
target_compile_features(ShaderXlatMetal PUBLIC cxx_std_17)
target_link_libraries(ShaderXlatMetal PUBLIC ShaderXlat metal_d3d11)

# WGSL back end for the web build (docs/web-port/SHADERS-WGSL.md): SPIR-V -> WGSL with Tint, validated the way Chrome
# validates it. Off by default; the default build has no Tint. Needs external/dawn and three of its submodules
# (cmake/deps/Tint.cmake).
option(ANOMALY_SHADERXLAT_WGSL "Build ShaderXlat's WGSL back end (Tint from external/dawn)" OFF)
if(ANOMALY_SHADERXLAT_WGSL)
    include(deps/Tint)
    add_library(ShaderXlatWgsl STATIC src/SpirvToWgsl.cpp)
    target_include_directories(ShaderXlatWgsl PUBLIC include PRIVATE src)
    target_compile_features(ShaderXlatWgsl PRIVATE cxx_std_20) # Tint's headers are C++20
    target_link_libraries(ShaderXlatWgsl PUBLIC ShaderXlat PRIVATE Anomaly::Tint)
    execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}/external/dawn" rev-parse --short=12 HEAD
                    OUTPUT_VARIABLE anomaly_dawn_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    target_compile_definitions(ShaderXlatWgsl PRIVATE ANOMALY_DAWN_REVISION="${anomaly_dawn_rev}")

    add_executable(shaderxlat_wgsl_selftest tests/wgsl_selftest.cpp)
    target_link_libraries(shaderxlat_wgsl_selftest PRIVATE ShaderXlatWgsl)
    add_test(NAME shaderxlat_wgsl_selftest COMMAND shaderxlat_wgsl_selftest)
endif()
