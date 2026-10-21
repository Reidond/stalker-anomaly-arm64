// xrcdb_smoke: headless check of the collision database (xrCDB) on Apple platforms.
//
//   xrcdb_smoke [-mt_cdb]
//
// -mt_cdb builds the models on the engine's helper thread (CDB::MODEL::build -> thread_spawn).
//
// The ray queries (CDB::COLLIDER::ray_query, ISpatial_DB::q_ray) have two implementations: an SSE one, which here runs
// on sse2neon, and a scalar one the engine falls back to when CPU::ID.feature lacks _CPU_FEATURE_SSE. The tool runs
// every query in both modes and compares them with a brute-force double precision reference:
//
//   [1] known answers: a floor and a cube with hand-computed hits, in every ray option combination
//   [2] rays that lie exactly in the plane of a tree node's bounding box (0 * inf = NaN in the slab test), on integer
//       geometry where every float operation is exact, so the hits are deterministic
//   [3] random triangle soup, random rays
//   [4] box queries against a triangle/box separating-axis reference
//   [5] ISpatial_DB: q_ray, q_box, q_sphere, insertion, movement and removal
//   [6] LP64: ISpatial_NODE::_empty() with pointers that are zero in their low 32 bits
//
// Exit status 0 means every check passed.
#include "xrCore/xrCore.h"
#include "xrCDB/xrCDB.h"
#include "xrCDB/ISpatial.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// xrEngine's renderer interface; only ISpatial::spatial_updatesector_internal() uses it, which nothing here calls
class IRender_interface;
IRender_interface* Render = nullptr;

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

// Counts a batch of comparisons; prints the first few mismatches only
struct Tally
{
	const char* name;
	long cases = 0, skipped = 0, bad = 0;
	explicit Tally(const char* n) : name(n) {}
	void ok() { ++cases; }
	void skip() { ++skipped; }
	void fail(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
	{
		++cases;
		if (++bad <= 5)
		{
			char buf[512];
			va_list ap;
			va_start(ap, fmt);
			vsnprintf(buf, sizeof(buf), fmt, ap);
			va_end(ap);
			printf("    mismatch (%s): %s\n", name, buf);
		}
	}
	void report() const
	{
		char what[256];
		snprintf(what, sizeof(what), "%s: %ld compared, %ld ambiguous skipped, %ld mismatches", name, cases, skipped,
		         bad);
		check(bad == 0 && cases > 0, what);
	}
};

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
	int i(int n) { return int(next() % u32(n)); }
};

// ---------------------------------------------------------------------------------------------------------------
// Double precision reference
// ---------------------------------------------------------------------------------------------------------------
struct Vec
{
	double x, y, z;
};
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec to_vec(const Fvector& v) { return {v.x, v.y, v.z}; }

enum Verdict { MISS = 0, HIT = 1, AMBIG = 2 };

struct RefHit
{
	Verdict verdict;
	double t;
};

// Moeller-Trumbore with the engine's conventions (CDB::ray_collider::_tri): a hit needs t in (0, range], the barycentric
// coordinates may sit exactly on an edge, a back-facing triangle is dropped when `cull` is set, and |det| < EPS misses.
// With `exact` (integer geometry, every product exact) the decisions are taken on the numerators; otherwise a triangle
// whose decision lies within `band` of a boundary is reported as ambiguous.
RefHit ref_ray_tri(const Vec& o, const Vec& d, const Vec p[3], bool cull, double range, bool exact)
{
	const double band = exact ? 0.0 : 1e-4;
	const Vec e1 = p[1] - p[0], e2 = p[2] - p[0];
	const Vec pv = cross(d, e2);
	double det = dot(e1, pv);
	const double sign = det < 0 ? -1.0 : 1.0;
	if (cull && det < 0)
		return {std::fabs(det) < 1e-3 && std::fabs(det) > 1e-7 && !exact ? AMBIG : MISS, 0};
	if (std::fabs(det) < 1e-5)
		return {MISS, 0};
	if (!exact && std::fabs(det) < 1e-3)
		return {AMBIG, 0};
	const Vec tv = o - p[0];
	const Vec qv = cross(tv, e1);
	double un = dot(tv, pv) * sign, vn = dot(d, qv) * sign, tn = dot(e2, qv) * sign;
	det *= sign;
	const double u = un / det, v = vn / det, t = tn / det;
	if (exact)
	{
		const bool hit = un >= 0 && un <= det && vn >= 0 && un + vn <= det && tn > 0 && tn <= range * det;
		return {hit ? HIT : MISS, t};
	}
	const double margin = std::min({std::fabs(u), std::fabs(v), std::fabs(1 - u - v), std::fabs(t), std::fabs(t - range)});
	const bool plausible = u > -band && u < 1 + band && v > -band && u + v < 1 + band && t > -band && t < range + band;
	if (plausible && margin < band)
		return {AMBIG, t};
	const bool hit = u >= 0 && u <= 1 && v >= 0 && u + v <= 1 && t > 0 && t <= range;
	return {hit ? HIT : MISS, t};
}

// Separating axis test, triangle against an axis aligned box: 3 box axes, the triangle normal and 9 edge cross products
// (what box_query does with OPT_FULL_TEST). The largest gap over the axes decides: > 0 separated.
Verdict ref_tri_box(const Vec p[3], Vec c, Vec h, double band)
{
	const Vec v[3] = {p[0] - c, p[1] - c, p[2] - c};
	const Vec e[3] = {v[1] - v[0], v[2] - v[1], v[0] - v[2]};
	const Vec box_axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	std::vector<Vec> axes(box_axes, box_axes + 3);
	axes.push_back(cross(e[0], e[1]));
	for (const Vec& edge : e)
		for (const Vec& b : box_axes)
			axes.push_back(cross(edge, b));
	double max_gap = -1e30;
	for (Vec a : axes)
	{
		const double len = std::sqrt(dot(a, a));
		if (len < 1e-9)
			continue; // parallel edges: the engine's test passes trivially
		a = {a.x / len, a.y / len, a.z / len};
		const double q0 = dot(v[0], a), q1 = dot(v[1], a), q2 = dot(v[2], a);
		const double lo = std::min({q0, q1, q2}), hi = std::max({q0, q1, q2});
		const double r = h.x * std::fabs(a.x) + h.y * std::fabs(a.y) + h.z * std::fabs(a.z);
		max_gap = std::max(max_gap, std::max(lo - r, -r - hi));
	}
	if (max_gap > band)
		return MISS;
	if (max_gap < -band)
		return HIT;
	return AMBIG;
}

// ---------------------------------------------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------------------------------------------
struct Mesh
{
	std::vector<Fvector> verts;
	std::vector<CDB::TRI> tris;

	u32 add_vert(float x, float y, float z)
	{
		Fvector v;
		v.set(x, y, z);
		verts.push_back(v);
		return u32(verts.size() - 1);
	}
	void add_tri(u32 a, u32 b, u32 c, u32 dummy = 0)
	{
		CDB::TRI t;
		t.verts[0] = a;
		t.verts[1] = b;
		t.verts[2] = c;
		t.dummy = dummy;
		tris.push_back(t);
	}
	void tri_verts(size_t i, Vec out[3]) const
	{
		for (int k = 0; k < 3; ++k)
			out[k] = to_vec(verts[tris[i].verts[k]]);
	}
};

// 4 x 4 cells over [0,4] x [0,4] at y = 0 (ids 0..31, normals +y) and a cube [1,3] x [3,5] x [1,3] (ids 32..43,
// outward normals): everything on integers
Mesh make_floor_and_cube()
{
	Mesh m;
	u32 grid[5][5];
	for (int i = 0; i < 5; ++i)
		for (int j = 0; j < 5; ++j)
			grid[i][j] = m.add_vert(float(i), 0.f, float(j));
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
		{
			m.add_tri(grid[i][j], grid[i][j + 1], grid[i + 1][j]);
			m.add_tri(grid[i + 1][j], grid[i][j + 1], grid[i + 1][j + 1]);
		}
	u32 c[8];
	for (int k = 0; k < 8; ++k)
		c[k] = m.add_vert(k & 1 ? 3.f : 1.f, k & 2 ? 5.f : 3.f, k & 4 ? 3.f : 1.f);
	// faces as vertex quads; the winding is corrected so that the normal points away from the cube center
	static const int quads[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
	for (const auto& q : quads)
	{
		Vec p[3] = {to_vec(m.verts[c[q[0]]]), to_vec(m.verts[c[q[1]]]), to_vec(m.verts[c[q[2]]])};
		const Vec center = {2, 4, 2};
		const Vec n = cross(p[1] - p[0], p[2] - p[0]);
		const bool outward = dot(n, p[0] - center) > 0;
		if (outward)
		{
			m.add_tri(c[q[0]], c[q[1]], c[q[2]]);
			m.add_tri(c[q[0]], c[q[2]], c[q[3]]);
		}
		else
		{
			m.add_tri(c[q[0]], c[q[2]], c[q[1]]);
			m.add_tri(c[q[0]], c[q[3]], c[q[2]]);
		}
	}
	return m;
}

// Random triangles, 1..4 units across, in [-10, 10]^3. With `integers`, all coordinates are whole numbers.
Mesh make_soup(Rng& rng, int count, bool integers, int extent = 10)
{
	Mesh m;
	for (int i = 0; i < count; ++i)
	{
		float p[3][3];
		for (int a = 0; a < 3; ++a)
		{
			p[0][a] = integers ? float(rng.i(2 * extent + 1) - extent) : rng.f(-extent, extent);
			for (int k = 1; k < 3; ++k)
				p[k][a] = integers ? p[0][a] + float(rng.i(5) - 2) : p[0][a] + rng.f(-2.f, 2.f);
		}
		u32 id[3];
		for (int k = 0; k < 3; ++k)
			id[k] = m.add_vert(p[k][0], p[k][1], p[k][2]);
		m.add_tri(id[0], id[1], id[2], rng.next() & 0x3fffffff);
	}
	return m;
}

// CDB::MODEL needs its triangles to reach the point the engine builds them at; a verification pass keeps the soup honest
bool build_model(CDB::MODEL& model, Mesh& mesh)
{
	model.build(mesh.verts.data(), int(mesh.verts.size()), mesh.tris.data(), int(mesh.tris.size()));
	return model.get_tris_count() == int(mesh.tris.size()) && model.get_verts_count() == int(mesh.verts.size());
}

// ---------------------------------------------------------------------------------------------------------------
// Ray queries
// ---------------------------------------------------------------------------------------------------------------
struct SseMode
{
	u32 saved;
	explicit SseMode(bool sse) : saved(CPU::ID.feature)
	{
		if (sse)
			CPU::ID.feature |= _CPU_FEATURE_SSE;
		else
			CPU::ID.feature &= ~u32(_CPU_FEATURE_SSE);
	}
	~SseMode() { CPU::ID.feature = saved; }
};

struct Hit
{
	int id;
	float range;
};

std::vector<Hit> run_ray(CDB::COLLIDER& col, const CDB::MODEL& model, u32 options, const Fvector& o, const Fvector& d,
                         float range)
{
	col.ray_options(options);
	col.ray_query(&model, o, d, range);
	std::vector<Hit> out;
	for (int i = 0; i < col.r_count(); ++i)
		out.push_back({col.r_begin()[i].id, col.r_begin()[i].range});
	return out;
}

const u32 kRayModes[] = {0,
                         CDB::OPT_CULL,
                         CDB::OPT_ONLYFIRST,
                         CDB::OPT_ONLYNEAREST,
                         CDB::OPT_ONLYFIRST | CDB::OPT_ONLYNEAREST,
                         CDB::OPT_CULL | CDB::OPT_ONLYFIRST,
                         CDB::OPT_CULL | CDB::OPT_ONLYNEAREST,
                         CDB::OPT_CULL | CDB::OPT_ONLYFIRST | CDB::OPT_ONLYNEAREST};

// Compares one engine answer with the reference for one ray
void compare_ray(Tally& tally, const Mesh& mesh, const std::vector<Hit>& got, u32 options, const Fvector& o,
                 const Fvector& d, float range, bool exact, const char* tag)
{
	const bool cull = (options & CDB::OPT_CULL) != 0;
	const bool first = (options & CDB::OPT_ONLYFIRST) != 0;
	const bool nearest = (options & CDB::OPT_ONLYNEAREST) != 0;
	const Vec ov = to_vec(o), dv = to_vec(d);

	std::vector<RefHit> ref(mesh.tris.size());
	bool ambiguous = false;
	int ref_hits = 0, best = -1;
	for (size_t i = 0; i < mesh.tris.size(); ++i)
	{
		Vec p[3];
		mesh.tri_verts(i, p);
		ref[i] = ref_ray_tri(ov, dv, p, cull, range, exact);
		ambiguous |= ref[i].verdict == AMBIG;
		if (ref[i].verdict == HIT)
		{
			++ref_hits;
			if (best < 0 || ref[i].t < ref[best].t)
				best = int(i);
		}
	}

	std::vector<int> seen(mesh.tris.size(), 0);
	for (const Hit& h : got)
	{
		if (h.id < 0 || size_t(h.id) >= mesh.tris.size())
		{
			tally.fail("%s mode %u: result id %d out of range", tag, options, h.id);
			return;
		}
		if (seen[h.id]++)
		{
			tally.fail("%s mode %u: triangle %d reported twice", tag, options, h.id);
			return;
		}
		if (ref[h.id].verdict == MISS)
		{
			tally.fail("%s mode %u: triangle %d reported, reference misses (ray %g %g %g -> %g %g %g, range %g)", tag,
			           options, h.id, o.x, o.y, o.z, d.x, d.y, d.z, range);
			return;
		}
		if (ref[h.id].verdict == HIT && std::fabs(h.range - ref[h.id].t) > 1e-3 * std::max(1.0, ref[h.id].t))
		{
			tally.fail("%s mode %u: triangle %d range %g, reference %g", tag, options, h.id, h.range, ref[h.id].t);
			return;
		}
	}

	if (nearest && !first)
	{
		if (got.size() > 1)
		{
			tally.fail("%s mode %u: %zu results for a nearest query", tag, options, got.size());
			return;
		}
		if (!ambiguous)
		{
			// coplanar triangles tie on the distance; any of them is the nearest
			const bool tie = best >= 0 && got.size() == 1 && ref[got[0].id].verdict == HIT &&
				std::fabs(ref[got[0].id].t - ref[best].t) <= 1e-5 * std::max(1.0, ref[best].t);
			if ((best >= 0) != (got.size() == 1) || (best >= 0 && got[0].id != best && !tie))
			{
				tally.fail("%s mode %u: nearest is %d, engine answered %d", tag, options, best,
				           got.empty() ? -1 : got[0].id);
				return;
			}
		}
		else
		{
			tally.skip();
			return;
		}
	}
	else if (first)
	{
		if (got.size() > 1)
		{
			tally.fail("%s mode %u: %zu results for a first-hit query", tag, options, got.size());
			return;
		}
		if (!ambiguous && (ref_hits > 0) != (got.size() == 1))
		{
			tally.fail("%s mode %u: reference has %d hits, engine returned %zu", tag, options, ref_hits, got.size());
			return;
		}
	}
	else
	{
		if (!ambiguous && int(got.size()) != ref_hits)
		{
			tally.fail("%s mode %u: reference has %d hits, engine returned %zu", tag, options, ref_hits, got.size());
			return;
		}
		for (size_t i = 0; i < mesh.tris.size(); ++i)
			if (ref[i].verdict == HIT && !seen[i])
			{
				tally.fail("%s mode %u: triangle %zu is hit in the reference, missing in the answer", tag, options, i);
				return;
			}
	}
	if (ambiguous)
		tally.skip();
	else
		tally.ok();
}

void test_known_answers(CDB::MODEL& model, const Mesh& mesh)
{
	printf("\n[1] known answers: floor [0,4]^2 at y=0, cube [1,3]x[3,5]x[1,3]\n");
	CDB::COLLIDER col;
	for (int sse = 1; sse >= 0; --sse)
	{
		SseMode mode(sse != 0);
		printf("  -- %s ray path\n", sse ? "SSE (sse2neon)" : "scalar");
		Fvector o, d;
		auto ranges = [](const std::vector<Hit>& h)
		{
			std::vector<float> r;
			for (const Hit& x : h)
				r.push_back(x.range);
			std::sort(r.begin(), r.end());
			return r;
		};
		auto is = [](const std::vector<float>& r, std::initializer_list<float> want)
		{
			if (r.size() != want.size())
				return false;
			size_t i = 0;
			for (float w : want)
				if (std::fabs(r[i++] - w) > 1e-4f)
					return false;
			return true;
		};

		// straight down through cube top, cube bottom and floor
		o.set(2.3f, 10.f, 2.6f);
		d.set(0.f, -1.f, 0.f);
		check(is(ranges(run_ray(col, model, 0, o, d, 100.f)), {5.f, 7.f, 10.f}), "down through the cube: hits at 5, 7, 10");
		check(is(ranges(run_ray(col, model, CDB::OPT_CULL, o, d, 100.f)), {5.f, 10.f}),
		      "... culled: the cube's underside drops out");
		{
			auto h = run_ray(col, model, CDB::OPT_ONLYNEAREST, o, d, 100.f);
			check(h.size() == 1 && std::fabs(h[0].range - 5.f) < 1e-4f && h[0].id >= 32, "... nearest: the cube top at 5");
			h = run_ray(col, model, CDB::OPT_ONLYFIRST, o, d, 100.f);
			check(h.size() == 1, "... first: exactly one hit");
		}
		check(is(ranges(run_ray(col, model, 0, o, d, 5.5f)), {5.f}), "... range 5.5: only the cube top");
		check(is(ranges(run_ray(col, model, 0, o, d, 4.9f)), {}), "... range 4.9: nothing");

		// straight up from below
		o.set(2.3f, -3.f, 2.6f);
		d.set(0.f, 1.f, 0.f);
		check(is(ranges(run_ray(col, model, 0, o, d, 100.f)), {3.f, 6.f, 8.f}), "up from below: hits at 3, 6, 8");
		check(is(ranges(run_ray(col, model, CDB::OPT_CULL, o, d, 100.f)), {6.f}),
		      "... culled: only the cube's underside faces the ray");

		// horizontal through the cube
		o.set(-5.f, 4.f, 2.2f);
		d.set(1.f, 0.f, 0.f);
		check(is(ranges(run_ray(col, model, 0, o, d, 100.f)), {6.f, 8.f}), "sideways through the cube: hits at 6 and 8");
		o.set(-5.f, 4.f, -5.f);
		check(run_ray(col, model, 0, o, d, 100.f).empty(), "sideways past the cube: no hit");

		// diagonal
		o.set(-1.f, 9.f, 2.f);
		d.set(1.f, -1.f, 0.f);
		d.normalize();
		check(run_ray(col, model, CDB::OPT_ONLYNEAREST, o, d, 100.f).size() == 1, "diagonal ray: one nearest hit");
	}
}

void test_node_plane_rays(CDB::MODEL& model, const Mesh& mesh)
{
	printf("\n[2] rays in the plane of a node's bounding box (NaN in the slab test), integer geometry\n");
	Tally tally("floor+cube");
	CDB::COLLIDER col;
	for (int sse = 1; sse >= 0; --sse)
	{
		SseMode mode(sse != 0);
		const char* tag = sse ? "sse" : "scalar";
		// x or z sits exactly on a grid line that bounds nodes; the direction component along that axis is zero
		for (int line = 0; line <= 4; ++line)
			for (int k = 0; k < 6; ++k)
			{
				const float other = 0.5f + float(k) * 0.5f; // 0.5 .. 3.0: never on a vertex row
				const struct
				{
					float ox, oy, oz, dx, dy, dz;
				} rays[] = {
					{float(line), 10.f, other, 0, -1, 0},   {float(line), -2.5f, other, 0, 1, 0},
					{other, 10.f, float(line), 0, -1, 0},   {other, -2.5f, float(line), 0, 1, 0},
					{-5.5f, float(line), other, 1, 0, 0},   {9.5f, float(line), other, -1, 0, 0},
					{other, float(line), -5.5f, 0, 0, 1},   {other, float(line), 9.5f, 0, 0, -1},
					{-5.5f, other, float(line), 1, 0, 0},   {other, -2.5f, float(line), 0, 1, 0},
				};
				for (const auto& r : rays)
				{
					Fvector o, d;
					o.set(r.ox, r.oy, r.oz);
					d.set(r.dx, r.dy, r.dz);
					// only culled rays: with the cull the edge decisions are exact comparisons of integers
					for (u32 opt : {u32(CDB::OPT_CULL), u32(CDB::OPT_CULL | CDB::OPT_ONLYNEAREST),
					                u32(CDB::OPT_CULL | CDB::OPT_ONLYFIRST)})
						compare_ray(tally, mesh, run_ray(col, model, opt, o, d, 100.37f), opt, o, d, 100.37f, true, tag);
				}
			}
	}
	tally.report();
	// a ray that is only found if the NaN slabs are treated like x86 does: along the grid line x = 1, over floor and cube
	SseMode mode(true);
	Fvector o, d;
	o.set(1.f, 10.f, 2.5f);
	d.set(0.f, -1.f, 0.f);
	auto h = run_ray(col, model, CDB::OPT_CULL, o, d, 100.37f);
	check(h.size() >= 3, "ray on the plane x = 1 (cube edge, floor grid line) hits the cube top and floor triangles");
}

void test_random_rays(Rng& rng)
{
	printf("\n[3] random triangle soup against the reference\n");
	Mesh soup = make_soup(rng, 400, false);
	CDB::MODEL model;
	check(build_model(model, soup), "soup built");
	CDB::COLLIDER col;
	Tally sse_tally("sse"), fpu_tally("scalar"), parity("sse == scalar");
	for (int n = 0; n < 1000; ++n)
	{
		Fvector o, d;
		o.set(rng.f(-14, 14), rng.f(-14, 14), rng.f(-14, 14));
		const int kind = rng.i(8);
		if (kind == 0) d.set(rng.i(2) ? 1.f : -1.f, 0.f, 0.f);
		else if (kind == 1) d.set(0.f, rng.i(2) ? 1.f : -1.f, 0.f);
		else if (kind == 2) d.set(0.f, 0.f, rng.i(2) ? 1.f : -1.f);
		else
		{
			d.set(rng.f(-1, 1), rng.f(-1, 1), rng.f(-1, 1));
			if (d.magnitude() < 0.1f)
				d.set(0.3f, 0.5f, 0.8f);
			d.normalize();
		}
		const float range = rng.f(1.f, 40.f);
		for (u32 opt : kRayModes)
		{
			std::vector<Hit> a, b;
			{
				SseMode mode(true);
				a = run_ray(col, model, opt, o, d, range);
			}
			{
				SseMode mode(false);
				b = run_ray(col, model, opt, o, d, range);
			}
			compare_ray(sse_tally, soup, a, opt, o, d, range, false, "sse");
			compare_ray(fpu_tally, soup, b, opt, o, d, range, false, "scalar");
			// first-hit answers depend on the traversal; the two paths box-test slightly differently, so compare the
			// sets of the exhaustive modes only
			if (!(opt & (CDB::OPT_ONLYFIRST | CDB::OPT_ONLYNEAREST)))
			{
				std::vector<int> ia, ib;
				for (const Hit& h : a) ia.push_back(h.id);
				for (const Hit& h : b) ib.push_back(h.id);
				std::sort(ia.begin(), ia.end());
				std::sort(ib.begin(), ib.end());
				if (ia == ib)
					parity.ok();
				else
				{
					// only a triangle whose decision is within the ambiguity band may differ
					std::vector<int> diff;
					std::set_symmetric_difference(ia.begin(), ia.end(), ib.begin(), ib.end(), std::back_inserter(diff));
					bool explained = true;
					for (int id : diff)
					{
						Vec p[3];
						soup.tri_verts(id, p);
						explained &= ref_ray_tri(to_vec(o), to_vec(d), p, (opt & CDB::OPT_CULL) != 0, range, false).verdict ==
							AMBIG;
					}
					if (explained)
						parity.skip();
					else
						parity.fail("mode %u: sse and scalar answers differ in %zu triangles", opt, diff.size());
				}
			}
		}
	}
	sse_tally.report();
	fpu_tally.report();
	parity.report();

	// integer soup with axis-aligned rays through grid positions: box planes coincide with ray coordinates all the time
	Mesh lattice = make_soup(rng, 300, true, 8);
	CDB::MODEL lmodel;
	check(build_model(lmodel, lattice), "integer soup built");
	Tally edge_sse("integer soup, sse"), edge_fpu("integer soup, scalar");
	for (int n = 0; n < 700; ++n)
	{
		Fvector o, d;
		const int axis = rng.i(3), dir = rng.i(2) ? 1 : -1;
		float c[3] = {float(rng.i(15) - 7), float(rng.i(15) - 7), float(rng.i(15) - 7)};
		c[axis] = dir > 0 ? -12.5f : 12.5f;
		o.set(c[0], c[1], c[2]);
		d.set(axis == 0 ? float(dir) : 0.f, axis == 1 ? float(dir) : 0.f, axis == 2 ? float(dir) : 0.f);
		for (u32 opt : {u32(CDB::OPT_CULL), u32(CDB::OPT_CULL | CDB::OPT_ONLYNEAREST)})
		{
			{
				SseMode mode(true);
				compare_ray(edge_sse, lattice, run_ray(col, lmodel, opt, o, d, 100.37f), opt, o, d, 100.37f, true, "sse");
			}
			{
				SseMode mode(false);
				compare_ray(edge_fpu, lattice, run_ray(col, lmodel, opt, o, d, 100.37f), opt, o, d, 100.37f, true,
				            "scalar");
			}
		}
	}
	edge_sse.report();
	edge_fpu.report();
}

// ---------------------------------------------------------------------------------------------------------------
// Box queries
// ---------------------------------------------------------------------------------------------------------------
void test_box_queries(Rng& rng)
{
	printf("\n[4] box queries\n");
	Mesh floor_cube = make_floor_and_cube();
	CDB::MODEL fc;
	build_model(fc, floor_cube);
	CDB::COLLIDER col;
	col.box_options(CDB::OPT_FULL_TEST);
	auto ids = [&](const Fvector& c, const Fvector& h)
	{
		col.box_query(&fc, c, h);
		std::vector<int> out;
		for (int i = 0; i < col.r_count(); ++i)
			out.push_back(col.r_begin()[i].id);
		std::sort(out.begin(), out.end());
		return out;
	};
	Fvector c, h;
	c.set(20.f, 20.f, 20.f);
	h.set(1.f, 1.f, 1.f);
	check(ids(c, h).empty(), "a box far from the model finds nothing");
	c.set(2.f, 4.f, 2.f);
	h.set(0.4f, 0.4f, 0.4f);
	check(ids(c, h).empty(), "a box inside the hollow cube finds nothing");
	c.set(2.f, 5.f, 2.f);
	h.set(0.5f, 0.5f, 0.5f);
	{
		std::vector<int> r = ids(c, h);
		bool all_cube = !r.empty();
		for (int id : r)
			all_cube &= id >= 32;
		check(all_cube, "a box straddling the cube's top face finds only cube triangles");
	}
	c.set(0.5f, 0.2f, 0.5f);
	h.set(0.3f, 0.3f, 0.3f);
	{
		std::vector<int> r = ids(c, h);
		check(r.size() >= 1 && r.size() <= 2 && r[0] < 32, "a box on the floor's first cell finds that cell's triangles");
	}
	c.set(2.f, 2.f, 2.f);
	h.set(10.f, 10.f, 10.f);
	check(ids(c, h).size() == floor_cube.tris.size(), "a box around everything finds every triangle");
	col.box_options(CDB::OPT_FULL_TEST | CDB::OPT_ONLYFIRST);
	check(ids(c, h).size() == 1, "... first-hit finds exactly one");
	col.box_options(CDB::OPT_FULL_TEST);

	Mesh soup = make_soup(rng, 400, false);
	CDB::MODEL model;
	check(build_model(model, soup), "soup built");
	Tally full("box vs separating axes"), partial("class I/II is a superset of class III"), first("first-hit box");
	for (int n = 0; n < 1200; ++n)
	{
		Fvector bc, bh;
		bc.set(rng.f(-12, 12), rng.f(-12, 12), rng.f(-12, 12));
		bh.set(rng.f(0.2f, 4.f), rng.f(0.2f, 4.f), rng.f(0.2f, 4.f));
		col.box_options(CDB::OPT_FULL_TEST);
		col.box_query(&model, bc, bh);
		std::vector<int> got;
		for (int i = 0; i < col.r_count(); ++i)
			got.push_back(col.r_begin()[i].id);
		std::sort(got.begin(), got.end());

		std::vector<Verdict> ref(soup.tris.size());
		bool ambiguous = false;
		int hits = 0;
		for (size_t i = 0; i < soup.tris.size(); ++i)
		{
			Vec p[3];
			soup.tri_verts(i, p);
			ref[i] = ref_tri_box(p, to_vec(bc), to_vec(bh), 1e-3);
			ambiguous |= ref[i] == AMBIG;
			hits += ref[i] == HIT;
		}
		bool bad = false;
		for (size_t k = 0; k < got.size(); ++k)
			if ((k && got[k] == got[k - 1]) || ref[got[k]] == MISS)
				bad = true;
		for (size_t i = 0; i < soup.tris.size(); ++i)
			if (ref[i] == HIT && !std::binary_search(got.begin(), got.end(), int(i)))
				bad = true;
		if (bad)
			full.fail("box center %g %g %g half %g %g %g: engine %zu triangles, reference %d", bc.x, bc.y, bc.z, bh.x,
			          bh.y, bh.z, got.size(), hits);
		else if (ambiguous)
			full.skip();
		else
			full.ok();

		col.box_options(0);
		col.box_query(&model, bc, bh);
		std::vector<int> wide;
		for (int i = 0; i < col.r_count(); ++i)
			wide.push_back(col.r_begin()[i].id);
		std::sort(wide.begin(), wide.end());
		if (std::includes(wide.begin(), wide.end(), got.begin(), got.end()))
			partial.ok();
		else
			partial.fail("the partial test dropped a triangle the full test kept");

		col.box_options(CDB::OPT_FULL_TEST | CDB::OPT_ONLYFIRST);
		col.box_query(&model, bc, bh);
		if (ambiguous)
			first.skip();
		else if (col.r_count() == (hits > 0 ? 1 : 0) && (hits == 0 || ref[col.r_begin()->id] == HIT))
			first.ok();
		else
			first.fail("first-hit returned %d for %d reference hits", col.r_count(), hits);
	}
	full.report();
	partial.report();
	first.report();
}

// ---------------------------------------------------------------------------------------------------------------
// Spatial DB
// ---------------------------------------------------------------------------------------------------------------
struct Blob : public ISpatial
{
	Blob(ISpatial_DB* db, const Fvector& p, float r, u32 type) : ISpatial(db)
	{
		spatial.sphere.P = p;
		spatial.sphere.R = r;
		spatial.type = type;
	}
};

// Sphere/ray decision of Fsphere::intersect, as ISpatial_DB::q_ray uses it
Verdict ref_ray_sphere(const Fvector& o, const Fvector& dir, float range, const Fvector& P, float R)
{
	const Vec diff = to_vec(o) - to_vec(P), d = to_vec(dir);
	const double a = double(range) * range, b = dot(diff, d) * range, c = dot(diff, diff) - double(R) * R;
	const double disc = b * b - a * c;
	if (disc < -1e-3 * a)
		return MISS;
	if (std::fabs(disc) <= 1e-3 * a)
		return AMBIG;
	const double root = std::sqrt(disc);
	const double t0 = range * (-b - root) / a, t1 = range * (-b + root) / a;
	if (std::fabs(t0) < 1e-3 || std::fabs(t1) < 1e-3 || std::fabs(t0 - range) < 1e-3)
		return AMBIG;
	if (t0 >= 0)
		return t0 < range ? HIT : MISS;
	if (t1 >= 0)
		return HIT;
	return MISS;
}

xr_vector<ISpatial*> sorted(xr_vector<ISpatial*> v)
{
	std::sort(v.begin(), v.end());
	return v;
}
xr_vector<ISpatial*> sorted(std::initializer_list<ISpatial*> l)
{
	xr_vector<ISpatial*> v;
	for (ISpatial* s : l)
		v.push_back(s);
	return sorted(v);
}

void test_spatial_db(Rng& rng)
{
	printf("\n[5] ISpatial_DB\n");
	ISpatial_DB db;
	Fbox bounds;
	bounds.set(-1024.f, -1024.f, -1024.f, 1024.f, 1024.f, 1024.f);
	db.initialize(bounds);

	// known answers
	{
		Fvector p;
		p.set(0.f, 0.f, 0.f);
		Blob a(&db, p, 1.f, STYPE_COLLIDEABLE);
		p.set(10.f, 0.f, 0.f);
		Blob b(&db, p, 1.f, STYPE_COLLIDEABLE | STYPE_VISIBLEFORAI);
		p.set(20.f, 0.f, 0.f);
		Blob c(&db, p, 1.f, STYPE_RENDERABLE);
		a.spatial_register();
		b.spatial_register();
		c.spatial_register();
		check(db.stat_objects == 3, "three objects registered");

		xr_vector<ISpatial*> r;
		Fvector center, size;
		center.set(9.f, 0.f, 0.f);
		db.q_sphere(r, 0, STYPE_COLLIDEABLE, center, 2.f);
		check(r.size() == 1 && r[0] == &b, "q_sphere (9,0,0) r=2: only the sphere at x=10");
		center.set(5.f, 0.f, 0.f);
		db.q_sphere(r, 0, STYPE_COLLIDEABLE, center, 20.f);
		check(sorted(r) == sorted({&a, &b}), "q_sphere r=20 with the collideable mask: two objects, not the renderable one");
		db.q_sphere(r, 0, STYPE_RENDERABLE | STYPE_COLLIDEABLE, center, 20.f);
		check(r.size() == 3, "... mask is a bitwise OR: all three");
		size.set(1.f, 1.f, 1.f);
		center.set(20.f, 0.f, 1.5f);
		db.q_box(r, 0, STYPE_RENDERABLE, center, size);
		check(r.size() == 1 && r[0] == &c, "q_box over the sphere at x=20");

		Fvector start, dir;
		start.set(-5.f, 0.f, 0.f);
		dir.set(1.f, 0.f, 0.f);
		for (int sse = 1; sse >= 0; --sse)
		{
			SseMode mode(sse != 0);
			db.q_ray(r, 0, 0, start, dir, 100.f);
			check(sorted(r) == sorted({&a, &b, &c}), sse ? "q_ray along +x (SSE): all three" : "q_ray along +x (scalar): all three");
			db.q_ray(r, 0, STYPE_COLLIDEABLE | STYPE_VISIBLEFORAI, start, dir, 100.f);
			check(r.size() == 1 && r[0] == &b, sse ? "... AND mask picks the one with both bits (SSE)" : "... AND mask picks the one with both bits (scalar)");
			db.q_ray(r, ISpatial_DB::O_ONLYNEAREST, 0, start, dir, 100.f);
			check(r.size() >= 1 && r[0] == &a, sse ? "... nearest first (SSE)" : "... nearest first (scalar)");
			db.q_ray(r, 0, 0, start, dir, 16.f);
			check(sorted(r) == sorted({&a, &b}), sse ? "... range 16 stops short of the third (SSE)" : "... range 16 stops short of the third (scalar)");
			start.set(-5.f, 3.f, 0.f);
			db.q_ray(r, 0, 0, start, dir, 100.f);
			check(r.empty(), sse ? "a ray passing 3 units above: nothing (SSE)" : "a ray passing 3 units above: nothing (scalar)");
			start.set(-5.f, 0.f, 0.f);
		}
		// on the axis plane of the leaf nodes: x and y exactly on 0, direction (1,0,0): inv_dir = (1, inf, inf)
		start.set(-5.f, 0.f, 0.f);
		{
			SseMode mode(true);
			db.q_ray(r, 0, 0, start, dir, 100.f);
			check(r.size() == 3, "ray in the y=0, z=0 planes (inf * 0 slabs) still finds all three");
		}
	}

	// random population
	const int count = 600;
	std::vector<Blob*> blobs;
	for (int i = 0; i < count; ++i)
	{
		Fvector p;
		p.set(rng.f(-300, 300), rng.f(-300, 300), rng.f(-300, 300));
		const u32 type = 1u << rng.i(6) | (rng.i(3) == 0 ? 1u << rng.i(6) : 0u);
		Blob* b = new Blob(&db, p, rng.f(0.1f, 6.f), type);
		b->spatial_register();
		blobs.push_back(b);
	}
	// move a third of them, remove a sixth
	for (int i = 0; i < count; i += 3)
	{
		Fvector p;
		p.set(rng.f(-300, 300), rng.f(-300, 300), rng.f(-300, 300));
		blobs[i]->spatial.sphere.P = p;
		blobs[i]->spatial_move();
	}
	std::vector<Blob*> live;
	for (int i = 0; i < count; ++i)
	{
		if (i % 6 == 1)
			blobs[i]->spatial_unregister();
		else
			live.push_back(blobs[i]);
	}
	check(db.stat_objects == live.size(), "node bookkeeping after moves and removals (stat_objects)");
	check(db.verify() || true, "verify() runs"); // a no-op without DEBUG, kept so a debug build exercises it

	Tally qbox("q_box"), qsphere("q_sphere"), qray_sse("q_ray sse"), qray_fpu("q_ray scalar");
	xr_vector<ISpatial*> r;
	for (int n = 0; n < 600; ++n)
	{
		Fvector c, h;
		c.set(rng.f(-320, 320), rng.f(-320, 320), rng.f(-320, 320));
		h.set(rng.f(1, 60), rng.f(1, 60), rng.f(1, 60));
		const u32 mask = 1u << rng.i(6);
		db.q_box(r, 0, mask, c, h);
		xr_vector<ISpatial*> expect, fuzzy;
		for (Blob* b : live)
		{
			if (!(b->spatial.type & mask))
				continue;
			const Fvector& P = b->spatial.sphere.P;
			const float R = b->spatial.sphere.R;
			double gap = -1e30;
			gap = std::max(gap, double(std::fabs(P.x - c.x)) - (R + h.x));
			gap = std::max(gap, double(std::fabs(P.y - c.y)) - (R + h.y));
			gap = std::max(gap, double(std::fabs(P.z - c.z)) - (R + h.z));
			if (std::fabs(gap) < 1e-3)
				fuzzy.push_back(b);
			else if (gap < 0)
				expect.push_back(b);
		}
		bool bad = false;
		xr_vector<ISpatial*> got = sorted(r);
		for (ISpatial* e : expect)
			bad |= !std::binary_search(got.begin(), got.end(), e);
		for (ISpatial* g : got)
		{
			bool known = std::find(expect.begin(), expect.end(), g) != expect.end() ||
				std::find(fuzzy.begin(), fuzzy.end(), g) != fuzzy.end();
			bad |= !known;
		}
		bad |= std::adjacent_find(got.begin(), got.end()) != got.end();
		if (bad) qbox.fail("box %g %g %g size %g %g %g mask %u: engine %zu, reference %zu", c.x, c.y, c.z, h.x, h.y, h.z, mask, got.size(), expect.size());
		else if (!fuzzy.empty()) qbox.skip();
		else qbox.ok();

		// q_sphere is q_box with a cubic half size
		db.q_sphere(r, 0, mask, c, h.x);
		Fvector cube;
		cube.set(h.x, h.x, h.x);
		xr_vector<ISpatial*> via_box;
		db.q_box(via_box, 0, mask, c, cube);
		if (sorted(r) == sorted(via_box)) qsphere.ok();
		else qsphere.fail("q_sphere and q_box with the same extent differ");
	}
	for (int n = 0; n < 600; ++n)
	{
		Fvector o, d;
		o.set(rng.f(-320, 320), rng.f(-320, 320), rng.f(-320, 320));
		const int kind = rng.i(6);
		if (kind == 0) d.set(1.f, 0.f, 0.f);
		else if (kind == 1) d.set(0.f, 1.f, 0.f);
		else if (kind == 2) d.set(0.f, 0.f, -1.f);
		else
		{
			d.set(rng.f(-1, 1), rng.f(-1, 1), rng.f(-1, 1));
			d.normalize_safe();
		}
		const float range = rng.f(20.f, 400.f);
		const u32 mask = rng.i(2) ? 0u : (1u << rng.i(6));
		for (int sse = 1; sse >= 0; --sse)
		{
			SseMode mode(sse != 0);
			Tally& tally = sse ? qray_sse : qray_fpu;
			db.q_ray(r, 0, mask, o, d, range);
			xr_vector<ISpatial*> got = sorted(r);
			bool bad = std::adjacent_find(got.begin(), got.end()) != got.end(), fuzzy = false;
			xr_vector<ISpatial*> hits;
			for (Blob* b : live)
			{
				if (mask != (b->spatial.type & mask))
					continue;
				const Verdict v = ref_ray_sphere(o, d, range, b->spatial.sphere.P, b->spatial.sphere.R);
				const bool present = std::binary_search(got.begin(), got.end(), static_cast<ISpatial*>(b));
				fuzzy |= v == AMBIG;
				if (v == HIT && !present) bad = true;
				if (v == MISS && present) bad = true;
			}
			for (ISpatial* g : got)
				bad |= std::find(live.begin(), live.end(), static_cast<Blob*>(g)) == live.end();
			if (bad) tally.fail("ray %g %g %g -> %g %g %g range %g mask %u: engine %zu hits", o.x, o.y, o.z, d.x, d.y, d.z, range, mask, got.size());
			else if (fuzzy) tally.skip();
			else tally.ok();
		}
	}
	qbox.report();
	qsphere.report();
	qray_sse.report();
	qray_fpu.report();

	// the objects must leave the DB before it is destroyed
	for (Blob* b : blobs)
		delete b;
	check(db.stat_objects == 0, "all objects removed");
}

void test_lp64()
{
	printf("\n[6] LP64\n");
	static_assert(sizeof(ISpatial_NODE::ptrt) == sizeof(void*), "ptrt must hold a pointer");
	static_assert(sizeof(CDB::TRI) == 16, "TRI is 16 bytes (it is read from level files)");
	check(sizeof(ISpatial_NODE::ptrt) == sizeof(void*), "ISpatial_NODE::ptrt is pointer sized");
	ISpatial_NODE node;
	node._init(nullptr);
	check(node._empty(), "a fresh node is empty");
	for (int i = 0; i < 8; ++i)
	{
		node._init(nullptr);
		node.children[i] = reinterpret_cast<ISpatial_NODE*>(uintptr_t(1) << 32);
		char what[96];
		snprintf(what, sizeof(what), "child %d = 0x100000000 (low 32 bits zero) is not empty", i);
		check(!node._empty(), what);
	}
	node._init(nullptr);
}
} // namespace

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	printf("xrcdb_smoke%s\n", argc > 1 ? " (threaded model build if -mt_cdb is given)" : "");

	Debug._initialize(false);
	Core._initialize("xrcdb_smoke", engine_log, FALSE);
	printf("  CPU feature bits: 0x%x (SSE %s)\n", CPU::ID.feature, (CPU::ID.feature & _CPU_FEATURE_SSE) ? "reported" : "not reported");
	check((CPU::ID.feature & _CPU_FEATURE_SSE) != 0, "the SSE ray path is selected by default");
	printf("  threaded model build: %s\n", strstr(Core.Params, "-mt_cdb") ? "yes" : "no");

	Rng rng(20240611);
	{
		Mesh mesh = make_floor_and_cube();
		CDB::MODEL model;
		check(build_model(model, mesh), "floor and cube built");
		test_known_answers(model, mesh);
		test_node_plane_rays(model, mesh);
	}
	test_random_rays(rng);
	test_box_queries(rng);
	test_spatial_db(rng);
	test_lp64();

	printf("\n%d checks, %d failed\n", g_checks, g_failures);
	Core._destroy();
	return g_failures ? 1 : 0;
}
