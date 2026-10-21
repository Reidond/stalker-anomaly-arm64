# xrEngine: the engine proper (device, input, console, level loading, Lua glue). The Windows project is the
# application (xrEngine.exe); here it is a static library and the app target comes from the platform shell
# (src/Platform/Apple) at M1 integration. Source list: src/xrEngine/xrEngine.vcxproj (also compiles the Discord SDK
# glue in src/3rd party/discord). Owners: engine-core and engine-input sessions.
# Opt-in: -DANOMALY_BUILD_XRENGINE=ON (or ANOMALY_BUILD_ALL_MODULES).
anomaly_module_begin(xrEngine "engine core")

anomaly_vcxproj_sources(xrengine_sources "${ANOMALY_SRC_DIR}/xrEngine/xrEngine.vcxproj")

anomaly_add_module(xrEngine
    SOURCES ${xrengine_sources}
    INCLUDE_DIRS
        "${ANOMALY_SRC_DIR}/xrEngine"
        "${ANOMALY_SRC_DIR}/3rd party/imgui"
        "${ANOMALY_SRC_DIR}/3rd party/giflib"
    DEFINES STATIC_RENDERER_R4 ENGINE_BUILD AL_LIBTYPE_STATIC
    LINK luabind LuaJIT::luajit OpenAL::OpenAL Vorbis::vorbis Ogg::ogg Theora::theora
         xrAPI xrCDB xrSound xrNetServer xrParticles xrPhysics xrXMLParser lua_extensions
)
