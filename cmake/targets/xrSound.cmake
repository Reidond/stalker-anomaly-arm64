# xrSound: OpenAL sound renderer. Source list: src/xrSound/xrSound.vcxproj.
# Owner: sound session. Opt-in: -DANOMALY_BUILD_XRSOUND=ON (or ANOMALY_BUILD_ALL_MODULES).
# OpenAL Soft (external/openal-soft), Ogg and Vorbis (external/) come from cmake/Dependencies.cmake.
anomaly_module_begin(xrSound "OpenAL sound renderer")

anomaly_vcxproj_sources(xrsound_sources "${ANOMALY_SRC_DIR}/xrSound/xrSound.vcxproj")

anomaly_add_module(xrSound
    SOURCES ${xrsound_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrSound"
    DEFINES XRSOUND_EXPORTS AL_LIBTYPE_STATIC
    LINK xrCDB xrAPI OpenAL::OpenAL Vorbis::vorbis Ogg::ogg
)
