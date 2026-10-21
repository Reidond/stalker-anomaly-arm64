# libtheora decoder plus the legacy theora_* API that xrEngine/xrTheora_Stream.cpp uses.
# Upstream only ships autotools/SCons, so the source list mirrors lib/Makefile.am's decoder set.

set(theora_dir "${ANOMALY_EXTERNAL_DIR}/theora")

add_library(theora STATIC
    "${theora_dir}/lib/apiwrapper.c"
    "${theora_dir}/lib/bitpack.c"
    "${theora_dir}/lib/decapiwrapper.c"
    "${theora_dir}/lib/decinfo.c"
    "${theora_dir}/lib/decode.c"
    "${theora_dir}/lib/dequant.c"
    "${theora_dir}/lib/fragment.c"
    "${theora_dir}/lib/huffdec.c"
    "${theora_dir}/lib/idct.c"
    "${theora_dir}/lib/info.c"
    "${theora_dir}/lib/internal.c"
    "${theora_dir}/lib/quant.c"
    "${theora_dir}/lib/state.c"
)
target_include_directories(theora SYSTEM PUBLIC "${theora_dir}/include" PRIVATE "${theora_dir}/lib")
target_link_libraries(theora PUBLIC Ogg::ogg)
target_compile_options(theora PRIVATE -w)
add_library(Theora::theora ALIAS theora)
