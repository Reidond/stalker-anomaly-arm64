// Win32 / MSVC CRT compatibility for macOS and iOS (arm64). Included from Platform.h; do not include directly.
//
// This is deliberately a thin layer: it declares only what the engine code calls, in terms of POSIX.
// Functions that need a real implementation (path resolution, directory listing, timers, ...) are
// implemented in src/xrCore/Platform/PlatformApple.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <alloca.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <utime.h>

#ifdef __cplusplus
#include <algorithm>
#include <string>
#include <cmath>
#include <fenv.h>
#endif

// ---------------------------------------------------------------------------------------------
// Compiler keywords
// ---------------------------------------------------------------------------------------------
#ifndef __forceinline
#	define __forceinline inline __attribute__((always_inline))
#endif
#define __stdcall
#define __cdecl
#define __fastcall
#define __thiscall
#define __vectorcall
#define _cdecl
#define WINAPI
#define CALLBACK
#define APIENTRY
#define PASCAL
#define STDMETHODCALLTYPE
#define __interface struct
#define __int64 long long
#define __int32 int
#define __int16 short
#define __int8 char
#define __w64
#define __unaligned
#define __assume(x) do { if (!(x)) __builtin_unreachable(); } while (0)

// __declspec(x) -> attribute. Only the specifiers the engine uses are mapped; anything else expands to nothing.
#define XR_DECLSPEC_noinline __attribute__((noinline))
#define XR_DECLSPEC_align(n) __attribute__((aligned(n)))
#define XR_DECLSPEC_dllexport
#define XR_DECLSPEC_dllimport
#define XR_DECLSPEC_novtable
#define XR_DECLSPEC_selectany __attribute__((weak))
#define XR_DECLSPEC_naked
#define XR_DECLSPEC_noreturn __attribute__((noreturn))
#define XR_DECLSPEC_deprecated __attribute__((deprecated))
#define XR_DECLSPEC_thread __thread
#define XR_DECLSPEC_restrict
#define XR_DECLSPEC_allocator
#define XR_DECLSPEC_property(...)
#define __declspec(x) XR_DECLSPEC_##x

#ifdef __cplusplus
#	define __pragma(x)
#endif

#define _alloca alloca

// ---------------------------------------------------------------------------------------------
// Basic Win32 types
// ---------------------------------------------------------------------------------------------
#define XR_WIN32_COMPAT_TYPES 1

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef unsigned int UINT;
typedef int INT;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef uint16_t USHORT;
typedef int16_t SHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef float FLOAT;
typedef double DOUBLE;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef size_t SIZE_T;
typedef ptrdiff_t SSIZE_T;
typedef unsigned long long DWORD64;
typedef intptr_t LPARAM;
typedef uintptr_t WPARAM;
typedef intptr_t LRESULT;
typedef int32_t HRESULT;
typedef int errno_t;
typedef char* LPSTR;
typedef char* PSTR;
typedef char* LPTSTR;
typedef const char* LPCSTR;
typedef const char* LPCTSTR;
typedef const char* PCSTR;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef void* PVOID;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HWND;
typedef void* HDC;
typedef void* HICON;
typedef void* HCURSOR;
typedef void* HKEY;
typedef BOOL* LPBOOL;
typedef DWORD* LPDWORD;
typedef BYTE* LPBYTE;
typedef LONG* PLONG;
typedef DWORD* PDWORD;
typedef BYTE* PBYTE;
typedef DWORD FOURCC;
typedef unsigned long _fsize_t;

#define CONST const
#define VOID void
#define FAR
#define NEAR
#define IN
#define OUT
#define OPTIONAL

#ifndef TRUE
#	define TRUE 1
#endif
#ifndef FALSE
#	define FALSE 0
#endif
#define NONE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE 0xFFFFFFFFu
#define MAX_PATH 260
#define _MAX_PATH 260
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256

#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define RGB(r, g, b) ((DWORD)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)

typedef struct tagRECT
{
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
} RECT, *PRECT, *LPRECT;

typedef struct tagPOINT
{
	LONG x;
	LONG y;
} POINT, *PPOINT, *LPPOINT;

typedef union _LARGE_INTEGER
{
	struct
	{
		DWORD LowPart;
		LONG HighPart;
	};
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER
{
	struct
	{
		DWORD LowPart;
		DWORD HighPart;
	};
	ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _EXCEPTION_POINTERS
{
	void* ExceptionRecord;
	void* ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

#ifdef __cplusplus
extern "C++" {

// ---------------------------------------------------------------------------------------------
// Path model
// ---------------------------------------------------------------------------------------------
// Virtual path -> OS path. Replaces '\\' with '/' and, if the result does not exist, resolves each
// component case-insensitively (the game data is mixed-case, iOS volumes may be case-sensitive).
// Paths that do not exist yet are resolved as far as they exist and the remainder is kept as given.
// The result lives in a small per-thread ring of buffers: use it immediately, do not store it.
const char* xr_native_path(const char* virtual_path);
// OS path -> virtual path (copies, replacing '/' with '\\').
void xr_virtual_path(const char* native_path, char* out, size_t out_size);
// Creates one directory level, resolving the parent case-insensitively.
int xr_mkdir_virtual(const char* virtual_path);

// ---------------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------------
void Sleep(DWORD ms);
DWORD GetTickCount();
unsigned long long GetTickCount64();
BOOL QueryPerformanceCounter(LARGE_INTEGER* counter);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq);
inline BOOL SwitchToThread() { return sched_yield() == 0; }
inline void timeBeginPeriod(unsigned) {}
inline void timeEndPeriod(unsigned) {}

// ---------------------------------------------------------------------------------------------
// Message boxes. There is no modal UI in xrCore: the text goes to stderr, and a front end (the SDL window layer)
// can install a hook to show a real dialog.
// ---------------------------------------------------------------------------------------------
#define MB_OK 0x00000000
#define MB_OKCANCEL 0x00000001
#define MB_ICONERROR 0x00000010
#define MB_ICONWARNING 0x00000030
#define MB_ICONINFORMATION 0x00000040
#define MB_SYSTEMMODAL 0x00001000
#define MB_SETFOREGROUND 0x00010000
#define MB_DEFBUTTON1 0x00000000
#define IDOK 1
#define IDCANCEL 2
typedef void xr_message_box_hook_t(const char* title, const char* text, unsigned type);
void xr_set_message_box_hook(xr_message_box_hook_t* hook);
int MessageBox(HWND window, const char* text, const char* caption, unsigned type);

// ---------------------------------------------------------------------------------------------
// Process / thread / environment
// ---------------------------------------------------------------------------------------------
const char* GetCommandLine();
DWORD GetCurrentThreadId();
inline DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
inline HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
BOOL IsDebuggerPresent();
void DebugBreak();
void OutputDebugString(const char* str);
inline DWORD GetLastError() { return (DWORD)errno; }
inline void SetLastError(DWORD e) { errno = (int)e; }
DWORD GetModuleFileName(HMODULE module, char* buffer, DWORD size);
inline HMODULE GetModuleHandle(const char*) { return nullptr; }
DWORD GetCurrentDirectory(DWORD size, char* buffer);
BOOL GetUserName(char* buffer, DWORD* size);
BOOL GetComputerName(char* buffer, DWORD* size);
inline BOOL IsBadReadPtr(const void* p, size_t) { return p == nullptr; }
inline BOOL IsBadWritePtr(void* p, size_t) { return p == nullptr; }
inline void ZeroMemory(void* p, size_t n) { memset(p, 0, n); }
inline void CopyMemory(void* d, const void* s, size_t n) { memcpy(d, s, n); }
inline void MoveMemory(void* d, const void* s, size_t n) { memmove(d, s, n); }
inline void FillMemory(void* d, size_t n, int v) { memset(d, v, n); }

// Interlocked*, on 32-bit LONG like Win32.
inline LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedDecrement(volatile LONG* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchangeAdd(volatile LONG* p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedCompareExchange(volatile LONG* p, LONG exchange, LONG comparand)
{
	__atomic_compare_exchange_n(p, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return comparand;
}

// ---------------------------------------------------------------------------------------------
// Code pages: UTF-8 <-> wchar_t (UTF-32 here) and Windows-1251, which the game's Russian text uses.
// ---------------------------------------------------------------------------------------------
#define CP_ACP 0
#define CP_UTF8 65001
int MultiByteToWideChar(UINT code_page, DWORD flags, const char* src, int src_len, wchar_t* dst, int dst_len);
int WideCharToMultiByte(UINT code_page, DWORD flags, const wchar_t* src, int src_len, char* dst, int dst_len,
                        const char* default_char, BOOL* used_default);

// ---------------------------------------------------------------------------------------------
// CRT: floating point
// ---------------------------------------------------------------------------------------------
#define _FPCLASS_SNAN 0x0001
#define _FPCLASS_QNAN 0x0002
#define _FPCLASS_NINF 0x0004
#define _FPCLASS_NN 0x0008
#define _FPCLASS_ND 0x0010
#define _FPCLASS_NZ 0x0020
#define _FPCLASS_PZ 0x0040
#define _FPCLASS_PD 0x0080
#define _FPCLASS_PN 0x0100
#define _FPCLASS_PINF 0x0200
inline int _fpclass(double x)
{
	const bool neg = std::signbit(x);
	switch (std::fpclassify(x))
	{
	case FP_NAN: return _FPCLASS_QNAN; // signaling NaNs are not distinguishable here
	case FP_INFINITE: return neg ? _FPCLASS_NINF : _FPCLASS_PINF;
	case FP_ZERO: return neg ? _FPCLASS_NZ : _FPCLASS_PZ;
	case FP_SUBNORMAL: return neg ? _FPCLASS_ND : _FPCLASS_PD;
	default: return neg ? _FPCLASS_NN : _FPCLASS_PN;
	}
}
inline int _finite(double x) { return std::isfinite(x) ? 1 : 0; }
inline int _isnan(double x) { return std::isnan(x) ? 1 : 0; }
inline double _copysign(double x, double y) { return std::copysign(x, y); }

// x87 control word emulation. Only the rounding mode and flush-to-zero have an arm64 equivalent.
#define _MCW_EM 0x0008001F
#define _MCW_RC 0x00000300
#define MCW_RC _MCW_RC
#define _MCW_PC 0x00030000
#define MCW_PC _MCW_PC
#define MCW_EM _MCW_EM
#define _RC_NEAR 0x00000000
#define _RC_CHOP 0x00000300
#define _PC_24 0x00020000
#define _PC_53 0x00010000
#define _PC_64 0x00000000
inline void _clear87() {}
inline unsigned _control87(unsigned value, unsigned mask)
{
	if (mask & _MCW_RC) fesetround((value & _MCW_RC) == _RC_CHOP ? FE_TOWARDZERO : FE_TONEAREST);
	return (fegetround() == FE_TOWARDZERO ? _RC_CHOP : _RC_NEAR);
}

// ---------------------------------------------------------------------------------------------
// CRT: strings
// ---------------------------------------------------------------------------------------------
inline int stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int strcmpi(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strcmpi(const char* a, const char* b) { return strcasecmp(a, b); }
inline char* strupr(char* s)
{
	for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
	return s;
}
inline char* strlwr(char* s)
{
	for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
	return s;
}
inline char* _strupr(char* s) { return strupr(s); }
inline char* _strlwr(char* s) { return strlwr(s); }
inline char* _strdup(const char* s) { return strdup(s); }
inline int wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline char* lstrcpy(char* d, const char* s) { return strcpy(d, s); }
inline int lstrlen(const char* s) { return (int)strlen(s); }
inline char* _itoa(int value, char* buf, int radix);
inline errno_t _itoa_s(int value, char* buf, size_t size, int radix)
{
	char tmp[34];
	_itoa(value, tmp, radix);
	if (strlen(tmp) + 1 > size) return ERANGE;
	strcpy(buf, tmp);
	return 0;
}
inline errno_t _i64toa_s(long long value, char* buf, size_t size, int)
{
	return snprintf(buf, size, "%lld", value) < (int)size ? 0 : ERANGE;
}
inline errno_t _ui64toa_s(unsigned long long value, char* buf, size_t size, int)
{
	return snprintf(buf, size, "%llu", value) < (int)size ? 0 : ERANGE;
}
inline char* _itoa(int value, char* buf, int radix)
{
	if (radix == 16) snprintf(buf, 34, "%x", (unsigned)value);
	else if (radix == 8) snprintf(buf, 34, "%o", (unsigned)value);
	else snprintf(buf, 34, "%d", value);
	return buf;
}
inline char* itoa(int value, char* buf, int radix) { return _itoa(value, buf, radix); }
inline char* _ltoa(long value, char* buf, int radix) { return _itoa((int)value, buf, radix); }
inline char* _ultoa(unsigned long value, char* buf, int) { snprintf(buf, 34, "%lu", value); return buf; }
inline char* _i64toa(long long value, char* buf, int) { snprintf(buf, 34, "%lld", value); return buf; }
inline char* _ui64toa(unsigned long long value, char* buf, int) { snprintf(buf, 34, "%llu", value); return buf; }
inline long long _atoi64(const char* s) { return atoll(s); }
inline long long _strtoi64(const char* s, char** e, int base) { return strtoll(s, e, base); }
inline unsigned long long _strtoui64(const char* s, char** e, int base) { return strtoull(s, e, base); }

// "Secure" CRT. Return values follow the MS semantics (errno_t); on overflow the destination is emptied.
inline errno_t strcpy_s(char* dest, size_t size, const char* src)
{
	if (!dest) return EINVAL;
	if (size == 0) return ERANGE;
	if (!src) { dest[0] = 0; return EINVAL; }
	for (size_t i = 0; i < size; ++i)
		if ((dest[i] = src[i]) == 0) return 0;
	dest[0] = 0;
	return ERANGE;
}
template <size_t N>
inline errno_t strcpy_s(char (&dest)[N], const char* src) { return strcpy_s(dest, N, src); }

inline errno_t strncpy_s(char* dest, size_t size, const char* src, size_t count)
{
	if (!dest || size == 0) return EINVAL;
	if (!src) { dest[0] = 0; return EINVAL; }
	size_t i = 0;
	size_t limit = (count < size) ? count : size - 1;
	for (; i < limit && src[i]; ++i) dest[i] = src[i];
	if (!src[i] || i == count) { dest[i] = 0; return 0; }
	dest[0] = 0;
	return ERANGE;
}
template <size_t N>
inline errno_t strncpy_s(char (&dest)[N], const char* src, size_t count) { return strncpy_s(dest, N, src, count); }

inline errno_t strcat_s(char* dest, size_t size, const char* src)
{
	if (!dest) return EINVAL;
	if (!src) { dest[0] = 0; return EINVAL; }
	size_t i = 0;
	while (i < size && dest[i]) ++i;
	if (i == size) { dest[0] = 0; return EINVAL; }
	for (size_t j = 0; i + j < size; ++j)
		if ((dest[i + j] = src[j]) == 0) return 0;
	dest[0] = 0;
	return ERANGE;
}
template <size_t N>
inline errno_t strcat_s(char (&dest)[N], const char* src) { return strcat_s(dest, N, src); }

inline errno_t strncat_s(char* dest, size_t size, const char* src, size_t count)
{
	if (!dest || !src) return EINVAL;
	size_t i = 0;
	while (i < size && dest[i]) ++i;
	if (i == size) { dest[0] = 0; return EINVAL; }
	for (size_t j = 0; i + j < size; ++j)
	{
		if (j == count || src[j] == 0) { dest[i + j] = 0; return 0; }
		dest[i + j] = src[j];
	}
	dest[0] = 0;
	return ERANGE;
}

inline void _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext);
inline errno_t _splitpath_s(const char* path, char* drive, size_t drive_size, char* dir, size_t dir_size, char* fname,
                            size_t fname_size, char* ext, size_t ext_size)
{
	char d[1024], f[1024], e[1024];
	_splitpath(path, nullptr, d, f, e);
	if (drive && drive_size) drive[0] = 0;
	if (dir && strcpy_s(dir, dir_size, d)) return ERANGE;
	if (fname && strcpy_s(fname, fname_size, f)) return ERANGE;
	if (ext && strcpy_s(ext, ext_size, e)) return ERANGE;
	return 0;
}

inline int vsnprintf_s(char* buffer, size_t size, size_t /*count*/, const char* format, va_list args)
{
	return vsnprintf(buffer, size, format, args);
}
inline int _vsnprintf(char* buffer, size_t size, const char* format, va_list args)
{
	return vsnprintf(buffer, size, format, args);
}
inline int vsprintf_s(char* buffer, size_t size, const char* format, va_list args)
{
	return vsnprintf(buffer, size, format, args);
}
template <size_t N>
inline int vsprintf_s(char (&buffer)[N], const char* format, va_list args)
{
	return vsnprintf(buffer, N, format, args);
}
inline int sprintf_s(char* buffer, size_t size, const char* format, ...) __attribute__((format(printf, 3, 4)));
inline int sprintf_s(char* buffer, size_t size, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int r = vsnprintf(buffer, size, format, args);
	va_end(args);
	return r;
}
template <size_t N>
inline int sprintf_s(char (&buffer)[N], const char* format, ...) __attribute__((format(printf, 2, 3)));
template <size_t N>
inline int sprintf_s(char (&buffer)[N], const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int r = vsnprintf(buffer, N, format, args);
	va_end(args);
	return r;
}
inline int _snprintf(char* buffer, size_t size, const char* format, ...) __attribute__((format(printf, 3, 4)));
inline int _snprintf(char* buffer, size_t size, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int r = vsnprintf(buffer, size, format, args);
	va_end(args);
	return r;
}
inline int swprintf_s(wchar_t* buffer, size_t size, const wchar_t* format, ...)
{
	va_list args;
	va_start(args, format);
	int r = vswprintf(buffer, size, format, args);
	va_end(args);
	return r;
}

// Reports through errno, not a table: _sys_errlist[errno] still works.
struct xr_errlist_t
{
	const char* operator[](int e) const { return strerror(e); }
};
static const xr_errlist_t _sys_errlist = {};

// _splitpath with MSVC semantics for virtual paths: no drive, 'dir' keeps its trailing separator, 'ext' starts at the
// last dot of the file name. Any of the outputs may be null.
inline void _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
	if (drive) drive[0] = 0;
	if (dir) dir[0] = 0;
	if (fname) fname[0] = 0;
	if (ext) ext[0] = 0;
	if (!path) return;

	const char* name = path;
	for (const char* p = path; *p; ++p)
		if (*p == '\\' || *p == '/') name = p + 1;
	if (dir)
	{
		memcpy(dir, path, (size_t)(name - path));
		dir[name - path] = 0;
	}
	const char* dot = strrchr(name, '.');
	const char* name_end = dot ? dot : name + strlen(name);
	if (fname)
	{
		memcpy(fname, name, (size_t)(name_end - name));
		fname[name_end - name] = 0;
	}
	if (ext && dot) strcpy(ext, dot);
}

// ---------------------------------------------------------------------------------------------
// CRT: low-level I/O, directories, stat. All take virtual paths and convert at the boundary.
// ---------------------------------------------------------------------------------------------
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_TRUNC O_TRUNC
#define _O_CREAT O_CREAT
#define _O_APPEND O_APPEND
#define _O_BINARY 0
#define O_BINARY 0
#define O_SEQUENTIAL 0
#define _SH_DENYNO 0
#define _SH_DENYWR 0
#define SH_DENYWR 0
#define _S_IREAD S_IRUSR
#define _S_IWRITE S_IWUSR
#define S_IREAD S_IRUSR
#define S_IWRITE S_IWUSR
#define _stat stat
#define _fdopen fdopen
#define _stat64 stat
#define _fstat fstat
#define _S_IFDIR S_IFDIR
#define _S_IFREG S_IFREG
#define _S_IFMT S_IFMT

#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20

struct _finddata64i32_t
{
	unsigned attrib;
	time_t time_create;
	time_t time_access;
	time_t time_write;
	_fsize_t size;
	char name[260];
};
typedef _finddata64i32_t _finddata_t;

// Directory enumeration with MSVC semantics: the pattern is matched with * and ? against the entry names, "." and ".."
// are returned. The directory part of 'pattern' is a virtual path.
intptr_t _findfirst(const char* pattern, _finddata_t* data);
int _findnext(intptr_t handle, _finddata_t* data);
int _findclose(intptr_t handle);

inline int _filelength(int fd)
{
	struct stat st;
	if (fstat(fd, &st) != 0) return -1;
	return (int)st.st_size;
}
inline int filelength(int fd) { return _filelength(fd); }
inline int _close(int fd) { return close(fd); }
inline int _write(int fd, const void* buf, unsigned n) { return (int)write(fd, buf, n); }
inline int _commit(int fd) { return fsync(fd); }
int _read(int fd, void* buf, unsigned n); // loops until n bytes or EOF, like MSVC for regular files
int _open(const char* path, int flags, ...);
int _sopen_s(int* fd, const char* path, int flags, int shflag, int mode);
inline int _unlink(const char* path) { return unlink(xr_native_path(path)); }
inline int xr_unlink(const char* path) { return unlink(xr_native_path(path)); }
inline int _rmdir(const char* path) { return rmdir(xr_native_path(path)); }
inline int _mkdir(const char* path) { return xr_mkdir_virtual(path); }
inline int _access(const char* path, int mode) { return access(xr_native_path(path), mode); }
inline int _chdir(const char* path) { return chdir(xr_native_path(path)); }
inline char* _getcwd(char* buffer, int size)
{
	char tmp[1024];
	if (!getcwd(tmp, sizeof(tmp))) return nullptr;
	xr_virtual_path(tmp, buffer, (size_t)size);
	return buffer;
}
inline int _chmod(const char* path, int mode) { return chmod(xr_native_path(path), (mode_t)mode); }
struct _utimbuf
{
	time_t actime;
	time_t modtime;
};
int _utime(const char* path, const _utimbuf* times);
int xr_stat(const char* virtual_path, struct stat* st);
int xr_rename(const char* from, const char* to);
FILE* xr_fopen(const char* virtual_path, const char* mode);

#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL 0x00000080
DWORD GetFileAttributes(const char* path);
BOOL SetFileAttributes(const char* path, DWORD attr);
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

// min/max: the engine uses its own _min/_max; keep the MS spellings available.
#undef min
#undef max
#ifndef __min
#	define __min(a, b) (((a) < (b)) ? (a) : (b))
#	define __max(a, b) (((a) > (b)) ? (a) : (b))
#endif

} // extern "C++"
#endif // __cplusplus
