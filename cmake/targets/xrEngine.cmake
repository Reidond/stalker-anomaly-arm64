# xrEngine: the engine proper (device, console, level loading, Lua glue). The Windows project is the application
# (xrEngine.exe); here it is a static library and the app target comes from the platform shell (src/Platform/Apple) at M1
# integration. Source list: src/xrEngine/xrEngine.vcxproj. Owners: engine-core (this file, everything under src/xrEngine
# except the input and media files below) and engine-input (cmake/targets/xrEngine_input.cmake, see the end).
# Opt-in: -DANOMALY_BUILD_XRENGINE=ON (or ANOMALY_BUILD_ALL_MODULES). Documented in docs/ios-port/XRENGINE.md.
anomaly_module_begin(xrEngine "engine core")

# Discord Rich Presence needs the Discord Game SDK (a Windows/macOS dylib, absent on iOS) and ICU. Off by default; the
# game keeps linking against the stubs in x_ray.cpp (discord_gameinfo, updateDiscordPresence, xr_ToUTF8).
option(ANOMALY_XRENGINE_DISCORD "Compile the Discord Rich Presence glue into xrEngine" OFF)

set(xrengine_exclude
    "text_console"          # Win32 console window (dedicated server only)
)
if(NOT ANOMALY_XRENGINE_DISCORD)
    list(APPEND xrengine_exclude "3rd party/discord/")
endif()

# Files owned by the engine-input session (Xr_input.cpp, IInputReceiver.cpp, line_edit_control.cpp, edit_actions.cpp,
# imgui_base_input.cpp, MonitorList.cpp, xrTheora_*, GIFResource.cpp, tntQAVI.cpp, xr_input_xinput.cpp). They are written
# against DirectInput, VFW and the Win32 monitor API and do not compile on Apple until that session's port lands; it
# announces itself with src/xrEngine/xr_input_sdl.cpp. Until then they are left out, so the library builds and the
# symbols they define show up as missing in tools/xrengine_smoke.
if(EXISTS "${ANOMALY_SRC_DIR}/xrEngine/xr_input_sdl.cpp")
    set(XRENGINE_INPUT_PORTED ON)
else()
    set(XRENGINE_INPUT_PORTED OFF)
    list(APPEND xrengine_exclude
        "xr_input\\.cpp" "iinputreceiver\\.cpp" "line_edit_control\\.cpp" "edit_actions\\.cpp" "imgui_base_input\\.cpp"
        "monitorlist\\.cpp" "tntqavi\\.cpp" "xr_input_xinput\\.cpp"
    )
endif()

anomaly_vcxproj_sources(xrengine_sources "${ANOMALY_SRC_DIR}/xrEngine/xrEngine.vcxproj" EXCLUDE ${xrengine_exclude})

# The Apple window layer and entry point (xr_window.h, xr_engine_app.h), and stand-ins for the Windows-only add-ons.
list(APPEND xrengine_sources
    "${ANOMALY_SRC_DIR}/xrEngine/xr_window_sdl.cpp"
    "${ANOMALY_SRC_DIR}/xrEngine/xr_platform_stubs_apple.cpp"
)

# Third-party code xrEngine compiles itself (the Windows solution has imgui.vcxproj and giflib.vcxproj). Dear ImGui
# without its DX9/DX10 backends: the renderer draws it through the stock DX11 backend, which runs on the Metal shim. imconfig.h routes IM_ASSERT to the engine log, so it takes the
# engine include configuration.
anomaly_vcxproj_sources(imgui_sources "${ANOMALY_SRC_DIR}/3rd party/imgui/imgui.vcxproj" EXCLUDE "backends/imgui_impl_dx9" "backends/imgui_impl_dx10")
anomaly_add_module(xrImGui SOURCES ${imgui_sources} INCLUDE_DIRS "${ANOMALY_SRC_DIR}/3rd party/imgui")
anomaly_vcxproj_sources(giflib_sources "${ANOMALY_SRC_DIR}/3rd party/giflib/giflib.vcxproj")
anomaly_add_module(giflib SOURCES ${giflib_sources} INCLUDE_DIRS "${ANOMALY_SRC_DIR}/3rd party/giflib")

set(xrengine_defines STATIC_RENDERER_R4 ENGINE_BUILD AL_LIBTYPE_STATIC)
if(ANOMALY_XRENGINE_DISCORD)
    list(APPEND xrengine_defines ANOMALY_XRENGINE_DISCORD)
endif()

anomaly_add_module(xrEngine
    SOURCES ${xrengine_sources}
    INCLUDE_DIRS
        "${ANOMALY_SRC_DIR}/xrEngine"
        "${ANOMALY_SRC_DIR}/3rd party/imgui"
        "${ANOMALY_SRC_DIR}/3rd party/giflib"
    DEFINES ${xrengine_defines}
    LINK luabind LuaJIT::luajit OpenAL::OpenAL Vorbis::vorbis Ogg::ogg Theora::theora
         xrAPI xrCDB xrSound xrNetServer xrParticles xrPhysics xrXMLParser lua_extensions
         xrImGui giflib anomaly_apple_shell metal_d3d11
)

# Fragment of the engine-input session: it extends the xrEngine target (target_sources/target_link_libraries) from
# cmake/targets/xrEngine_input.cmake, which the root CMakeLists includes after this file.

# tools/xrengine_smoke: the macOS smoke test (engine init through the Apple shell, the SDL3 window).
if(NOT ANOMALY_IOS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrengine_smoke" "${CMAKE_BINARY_DIR}/tools/xrengine_smoke")
endif()
