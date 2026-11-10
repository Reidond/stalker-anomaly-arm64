# Tint (Dawn's shader compiler): SPIR-V reader, WGSL writer and WGSL reader/validator, for the WGSL back end of
# src/Platform/ShaderXlat (ANOMALY_SHADERXLAT_WGSL, off by default). Only Tint and what it needs are built: every Dawn
# backend, sample, test and command-line tool is off, and the subdirectory is EXCLUDE_FROM_ALL, so nothing is compiled
# unless a target links it.
#
# Pin: external/dawn at the revision Chrome 157 stable ships (157.0.8103.51, Dawn branch chromium/8103), commit
# 3c24c91e70eaf701ae92ab8cc414669cc4c38892. Its WGSL validator is the one the browser runs on createShaderModule.
#
# Dawn's own dependencies are nested submodules; Tint needs three of them:
#   git submodule update --init --depth 1 external/dawn
#   git -C external/dawn submodule update --init --depth 1 \
#       third_party/abseil-cpp third_party/spirv-headers/src third_party/spirv-tools/src

set(ANOMALY_DAWN_DIR "${PROJECT_SOURCE_DIR}/external/dawn")
foreach(sub third_party/abseil-cpp/CMakeLists.txt third_party/spirv-headers/src/CMakeLists.txt
            third_party/spirv-tools/src/CMakeLists.txt src/tint/CMakeLists.txt)
    if(NOT EXISTS "${ANOMALY_DAWN_DIR}/${sub}")
        message(FATAL_ERROR "ANOMALY_SHADERXLAT_WGSL needs external/dawn and three of its submodules (missing ${sub}); see "
                            "cmake/deps/Tint.cmake for the commands")
    endif()
endforeach()

# No Dawn backends, no wire, no samples, tests, benchmarks, fuzzers or protobuf.
foreach(backend D3D11 D3D12 METAL NULL WEBGPU_ON_WEBGPU DESKTOP_GL OPENGLES VULKAN SWIFTSHADER)
    set(DAWN_ENABLE_${backend} OFF CACHE BOOL "" FORCE)
endforeach()
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_NODE_BINDINGS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_MONOLITHIC_LIBRARY OFF CACHE STRING "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_USE_X11 OFF CACHE BOOL "" FORCE)
set(DAWN_USE_WAYLAND OFF CACHE BOOL "" FORCE)
set(DAWN_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(DAWN_WERROR OFF CACHE BOOL "" FORCE)

# Tint: SPIR-V in, WGSL out, WGSL parsed back for validation. Every other reader and writer is off.
set(TINT_BUILD_SPV_READER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_READER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_WRITER ON CACHE BOOL "" FORCE)
foreach(off GLSL_WRITER GLSL_VALIDATOR HLSL_WRITER MSL_WRITER SPV_WRITER NULL_WRITER IR_BINARY CMD_TOOLS TESTS BENCHMARKS
            FUZZERS MESA)
    set(TINT_BUILD_${off} OFF CACHE BOOL "" FORCE)
endforeach()
set(TINT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

# Abseil and SPIRV-Tools build their own tests unless told otherwise.
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)

add_subdirectory("${ANOMALY_DAWN_DIR}" "${CMAKE_BINARY_DIR}/external/dawn" EXCLUDE_FROM_ALL SYSTEM)

# One interface target for ShaderXlat's WGSL back end: the SPIR-V reader, the WGSL writer, the WGSL reader (whose
# resolver is the validator) and the inspector (resource bindings, sampler/texture pairs).
add_library(anomaly_tint INTERFACE)
target_link_libraries(anomaly_tint INTERFACE
    tint_api
    tint_lang_spirv_reader
    tint_lang_wgsl_writer
    tint_lang_wgsl_reader
    tint_lang_wgsl_inspector
    tint_lang_wgsl_program
    tint_lang_core_ir
    dawn_utils_config # DAWN_HAVE_UTILS_BUILD_CONFIG and DAWN_ENABLE_ASSERTS, which Tint's headers check
)
add_library(Anomaly::Tint ALIAS anomaly_tint)
