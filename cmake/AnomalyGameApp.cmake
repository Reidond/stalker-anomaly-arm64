# Bundle settings for the final game app (and anything else that ships under the game's bundle id).

include(AnomalyAppIcon)

# Applies the final game bundle id (com.andriishafar.anomaly, no suffix, so the game data pushed into
# the app container survives switching between shell_lab and the game), the final Info.plist template,
# the generated entitlements and the app icon (cmake/AnomalyAppIcon.cmake). DISPLAY_NAME overrides the name shown under the icon.
function(anomaly_configure_game_app target)
    cmake_parse_arguments(ARG "" "DISPLAY_NAME" "" ${ARGN})
    if(NOT ARG_DISPLAY_NAME)
        set(ARG_DISPLAY_NAME "Anomaly")
    endif()
    anomaly_configure_app(${target}
        DISPLAY_NAME "${ARG_DISPLAY_NAME}"
        INFO_PLIST "${PROJECT_SOURCE_DIR}/platform/${ANOMALY_PLATFORM_NAME}/Info.plist.in"
        ENTITLEMENTS "${ANOMALY_IOS_ENTITLEMENTS}"
    )
    anomaly_set_app_icon(${target})
endfunction()
