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
#else
#	error Unsupported platform: the port targets Windows, macOS and iOS/iPadOS.
#endif

#if defined(_M_X64) || defined(__x86_64__)
#	define XR_ARCH_X64 1
#elif defined(_M_IX86) || defined(__i386__)
#	define XR_ARCH_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
#	define XR_ARCH_ARM64 1
#else
#	error Unsupported architecture.
#endif

#if defined(XR_PLATFORM_APPLE)
#	include "PlatformApple.inl"
#endif
