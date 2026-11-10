// Win32 / MSVC CRT compatibility for WebAssembly (Emscripten, wasm32). Included from Platform.h; do not include directly.
//
// Spike S1b: Emscripten's libc is POSIX, so the layer PlatformApple.inl declares in terms of POSIX applies as it stands. The
// two Apple-only things in it (arm64 `yield`, `__APPLE__` includes) are already guarded there. Web-specific declarations
// go here. W1 splits PlatformApple.inl into a POSIX-common part, an Apple part and this one.
#pragma once

#include "PlatformApple.inl"

// Emscripten's wasm32 `long` is 32 bits (LP32-like for data: int, long and pointers are 32 bits, long long is 64), unlike
// the LP64 Apple targets and like Windows. Code that was audited for Apple's 64-bit `long` (XR_PLATFORM_APPLE branches that
// avoid `long`) is correct here as well.
static_assert(sizeof(void*) == 4, "the web build is wasm32; Memory64 is a later switch");

// No wall-clock CPU counter exists in wasm: CPU::GetCLK() (xrCore/_math.h) reads the monotonic clock in nanoseconds instead.
#define XR_WEB_CLK_HZ 1000000000ull
