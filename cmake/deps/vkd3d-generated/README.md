Generated sources for `external/vkd3d` (tag vkd3d-2.1), checked in so building the shader translator
does not need bison 3, flex, perl or Wine's `widl`. `regenerate.sh` documents how they were made.

- `hlsl.tab.[ch]`, `hlsl.yy.c`, `preproc.tab.[ch]`, `preproc.yy.c`: bison/flex output
- `private/spirv_grammar.h`: from `include/private/spirv.core.grammar.json`
- `vkd3d_d3dcommon.h`, `vkd3d_d3dx9shader.h`: widl output from the matching `.idl` files
- `config.h`, `private/vkd3d_version.h`: hand-written for Apple clang
