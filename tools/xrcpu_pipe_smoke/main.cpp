// xrcpu_pipe_smoke: headless check of xrCPU_Pipe on Apple platforms.
//
//   [1] ttapi: the worker pool runs every queued worker exactly once per ttapi_RunAllWorkers() and the call is
//       synchronous (the Windows spin pool is replaced by Grand Central Dispatch)
//   [2] skinning: the 1/2/3/4-weight CPU skinners against an independent double precision reference, and the threaded
//       4-weight skinner (xrSkin4W_thread, on ttapi) against the single-threaded one
//   [3] the vertex formats the skinners read straight from model files
//
// Exit status 0 means every check passed.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "xrCPU_Pipe/StdAfx.h"

extern "C" void xrBind_PSGP(xrDispatchTable* T, _processor_info* ID);
extern xrSkin1W xrSkin1W_x86;
extern xrSkin2W xrSkin2W_x86;
extern xrSkin3W xrSkin3W_x86;
extern xrSkin4W xrSkin4W_x86;
extern xrSkin4W xrSkin4W_thread;

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
	u32 n(u32 count) { return next() % count; }
};

// ---------------------------------------------------------------------------------------------------------------
// ttapi
// ---------------------------------------------------------------------------------------------------------------
struct Slot
{
	std::atomic<int> runs{0};
	std::thread::id thread;
	double result = 0;
	char pad[64];
};

void slot_worker(LPVOID p)
{
	Slot* s = static_cast<Slot*>(p);
	// some real work, so that a worker which returned early would be noticed
	double acc = 0;
	for (int i = 1; i < 20000; ++i)
		acc += std::sqrt(double(i));
	s->result = acc;
	s->thread = std::this_thread::get_id();
	s->runs.fetch_add(1);
}

void test_ttapi()
{
	printf("\n[1] ttapi worker pool\n");
	const DWORD workers = ttapi_Init(&CPU::ID);
	printf("  workers: %u (hardware threads: %u)\n", workers, std::thread::hardware_concurrency());
	check(workers >= 1, "ttapi_Init reports at least one worker");
	check(ttapi_GetWorkersCount() == workers, "ttapi_GetWorkersCount agrees");
	check(ttapi_Init(&CPU::ID) == workers, "a second ttapi_Init is a no-op that returns the same count");

	std::vector<Slot> slots(workers);
	std::set<std::thread::id> threads;
	bool exactly_once = true, synchronous = true;
	double expected = 0;
	for (int i = 1; i < 20000; ++i)
		expected += std::sqrt(double(i));
	for (int round = 0; round < 400; ++round)
	{
		const DWORD jobs = 1 + DWORD(round) % workers;
		for (DWORD i = 0; i < jobs; ++i)
		{
			slots[i].runs.store(0);
			slots[i].result = 0;
			ttapi_AddWorker(slot_worker, &slots[i]);
		}
		ttapi_RunAllWorkers();
		for (DWORD i = 0; i < jobs; ++i)
		{
			exactly_once &= slots[i].runs.load() == 1;
			synchronous &= slots[i].result == expected;
			threads.insert(slots[i].thread);
		}
	}
	check(exactly_once, "every queued worker ran exactly once per round (400 rounds, 1..N workers)");
	check(synchronous, "all results are in place when ttapi_RunAllWorkers returns");
	printf("  distinct threads that ran workers: %zu\n", threads.size());
	if (workers > 1)
		check(threads.size() > 1, "with several workers, more than one thread did the work");

	// an empty round, then shutdown and restart
	ttapi_RunAllWorkers();
	check(true, "ttapi_RunAllWorkers with nothing queued returns");
	ttapi_Done();
	check(ttapi_GetWorkersCount() == 0, "ttapi_Done releases the pool");
	check(ttapi_Init(&CPU::ID) == workers, "ttapi_Init works again after ttapi_Done");
}

// ---------------------------------------------------------------------------------------------------------------
// skinning
// ---------------------------------------------------------------------------------------------------------------
struct Vec
{
	double x, y, z;
};

// The skinners use M.transform_tiny / transform_dir of CBoneInstance::mRenderTransform: v * M with the translation in row 4
Vec xform_pos(const Fmatrix& m, const Fvector& v)
{
	return {v.x * m._11 + v.y * m._21 + v.z * m._31 + m._41, v.x * m._12 + v.y * m._22 + v.z * m._32 + m._42,
	        v.x * m._13 + v.y * m._23 + v.z * m._33 + m._43};
}
Vec xform_dir(const Fmatrix& m, const Fvector& v)
{
	return {v.x * m._11 + v.y * m._21 + v.z * m._31, v.x * m._12 + v.y * m._22 + v.z * m._32,
	        v.x * m._13 + v.y * m._23 + v.z * m._33};
}

void fill_vertex_common(Rng& rng, Fvector& P, Fvector& N, Fvector& T, Fvector& B, float& u, float& v)
{
	P.set(rng.f(-2, 2), rng.f(-2, 2), rng.f(-2, 2));
	N.set(rng.f(-1, 1), rng.f(-1, 1), rng.f(-1, 1));
	N.normalize_safe();
	T.set(rng.f(-1, 1), rng.f(-1, 1), rng.f(-1, 1));
	B.set(rng.f(-1, 1), rng.f(-1, 1), rng.f(-1, 1));
	u = rng.f(0, 1);
	v = rng.f(0, 1);
}

std::vector<CBoneInstance> make_bones(Rng& rng, u32 count)
{
	std::vector<CBoneInstance> bones(count);
	for (CBoneInstance& b : bones)
	{
		memset(&b, 0, sizeof(b));
		Fmatrix& m = b.mRenderTransform;
		m.identity();
		m._11 = rng.f(0.5f, 1.5f); m._12 = rng.f(-0.5f, 0.5f); m._13 = rng.f(-0.5f, 0.5f);
		m._21 = rng.f(-0.5f, 0.5f); m._22 = rng.f(0.5f, 1.5f); m._23 = rng.f(-0.5f, 0.5f);
		m._31 = rng.f(-0.5f, 0.5f); m._32 = rng.f(-0.5f, 0.5f); m._33 = rng.f(0.5f, 1.5f);
		m._41 = rng.f(-5, 5); m._42 = rng.f(-5, 5); m._43 = rng.f(-5, 5);
	}
	return bones;
}

bool close(const Fvector& got, const Vec& want, double tol = 2e-5)
{
	return std::fabs(got.x - want.x) <= tol * std::max(1.0, std::fabs(want.x)) &&
		std::fabs(got.y - want.y) <= tol * std::max(1.0, std::fabs(want.y)) &&
		std::fabs(got.z - want.z) <= tol * std::max(1.0, std::fabs(want.z));
}

// a canary after the destination buffer catches writes past the end
constexpr u32 kCanary = 0xDEADBEEF;
struct DestBuffer
{
	std::vector<vertRender> v;
	explicit DestBuffer(u32 count) : v(count + 2)
	{
		memset(v.data(), 0xCD, v.size() * sizeof(vertRender));
		memcpy(&v[count], &kCanary, sizeof(kCanary));
	}
	bool canary_ok(u32 count) const { u32 c; memcpy(&c, &v[count], sizeof(c)); return c == kCanary; }
};

void test_skinning(Rng& rng)
{
	printf("\n[2] CPU skinning\n");
	const u32 bone_count = 24;
	std::vector<CBoneInstance> bones = make_bones(rng, bone_count);

	// bind the dispatch table the way the engine does
	xrDispatchTable table;
	memset(&table, 0, sizeof(table));
	xrBind_PSGP(&table, &CPU::ID);
	check(table.skin1W == xrSkin1W_x86 && table.skin2W == xrSkin2W_x86 && table.skin3W == xrSkin3W_x86,
	      "xrBind_PSGP binds the scalar 1/2/3-weight skinners");
	check(table.skin4W == (ttapi_GetWorkersCount() > 1 ? xrSkin4W_thread : xrSkin4W_x86),
	      "... and the threaded 4-weight skinner when there is more than one worker");

	const u32 counts[] = {1, 2, 7, 8, 9, 63, 100, 1000, 4097};
	long bad = 0;
	long compared = 0;
	for (u32 count : counts)
	{
		// 1 weight
		{
			std::vector<vertBoned1W> in(count);
			for (auto& s : in)
			{
				fill_vertex_common(rng, s.P, s.N, s.T, s.B, s.u, s.v);
				s.matrix = rng.n(bone_count);
			}
			DestBuffer out(count);
			table.skin1W(out.v.data(), in.data(), count, bones.data());
			bad += !out.canary_ok(count);
			for (u32 i = 0; i < count; ++i, ++compared)
			{
				const Fmatrix& m = bones[in[i].matrix].mRenderTransform;
				bad += !(close(out.v[i].P, xform_pos(m, in[i].P)) && close(out.v[i].N, xform_dir(m, in[i].N)) &&
						 out.v[i].u == in[i].u && out.v[i].v == in[i].v);
			}
		}
		// 2 weights
		{
			std::vector<vertBoned2W> in(count);
			for (auto& s : in)
			{
				fill_vertex_common(rng, s.P, s.N, s.T, s.B, s.u, s.v);
				s.matrix0 = u16(rng.n(bone_count));
				s.matrix1 = rng.n(4) ? u16(rng.n(bone_count)) : s.matrix0; // some vertices use one bone twice
				s.w = rng.f(0, 1);
			}
			DestBuffer out(count);
			table.skin2W(out.v.data(), in.data(), count, bones.data());
			bad += !out.canary_ok(count);
			for (u32 i = 0; i < count; ++i, ++compared)
			{
				const Fmatrix& m0 = bones[in[i].matrix0].mRenderTransform;
				const Fmatrix& m1 = bones[in[i].matrix1].mRenderTransform;
				Vec p0 = xform_pos(m0, in[i].P), p1 = xform_pos(m1, in[i].P);
				Vec n0 = xform_dir(m0, in[i].N), n1 = xform_dir(m1, in[i].N);
				const double w = in[i].w;
				const Vec p = in[i].matrix0 == in[i].matrix1 ? p0 : Vec{p0.x + (p1.x - p0.x) * w, p0.y + (p1.y - p0.y) * w, p0.z + (p1.z - p0.z) * w};
				const Vec n = in[i].matrix0 == in[i].matrix1 ? n0 : Vec{n0.x + (n1.x - n0.x) * w, n0.y + (n1.y - n0.y) * w, n0.z + (n1.z - n0.z) * w};
				bad += !(close(out.v[i].P, p) && close(out.v[i].N, n) && out.v[i].u == in[i].u);
			}
		}
		// 3 weights
		{
			std::vector<vertBoned3W> in(count);
			for (auto& s : in)
			{
				fill_vertex_common(rng, s.P, s.N, s.T, s.B, s.u, s.v);
				for (int k = 0; k < 3; ++k)
					s.m[k] = u16(rng.n(bone_count));
				s.w[0] = rng.f(0, 0.5f);
				s.w[1] = rng.f(0, 0.5f);
			}
			DestBuffer out(count);
			table.skin3W(out.v.data(), in.data(), count, bones.data());
			bad += !out.canary_ok(count);
			for (u32 i = 0; i < count; ++i, ++compared)
			{
				const double w[3] = {in[i].w[0], in[i].w[1], 1.0 - in[i].w[0] - in[i].w[1]};
				Vec p = {0, 0, 0}, n = {0, 0, 0};
				for (int k = 0; k < 3; ++k)
				{
					const Fmatrix& m = bones[in[i].m[k]].mRenderTransform;
					const Vec a = xform_pos(m, in[i].P), b = xform_dir(m, in[i].N);
					p = {p.x + a.x * w[k], p.y + a.y * w[k], p.z + a.z * w[k]};
					n = {n.x + b.x * w[k], n.y + b.y * w[k], n.z + b.z * w[k]};
				}
				bad += !(close(out.v[i].P, p) && close(out.v[i].N, n));
			}
		}
	}
	char what[160];
	snprintf(what, sizeof(what), "1/2/3-weight skinners: %ld vertices per skinner, %ld mismatches with the reference", compared, bad);
	check(bad == 0, what);

	// 4 weights: single-threaded against the reference, then the table entry (threaded) against single-threaded
	const u32 workers = ttapi_GetWorkersCount();
	std::vector<u32> big = {1, 5, 63, workers * 64 - 1, workers * 64, workers * 64 + 1, 1000, 12345, 100000};
	long bad4 = 0, bad_mt = 0;
	for (u32 count : big)
	{
		std::vector<vertBoned4W> in(count);
		for (auto& s : in)
		{
			fill_vertex_common(rng, s.P, s.N, s.T, s.B, s.u, s.v);
			for (int k = 0; k < 4; ++k)
				s.m[k] = u16(rng.n(bone_count));
			s.w[0] = rng.f(0, 0.3f);
			s.w[1] = rng.f(0, 0.3f);
			s.w[2] = rng.f(0, 0.3f);
		}
		DestBuffer single(count), threaded(count);
		xrSkin4W_x86(single.v.data(), in.data(), count, bones.data());
		table.skin4W(threaded.v.data(), in.data(), count, bones.data());
		bad_mt += !threaded.canary_ok(count) || !single.canary_ok(count);
		bad_mt += memcmp(single.v.data(), threaded.v.data(), count * sizeof(vertRender)) != 0;
		for (u32 i = 0; i < count; i += (count > 2000 ? 7 : 1))
		{
			const double w[4] = {in[i].w[0], in[i].w[1], in[i].w[2], 1.0 - in[i].w[0] - in[i].w[1] - in[i].w[2]};
			Vec p = {0, 0, 0}, n = {0, 0, 0};
			for (int k = 0; k < 4; ++k)
			{
				const Fmatrix& m = bones[in[i].m[k]].mRenderTransform;
				const Vec a = xform_pos(m, in[i].P), b = xform_dir(m, in[i].N);
				p = {p.x + a.x * w[k], p.y + a.y * w[k], p.z + a.z * w[k]};
				n = {n.x + b.x * w[k], n.y + b.y * w[k], n.z + b.z * w[k]};
			}
			bad4 += !(close(single.v[i].P, p) && close(single.v[i].N, n) && single.v[i].u == in[i].u);
		}
	}
	snprintf(what, sizeof(what), "4-weight skinner against the reference (%zu sizes up to 100000): %ld mismatches", big.size(), bad4);
	check(bad4 == 0, what);
	snprintf(what, sizeof(what), "the table's 4-weight skinner (%u workers) is bit-identical to the single-threaded one: %ld mismatches",
	         workers, bad_mt);
	check(bad_mt == 0, what);

	// the pool is reused every frame: hammer it
	{
		const u32 count = 5000;
		std::vector<vertBoned4W> in(count);
		for (auto& s : in)
		{
			fill_vertex_common(rng, s.P, s.N, s.T, s.B, s.u, s.v);
			for (int k = 0; k < 4; ++k)
				s.m[k] = u16(rng.n(bone_count));
			s.w[0] = s.w[1] = s.w[2] = 0.25f;
		}
		DestBuffer single(count);
		xrSkin4W_x86(single.v.data(), in.data(), count, bones.data());
		long diff = 0;
		for (int frame = 0; frame < 500; ++frame)
		{
			DestBuffer threaded(count);
			table.skin4W(threaded.v.data(), in.data(), count, bones.data());
			diff += memcmp(single.v.data(), threaded.v.data(), count * sizeof(vertRender)) != 0;
		}
		snprintf(what, sizeof(what), "500 consecutive threaded skinning calls: %ld differ", diff);
		check(diff == 0, what);
	}
}

void test_formats()
{
	printf("\n[3] vertex formats read from model files\n");
	check(sizeof(vertRender) == 32, "sizeof(vertRender) == 32");
	check(sizeof(vertBoned1W) == 60, "sizeof(vertBoned1W) == 60");
	check(sizeof(vertBoned2W) == 64, "sizeof(vertBoned2W) == 64");
	check(sizeof(vertBoned3W) == 70, "sizeof(vertBoned3W) == 70");
	check(sizeof(vertBoned4W) == 76, "sizeof(vertBoned4W) == 76");
}
} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	printf("xrcpu_pipe_smoke\n");
	Debug._initialize(false);
	Core._initialize("xrcpu_pipe_smoke", engine_log, FALSE);
	Rng rng(11);
	test_ttapi();
	test_skinning(rng);
	test_formats();
	ttapi_Done();
	printf("\n%d checks, %d failed\n", g_checks, g_failures);
	Core._destroy();
	return g_failures ? 1 : 0;
}
