# LuaJIT 2.1 via its own Makefile. The tree is copied into the build directory because the
# Makefile builds in place and generates src/luajit.h from luajit_rolling.h.
#
# On iOS, lj_arch.h defines LJ_OS_NOJIT unless LUAJIT_ENABLE_JIT is set, so the default build is
# interpreter-only. With ANOMALY_IOS_LUAJIT_JIT the build copy is patched (cmake/deps/patches/luajit)
# to run its JIT from a debugger-authorized, dual-mapped code region on TXM devices; the app must
# call luajit_txm_init() first and the JIT stays off if the region is not authorized.
# See docs/ios-port/IPAD-JIT.md.

include(ExternalProject)

set(luajit_src "${ANOMALY_EXTERNAL_DIR}/LuaJIT")
set(luajit_build "${CMAKE_BINARY_DIR}/external/LuaJIT")
set(luajit_lib "${luajit_build}/src/libluajit.a")

# JIT on the iPad (TXM): patched LuaJIT, still interpreter-only at runtime unless a debugger
# authorizes the code region. Off by default.
option(ANOMALY_IOS_LUAJIT_JIT "Build LuaJIT with the debugger-authorized JIT region for TXM iPads" OFF)
# Run the same patched code path on macOS (no debugger, no brk) to test the dual-mapped writer.
option(ANOMALY_LUAJIT_TXM_EMULATE "Build LuaJIT with the dual-mapped JIT region emulated on macOS (testing)" OFF)
mark_as_advanced(ANOMALY_LUAJIT_TXM_EMULATE)

set(luajit_make_args BUILDMODE=static)
set(luajit_patches "")
set(luajit_variant "stock")
if((ANOMALY_IOS AND ANOMALY_IOS_LUAJIT_JIT) OR (NOT ANOMALY_IOS AND ANOMALY_LUAJIT_TXM_EMULATE))
    file(GLOB luajit_patches CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/patches/luajit/*.patch")
    list(SORT luajit_patches)
    find_program(ANOMALY_PATCH_EXECUTABLE patch REQUIRED)
    if(ANOMALY_IOS)
        set(luajit_variant "ios-txm-jit")
        list(APPEND luajit_make_args "XCFLAGS=-DLUAJIT_ENABLE_JIT -DLUAJIT_TXM_DUALMAP")
    else()
        set(luajit_variant "macos-txm-emulate")
        # No external unwinding: the dual-mapped writer does not support unwind info in mcode (same as iOS).
        list(APPEND luajit_make_args "XCFLAGS=-DLUAJIT_TXM_DUALMAP -DLUAJIT_TXM_EMULATE -DLUAJIT_UNWIND_INTERNAL")
    endif()
endif()
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

# The build copy is made again (from the pristine submodule) whenever the variant or a patch changes.
set(luajit_variant_file "${CMAKE_BINARY_DIR}/external/luajit.variant")
set(luajit_variant_text "${luajit_variant}\n")
if(EXISTS "${luajit_variant_file}")
    file(READ "${luajit_variant_file}" luajit_variant_old)
else()
    set(luajit_variant_old "")
endif()
if(NOT luajit_variant_old STREQUAL luajit_variant_text)
    file(WRITE "${luajit_variant_file}" "${luajit_variant_text}")
endif()

set(luajit_patch_commands "")
foreach(patch_file IN LISTS luajit_patches)
    list(APPEND luajit_patch_commands
        COMMAND "${ANOMALY_PATCH_EXECUTABLE}" -p1 -N --no-backup-if-mismatch -s -d "${luajit_build}" -i "${patch_file}")
endforeach()

ExternalProject_Add(luajit_build
    SOURCE_DIR "${luajit_build}"
    DOWNLOAD_COMMAND "${CMAKE_COMMAND}" -E rm -rf "${luajit_build}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${luajit_src}" "${luajit_build}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${luajit_build}/.git"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/external/luajit.relver" "${luajit_build}/.relver"
        ${luajit_patch_commands}
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND "${CMAKE_COMMAND}" -E env ${luajit_env} make -C src -j8 libluajit.a ${luajit_make_args}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${luajit_lib}"
    LOG_BUILD TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

ExternalProject_Add_StepDependencies(luajit_build download "${luajit_variant_file}" ${luajit_patches})

file(MAKE_DIRECTORY "${luajit_build}/src")
add_library(luajit STATIC IMPORTED GLOBAL)
set_target_properties(luajit PROPERTIES
    IMPORTED_LOCATION "${luajit_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${luajit_build}/src"
)
add_dependencies(luajit luajit_build)
add_library(LuaJIT::luajit ALIAS luajit)
