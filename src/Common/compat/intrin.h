// Stand-in for MSVC <intrin.h> on arm64: SSE intrinsics through sse2neon plus the few MSVC builtins the engine uses.
// The WebAssembly build takes Emscripten's SSE headers (SSE up to 4.2 emulated on wasm SIMD) instead.
#pragma once
#include "../Platform.h"
#if defined(__EMSCRIPTEN__)
#include <nmmintrin.h> // (the forwarding header next to this one, which handles __unaligned)
#else
#include "sse2neon/sse2neon.h"
#endif
