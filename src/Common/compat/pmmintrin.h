// Stand-in for the x86 intrinsics header <pmmintrin.h> on arm64: sse2neon implements the SSE subset the engine uses.
// The WebAssembly build (Emscripten, -msimd128 -msse4.2) takes Emscripten's own header, which emulates SSE up to 4.2 on wasm SIMD.
#pragma once
#if defined(__EMSCRIPTEN__)
// Emscripten's headers use __unaligned as a struct tag (_mm_storeu_ps & co.); under -fms-extensions it is the MSVC type
// qualifier keyword (Platform.h turns it into an empty macro). Make it an ordinary identifier for the duration of the header.
#pragma push_macro("__unaligned")
#undef __unaligned
#define __unaligned xr_emscripten_unaligned
#include_next <pmmintrin.h>
#pragma pop_macro("__unaligned")
#else
#include "sse2neon/sse2neon.h"
#endif
