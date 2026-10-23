# ModManagerCore: the mod library, archive import and FOMOD installer (Swift static library, no engine code).
# Owner: mod-core session. Opt-in: -DANOMALY_BUILD_MODMANAGERCORE=ON (or ANOMALY_BUILD_ALL_MODULES).
# Sources: src/Platform/ModManagerCore. Tests: tools/modmanager_test (ctest: modmanager_core).
# Docs: docs/ios-port/MOD-MANAGER.md ("Import and FOMOD").
anomaly_module_begin(ModManagerCore "mod library, archive import and FOMOD installer (Swift)")

enable_language(Swift)
include(deps/LibArchive)

set(modmanager_dir "${PROJECT_SOURCE_DIR}/src/Platform/ModManagerCore")
file(GLOB modmanager_sources CONFIGURE_DEPENDS "${modmanager_dir}/Sources/*.swift")

add_library(ModManagerCore STATIC ${modmanager_sources})
set_target_properties(ModManagerCore PROPERTIES
    Swift_LANGUAGE_VERSION 5
    Swift_MODULE_NAME ModManagerCore
    Swift_MODULE_DIRECTORY "${CMAKE_BINARY_DIR}/swift/ModManagerCore")
# With Ninja, CMake does not pass the deployment target to swiftc, which would then build for the SDK's newest OS (the Xcode
# generator passes it itself, and a second -target there only draws a warning).
set(modmanager_swift_target_option "")
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    if(ANOMALY_IOS)
        set(modmanager_swift_target_option "SHELL:-target arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(modmanager_swift_target_option "SHELL:-target arm64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
endif()
# libarchive's C API reaches Swift through the CArchive module that libarchive ships (libarchive/module.modulemap): the
# compiler needs that directory here and in every target that imports ModManagerCore.
target_include_directories(ModManagerCore PUBLIC
    "${libarchive_dir}/libarchive"
    "${CMAKE_BINARY_DIR}/swift/ModManagerCore")
target_compile_options(ModManagerCore PRIVATE
    "$<$<COMPILE_LANGUAGE:Swift>:-enable-testing>"
    ${modmanager_swift_target_option})
target_link_libraries(ModManagerCore PUBLIC anomaly_libarchive "-framework Foundation" "-framework SwiftUI" "-framework CryptoKit"
    "-framework ImageIO")

# Host test (macOS): fixture archives are made at run time with libarchive's own writers; RAR fixtures are libarchive's
# test files, uuencoded in external/libarchive/libarchive/test.
if(NOT ANOMALY_IOS)
    file(GLOB modmanager_test_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/modmanager_test/*.swift")
    add_executable(modmanager_core_test ${modmanager_test_sources})
    target_link_libraries(modmanager_core_test PRIVATE ModManagerCore)
    set_target_properties(modmanager_core_test PROPERTIES Swift_LANGUAGE_VERSION 5)
    target_compile_options(modmanager_core_test PRIVATE ${modmanager_swift_target_option})
    add_test(NAME modmanager_core COMMAND modmanager_core_test)
    set_tests_properties(modmanager_core PROPERTIES
        ENVIRONMENT "ANOMALY_LIBARCHIVE_TEST_DIR=${libarchive_dir}/libarchive/test"
        TIMEOUT 600)
endif()
