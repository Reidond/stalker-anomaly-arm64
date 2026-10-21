# xrSound: OpenAL sound renderer. Source list: src/xrSound/xrSound.vcxproj.
# Owner: sound session. Opt-in: -DANOMALY_BUILD_XRSOUND=ON (or ANOMALY_BUILD_ALL_MODULES).
# OpenAL Soft (external/openal-soft), Ogg and Vorbis (external/) come from cmake/Dependencies.cmake.
anomaly_module_begin(xrSound "OpenAL sound renderer")

# guids.cpp (INITGUID + <dsound.h>) and NotificationClient.cpp (WASAPI IMMNotificationClient) are Windows-only.
# SoundRender_Apple.cpp replaces the notification client: OpenAL Soft's system events plus, on iOS, AVAudioSession
# (AudioSession_Apple.mm). docs/ios-port/SOUND.md.
anomaly_vcxproj_sources(xrsound_sources "${ANOMALY_SRC_DIR}/xrSound/xrSound.vcxproj"
    EXCLUDE "(^|/)guids\\.cpp$" "(^|/)notificationclient\\.cpp$")

anomaly_add_module(xrSound
    SOURCES ${xrsound_sources}
        "${ANOMALY_SRC_DIR}/xrSound/SoundRender_Apple.cpp"
        "${ANOMALY_SRC_DIR}/xrSound/AudioSession_Apple.mm"
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrSound"
    DEFINES XRSOUND_EXPORTS AL_LIBTYPE_STATIC
    LINK xrCDB xrAPI OpenAL::OpenAL Vorbis::vorbis Ogg::ogg
)
set_source_files_properties("${ANOMALY_SRC_DIR}/xrSound/AudioSession_Apple.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(xrSound PUBLIC "-framework Foundation" "-framework AVFoundation")

# TODO(shim): belongs in cmake/Dependencies.cmake, next to the OpenAL target (it fixes every iOS consumer of OpenAL).
# OpenAL Soft keys several workarounds on MAC_OS_X_VERSION_MIN_REQUIRED, which AvailabilityMacros.h defines as 10.5 on iOS:
# common/almalloc.cpp then replaces the global aligned operator new with a posix_memalign() that rejects 4-byte alignment,
# so alGenEffects (al::Effect is 4-aligned) fails with AL_OUT_OF_MEMORY and Anomaly's EAX reverb is never created; atomic.h
# and alformat.hpp fall back to code for macOS < 11 / < 13.3. A value matching the iOS 17 deployment target removes them.
if(ANOMALY_IOS AND TARGET OpenAL)
    target_compile_definitions(OpenAL PRIVATE MAC_OS_X_VERSION_MIN_REQUIRED=140000)
endif()

# Headless check of the module against the real game data (tools/xrsound_smoke); also builds for iOS as a small app.
add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrsound_smoke" "${PROJECT_BINARY_DIR}/tools/xrsound_smoke")
