// Minimal Windows/COM vocabulary for the D3D11-on-Metal shim.
//
// The shim does not use real COM: every interface is a plain C++ class with virtual AddRef/Release.
// Only what the R4 renderer's D3D-facing code needs is declared. Define MTL_D3D11_NO_WINTYPES if the
// engine's platform layer already provides these types; the typedefs below are skipped then, but the
// shim headers still need the names (HRESULT, UINT, SIZE_T, GUID, RECT, ...) to exist.
#pragma once

#include <cstddef>
#include <cstdint>

#ifndef MTL_D3D11_NO_WINTYPES
typedef int32_t HRESULT;
typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int32_t INT;
typedef uint8_t UINT8;
typedef uint64_t UINT64;
typedef float FLOAT;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef size_t SIZE_T;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef wchar_t WCHAR;
typedef const wchar_t* LPCWSTR;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef void* HANDLE;
typedef void* HWND;
typedef void* HMONITOR;
typedef void* HMODULE;
#ifndef __OBJC__
// Objective-C already owns BOOL (a bool on arm64); the shim never uses the name in its own signatures.
typedef int32_t BOOL;
#endif

struct RECT
{
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};
struct POINT
{
    LONG x;
    LONG y;
};
struct LUID
{
    uint32_t LowPart;
    int32_t HighPart;
};
#endif // MTL_D3D11_NO_WINTYPES

// Signed 32-bit boolean with D3D layout. Public structs use this so the layout matches the Windows
// headers in both C++ and Objective-C++ translation units.
typedef int32_t MtlBOOL;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif

#ifndef S_OK
#define S_OK ((HRESULT)0)
#endif
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005)
#endif
#ifndef E_NOTIMPL
#define E_NOTIMPL ((HRESULT)0x80004001)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE ((HRESULT)0x80004002)
#endif
#ifndef E_POINTER
#define E_POINTER ((HRESULT)0x80004003)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#endif
#ifndef FAILED
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#ifndef MAKE_HRESULT
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((uint32_t)(sev) << 31) | ((uint32_t)(fac) << 16) | ((uint32_t)(code))))
#endif
#ifndef ZeroMemory
#include <cstring>
#define ZeroMemory(p, n) memset((p), 0, (n))
#endif

#ifndef MTL_GUID_DEFINED
#define MTL_GUID_DEFINED
// Named MtlGUID so that CoreFoundation's REFIID/CFUUID names and the engine's own GUID can coexist.
struct MtlGUID
{
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
inline bool operator==(const MtlGUID& a, const MtlGUID& b)
{
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, 8) == 0;
}
inline bool operator!=(const MtlGUID& a, const MtlGUID& b) { return !(a == b); }
typedef MtlGUID MtlIID;
typedef const MtlGUID& MtlREFIID;
#endif

// Replacement for __uuidof(T). Interfaces the engine asks for by IID get a specialisation below; any other
// type gets a process-unique GUID derived from a static's address, which is all QueryInterface needs.
template <class T>
inline const MtlGUID& mtl_uuidof()
{
    static const MtlGUID g = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g)), 0xFFFF, 0xFFFF,
                              {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}};
    return g;
}

#define MTL_DECLARE_IID(type, d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7)                         \
    template <>                                                                                   \
    inline const MtlGUID& mtl_uuidof<type>()                                                      \
    {                                                                                             \
        static const MtlGUID g = {d1, d2, d3, {b0, b1, b2, b3, b4, b5, b6, b7}};                 \
        return g;                                                                                 \
    }

// MSVC's __uuidof(Type) and IID_PPV_ARGS(&ptr), type form only.
#ifndef __uuidof
#define __uuidof(T) (mtl_uuidof<T>())
#endif

template <class T>
inline void** mtl_ppv_args_helper(T** pp)
{
    return reinterpret_cast<void**>(pp);
}
template <class T>
inline const MtlGUID& mtl_iid_of_ptrptr(T**)
{
    return mtl_uuidof<T>();
}
#ifndef IID_PPV_ARGS
#define IID_PPV_ARGS(pp) mtl_iid_of_ptrptr(pp), mtl_ppv_args_helper(pp)
#endif

// Base of every shim interface.
struct IUnknown
{
    virtual HRESULT QueryInterface(MtlREFIID riid, void** ppvObject) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;

    // The templated overload MSVC's headers provide: p->QueryInterface(&q).
    template <class Q>
    HRESULT QueryInterface(Q** pp)
    {
        return QueryInterface(mtl_uuidof<Q>(), reinterpret_cast<void**>(pp));
    }

protected:
    virtual ~IUnknown() = default;
};
MTL_DECLARE_IID(IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
