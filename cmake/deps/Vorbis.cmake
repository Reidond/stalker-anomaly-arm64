# libvorbis + libvorbisfile (decoder only), built from external/vorbis without its own CMake,
# which insists on find_package(Ogg) and an install tree.

set(vorbis_dir "${ANOMALY_EXTERNAL_DIR}/vorbis")

add_library(vorbis STATIC
    "${vorbis_dir}/lib/analysis.c"
    "${vorbis_dir}/lib/bitrate.c"
    "${vorbis_dir}/lib/block.c"
    "${vorbis_dir}/lib/codebook.c"
    "${vorbis_dir}/lib/envelope.c"
    "${vorbis_dir}/lib/floor0.c"
    "${vorbis_dir}/lib/floor1.c"
    "${vorbis_dir}/lib/info.c"
    "${vorbis_dir}/lib/lookup.c"
    "${vorbis_dir}/lib/lpc.c"
    "${vorbis_dir}/lib/lsp.c"
    "${vorbis_dir}/lib/mapping0.c"
    "${vorbis_dir}/lib/mdct.c"
    "${vorbis_dir}/lib/psy.c"
    "${vorbis_dir}/lib/registry.c"
    "${vorbis_dir}/lib/res0.c"
    "${vorbis_dir}/lib/sharedbook.c"
    "${vorbis_dir}/lib/smallft.c"
    "${vorbis_dir}/lib/synthesis.c"
    "${vorbis_dir}/lib/window.c"
    "${vorbis_dir}/lib/vorbisfile.c"
)
target_include_directories(vorbis SYSTEM PUBLIC "${vorbis_dir}/include" PRIVATE "${vorbis_dir}/lib")
target_link_libraries(vorbis PUBLIC Ogg::ogg)
target_compile_options(vorbis PRIVATE -w)
add_library(Vorbis::vorbis ALIAS vorbis)
