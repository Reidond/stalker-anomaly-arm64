# Platform detection and shared compile settings for the Apple port.

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(ANOMALY_IOS ON)
    set(ANOMALY_PLATFORM_NAME "ios")
else()
    set(ANOMALY_IOS OFF)
    set(ANOMALY_PLATFORM_NAME "macos")
endif()

if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "Only arm64 is supported (got '${CMAKE_OSX_ARCHITECTURES}'); use a preset from CMakePresets.json.")
endif()

# Sanitizers (macOS only), applied to every target defined after this file: the engine modules, the third-party libraries
# built from external/, the shell and the `anomaly` app. Usage and the suppression files: docs/ios-port/SANITIZERS.md.
#   cmake --preset macos-arm64-asan            # = -DANOMALY_SANITIZE="address;undefined" on the RelWithDebInfo build
#   cmake --preset macos-arm64-tsan            # = -DANOMALY_SANITIZE="thread" (data races; cannot be combined with the others)
set(ANOMALY_SANITIZE "" CACHE STRING "Sanitizers to build with, ';'-separated: address, undefined, thread (macOS only)")
set(ANOMALY_SANITIZE_COMPILE_FLAGS "")
set(ANOMALY_SANITIZE_LINK_FLAGS "")
if(ANOMALY_SANITIZE)
    if(ANOMALY_IOS)
        message(FATAL_ERROR "ANOMALY_SANITIZE is for the macOS build; the iOS Simulator is not used and the device build is not sanitized.")
    endif()
    if("thread" IN_LIST ANOMALY_SANITIZE)
        list(LENGTH ANOMALY_SANITIZE anomaly_sanitize_count)
        if(NOT anomaly_sanitize_count EQUAL 1)
            message(FATAL_ERROR "ANOMALY_SANITIZE=thread cannot be combined with other sanitizers (got '${ANOMALY_SANITIZE}').")
        endif()
    endif()
    string(REPLACE ";" "," anomaly_sanitize_list "${ANOMALY_SANITIZE}")
    set(ANOMALY_SANITIZE_COMPILE_FLAGS "-fsanitize=${anomaly_sanitize_list}" -fno-omit-frame-pointer)
    set(ANOMALY_SANITIZE_LINK_FLAGS "-fsanitize=${anomaly_sanitize_list}")
    if("address" IN_LIST ANOMALY_SANITIZE)
        # let ASAN_OPTIONS=halt_on_error=0 keep a scenario going after the first report
        list(APPEND ANOMALY_SANITIZE_COMPILE_FLAGS -fsanitize-recover=address)
    endif()
    if("undefined" IN_LIST ANOMALY_SANITIZE)
        # vptr: the engine casts through unrelated bases on purpose (CObject <-> CGameObject mixins, luabind) and has
        # no use for the RTTI-based check. function: luabind and the script exports call through casted pointers.
        # Both are noise, not bugs. alignment: the engine reads packed file data through typed pointers and nests structs in
        # `#pragma pack` records, ~90% of the first run's reports; arm64 loads and stores do not care, so it is off unless
        # "alignment" is in ANOMALY_SANITIZE. Everything else in `undefined` stays on and prints instead of aborting.
        list(APPEND ANOMALY_SANITIZE_COMPILE_FLAGS -fno-sanitize=vptr,function -fsanitize-recover=undefined)
        # float-cast-overflow is not part of `undefined` but is the check for the x86-64 -> arm64 change in float to unsigned
        # conversion: out of range, MSVC's cvttsd2si wraps (-1.0 -> 0xFFFFFFFF), arm64's fcvtzu saturates (-1.0 -> 0).
        list(APPEND ANOMALY_SANITIZE_COMPILE_FLAGS -fsanitize=float-cast-overflow)
        if(NOT "alignment" IN_LIST ANOMALY_SANITIZE)
            list(APPEND ANOMALY_SANITIZE_COMPILE_FLAGS -fno-sanitize=alignment)
        endif()
    endif()
    if("thread" IN_LIST ANOMALY_SANITIZE)
        # ThreadSanitizer adds an unwind cleanup (the __tsan_func_exit call) to every function that calls something that may throw,
        # and clang gives those the C personality routine: with gxx, objc (SDL, the DualSense code) and LuaJIT's interpreter
        # that is four, one more than compact unwind can encode ("Too many personality routines"). The DWARF tables the
        # interpreter carries are used instead, and LuaJIT is built with internal unwinding (cmake/deps/LuaJIT.cmake), which does
        # not need any unwind info. A C++ exception that is really thrown cannot be unwound in this build: it terminates.
        list(APPEND ANOMALY_SANITIZE_LINK_FLAGS -Wl,-no_compact_unwind)
    endif()
    message(STATUS "Sanitizers: ${anomaly_sanitize_list}")
    add_compile_options(${ANOMALY_SANITIZE_COMPILE_FLAGS})
    add_link_options(${ANOMALY_SANITIZE_LINK_FLAGS})
endif()

set(ANOMALY_BUNDLE_ID_PREFIX "com.andriishafar.anomaly" CACHE STRING "Reverse-DNS prefix for app bundle identifiers")
set(ANOMALY_IOS_TEAM_ID "" CACHE STRING "Apple Developer team ID used to sign iOS builds")

if(ANOMALY_IOS AND NOT ANOMALY_IOS_TEAM_ID)
    message(WARNING "ANOMALY_IOS_TEAM_ID is empty; iOS targets will not be signed. Set it in CMakeUserPresets.json.")
endif()

# Memory entitlements. A free Personal Team cannot sign extended virtual addressing, so it is
# opt-in; turn it on with a paid Apple Developer Program team.
option(ANOMALY_IOS_INCREASED_MEMORY_LIMIT "Request com.apple.developer.kernel.increased-memory-limit" ON)
option(ANOMALY_IOS_EXTENDED_VIRTUAL_ADDRESSING "Request com.apple.developer.kernel.extended-virtual-addressing (paid team only)" OFF)

set(ANOMALY_ENTITLEMENT_ENTRIES "")
if(ANOMALY_IOS_INCREASED_MEMORY_LIMIT)
    # Raise the jetsam limit on 8 GB iPads; Anomaly needs several GB of RAM plus GPU memory.
    string(APPEND ANOMALY_ENTITLEMENT_ENTRIES "\t<key>com.apple.developer.kernel.increased-memory-limit</key>\n\t<true/>\n")
endif()
if(ANOMALY_IOS_EXTENDED_VIRTUAL_ADDRESSING)
    # Larger address space for memory-mapped .db archives (~17 GB of game data).
    string(APPEND ANOMALY_ENTITLEMENT_ENTRIES "\t<key>com.apple.developer.kernel.extended-virtual-addressing</key>\n\t<true/>\n")
endif()
set(ANOMALY_IOS_ENTITLEMENTS "${CMAKE_BINARY_DIR}/Anomaly.entitlements")
configure_file("${PROJECT_SOURCE_DIR}/platform/ios/Anomaly.entitlements.in" "${ANOMALY_IOS_ENTITLEMENTS}" @ONLY)

# Applies bundle, signing and entitlement settings shared by every app target.
function(anomaly_configure_app target)
    cmake_parse_arguments(ARG "" "BUNDLE_SUFFIX;DISPLAY_NAME;INFO_PLIST;ENTITLEMENTS" "" ${ARGN})

    set(bundle_id "${ANOMALY_BUNDLE_ID_PREFIX}${ARG_BUNDLE_SUFFIX}")
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
        MACOSX_BUNDLE_BUNDLE_NAME "${ARG_DISPLAY_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${bundle_id}"
    )
    if(ARG_INFO_PLIST)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${ARG_INFO_PLIST}")
    endif()

    if(ANOMALY_IOS)
        set_target_properties(${target} PROPERTIES
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2"
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${ANOMALY_IOS_TEAM_ID}"
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
        )
        if(ARG_ENTITLEMENTS)
            set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${ARG_ENTITLEMENTS}")
        endif()
    endif()
endfunction()
