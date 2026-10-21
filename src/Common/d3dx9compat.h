// The slice of D3DX9 that engine and renderer sources still use without a D3D9 runtime behind it: the vertex
// declaration helpers, the math types and the few matrix, vector and plane functions (D3D conventions: row vectors,
// row-major matrices, left-handed projections), and 16-bit float conversion. Texture, surface and shader compiler
// calls are not provided here: the D3D11 shim (d3dx11.h, d3dcompiler.h) covers the DX11 renderer, and the D3D9-only
// paths are replaced by the renderer port.
#pragma once

#include "Platform.h"
#include "d3d9compat.h"

#ifdef __cplusplus
#include <cmath>
#include <cstring>

#define D3DX_DEFAULT ((UINT)-1)
#define D3DX_DEFAULT_NONPOW2 ((UINT)-2)
#define D3DX_DEFAULT_FLOAT FLT_MAX
#define D3DX_PI 3.14159265358979323846f
#define MAX_FVF_DECL_SIZE (MAXD3DDECLLENGTH + 1)

// ---------------------------------------------------------------------------------------------------------------
// Vertex declarations
// ---------------------------------------------------------------------------------------------------------------
inline UINT D3DXGetDeclTypeSize_(BYTE type)
{
	switch (type)
	{
	case D3DDECLTYPE_FLOAT1: return 4;
	case D3DDECLTYPE_FLOAT2: return 8;
	case D3DDECLTYPE_FLOAT3: return 12;
	case D3DDECLTYPE_FLOAT4: return 16;
	case D3DDECLTYPE_D3DCOLOR:
	case D3DDECLTYPE_UBYTE4:
	case D3DDECLTYPE_SHORT2:
	case D3DDECLTYPE_UBYTE4N:
	case D3DDECLTYPE_SHORT2N:
	case D3DDECLTYPE_USHORT2N:
	case D3DDECLTYPE_UDEC3:
	case D3DDECLTYPE_DEC3N:
	case D3DDECLTYPE_FLOAT16_2: return 4;
	case D3DDECLTYPE_SHORT4:
	case D3DDECLTYPE_SHORT4N:
	case D3DDECLTYPE_USHORT4N:
	case D3DDECLTYPE_FLOAT16_4: return 8;
	}
	return 0;
}

// Number of elements before the D3DDECL_END() terminator.
inline UINT D3DXGetDeclLength(const D3DVERTEXELEMENT9* decl)
{
	UINT n = 0;
	while (decl[n].Stream != 0xFF) ++n;
	return n;
}

// Size in bytes of one vertex of the given stream.
inline UINT D3DXGetDeclVertexSize(const D3DVERTEXELEMENT9* decl, DWORD stream)
{
	UINT size = 0;
	for (; decl->Stream != 0xFF; ++decl)
	{
		if (decl->Stream != stream) continue;
		UINT end = decl->Offset + D3DXGetDeclTypeSize_(decl->Type);
		if (end > size) size = end;
	}
	return size;
}

// Size in bytes of an FVF vertex.
inline UINT D3DXGetFVFVertexSize(DWORD fvf)
{
	UINT size = 0;
	switch (fvf & D3DFVF_POSITION_MASK)
	{
	case D3DFVF_XYZ: size += 12; break;
	case D3DFVF_XYZRHW: size += 16; break;
	case D3DFVF_XYZB1: size += 16; break;
	case D3DFVF_XYZB2: size += 20; break;
	case D3DFVF_XYZB3: size += 24; break;
	case D3DFVF_XYZB4: size += 28; break;
	case D3DFVF_XYZB5: size += 32; break;
	case D3DFVF_XYZW: size += 16; break;
	}
	if (fvf & D3DFVF_NORMAL) size += 12;
	if (fvf & D3DFVF_PSIZE) size += 4;
	if (fvf & D3DFVF_DIFFUSE) size += 4;
	if (fvf & D3DFVF_SPECULAR) size += 4;
	const UINT tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (UINT i = 0; i < tex_count; ++i)
	{
		switch ((fvf >> (16 + i * 2)) & 3)
		{
		case D3DFVF_TEXTUREFORMAT2: size += 8; break;
		case D3DFVF_TEXTUREFORMAT3: size += 12; break;
		case D3DFVF_TEXTUREFORMAT4: size += 16; break;
		case D3DFVF_TEXTUREFORMAT1: size += 4; break;
		}
	}
	return size;
}

// FVF -> declaration, as D3DX does it (stream 0, elements in FVF order, D3DDECL_END() last).
inline HRESULT D3DXDeclaratorFromFVF(DWORD fvf, D3DVERTEXELEMENT9 decl[MAX_FVF_DECL_SIZE])
{
	UINT n = 0;
	WORD offset = 0;
	auto add = [&](BYTE type, BYTE usage, BYTE usage_index)
	{
		decl[n].Stream = 0;
		decl[n].Offset = offset;
		decl[n].Type = type;
		decl[n].Method = D3DDECLMETHOD_DEFAULT;
		decl[n].Usage = usage;
		decl[n].UsageIndex = usage_index;
		offset = (WORD)(offset + D3DXGetDeclTypeSize_(type));
		++n;
	};
	const DWORD pos = fvf & D3DFVF_POSITION_MASK;
	if (pos == D3DFVF_XYZ) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
	else if (pos == D3DFVF_XYZRHW) add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0);
	else if (pos == D3DFVF_XYZW) add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
	else if (pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5)
	{
		add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
		const UINT blends = (pos - D3DFVF_XYZRHW) / 2; // XYZB1 (6) carries 1 blend weight, XYZB5 (14) carries 5
		if (fvf & D3DFVF_LASTBETA_UBYTE4)
		{
			if (blends > 1) add((BYTE)(D3DDECLTYPE_FLOAT1 + blends - 2), D3DDECLUSAGE_BLENDWEIGHT, 0);
			add(D3DDECLTYPE_UBYTE4, D3DDECLUSAGE_BLENDINDICES, 0);
		}
		else if (fvf & D3DFVF_LASTBETA_D3DCOLOR)
		{
			if (blends > 1) add((BYTE)(D3DDECLTYPE_FLOAT1 + blends - 2), D3DDECLUSAGE_BLENDWEIGHT, 0);
			add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDINDICES, 0);
		}
		else add((BYTE)(D3DDECLTYPE_FLOAT1 + blends - 1), D3DDECLUSAGE_BLENDWEIGHT, 0);
	}
	if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
	if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
	if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
	if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);
	const UINT tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (UINT i = 0; i < tex_count && n < MAXD3DDECLLENGTH; ++i)
	{
		BYTE type = D3DDECLTYPE_FLOAT2;
		switch ((fvf >> (16 + i * 2)) & 3)
		{
		case D3DFVF_TEXTUREFORMAT1: type = D3DDECLTYPE_FLOAT1; break;
		case D3DFVF_TEXTUREFORMAT2: type = D3DDECLTYPE_FLOAT2; break;
		case D3DFVF_TEXTUREFORMAT3: type = D3DDECLTYPE_FLOAT3; break;
		case D3DFVF_TEXTUREFORMAT4: type = D3DDECLTYPE_FLOAT4; break;
		}
		add(type, D3DDECLUSAGE_TEXCOORD, (BYTE)i);
	}
	const D3DVERTEXELEMENT9 end = D3DDECL_END();
	decl[n] = end;
	return S_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Math types
// ---------------------------------------------------------------------------------------------------------------
struct D3DXVECTOR2
{
	float x, y;
	D3DXVECTOR2() : x(0), y(0) {}
	D3DXVECTOR2(float fx, float fy) : x(fx), y(fy) {}
	D3DXVECTOR2(const float* p) : x(p[0]), y(p[1]) {}
	operator float*() { return &x; }
	operator const float*() const { return &x; }
#ifndef XR_D3DX9COMPAT_VECTOR_OPS
#define XR_D3DX9COMPAT_VECTOR_OPS // operators of D3DXVECTOR2 and D3DXVECTOR4, float * D3DXVECTOR2/3/4 after D3DXVECTOR4
#endif
	D3DXVECTOR2 operator+(const D3DXVECTOR2& v) const { return D3DXVECTOR2(x + v.x, y + v.y); }
	D3DXVECTOR2 operator-(const D3DXVECTOR2& v) const { return D3DXVECTOR2(x - v.x, y - v.y); }
	D3DXVECTOR2 operator*(float f) const { return D3DXVECTOR2(x * f, y * f); }
	D3DXVECTOR2 operator/(float f) const { return D3DXVECTOR2(x / f, y / f); }
	D3DXVECTOR2 operator-() const { return D3DXVECTOR2(-x, -y); }
	D3DXVECTOR2& operator+=(const D3DXVECTOR2& v) { x += v.x; y += v.y; return *this; }
	D3DXVECTOR2& operator-=(const D3DXVECTOR2& v) { x -= v.x; y -= v.y; return *this; }
	D3DXVECTOR2& operator*=(float f) { x *= f; y *= f; return *this; }
	D3DXVECTOR2& operator/=(float f) { x /= f; y /= f; return *this; }
};

struct D3DXVECTOR3 : public D3DVECTOR
{
	D3DXVECTOR3() { x = y = z = 0; }
	D3DXVECTOR3(float fx, float fy, float fz) { x = fx; y = fy; z = fz; }
	D3DXVECTOR3(const float* p) { x = p[0]; y = p[1]; z = p[2]; }
	operator float*() { return &x; }
	operator const float*() const { return &x; }
	D3DXVECTOR3 operator+(const D3DXVECTOR3& v) const { return D3DXVECTOR3(x + v.x, y + v.y, z + v.z); }
	D3DXVECTOR3 operator-(const D3DXVECTOR3& v) const { return D3DXVECTOR3(x - v.x, y - v.y, z - v.z); }
	D3DXVECTOR3 operator*(float f) const { return D3DXVECTOR3(x * f, y * f, z * f); }
	D3DXVECTOR3 operator/(float f) const { return D3DXVECTOR3(x / f, y / f, z / f); }
	D3DXVECTOR3 operator-() const { return D3DXVECTOR3(-x, -y, -z); }
	D3DXVECTOR3& operator+=(const D3DXVECTOR3& v) { x += v.x; y += v.y; z += v.z; return *this; }
	D3DXVECTOR3& operator-=(const D3DXVECTOR3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
	D3DXVECTOR3& operator*=(float f) { x *= f; y *= f; z *= f; return *this; }
};

struct D3DXVECTOR4
{
	float x, y, z, w;
	D3DXVECTOR4() : x(0), y(0), z(0), w(0) {}
	D3DXVECTOR4(float fx, float fy, float fz, float fw) : x(fx), y(fy), z(fz), w(fw) {}
	D3DXVECTOR4(const float* p) : x(p[0]), y(p[1]), z(p[2]), w(p[3]) {}
	operator float*() { return &x; }
	operator const float*() const { return &x; }
	D3DXVECTOR4 operator+(const D3DXVECTOR4& v) const { return D3DXVECTOR4(x + v.x, y + v.y, z + v.z, w + v.w); }
	D3DXVECTOR4 operator-(const D3DXVECTOR4& v) const { return D3DXVECTOR4(x - v.x, y - v.y, z - v.z, w - v.w); }
	D3DXVECTOR4 operator*(float f) const { return D3DXVECTOR4(x * f, y * f, z * f, w * f); }
	D3DXVECTOR4 operator/(float f) const { return D3DXVECTOR4(x / f, y / f, z / f, w / f); }
	D3DXVECTOR4 operator-() const { return D3DXVECTOR4(-x, -y, -z, -w); }
	D3DXVECTOR4& operator+=(const D3DXVECTOR4& v) { x += v.x; y += v.y; z += v.z; w += v.w; return *this; }
	D3DXVECTOR4& operator-=(const D3DXVECTOR4& v) { x -= v.x; y -= v.y; z -= v.z; w -= v.w; return *this; }
	D3DXVECTOR4& operator*=(float f) { x *= f; y *= f; z *= f; w *= f; return *this; }
	D3DXVECTOR4& operator/=(float f) { x /= f; y /= f; z /= f; w /= f; return *this; }
};

inline D3DXVECTOR2 operator*(float f, const D3DXVECTOR2& v) { return v * f; }
inline D3DXVECTOR3 operator*(float f, const D3DXVECTOR3& v) { return v * f; }
inline D3DXVECTOR4 operator*(float f, const D3DXVECTOR4& v) { return v * f; }

struct D3DXPLANE
{
	float a, b, c, d;
	D3DXPLANE() : a(0), b(0), c(0), d(0) {}
	D3DXPLANE(float fa, float fb, float fc, float fd) : a(fa), b(fb), c(fc), d(fd) {}
	D3DXPLANE(const float* p) : a(p[0]), b(p[1]), c(p[2]), d(p[3]) {}
	operator float*() { return &a; }
	operator const float*() const { return &a; }
};

struct D3DXMATRIX : public D3DMATRIX
{
	D3DXMATRIX() { memset(m, 0, sizeof(m)); }
	D3DXMATRIX(const float* p) { memcpy(m, p, sizeof(m)); }
	D3DXMATRIX(const D3DMATRIX& o) { memcpy(m, o.m, sizeof(m)); }
	D3DXMATRIX(float f11, float f12, float f13, float f14, float f21, float f22, float f23, float f24, float f31, float f32,
	           float f33, float f34, float f41, float f42, float f43, float f44)
	{
		_11 = f11; _12 = f12; _13 = f13; _14 = f14;
		_21 = f21; _22 = f22; _23 = f23; _24 = f24;
		_31 = f31; _32 = f32; _33 = f33; _34 = f34;
		_41 = f41; _42 = f42; _43 = f43; _44 = f44;
	}
	float& operator()(UINT row, UINT col) { return m[row][col]; }
	float operator()(UINT row, UINT col) const { return m[row][col]; }
	operator float*() { return &_11; }
	operator const float*() const { return &_11; }
	D3DXMATRIX operator*(const D3DXMATRIX& o) const
	{
		D3DXMATRIX r;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j] + m[i][3] * o.m[3][j];
		return r;
	}
	D3DXMATRIX& operator*=(const D3DXMATRIX& o) { return *this = *this * o; }
};

// 16-bit float storage type (the 3D fluid renderer converts its initial data with it).
struct D3DXFLOAT16
{
	uint16_t value;
	D3DXFLOAT16() : value(0) {}
};

inline void D3DXFloat32To16Array(D3DXFLOAT16* out, const float* in, UINT n)
{
	for (UINT i = 0; i < n; ++i)
	{
		__fp16 h = (__fp16)in[i];
		memcpy(&out[i].value, &h, sizeof(h));
	}
}

inline void D3DXFloat16To32Array(float* out, const D3DXFLOAT16* in, UINT n)
{
	for (UINT i = 0; i < n; ++i)
	{
		__fp16 h;
		memcpy(&h, &in[i].value, sizeof(h));
		out[i] = (float)h;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Math functions
// ---------------------------------------------------------------------------------------------------------------
inline D3DXMATRIX* D3DXMatrixIdentity(D3DXMATRIX* out)
{
	memset(out->m, 0, sizeof(out->m));
	out->_11 = out->_22 = out->_33 = out->_44 = 1.0f;
	return out;
}

inline D3DXMATRIX* D3DXMatrixMultiply(D3DXMATRIX* out, const D3DXMATRIX* a, const D3DXMATRIX* b)
{
	D3DXMATRIX r = *a * *b;
	*out = r;
	return out;
}

inline D3DXMATRIX* D3DXMatrixTranspose(D3DXMATRIX* out, const D3DXMATRIX* in)
{
	D3DXMATRIX r;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) r.m[i][j] = in->m[j][i];
	*out = r;
	return out;
}

inline D3DXMATRIX* D3DXMatrixTranslation(D3DXMATRIX* out, float x, float y, float z)
{
	D3DXMatrixIdentity(out);
	out->_41 = x;
	out->_42 = y;
	out->_43 = z;
	return out;
}

inline D3DXMATRIX* D3DXMatrixScaling(D3DXMATRIX* out, float x, float y, float z)
{
	D3DXMatrixIdentity(out);
	out->_11 = x;
	out->_22 = y;
	out->_33 = z;
	return out;
}

inline D3DXMATRIX* D3DXMatrixOrthoOffCenterLH(D3DXMATRIX* out, float l, float r, float b, float t, float zn, float zf)
{
	D3DXMatrixIdentity(out);
	out->_11 = 2.0f / (r - l);
	out->_22 = 2.0f / (t - b);
	out->_33 = 1.0f / (zf - zn);
	out->_41 = (l + r) / (l - r);
	out->_42 = (t + b) / (b - t);
	out->_43 = zn / (zn - zf);
	return out;
}

// General 4x4 inverse (cofactor expansion). Returns null (and a determinant of 0) for a singular matrix.
inline D3DXMATRIX* D3DXMatrixInverse(D3DXMATRIX* out, float* determinant, const D3DXMATRIX* in)
{
	const float* m = &in->_11;
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (determinant) *determinant = det;
	if (det == 0.0f) return nullptr;
	const float inv_det = 1.0f / det;
	float* o = &out->_11;
	for (int i = 0; i < 16; ++i) o[i] = inv[i] * inv_det;
	return out;
}

inline float D3DXVec2Dot(const D3DXVECTOR2* a, const D3DXVECTOR2* b) { return a->x * b->x + a->y * b->y; }
inline float D3DXVec2Length(const D3DXVECTOR2* v) { return sqrtf(v->x * v->x + v->y * v->y); }
inline float D3DXVec3Dot(const D3DXVECTOR3* a, const D3DXVECTOR3* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
inline float D3DXVec3Length(const D3DXVECTOR3* v) { return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z); }

inline D3DXVECTOR3* D3DXVec3Cross(D3DXVECTOR3* out, const D3DXVECTOR3* a, const D3DXVECTOR3* b)
{
	D3DXVECTOR3 r(a->y * b->z - a->z * b->y, a->z * b->x - a->x * b->z, a->x * b->y - a->y * b->x);
	*out = r;
	return out;
}

inline D3DXVECTOR3* D3DXVec3Normalize(D3DXVECTOR3* out, const D3DXVECTOR3* v)
{
	const float len = D3DXVec3Length(v);
	if (len > 0.0f) *out = D3DXVECTOR3(v->x / len, v->y / len, v->z / len);
	else *out = D3DXVECTOR3(0, 0, 0);
	return out;
}

inline D3DXVECTOR4* D3DXVec3Transform(D3DXVECTOR4* out, const D3DXVECTOR3* v, const D3DXMATRIX* m)
{
	D3DXVECTOR4 r(v->x * m->_11 + v->y * m->_21 + v->z * m->_31 + m->_41, v->x * m->_12 + v->y * m->_22 + v->z * m->_32 + m->_42,
	              v->x * m->_13 + v->y * m->_23 + v->z * m->_33 + m->_43, v->x * m->_14 + v->y * m->_24 + v->z * m->_34 + m->_44);
	*out = r;
	return out;
}

inline D3DXVECTOR3* D3DXVec3TransformCoord(D3DXVECTOR3* out, const D3DXVECTOR3* v, const D3DXMATRIX* m)
{
	D3DXVECTOR4 t;
	D3DXVec3Transform(&t, v, m);
	const float w = t.w != 0.0f ? 1.0f / t.w : 1.0f;
	*out = D3DXVECTOR3(t.x * w, t.y * w, t.z * w);
	return out;
}

inline D3DXVECTOR3* D3DXVec3TransformNormal(D3DXVECTOR3* out, const D3DXVECTOR3* v, const D3DXMATRIX* m)
{
	D3DXVECTOR3 r(v->x * m->_11 + v->y * m->_21 + v->z * m->_31, v->x * m->_12 + v->y * m->_22 + v->z * m->_32,
	              v->x * m->_13 + v->y * m->_23 + v->z * m->_33);
	*out = r;
	return out;
}

inline D3DXVECTOR3* D3DXVec3TransformCoordArray(D3DXVECTOR3* out, UINT out_stride, const D3DXVECTOR3* in, UINT in_stride,
                                                const D3DXMATRIX* m, UINT n)
{
	for (UINT i = 0; i < n; ++i)
	{
		const D3DXVECTOR3* src = (const D3DXVECTOR3*)((const char*)in + i * in_stride);
		D3DXVECTOR3* dst = (D3DXVECTOR3*)((char*)out + i * out_stride);
		D3DXVec3TransformCoord(dst, src, m);
	}
	return out;
}

inline float D3DXPlaneDotCoord(const D3DXPLANE* p, const D3DXVECTOR3* v) { return p->a * v->x + p->b * v->y + p->c * v->z + p->d; }
inline float D3DXPlaneDotNormal(const D3DXPLANE* p, const D3DXVECTOR3* v) { return p->a * v->x + p->b * v->y + p->c * v->z; }

inline D3DXPLANE* D3DXPlaneNormalize(D3DXPLANE* out, const D3DXPLANE* p)
{
	const float len = sqrtf(p->a * p->a + p->b * p->b + p->c * p->c);
	if (len > 0.0f) *out = D3DXPLANE(p->a / len, p->b / len, p->c / len, p->d / len);
	else *out = D3DXPLANE(0, 0, 0, 0);
	return out;
}

// Transforms a plane by the inverse transpose of `m` (the caller passes the already inverted-transposed matrix, as D3DX does).
inline D3DXPLANE* D3DXPlaneTransform(D3DXPLANE* out, const D3DXPLANE* p, const D3DXMATRIX* m)
{
	D3DXPLANE r(p->a * m->_11 + p->b * m->_21 + p->c * m->_31 + p->d * m->_41,
	            p->a * m->_12 + p->b * m->_22 + p->c * m->_32 + p->d * m->_42,
	            p->a * m->_13 + p->b * m->_23 + p->c * m->_33 + p->d * m->_43,
	            p->a * m->_14 + p->b * m->_24 + p->c * m->_34 + p->d * m->_44);
	*out = r;
	return out;
}
#endif // __cplusplus
