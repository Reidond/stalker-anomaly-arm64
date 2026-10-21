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
