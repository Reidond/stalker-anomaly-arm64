# LuaJIT 2.1 via its own Makefile. The tree is copied into the build directory because the
# Makefile builds in place and generates src/luajit.h from luajit_rolling.h.
#
# On iOS, lj_arch.h defines LJ_OS_NOJIT unless LUAJIT_ENABLE_JIT is set, so the default build is
# interpreter-only. The StikDebug JIT path (docs/ios-port/PLAN.md) will add LUAJIT_ENABLE_JIT.

include(ExternalProject)

set(luajit_src "${ANOMALY_EXTERNAL_DIR}/LuaJIT")
set(luajit_build "${CMAKE_BINARY_DIR}/external/LuaJIT")
set(luajit_lib "${luajit_build}/src/libluajit.a")

set(luajit_make_args BUILDMODE=static)
if(ANOMALY_IOS)
    execute_process(COMMAND xcrun --sdk iphoneos --show-sdk-path OUTPUT_VARIABLE ios_sdk OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND xcrun --sdk iphoneos --find clang OUTPUT_VARIABLE ios_clang OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    get_filename_component(ios_bin_dir "${ios_clang}" DIRECTORY)
    # The host tools (minilua, buildvm) must be macOS binaries. Xcode exports SDKROOT and
    # IPHONEOS_DEPLOYMENT_TARGET, which would make the host clang build them for iOS instead.
    list(APPEND luajit_make_args
        DEFAULT_CC=clang
        "HOST_CC=xcrun --sdk macosx clang -target arm64-apple-macos14.0"
        "CROSS=${ios_bin_dir}/"
        "TARGET_FLAGS=-arch arm64 -isysroot ${ios_sdk} -miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        TARGET_SYS=iOS)
    set(luajit_env --unset=SDKROOT --unset=IPHONEOS_DEPLOYMENT_TARGET --unset=MACOSX_DEPLOYMENT_TARGET)
else()
    set(luajit_env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# The copied tree has no usable .git, so hand the Makefile the rolling release number
# (commit timestamp) through .relver, which it reads when git is unavailable.
execute_process(COMMAND git -C "${luajit_src}" show -s --format=%ct HEAD
    OUTPUT_VARIABLE luajit_relver OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${CMAKE_BINARY_DIR}/external/luajit.relver" "${luajit_relver}\n")

ExternalProject_Add(luajit_build
    SOURCE_DIR "${luajit_build}"
    DOWNLOAD_COMMAND "${CMAKE_COMMAND}" -E copy_directory "${luajit_src}" "${luajit_build}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${luajit_build}/.git"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/external/luajit.relver" "${luajit_build}/.relver"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND "${CMAKE_COMMAND}" -E env ${luajit_env} make -C src -j8 libluajit.a ${luajit_make_args}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${luajit_lib}"
    LOG_BUILD TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

file(MAKE_DIRECTORY "${luajit_build}/src")
add_library(luajit STATIC IMPORTED GLOBAL)
set_target_properties(luajit PROPERTIES
    IMPORTED_LOCATION "${luajit_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${luajit_build}/src"
)
add_dependencies(luajit luajit_build)
add_library(LuaJIT::luajit ALIAS luajit)
