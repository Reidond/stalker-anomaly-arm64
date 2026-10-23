# The app icon: generated at configure time by tools/make_app_icon.sh into the build tree (gitignored), from the Anomaly
# launcher's logo when the game data is present, otherwise from the repository's own platform/icon/fallback.png.
#   -DANOMALY_GAME_DIR=<path to the Anomaly install>   where AnomalyLauncher.exe lives (default: game/Anomaly)
# iOS gets an asset catalog (AppIcon), macOS an AppIcon.icns. Documented in docs/ios-port/BUILDING.md.
include_guard(GLOBAL)

set(ANOMALY_GAME_DIR "${PROJECT_SOURCE_DIR}/game/Anomaly" CACHE PATH "Anomaly install used for the app icon (AnomalyLauncher.exe); the repository's fallback icon is used without it")

# This file is included from several directories, and a variable set here only exists in the directory that included it
# first, so the results go into global properties: ANOMALY_APP_ICON_DIR, and ANOMALY_APP_ICON_SOURCE, which is "game" or
# "fallback" (empty when generation failed).
function(anomaly_generate_app_icon)
    set(icon_dir "${CMAKE_BINARY_DIR}/app_icon")
    set(script "${PROJECT_SOURCE_DIR}/tools/make_app_icon.sh")
    set(launcher "${ANOMALY_GAME_DIR}/AnomalyLauncher.exe")
    set(inputs
        "${script}"
        "${PROJECT_SOURCE_DIR}/tools/app_icon/compose_icon.swift"
        "${PROJECT_SOURCE_DIR}/tools/app_icon/extract_launcher_icon.py"
        "${PROJECT_SOURCE_DIR}/platform/icon/fallback.png")
    if(EXISTS "${launcher}")
        list(APPEND inputs "${launcher}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})

    # The Swift step takes about ten seconds, so a configure that changes none of the inputs keeps the previous output.
    set(stamp "${ANOMALY_GAME_DIR}")
    foreach(input IN LISTS inputs)
        file(SHA256 "${input}" hash)
        string(APPEND stamp "\n${input} ${hash}")
    endforeach()
    set(stamp_file "${icon_dir}.stamp")
    if(EXISTS "${stamp_file}" AND EXISTS "${icon_dir}/source.txt")
        file(READ "${stamp_file}" previous)
        if(previous STREQUAL stamp)
            set(up_to_date TRUE)
        endif()
    endif()
    if(NOT up_to_date)
        execute_process(COMMAND "${script}" "${icon_dir}" "${ANOMALY_GAME_DIR}"
            RESULT_VARIABLE result ERROR_VARIABLE errors OUTPUT_QUIET)
        if(errors)
            string(STRIP "${errors}" errors)
            message(STATUS "app icon: ${errors}")
        endif()
        if(NOT result EQUAL 0)
            file(REMOVE "${stamp_file}")
            message(WARNING "The app icon could not be generated (tools/make_app_icon.sh failed); the app gets no icon.")
            return()
        endif()
        file(WRITE "${stamp_file}" "${stamp}")
    endif()

    file(READ "${icon_dir}/source.txt" source)
    string(STRIP "${source}" source)
    set_property(GLOBAL PROPERTY ANOMALY_APP_ICON_DIR "${icon_dir}")
    set_property(GLOBAL PROPERTY ANOMALY_APP_ICON_SOURCE "${source}")
    if(source STREQUAL "game")
        message(STATUS "app icon: Anomaly launcher logo from ${ANOMALY_GAME_DIR}")
    else()
        message(STATUS "app icon: fallback artwork (no ${launcher}); set ANOMALY_GAME_DIR to use the Anomaly logo")
    endif()
endfunction()

anomaly_generate_app_icon()

# Adds the icon to a bundle target. Call from the directory that defines the target.
function(anomaly_set_app_icon target)
    get_property(source GLOBAL PROPERTY ANOMALY_APP_ICON_SOURCE)
    if(NOT source)
        return()
    endif()
    get_property(icon_dir GLOBAL PROPERTY ANOMALY_APP_ICON_DIR)
    if(ANOMALY_IOS)
        set(catalog "${icon_dir}/Assets.xcassets")
        target_sources(${target} PRIVATE "${catalog}")
        set_source_files_properties("${catalog}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
    elseif(EXISTS "${icon_dir}/AppIcon.icns")
        set(icns "${icon_dir}/AppIcon.icns")
        target_sources(${target} PRIVATE "${icns}")
        set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")
    endif()
endfunction()
