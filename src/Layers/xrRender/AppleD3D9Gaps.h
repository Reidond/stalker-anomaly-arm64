// AppleD3D9Gaps.h: D3D9 names and D3DX vector operators the renderer uses that src/Common/d3d9compat.h and
// d3dx9compat.h do not provide yet.
// TODO(shim): requested from the shim owner (see docs/ios-port/RENDERER.md, "Shim requests"). The shim defines
// XR_D3D9COMPAT_QUERY_RTYPE and XR_D3DX9COMPAT_VECTOR_OPS next to the real definitions; delete this file then.
#pragma once

#if defined(XR_PLATFORM_APPLE)

#ifndef XR_D3D9COMPAT_QUERY_RTYPE
// dual-source blend factors (dx10StateUtils.cpp)
#define D3DBLEND_SRCCOLOR2 ((D3DBLEND)16)
#define D3DBLEND_INVSRCCOLOR2 ((D3DBLEND)17)

typedef enum _D3DRESOURCETYPE
{
	D3DRTYPE_SURFACE = 1,
	D3DRTYPE_VOLUME = 2,
	D3DRTYPE_TEXTURE = 3,
	D3DRTYPE_VOLUMETEXTURE = 4,
	D3DRTYPE_CUBETEXTURE = 5,
	D3DRTYPE_VERTEXBUFFER = 6,
	D3DRTYPE_INDEXBUFFER = 7,
	D3DRTYPE_FORCE_DWORD = 0x7fffffff
} D3DRESOURCETYPE;

typedef enum _D3DQUERYTYPE
{
	D3DQUERYTYPE_VCACHE = 4,
	D3DQUERYTYPE_RESOURCEMANAGER = 5,
	D3DQUERYTYPE_VERTEXSTATS = 6,
	D3DQUERYTYPE_EVENT = 8,
	D3DQUERYTYPE_OCCLUSION = 9,
	D3DQUERYTYPE_TIMESTAMP = 10,
	D3DQUERYTYPE_TIMESTAMPDISJOINT = 11,
	D3DQUERYTYPE_TIMESTAMPFREQ = 12,
	D3DQUERYTYPE_PIPELINETIMINGS = 13,
	D3DQUERYTYPE_INTERFACETIMINGS = 14,
	D3DQUERYTYPE_VERTEXTIMINGS = 15,
	D3DQUERYTYPE_PIXELTIMINGS = 16,
	D3DQUERYTYPE_BANDWIDTHTIMINGS = 17,
	D3DQUERYTYPE_CACHEUTILIZATION = 18,
	D3DQUERYTYPE_MEMORYPRESSURE = 19
} D3DQUERYTYPE;
#endif // XR_D3D9COMPAT_QUERY_RTYPE

#ifndef XR_D3DX9COMPAT_VECTOR_OPS
inline D3DXVECTOR2 operator+(const D3DXVECTOR2& a, const D3DXVECTOR2& b) { return D3DXVECTOR2(a.x + b.x, a.y + b.y); }
inline D3DXVECTOR2 operator-(const D3DXVECTOR2& a, const D3DXVECTOR2& b) { return D3DXVECTOR2(a.x - b.x, a.y - b.y); }
inline D3DXVECTOR2 operator-(const D3DXVECTOR2& a) { return D3DXVECTOR2(-a.x, -a.y); }
inline D3DXVECTOR2 operator*(const D3DXVECTOR2& a, float f) { return D3DXVECTOR2(a.x * f, a.y * f); }
inline D3DXVECTOR2 operator*(float f, const D3DXVECTOR2& a) { return D3DXVECTOR2(a.x * f, a.y * f); }
inline D3DXVECTOR2 operator/(const D3DXVECTOR2& a, float f) { return D3DXVECTOR2(a.x / f, a.y / f); }
inline D3DXVECTOR2& operator+=(D3DXVECTOR2& a, const D3DXVECTOR2& b) { a.x += b.x; a.y += b.y; return a; }
inline D3DXVECTOR2& operator-=(D3DXVECTOR2& a, const D3DXVECTOR2& b) { a.x -= b.x; a.y -= b.y; return a; }
inline D3DXVECTOR2& operator*=(D3DXVECTOR2& a, float f) { a.x *= f; a.y *= f; return a; }
inline D3DXVECTOR2& operator/=(D3DXVECTOR2& a, float f) { a.x /= f; a.y /= f; return a; }

inline D3DXVECTOR3 operator*(float f, const D3DXVECTOR3& a) { return D3DXVECTOR3(a.x * f, a.y * f, a.z * f); }

inline D3DXVECTOR4 operator+(const D3DXVECTOR4& a, const D3DXVECTOR4& b) { return D3DXVECTOR4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
inline D3DXVECTOR4 operator-(const D3DXVECTOR4& a, const D3DXVECTOR4& b) { return D3DXVECTOR4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
inline D3DXVECTOR4 operator-(const D3DXVECTOR4& a) { return D3DXVECTOR4(-a.x, -a.y, -a.z, -a.w); }
inline D3DXVECTOR4 operator*(const D3DXVECTOR4& a, float f) { return D3DXVECTOR4(a.x * f, a.y * f, a.z * f, a.w * f); }
inline D3DXVECTOR4 operator*(float f, const D3DXVECTOR4& a) { return D3DXVECTOR4(a.x * f, a.y * f, a.z * f, a.w * f); }
inline D3DXVECTOR4 operator/(const D3DXVECTOR4& a, float f) { return D3DXVECTOR4(a.x / f, a.y / f, a.z / f, a.w / f); }
inline D3DXVECTOR4& operator+=(D3DXVECTOR4& a, const D3DXVECTOR4& b) { a.x += b.x; a.y += b.y; a.z += b.z; a.w += b.w; return a; }
inline D3DXVECTOR4& operator-=(D3DXVECTOR4& a, const D3DXVECTOR4& b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; a.w -= b.w; return a; }
inline D3DXVECTOR4& operator*=(D3DXVECTOR4& a, float f) { a.x *= f; a.y *= f; a.z *= f; a.w *= f; return a; }
inline D3DXVECTOR4& operator/=(D3DXVECTOR4& a, float f) { a.x /= f; a.y /= f; a.z /= f; a.w /= f; return a; }
#endif // XR_D3DX9COMPAT_VECTOR_OPS

#endif // XR_PLATFORM_APPLE
