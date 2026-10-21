# xrEngine input and media on Apple platforms (engine-input session): the SDL3 CInput (xr_input_sdl.cpp), the SDL scancode
# to DIK table (xr_input_keymap.cpp) and the libraries they and the media files need. The rest of xrEngine is
# cmake/targets/xrEngine.cmake (engine-core); this file only adds to its target, and reads after it (the glob is sorted and
# "xrEngine.cmake" sorts before "xrEngine_input.cmake").
#
#   input   xr_input_sdl.cpp + xr_input_keymap.cpp, GameInput (the DualSense layer) through gameinput_sdl, SDL3
#   media   xrTheora_* build as they are on our Theora/Ogg targets; GIFResource needs giflib, built here from the in-tree
#           sources; tntQAVI (Video for Windows) is a stub that fails Load() on Apple

# The headless test of both (macOS only; it compiles the engine sources it needs itself, so it does not need the module on).
add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrinput_smoke" "${PROJECT_BINARY_DIR}/tools/xrinput_smoke")

if(NOT TARGET xrEngine)
    return()
endif()

set(xrengine_dir "${ANOMALY_SRC_DIR}/xrEngine")

target_sources(xrEngine PRIVATE
    "${xrengine_dir}/xr_input_keymap.cpp"
    "${xrengine_dir}/xr_input_sdl.cpp"
)

# giflib (GIFResource.cpp, Layers/xrRender/gifPlayer.cpp): plain C, builds as is.
if(NOT TARGET giflib)
    set(giflib_dir "${ANOMALY_SRC_DIR}/3rd party/giflib")
    add_library(giflib STATIC
        "${giflib_dir}/dgif_lib.c"
        "${giflib_dir}/egif_lib.c"
        "${giflib_dir}/gif_err.c"
        "${giflib_dir}/gif_font.c"
        "${giflib_dir}/gif_hash.c"
        "${giflib_dir}/gifalloc.c"
        "${giflib_dir}/openbsd-reallocarray.c"
    )
    target_include_directories(giflib SYSTEM PUBLIC "${giflib_dir}")
    target_compile_options(giflib PRIVATE -w)
endif()

target_link_libraries(xrEngine PUBLIC giflib gameinput_sdl SDL3::SDL3-static)
