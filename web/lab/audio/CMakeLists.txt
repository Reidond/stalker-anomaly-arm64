# Item 5 (stretch): the repo's pinned OpenAL Soft built for wasm with its SDL3 backend, plus a small
# test program. Configure with emcmake; see web/lab/build_audio.sh.
cmake_minimum_required(VERSION 3.20)
project(web_lab_audio CXX C)

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(LIBTYPE STATIC CACHE STRING "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_SDL3 ON CACHE BOOL "" FORCE)
set(ALSOFT_REQUIRE_SDL3 ON CACHE BOOL "" FORCE)
find_package(SDL3 REQUIRED CONFIG)
# build.sh passes a patched copy (openal-soft-emscripten.patch); the submodule itself is untouched.
set(OPENAL_SOURCE_DIR "${REPO_ROOT}/external/openal-soft" CACHE PATH "OpenAL Soft sources")
add_subdirectory("${OPENAL_SOURCE_DIR}" openal-soft EXCLUDE_FROM_ALL SYSTEM)
# Same as cmake/Dependencies.cmake: 1.25.2 adds -Werror=function-effects, which newer clang trips.
target_compile_options(OpenAL PRIVATE -Wno-error=function-effects -w)

add_executable(audio_lab audio_lab.cpp)
target_link_libraries(audio_lab PRIVATE OpenAL SDL3::SDL3-static)
target_compile_features(audio_lab PRIVATE cxx_std_20)
target_link_options(audio_lab PRIVATE -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=4 -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB -sEXPORTED_FUNCTIONS=_main,_audio_command -sEXPORTED_RUNTIME_METHODS=HEAPU8 -sENVIRONMENT=web,worker)
set_target_properties(audio_lab PROPERTIES SUFFIX ".js")
