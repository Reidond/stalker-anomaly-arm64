#!/bin/bash
# Regenerates the files in this directory from external/vkd3d (pinned at vkd3d-2.1). They are checked
# in so the build needs none of these tools; rerun only when bumping the vkd3d submodule.
#
# Needs: GNU bison >= 3 (macOS ships 2.3), flex 2.6, perl with the JSON module, and widl from Wine
# (`brew install wine` or build tools/widl from the Wine source tree; pass its path as $WIDL).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../../external/vkd3d
cd "$src"
BISON=${BISON:-bison}
WIDL=${WIDL:-widl}
flex  -o "$here/preproc.yy.c"  libs/vkd3d-shader/preproc.l
$BISON -d -o "$here/preproc.tab.c" libs/vkd3d-shader/preproc.y
flex  -o "$here/hlsl.yy.c"     libs/vkd3d-shader/hlsl.l
$BISON -d -o "$here/hlsl.tab.c"    libs/vkd3d-shader/hlsl.y
perl libs/vkd3d-shader/make_spirv include/private/spirv.core.grammar.json > "$here/private/spirv_grammar.h"
for h in vkd3d_d3dcommon vkd3d_d3dx9shader; do
    $WIDL -I./include -h -o "$here/$h.h" "include/$h.idl"
done
