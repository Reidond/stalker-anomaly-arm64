// Platform detection and the Win32/MSVC compatibility layer for the macOS / iPadOS port.
//
// Include this before anything else from code that has to build on both Windows and Apple
// platforms. On Windows it only defines the XR_PLATFORM_* markers; on Apple platforms it also pulls in
// PlatformApple.inl, which declares the subset of the Win32 API and MSVC CRT the engine uses.
//
// Path model on non-Windows platforms: the engine keeps "virtual" Windows-style paths ('\\' separators,
// lower case) everywhere. They are converted to real paths only where they reach the OS, via
// xr_native_path() (see PlatformApple.inl).
#pragma once

#if defined(_WIN32)
#	define XR_PLATFORM_WINDOWS 1
// Virtual paths are native paths on Windows; these wrappers exist so shared code can call them on every platform.
#	define xr_native_path(p) (p)
#	define xr_stat(p, st) stat(p, st)
#	define xr_unlink(p) _unlink(p)
#	define xr_rename(a, b) rename(a, b)
#	define xr_fopen(p, m) fopen(p, m)
#elif defined(__APPLE__)
#	include <TargetConditionals.h>
#	define XR_PLATFORM_APPLE 1
#	define XR_PLATFORM_POSIX 1
#	if TARGET_OS_IPHONE
#		define XR_PLATFORM_IOS 1
#	else
#		define XR_PLATFORM_MACOS 1
#	endif
#elif defined(__EMSCRIPTEN__)
// WebAssembly (Emscripten, wasm32, pthreads). Spike S1b state, docs/web-port/WASM-BUILD.md: the shared non-Windows branches of
// xrCore are keyed on XR_PLATFORM_APPLE ("the POSIX port path"), so the web build takes them by defining it as well; code that
// calls an Apple API (GCD, malloc_size, mach, sysctl, CoreFoundation, arm64 asm) puts an XR_PLATFORM_WEB branch ahead of it.
// W1 renames the sites that only mean "POSIX" to XR_PLATFORM_POSIX and drops the XR_PLATFORM_APPLE line below.
#	define XR_PLATFORM_WEB 1
#	define XR_PLATFORM_POSIX 1
#	define XR_PLATFORM_APPLE 1
#else
#	error Unsupported platform: the port targets Windows, macOS, iOS/iPadOS and WebAssembly (Emscripten).
#endif

#if defined(_M_X64) || defined(__x86_64__)
#	define XR_ARCH_X64 1
#elif defined(_M_IX86) || defined(__i386__)
#	define XR_ARCH_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
#	define XR_ARCH_ARM64 1
#elif defined(__wasm32__)
#	define XR_ARCH_WASM32 1
#else
#	error Unsupported architecture.
#endif

#if defined(XR_PLATFORM_WEB)
#	include "PlatformWeb.inl"
#elif defined(XR_PLATFORM_APPLE)
#	include "PlatformApple.inl"
#endif
