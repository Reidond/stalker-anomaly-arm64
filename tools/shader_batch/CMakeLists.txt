# Batch-translates every R4 shader entry across macro permutations and validates the MSL with xcrun metal.
add_executable(shader_batch main.cpp profiles.cpp)
target_link_libraries(shader_batch PRIVATE ShaderXlat Threads::Threads)
find_package(Threads REQUIRED)

# shader_batch --target wgsl runs this once per job (Tint aborts on an internal error, so each job gets its own process).
# Only with -DANOMALY_SHADERXLAT_WGSL=ON; see docs/web-port/SHADERS-WGSL.md.
if(TARGET ShaderXlatWgsl)
    add_executable(shader_wgsl wgsl_main.cpp)
    target_link_libraries(shader_wgsl PRIVATE ShaderXlatWgsl)
    add_dependencies(shader_batch shader_wgsl)
endif()
