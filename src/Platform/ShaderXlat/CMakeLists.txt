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
