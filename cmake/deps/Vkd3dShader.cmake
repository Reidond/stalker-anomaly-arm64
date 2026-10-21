# vkd3d-shader: the HLSL (SM1-5) compiler behind the shader translator (src/Platform/ShaderXlat).
# Pure C, builds unchanged for macOS and iOS. Licensed LGPL-2.1: see docs/ios-port/SHADERS.md.
#
# Only libvkd3d-shader and its helpers from libvkd3d-common are built; the Vulkan/D3D12 runtime in
# the same repository is not needed. Lexer/parser/header generation (bison, flex, perl, widl) is
# done ahead of time, see cmake/deps/vkd3d-generated/.
set(VKD3D_DIR "${ANOMALY_EXTERNAL_DIR}/vkd3d")
set(VKD3D_GENERATED "${PROJECT_SOURCE_DIR}/cmake/deps/vkd3d-generated")

# vkd3d includes <spirv/unified1/spirv.h>; SPIRV-Cross carries the same header at its top level.
set(VKD3D_SPIRV_SHIM "${CMAKE_BINARY_DIR}/external/vkd3d-spirv-shim")
file(WRITE "${VKD3D_SPIRV_SHIM}/spirv/unified1/spirv.h" "#include \"${ANOMALY_EXTERNAL_DIR}/SPIRV-Cross/spirv.h\"\n")
file(WRITE "${VKD3D_SPIRV_SHIM}/spirv/unified1/GLSL.std.450.h" "#include \"${ANOMALY_EXTERNAL_DIR}/SPIRV-Cross/GLSL.std.450.h\"\n")

add_library(vkd3d-shader STATIC
    "${VKD3D_DIR}/libs/vkd3d-common/debug.c"
    "${VKD3D_DIR}/libs/vkd3d-common/error.c"
    "${VKD3D_DIR}/libs/vkd3d-common/memory.c"
    "${VKD3D_DIR}/libs/vkd3d-common/utf8.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/checksum.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/d3d_asm.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/d3dbc.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/dxbc.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/dxil.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/fx.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/glsl.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/hlsl.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/hlsl_constant_ops.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/ir.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/msl.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/spirv.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/tpf.c"
    "${VKD3D_DIR}/libs/vkd3d-shader/vkd3d_shader_main.c"
    "${VKD3D_GENERATED}/hlsl.tab.c"
    "${VKD3D_GENERATED}/hlsl.yy.c"
    "${VKD3D_GENERATED}/preproc.tab.c"
    "${VKD3D_GENERATED}/preproc.yy.c"
)
set_target_properties(vkd3d-shader PROPERTIES C_STANDARD 11 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(vkd3d-shader PRIVATE LIBVKD3D_SHADER_SOURCE VKD3D_DEBUG_ENV=VKD3D_SHADER_DEBUG)
target_include_directories(vkd3d-shader
    PUBLIC "${VKD3D_DIR}/include"
    PRIVATE
        "${VKD3D_GENERATED}" "${VKD3D_GENERATED}/private"
        "${VKD3D_DIR}/include/private" "${VKD3D_DIR}/libs/vkd3d-shader"
        "${VKD3D_SPIRV_SHIM}")
# Third-party code: keep its warnings out of the build log.
target_compile_options(vkd3d-shader PRIVATE -w)
