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
