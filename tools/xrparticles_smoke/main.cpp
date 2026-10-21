// xrparticles_smoke: headless check of xrParticles on Apple platforms.
//
// The SSE code in xrParticles (noise.cpp: iFloor_SSE; particle_actions_collection.cpp: PATurbulence::Execute) runs on
// sse2neon here. The tool compares it with a scalar reference written in plain C++:
//
//   [1] noise3 / fractalsum3 against a scalar copy of Perlin's noise (cvttss2si truncation is `(int)`), bit for bit
//   [2] PATurbulence::Execute against the scalar velocity update (the `#else` branch of the same function)
//   [3] file formats: sizes of the structures read raw from particle files, action Save/Load round trip, domains
//
// Exit status 0 means every check passed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "xrCore/xrCore.h"
#include "xrParticles/stdafx.h"
#include "xrParticles/particle_actions_collection.h"
#include "xrParticles/particle_effect.h"
#include "xrParticles/noise.h"

// defined by the renderer (ParticleEffectDef.cpp), read by PATurbulence::Execute
float ps_particle_update_coeff = 1.f;

extern void noise3Init();

using namespace PAPI;

namespace
{
int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
	++g_checks;
	if (!ok)
		++g_failures;
	printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
}

void engine_log(LPCSTR line) { printf("    | %s\n", line); }

struct Rng
{
	u64 s;
	explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
	u32 next()
	{
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return u32((s * 0x2545F4914F6CDD1Dull) >> 32);
	}
	float f(float lo, float hi) { return lo + (hi - lo) * (float(next() >> 8) / float(1 << 24)); }
};

// ---------------------------------------------------------------------------------------------------------------
// Scalar reference: noise.cpp as it reads without SSE, with `(int)` where the SSE build has _mm_cvtt_ss2si
// ---------------------------------------------------------------------------------------------------------------
namespace ref
{
const int B = 256;
int p[B + B + 2];
float g[B + B + 2][3];

void init()
{
	int i, j, k;
	float v[3], s;
	int rnd;

	srand(1);
	for (i = 0; i < B; i++)
	{
		do
		{
			for (j = 0; j < 3; j++)
			{
				rnd = rand();
				v[j] = float((rnd % (B + B)) - B) / B;
			}
			s = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
		}
		while (s > 1.0);
		s = sqrtf(s);
		for (j = 0; j < 3; j++)
			g[i][j] = v[j] / s;
	}
	for (i = 0; i < B; i++)
		p[i] = i;
	for (i = B; i > 0; i -= 2)
	{
		rnd = rand();
		k = p[i];
		p[i] = p[(j = rnd % B)];
		p[j] = k;
	}
	for (i = 0; i < B + 2; i++)
	{
		p[B + i] = p[i];
		for (j = 0; j < 3; j++)
			g[B + i][j] = g[i][j];
	}
}

inline void setup(float vec, int& b0, int& b1, float& r0, float& r1)
{
	const float t = vec + 10000.f;
	const int tt = (int)t;
	b0 = tt & (B - 1);
	b1 = (b0 + 1) & (B - 1);
	r0 = t - float(tt);
	r1 = r0 - 1.f;
}

inline float s_curve(float t) { return t * t * (3.f - 2.f * t); }
inline float lerp(float t, float a, float b) { return a + t * (b - a); }
inline float at(const float* q, float rx, float ry, float rz) { return rx * q[0] + ry * q[1] + rz * q[2]; }

float noise3(const Fvector& vec)
{
	int bx0, bx1, by0, by1, bz0, bz1;
	float rx0, rx1, ry0, ry1, rz0, rz1;
	setup(vec.x, bx0, bx1, rx0, rx1);
	setup(vec.y, by0, by1, ry0, ry1);
	setup(vec.z, bz0, bz1, rz0, rz1);

	const int i = p[bx0], j = p[bx1];
	const int b00 = p[i + by0], b10 = p[j + by0], b01 = p[i + by1], b11 = p[j + by1];
	const float sx = s_curve(rx0), sy = s_curve(ry0), sz = s_curve(rz0);

	float u = at(g[b00 + bz0], rx0, ry0, rz0);
	float v = at(g[b10 + bz0], rx1, ry0, rz0);
	const float a = lerp(sx, u, v);
	u = at(g[b01 + bz0], rx0, ry1, rz0);
	v = at(g[b11 + bz0], rx1, ry1, rz0);
	const float b = lerp(sx, u, v);
	const float c = lerp(sy, a, b);

	u = at(g[b00 + bz1], rx0, ry0, rz1);
	v = at(g[b10 + bz1], rx1, ry0, rz1);
	const float a2 = lerp(sx, u, v);
	u = at(g[b01 + bz1], rx0, ry1, rz1);
	v = at(g[b11 + bz1], rx1, ry1, rz1);
	const float b2 = lerp(sx, u, v);
	const float d = lerp(sy, a2, b2);
	return 1.5f * lerp(sz, c, d);
}

float fractalsum3(const Fvector& v, float freq, int octaves)
{
	float sum = 0.0;
	Fvector v_;
	const float boost = freq;
	v_[0] = v[0] * freq;
	v_[1] = v[1] * freq;
	v_[2] = v[2] * freq;
	for (int i = 0; i < octaves; i++)
	{
		sum += ref::noise3(v_) / freq;
		freq *= 2.059f;
		v_[0] = v[0] * freq;
		v_[1] = v[1] * freq;
		v_[2] = v[2] * freq;
	}
	return sum * boost;
}
} // namespace ref

void test_noise(Rng& rng)
{
	printf("\n[1] noise3 / fractalsum3 (iFloor_SSE on sse2neon) against the scalar reference\n");
	noise3Init();
	ref::init();

	long compared = 0, bad = 0;
	double worst = 0;
	auto compare = [&](const Fvector& v, float freq, int octaves)
	{
		const float a = ::noise3(v), b = ref::noise3(v);
		const float fa = ::fractalsum3(v, freq, octaves), fb = ref::fractalsum3(v, freq, octaves);
		++compared;
		if (!(a == b) || !(fa == fb))
		{
			if (++bad <= 5)
				printf("    mismatch at %g %g %g: noise3 %.9g vs %.9g, fractalsum3 %.9g vs %.9g\n", v.x, v.y, v.z, a, b, fa, fb);
		}
		worst = std::max(worst, (double)std::fabs(a - b));
	};
	for (int n = 0; n < 60000; ++n)
	{
		Fvector v;
		v.set(rng.f(-60, 60), rng.f(-60, 60), rng.f(-60, 60));
		compare(v, rng.f(0.2f, 3.f), 1 + int(rng.next() % 4));
	}
	// integers and halves: on lattice planes the fractional part is exactly 0 and the floor is exact
	for (int x = -4; x <= 4; ++x)
		for (int y = -4; y <= 4; ++y)
			for (int z = -4; z <= 4; ++z)
			{
				Fvector v;
				v.set(float(x) * 0.5f, float(y) * 0.5f, float(z));
				compare(v, 1.f, 2);
			}
	// the offset that keeps the argument positive is 10000: below it the conversion truncates, which is what SSE does
	for (int n = 0; n < 5000; ++n)
	{
		Fvector v;
		v.set(rng.f(-10100, -9900), rng.f(-10100, -9900), rng.f(-10100, -9900));
		compare(v, 1.f, 2);
	}
	char what[160];
	snprintf(what, sizeof(what), "%ld points: noise3 and fractalsum3 equal the scalar reference exactly (%ld differ, worst %.3g)",
	         compared, bad, worst);
	check(bad == 0, what);
	{
		// lattice points of the field are zero
		Fvector v;
		v.set(-10000.f + 3.f, -10000.f + 5.f, -10000.f + 7.f);
		check(::noise3(v) == 0.f, "noise3 is zero on a lattice point");
	}
}

void test_turbulence(Rng& rng)
{
	printf("\n[2] PATurbulence::Execute (SSE velocity update) against the scalar update\n");
	const u32 count = 400;
	PATurbulence act;
	act.type = PATurbulenceID;
	act.frequency = 1.7f;
	act.octaves = 3;
	act.magnitude = 0.8f;
	act.epsilon = 0.05f;
	act.offset.set(0.1f, 0.2f, -0.3f);
	act.age = 0.25f;
	const PATurbulence start = act;

	for (int round = 0; round < 4; ++round)
	{
		ParticleEffect effect(512);
		effect.p_count = count;
		std::vector<Particle> before(count);
		for (u32 i = 0; i < count; ++i)
		{
			Particle& m = effect.particles[i];
			memset(&m, 0, sizeof(m));
			m.pos.set(rng.f(-20, 20), rng.f(-20, 20), rng.f(-20, 20));
			// a spread of speeds, including resting particles
			const float speed = (i % 7 == 0) ? 0.f : rng.f(0.01f, 10.f);
			m.vel.set(rng.f(-1, 1), rng.f(-1, 1), rng.f(-1, 1));
			if (m.vel.magnitude() > 1e-3f)
				m.vel.normalize();
			m.vel.mul(speed);
			before[i] = m;
		}
		const float dt = 0.005f + 0.01f * round;
		act = start;
		float tm_max = 0.f;
		act.Execute(&effect, dt, tm_max);

		// scalar update, as in the non-SSE branch
		const float age = start.age + dt;
		long bad = 0;
		double worst = 0;
		for (u32 i = 0; i < count; ++i)
		{
			pVector pV, vX, vY, vZ, D, vel = before[i].vel;
			pV.mad(before[i].pos, start.offset, age);
			vX.set(pV.x + start.epsilon, pV.y, pV.z);
			vY.set(pV.x, pV.y + start.epsilon, pV.z);
			vZ.set(pV.x, pV.y, pV.z + start.epsilon);
			const float d = ref::fractalsum3(pV, start.frequency, start.octaves);
			D.x = (ref::fractalsum3(vX, start.frequency, start.octaves) - d) * start.magnitude;
			D.y = (ref::fractalsum3(vY, start.frequency, start.octaves) - d) * start.magnitude;
			D.z = (ref::fractalsum3(vZ, start.frequency, start.octaves) - d) * start.magnitude;
			const float orig = vel.magnitude();
			vel.add(D);
			const float now = vel.magnitude();
			vel.mul(orig / now);
			const Fvector& got = effect.particles[i].vel;
			for (int a = 0; a < 3; ++a)
			{
				const double diff = std::fabs(double(got[a]) - double(vel[a]));
				worst = std::max(worst, diff / std::max(1.0, std::fabs((double)vel[a])));
			}
			if (!(std::fabs(got.x - vel.x) <= 2e-5f * std::max(1.f, std::fabs(vel.x)) &&
				  std::fabs(got.y - vel.y) <= 2e-5f * std::max(1.f, std::fabs(vel.y)) &&
				  std::fabs(got.z - vel.z) <= 2e-5f * std::max(1.f, std::fabs(vel.z))))
			{
				if (++bad <= 5)
					printf("    particle %u: got %g %g %g, scalar %g %g %g\n", i, got.x, got.y, got.z, vel.x, vel.y, vel.z);
			}
			// positions are not touched by the action
			if (effect.particles[i].pos.x != before[i].pos.x)
				++bad;
		}
		char what[200];
		snprintf(what, sizeof(what), "dt %.3f: %u particles, %ld outside 2e-5 of the scalar result (worst relative %.2g)", dt,
		         count, bad, worst);
		check(bad == 0, what);
		check(std::fabs(act.age - age) < 1e-6f, "the action's age advanced by dt");
	}
}

void test_formats()
{
	printf("\n[3] file formats and actions\n");
	// pDomain, ParticleEffect and Particle are written/read as raw bytes by the particle editor and the .pe/.pg loaders
	check(sizeof(pDomain) == 68, "sizeof(pDomain) == 68 (stored raw in action lists)");
	check(sizeof(Particle) == 76, "sizeof(Particle) == 76");

	// Save / Load round trip of a PATurbulence through the in-memory writer
	PATurbulence a;
	a.type = PATurbulenceID;
	a.m_Flags.assign(0x5);
	a.frequency = 2.5f;
	a.octaves = 4;
	a.magnitude = 0.75f;
	a.epsilon = 0.125f;
	a.offset.set(1.f, -2.f, 3.f);
	CMemoryWriter W;
	a.Save(W);
	IReader R(W.pointer(), (int)W.size());
	PATurbulence b;
	b.Load(R);
	check(R.eof(), "Load consumes exactly what Save wrote");
	check(b.frequency == a.frequency && b.octaves == a.octaves && b.magnitude == a.magnitude &&
		  b.epsilon == a.epsilon && b.offset.x == 1.f && b.offset.y == -2.f && b.offset.z == 3.f &&
		  b.m_Flags.get() == a.m_Flags.get() && b.type == a.type,
	      "PATurbulence survives Save/Load");

	// domains
	pDomain sphere(PDSphere, 0.f, 0.f, 0.f, 2.f, 0.f);
	pVector in(1.f, 1.f, 0.f), out(3.f, 0.f, 0.f);
	check(sphere.Within(in) && !sphere.Within(out), "a sphere domain of radius 2 contains (1,1,0) but not (3,0,0)");
	bool all_inside = true;
	for (int i = 0; i < 200; ++i)
	{
		pVector p;
		sphere.Generate(p);
		all_inside &= p.length() <= 2.0001f;
	}
	check(all_inside, "Generate stays inside the sphere");
	pDomain box(PDBox, -1.f, -1.f, -1.f, 1.f, 1.f, 1.f);
	check(box.Within(pVector(0.5f, -0.5f, 0.f)) && !box.Within(pVector(1.5f, 0.f, 0.f)), "a box domain");

	// the manager builds actions by type
	IParticleManager* pm = ParticleManager();
	ParticleAction* pa = pm->CreateAction(PATurbulenceID);
	check(pa != nullptr && pa->type == PATurbulenceID, "the manager creates a turbulence action");
	xr_delete(pa);
}
} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	printf("xrparticles_smoke\n");
	Debug._initialize(false);
	Core._initialize("xrparticles_smoke", engine_log, FALSE);
	Rng rng(7);
	test_noise(rng);
	test_turbulence(rng);
	test_formats();
	printf("\n%d checks, %d failed\n", g_checks, g_failures);
	Core._destroy();
	return g_failures ? 1 : 0;
}
