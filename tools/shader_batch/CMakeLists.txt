# Batch-translates every R4 shader entry across macro permutations and validates the MSL with xcrun metal.
add_executable(shader_batch main.cpp profiles.cpp)
target_link_libraries(shader_batch PRIVATE ShaderXlat Threads::Threads)
find_package(Threads REQUIRED)
