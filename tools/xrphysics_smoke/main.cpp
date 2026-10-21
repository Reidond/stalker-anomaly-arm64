// xrphysics_smoke: headless check that xrPhysics runs on Apple platforms without xrEngine or xrGame.
//
//   xrphysics_smoke [--seconds N] [--print] [--digest] [--only boxa,boxb,sphere,chain,actor,ai]
//                   [--stress N [--seed S]]
//
// It builds the physics world through xrPhysics's own API (create_physics_world, P_create_Shell / P_create_Element,
// create_actor_character / create_ai_character, P_create_Joint) on top of a static mesh, steps it the way the engine
// does (the world registers itself as a `pureFrame` with the device; the device here is a stub whose only job is to
// call OnFrame), and checks that the simulation is stable: nothing leaves the world or turns into NaN, nothing sinks
// into the ground, and everything comes to rest on it.
//
// The runs are not repeatable: X-Ray's ODE shuffles the constraint rows of dWorldQuickStep with a std::mt19937 seeded
// from std::random_device (quickstep.cpp), on Windows as well. The thresholds below leave room for that.
//
// What the engine would normally provide, and what stands in for it here:
//   CRenderDeviceBase     StubDevice below (AddSeqFrame/RemoveSeqFrame/StatPhysics, fTimeDelta)
//   CGameMtlLibrary       two hand-made SGameMtl entries in a zeroed library object (xrEngine's GameMtlLib.cpp is not linked)
//   IPhysicsShellHolder   StubHolder below
//   PGMLib, Render, g_actor_allow_ladder, g_dead_body_collision, typeinfo for CObject: defined at the end of this file
// Everything else (ISpatial, CObjectSpace, CDB::COLLIDER, xrXRC) is the real xrCDB.
//
// --stress N drop N random bodies (boxes, spheres, chains, a few characters) in a heap instead of the fixed scene and
//            report how many left the level, turned NaN or fell through the ground (--seed S picks the heap)
// --print    dump the state of every body once per simulated second
// --digest   print an FNV-1a hash of all tracked float bit patterns. Two runs only agree if ODE's shuffle is made
//            repeatable (the -ffp-contract experiment in docs/ios-port/XRPHYSICS.md builds ODE's quickstep.cpp with a
//            fixed seed for that)
//
// Exit status 0 means every check passed.
#include "stdafx.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../xrEngine/gamemtllib.h"
#include "../xrEngine/device.h"
#include "../xrEngine/xrLevel.h"
#include "../xrEngine/iphysicsshell.h"

#include "PHWorld.h"
#include "PhysicsShell.h"
#include "PHCharacter.h"
#include "IPhysicsShellHolder.h"
#include "console_vars.h"
#include "physicscommon.h"

extern BOOL DllMainXrPhysics(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);

namespace
{
// The material library keeps its tables private and its constructor lives in xrEngine (GameMtlLib.cpp is not linked):
// the test fills the table itself. Explicit instantiation is exempt from access checks, which makes this legal C++.
template <typename Tag, typename Tag::type Member>
struct Rob
{
	friend typename Tag::type get(Tag) { return Member; }
};

struct MaterialsTag
{
	typedef GameMtlVec CGameMtlLibrary::*type;
	friend type get(MaterialsTag);
};

template struct Rob<MaterialsTag, &CGameMtlLibrary::materials>;

int g_failures = 0;

void check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
	if (!ok)
		++g_failures;
}

void engine_log(LPCSTR line) { printf("    | %s\n", line); }

// ------------------------------------------------------------------------------------------------------------------
// Stand-ins for engine objects
// ------------------------------------------------------------------------------------------------------------------
struct StubDevice : public CRenderDeviceBase
{
	CStatsPhysics stats;
	xr_vector<pureFrame*> frames;

	StubDevice()
	{
		fTimeDelta = 0.f;
		dwFrame = 0;
		dwTimeGlobal = 0;
	}

	CStatsPhysics* _BCL StatPhysics() override { return &stats; }
	void _BCL AddSeqFrame(pureFrame* f, bool) override { frames.push_back(f); }

	void _BCL RemoveSeqFrame(pureFrame* f) override
	{
		for (size_t i = 0; i < frames.size(); ++i)
			if (frames[i] == f)
			{
				frames.erase(frames.begin() + i);
				return;
			}
	}

	void frame(float dt)
	{
		fTimeDelta = dt;
		++dwFrame;
		dwTimeGlobal += u32(dt * 1000.f);
		for (size_t i = 0; i < frames.size(); ++i)
			frames[i]->OnFrame();
	}
};

struct StubHolder : public IPhysicsShellHolder
{
	Fmatrix xform;
	Fvector pos;
	CPhysicsShell* shell;
	u16 id;

	StubHolder(u16 id_, const Fvector& p) : shell(0), id(id_)
	{
		xform.identity();
		xform.translate_over(p);
		pos = p;
	}

	Fmatrix& _BCL ObjectXFORM() override { return xform; }
	Fvector& _BCL ObjectPosition() override { return pos; }
	LPCSTR _BCL ObjectName() const override { return "smoke"; }
	LPCSTR _BCL ObjectNameVisual() const override { return "smoke"; }
	LPCSTR _BCL ObjectNameSect() const override { return "smoke"; }
	bool _BCL ObjectGetDestroy() const override { return false; }
	ICollisionHitCallback* _BCL ObjectGetCollisionHitCallback() override { return 0; }
	u16 _BCL ObjectID() const override { return id; }
	ICollisionForm* _BCL ObjectCollisionModel() override { return 0; }
	IKinematics* _BCL ObjectKinematics() override { return 0; }
	IDamageSource* _BCL ObjectCastIDamageSource() override { return 0; }
	void _BCL ObjectProcessingDeactivate() override {}
	void _BCL ObjectProcessingActivate() override {}
	void _BCL ObjectSpatialMove() override {}
	CPhysicsShell*& _BCL ObjectPPhysicsShell() override { return shell; }
	void _BCL enable_notificate() override {}
	bool _BCL has_parent_object() override { return false; }
	void _BCL on_physics_disable() override {}
	IPHCapture* _BCL PHCapture() override { return 0; }
	bool _BCL IsInventoryItem() override { return false; }
	bool _BCL IsActor() override { return false; }
	bool _BCL IsStalker() override { return false; }
	void _BCL HideAllWeapons(bool) override {}
	void _BCL MovementCollisionEnable(bool) override {}
	CPHSoundPlayer* _BCL ObjectPhSoundPlayer() override { return 0; }
	ICollisionDamageReceiver* _BCL ObjectPhCollisionDamageReceiver() override { return 0; }
	void _BCL BonceDamagerCallback(float&) override {}
#ifdef DEBUG
	std::string _BCL dump(EDumpType) const override { return std::string(); }
#endif
};

// ------------------------------------------------------------------------------------------------------------------
// Digest of everything the test tracks
// ------------------------------------------------------------------------------------------------------------------
struct Digest
{
	uint64_t h = 1469598103934665603ull;

	void add(float f)
	{
		uint32_t bits;
		memcpy(&bits, &f, sizeof(bits));
		for (int i = 0; i < 4; ++i)
		{
			h ^= (bits >> (8 * i)) & 0xff;
			h *= 1099511628211ull;
		}
	}

	void add(const Fvector& v)
	{
		add(v.x);
		add(v.y);
		add(v.z);
	}
};

bool finite3(const Fvector& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// ------------------------------------------------------------------------------------------------------------------
// The scene
// ------------------------------------------------------------------------------------------------------------------
const float k_half = 0.25f; // half extent of the boxes, radius of the sphere

struct Body
{
	const char* name;
	CPhysicsShell* shell;
	StubHolder* holder;
};

struct Walker
{
	const char* name;
	CPHCharacter* ch;
	StubHolder* holder;
	u16 last_material;
};

CPhysicsShell* make_shell(StubHolder* holder, CPhysicsElement* e, float mass, const Fmatrix& at)
{
	CPhysicsShell* s = P_create_Shell();
	s->add_Element(e);
	s->setMass(mass);
	s->set_PhysicsRefObject(holder);
	holder->shell = s;
	s->Activate(at, 0.f, at, false); // the (form, bool) overload is an empty stub; P_build_SimpleShell uses this one
	return s;
}

CPhysicsElement* box_element(float hx, float hy, float hz)
{
	CPhysicsElement* e = P_create_Element();
	Fobb b;
	b.m_translate.set(0, 0, 0);
	b.m_halfsize.set(hx, hy, hz);
	b.m_rotate.identity();
	e->add_Box(b);
	return e;
}

CPhysicsElement* sphere_element(float r)
{
	CPhysicsElement* e = P_create_Element();
	Fsphere s;
	s.P.set(0, 0, 0);
	s.R = r;
	e->add_Sphere(s);
	return e;
}

Fmatrix place(float x, float y, float z, float yaw)
{
	Fmatrix m;
	m.setHPB(yaw, 0.f, 0.f);
	m.c.set(x, y, z);
	return m;
}

void state_of(CPhysicsShell* s, Fvector& pos, Fvector& vel)
{
	Fmatrix m;
	s->GetGlobalTransformDynamic(&m);
	pos.set(m.c);
	s->get_LinearVel(vel);
}

// Three thin boxes joined end to end by ball joints, like a short rope or a limb of a ragdoll.
const int k_links = 3;
const float k_link_len = 0.3f; // distance between the link centres

CPhysicsShell* make_chain(StubHolder* holder, const Fmatrix& at)
{
	CPhysicsShell* s = P_create_Shell();
	CPhysicsElement* e[k_links];
	for (int i = 0; i < k_links; ++i)
	{
		e[i] = box_element(0.5f * k_link_len, 0.05f, 0.05f);
		e[i]->mXFORM.identity();
		e[i]->mXFORM.c.set(i * k_link_len, 0.f, 0.f); // element frame in the frame of the shell
		s->add_Element(e[i]);
	}
	for (int i = 0; i + 1 < k_links; ++i)
	{
		CPhysicsJoint* j = P_create_Joint(CPhysicsJoint::ball, e[i], e[i + 1]);
		j->SetAnchorVsFirstElement(0.5f * k_link_len, 0.f, 0.f);
		s->add_Joint(j);
	}
	s->setMass(3.f);
	s->set_PhysicsRefObject(holder);
	holder->shell = s;
	s->SetPrefereExactIntegration(); // like the ragdolls of single player: its island goes to dWorldStep, not dWorldQuickStep
	s->Activate(at, 0.f, at, false);
	return s;
}

// worst error of the link spacing of an assembled chain
float chain_stretch(CPhysicsShell* s)
{
	float worst = 0.f;
	for (int i = 0; i + 1 < k_links; ++i)
	{
		Fmatrix a, b;
		s->get_ElementByStoreOrder(u16(i))->GetGlobalTransformDynamic(&a);
		s->get_ElementByStoreOrder(u16(i + 1))->GetGlobalTransformDynamic(&b);
		worst = std::max(worst, fabsf(a.c.distance_to(b.c) - k_link_len));
	}
	return worst;
}

// ------------------------------------------------------------------------------------------------------------------
// --stress: a random heap of bodies
// ------------------------------------------------------------------------------------------------------------------
struct Rng
{
	uint32_t x;
	explicit Rng(uint32_t seed) : x(seed * 2654435761u + 1u) {}

	uint32_t next()
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return x;
	}

	float f(float lo, float hi) { return lo + (hi - lo) * (float(next() >> 8) / 16777216.f); }
};

int run_stress(StubDevice& device, int count, unsigned seed, float seconds, bool digest)
{
	Rng rng(seed);
	xr_vector<CPhysicsShell*> shells;
	xr_vector<CPHCharacter*> walkers;
	xr_vector<StubHolder*> holders;
	xr_vector<u16> last_material;
	last_material.resize(8, 0);
	int made_chains = 0;
	for (int i = 0; i < count; ++i)
	{
		StubHolder* h = new StubHolder(u16(100 + i), Fvector().set(0.f, 0.f, 0.f));
		holders.push_back(h);
		Fmatrix at;
		at.setHPB(rng.f(0.f, 6.28f), rng.f(-1.5f, 1.5f), rng.f(-3.1f, 3.1f));
		at.c.set(rng.f(-2.f, 2.f), 0.6f + 0.35f * i, rng.f(-2.f, 2.f));
		switch (i % 4)
		{
		case 0:
			shells.push_back(make_shell(h, box_element(rng.f(0.1f, 0.4f), rng.f(0.1f, 0.4f), rng.f(0.1f, 0.4f)),
			                            rng.f(2.f, 30.f), at));
			break;
		case 1:
			shells.push_back(make_shell(h, sphere_element(rng.f(0.1f, 0.35f)), rng.f(1.f, 20.f), at));
			break;
		case 2:
			shells.push_back(make_chain(h, at));
			++made_chains;
			break;
		default:
			shells.push_back(make_shell(h, box_element(rng.f(0.3f, 0.5f), 0.05f, rng.f(0.3f, 0.5f)), 8.f, at));
			shells.back()->SetForceExactIntegration(); // dWorldStep for this island as long as it is small
			break;
		}
	}
	for (int i = 0; i < 4; ++i)
	{
		CPHCharacter* c = (i & 1) ? create_ai_character() : create_actor_character(true);
		c->SetMas(80.f);
		c->SetPhysicsRefObject(holders[i % holders.size()]);
		c->SetPLastMaterialIDX(&last_material[i]);
		dVector3 size = {0.6f, 1.8f, 0.6f};
		c->Create(size);
		c->SetPosition(Fvector().set(-3.f + 2.f * i, 1.f + 0.2f * i, -4.f));
		c->SetMaterial(0);
		walkers.push_back(c);
	}
	printf("  %d shells (%d chains), %d characters, seed %u\n", int(shells.size()), made_chains, int(walkers.size()), seed);

	const float dt = 1.f / 30.f;
	const int frames = int(seconds / dt);
	Digest dg;
	int nan_events = 0, escaped = 0, sunk = 0;
	float worst_stretch = 0.f;
	for (int f = 0; f < frames; ++f)
	{
		device.frame(dt);
		for (size_t i = 0; i < shells.size(); ++i)
		{
			Fvector p, v;
			state_of(shells[i], p, v);
			dg.add(p);
			dg.add(v);
			if (!finite3(p) || !finite3(v))
				++nan_events;
			else if (fabsf(p.x) > 50.f || fabsf(p.z) > 50.f || p.y > 80.f)
				++escaped;
			else if (p.y < -0.2f)
				++sunk;
			if (shells[i]->get_ElementsNumber() == u16(k_links))
				worst_stretch = std::max(worst_stretch, chain_stretch(shells[i]));
		}
		for (size_t i = 0; i < walkers.size(); ++i)
		{
			Fvector p, v;
			walkers[i]->GetPosition(p);
			walkers[i]->GetVelocity(v);
			dg.add(p);
			dg.add(v);
			if (!finite3(p) || !finite3(v))
				++nan_events;
			else if (fabsf(p.x) > 50.f || fabsf(p.z) > 50.f || p.y > 80.f)
				++escaped;
			else if (p.y < -0.5f)
				++sunk;
		}
	}
	printf("  after %.0f s: %llu physics steps, worst chain spacing error %.3f m\n", seconds,
	       (unsigned long long)physics_world()->StepsNum(), worst_stretch);
	check(nan_events == 0, "no body turned NaN");
	check(escaped == 0, "no body left the level");
	check(sunk == 0, "no body fell through the ground");
	char what[96];
	snprintf(what, sizeof(what), "chain joints held in the heap (worst link spacing error %.3f m)", worst_stretch);
	check(worst_stretch < 1.f, what); // soft joints: a chain crushed by the heap stretches by tenths of a metre

	for (size_t i = 0; i < walkers.size(); ++i)
	{
		walkers[i]->Destroy();
		xr_delete(walkers[i]);
	}
	for (size_t i = 0; i < shells.size(); ++i)
		destroy_physics_shell(shells[i]);
	if (digest)
		printf("digest %016llx\n", (unsigned long long)dg.h);
	return g_failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
	float seconds = 6.f;
	bool print = false, digest = false;
	int stress = 0;
	unsigned seed = 1;
	std::string only; // comma separated subset of the scene: boxa, boxb, sphere, chain, actor, ai (empty: all)
	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = float(atof(argv[++i]));
		else if (!strcmp(argv[i], "--print"))
			print = true;
		else if (!strcmp(argv[i], "--digest"))
			digest = true;
		else if (!strcmp(argv[i], "--only") && i + 1 < argc)
			only = std::string(",") + argv[++i] + ",";
		else if (!strcmp(argv[i], "--stress") && i + 1 < argc)
			stress = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
			seed = unsigned(atoi(argv[++i]));
		else
		{
			printf("usage: xrphysics_smoke [--seconds N] [--print] [--digest] [--only boxa,boxb,sphere,chain,actor,ai] [--stress N [--seed S]]\n");
			return 2;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0); // keep the output ahead of a crash report
	printf("xrphysics_smoke\n");
	Debug._initialize(false);
	Core._initialize("xrphysics_smoke", engine_log, FALSE);
	DllMainXrPhysics(NULL, DLL_PROCESS_ATTACH, NULL); // ODE allocation hooks, as x_ray.cpp does

	// ---------------------------------------------------------------------------------------------
	printf("\n[1] materials, static mesh, world\n");
	{
		// zeroed storage is a valid empty state for the library's std::vector members
		CGameMtlLibrary* lib = (CGameMtlLibrary*)calloc(1, sizeof(CGameMtlLibrary));
		GameMtlVec& materials = lib->*get(MaterialsTag());
		new (&materials) GameMtlVec();
		SGameMtl* ground = new SGameMtl();
		ground->m_Name = "materials\\earth";
		SGameMtl* small_box = new SGameMtl();
		small_box->m_Name = "objects\\small_box";
		materials.push_back(ground);
		materials.push_back(small_box);
		set_mtl_lib(lib);
		check(GMLibrary().GetMaterialIdx("objects\\small_box") == 1, "material library answers by name");
	}

	// a flat 200 x 200 m ground at y = 0, normal up
	Fvector verts[4] = {{-100.f, 0.f, -100.f}, {-100.f, 0.f, 100.f}, {100.f, 0.f, 100.f}, {100.f, 0.f, -100.f}};
	CDB::TRI tris[2];
	memset(tris, 0, sizeof(tris));
	tris[0].verts[0] = 0;
	tris[0].verts[1] = 1;
	tris[0].verts[2] = 2;
	tris[1].verts[0] = 0;
	tris[1].verts[1] = 2;
	tris[1].verts[2] = 3;
	tris[0].material = tris[1].material = 0;
	hdrCFORM H;
	H.version = CFORM_CURRENT_VERSION;
	H.vertcount = 4;
	H.facecount = 2;
	H.aabb.set(-100.f, -30.f, -100.f, 100.f, 60.f, 100.f);

	CObjectSpace* space = mesh_create_object_space(verts, tris, H, 0);
	check(space != 0, "mesh_create_object_space");

	static StubDevice device;
	create_physics_world(false, space, 0, &device);
	check(physics_world() != 0, "create_physics_world");
	check(device.frames.size() == 1, "the world registered itself for frame callbacks");
	printf("  gravity %.3f, fixed step %.4f s, solver iterations %d\n", physics_world()->Gravity(), fixed_step,
	       phIterations);

	if (stress > 0)
	{
		printf("\n[2] stress heap\n");
		const int rc = run_stress(device, stress, seed, seconds, digest);
		destroy_physics_world();
		destroy_object_space(space);
		printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "all checks passed", g_failures,
		       g_failures == 1 ? "" : "s");
		return rc;
	}

	// ---------------------------------------------------------------------------------------------
	printf("\n[2] shells and characters\n");
	static StubHolder h_box_a(1, Fvector().set(0.f, 0.f, 0.f)), h_box_b(2, Fvector().set(0.f, 0.f, 0.f)),
		h_ball(3, Fvector().set(0.f, 0.f, 0.f)), h_chain(4, Fvector().set(0.f, 0.f, 0.f)),
		h_actor(10, Fvector().set(0.f, 0.f, 0.f)),
		h_ai(11, Fvector().set(0.f, 0.f, 0.f));

	Body bodies[4];
	// box A rests (nearly) on the ground, box B is dropped onto it, a sphere lands next to them
	auto want = [&](const char* what) { return only.empty() || only.find(std::string(",") + what + ",") != std::string::npos; };
	memset(bodies, 0, sizeof(bodies));
	bodies[0].name = "box A";
	bodies[0].holder = &h_box_a;
	if (want("boxa"))
		bodies[0].shell = make_shell(&h_box_a, box_element(k_half, k_half, k_half), 10.f, place(0.f, 0.30f, 0.f, 0.5f));
	bodies[1].name = "box B";
	bodies[1].holder = &h_box_b;
	if (want("boxb"))
		bodies[1].shell = make_shell(&h_box_b, box_element(k_half, k_half, k_half), 10.f, place(0.05f, 1.6f, 0.02f, 0.2f));
	bodies[2].name = "sphere";
	bodies[2].holder = &h_ball;
	if (want("sphere"))
		bodies[2].shell = make_shell(&h_ball, sphere_element(k_half), 5.f, place(1.5f, 3.0f, 0.f, 0.f));
	// a three link chain stood up on one end (tilted 70 degrees), 2 m away from the others: it topples and folds at the joints
	bodies[3].name = "chain";
	bodies[3].holder = &h_chain;
	if (want("chain"))
	{
		Fmatrix at;
		at.setHPB(0.f, 0.f, 1.2f);
		at.c.set(-1.f, 0.5f, 2.f);
		bodies[3].shell = make_chain(&h_chain, at);
		check(bodies[3].shell->get_ElementsNumber() == k_links && bodies[3].shell->get_JointsNumber() == k_links - 1,
		      "chain has 3 elements and 2 joints");
	}
	for (int i = 0; i < 4; ++i)
		if (bodies[i].shell)
			check(bodies[i].shell->isActive(), bodies[i].name);

	Walker walkers[2];
	memset(walkers, 0, sizeof(walkers));
	walkers[0].name = "actor";
	walkers[0].holder = &h_actor;
	walkers[1].name = "ai";
	walkers[1].holder = &h_ai;
	if (want("actor"))
		walkers[0].ch = create_actor_character(true);
	if (want("ai"))
		walkers[1].ch = create_ai_character();
	const Fvector walker_start[2] = {{-3.f, 1.5f, 0.f}, {3.f, 1.5f, 0.5f}};
	for (int i = 0; i < 2; ++i)
	{
		Walker& w = walkers[i];
		if (!w.ch)
			continue;
		w.last_material = 0;
		w.ch->SetMas(80.f);
		w.ch->SetPhysicsRefObject(w.holder);
		w.ch->SetPLastMaterialIDX(&w.last_material);
		dVector3 size = {0.6f, 1.8f, 0.6f};
		w.ch->Create(size);
		w.ch->SetPosition(walker_start[i]);
		w.ch->SetMaterial(0);
		check(w.ch->b_exist, w.name);
	}

	// ---------------------------------------------------------------------------------------------
	printf("\n[3] simulate %.1f s at 30 fps\n", seconds);
	const float dt = 1.f / 30.f;
	const int frames = int(seconds / dt);
	Digest dg;
	Digest per[6]; // one per tracked body, to see which of them diverges between two runs
	float min_y[6] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
	float worst_stretch = 0.f;
	bool all_finite = true;
	bool inside = true;
	for (int f = 0; f < frames; ++f)
	{
		device.frame(dt);
		const bool second_mark = ((f + 1) % 30) == 0;
		for (int i = 0; i < 4; ++i)
		{
			if (!bodies[i].shell)
				continue;
			Fvector p, v;
			state_of(bodies[i].shell, p, v);
			all_finite = all_finite && finite3(p) && finite3(v);
			inside = inside && fabsf(p.x) < 50.f && fabsf(p.z) < 50.f && p.y > -5.f && p.y < 50.f;
			min_y[i] = std::min(min_y[i], p.y);
			dg.add(p);
			dg.add(v);
			per[i].add(p);
			per[i].add(v);
			if (print && second_mark)
				printf("  t=%4.1f %-6s pos %8.4f %8.4f %8.4f  vel %8.4f %8.4f %8.4f\n", (f + 1) * dt, bodies[i].name, p.x,
				       p.y, p.z, v.x, v.y, v.z);
		}
		if (bodies[3].shell)
		{
			const float stretch = chain_stretch(bodies[3].shell);
			worst_stretch = std::max(worst_stretch, stretch);
			if (print && second_mark)
				printf("  t=%4.1f chain link spacing error %.4f\n", (f + 1) * dt, stretch);
			dg.add(stretch);
			per[3].add(stretch);
		}
		for (int i = 0; i < 2; ++i)
		{
			if (!walkers[i].ch)
				continue;
			Fvector p, v;
			walkers[i].ch->GetPosition(p);
			walkers[i].ch->GetVelocity(v);
			all_finite = all_finite && finite3(p) && finite3(v);
			inside = inside && fabsf(p.x) < 50.f && fabsf(p.z) < 50.f && p.y > -5.f && p.y < 50.f;
			min_y[4 + i] = std::min(min_y[4 + i], p.y);
			dg.add(p);
			dg.add(v);
			per[4 + i].add(p);
			per[4 + i].add(v);
			if (print && second_mark)
				printf("  t=%4.1f %-6s pos %8.4f %8.4f %8.4f  vel %8.4f %8.4f %8.4f\n", (f + 1) * dt, walkers[i].name, p.x,
				       p.y, p.z, v.x, v.y, v.z);
		}
	}
	printf("  physics steps run: %llu\n", (unsigned long long)physics_world()->StepsNum());
	check(physics_world()->StepsNum() > u64(frames / 2), "the world stepped");
	check(all_finite, "every position and velocity stayed finite");
	check(inside, "nothing left the level");

	// ---------------------------------------------------------------------------------------------
	printf("\n[4] resting state\n");
	{
		Fvector p[4], v[4];
		for (int i = 0; i < 4; ++i)
			if (bodies[i].shell)
				state_of(bodies[i].shell, p[i], v[i]);
		// y of the centre is the half extent when resting; the soft contact lets a body sink a few millimetres
		if (bodies[0].shell)
			check(fabsf(p[0].y - k_half) < 0.03f, "box A rests on the ground");
		if (bodies[0].shell && bodies[1].shell)
			check(fabsf(p[1].y - (3.f * k_half)) < 0.05f, "box B rests on box A");
		if (bodies[2].shell)
			check(fabsf(p[2].y - k_half) < 0.03f, "sphere rests on the ground");
		for (int i = 0; i < 4; ++i)
		{
			if (!bodies[i].shell)
				continue;
			char what[96];
			snprintf(what, sizeof(what), "%s is at rest (|v| = %.4f)", bodies[i].name, v[i].magnitude());
			check(v[i].magnitude() < 0.05f, what);
			snprintf(what, sizeof(what), "%s never went through the ground (min y %.3f)", bodies[i].name, min_y[i]);
			check(min_y[i] > (i == 3 ? 0.f : 0.1f), what);
		}
		if (bodies[3].shell)
		{
			char what[96];
			snprintf(what, sizeof(what), "chain joints held (worst link spacing error %.4f m)", worst_stretch);
			check(worst_stretch < 0.15f, what);
			snprintf(what, sizeof(what), "chain lies on the ground (y %.3f)", p[3].y);
			check(p[3].y > 0.f && p[3].y < 0.12f, what);
		}
		for (int i = 0; i < 2; ++i)
		{
			if (!walkers[i].ch)
				continue;
			Fvector pos, vel;
			walkers[i].ch->GetPosition(pos);
			walkers[i].ch->GetVelocity(vel);
			char what[96];
			snprintf(what, sizeof(what), "%s stands on the ground (y %.3f, |v| %.4f)", walkers[i].name, pos.y,
			         vel.magnitude());
			// GetPosition is the foot centre: the ground is at y = 0 and the contact is soft, a centimetre of sag is normal
			check(fabsf(pos.y) < 0.05f && vel.magnitude() < 0.05f, what);
			snprintf(what, sizeof(what), "%s did not sink (min y %.3f)", walkers[i].name, min_y[4 + i]);
			check(min_y[4 + i] > -0.1f, what);
		}
	}

	// ---------------------------------------------------------------------------------------------
	printf("\n[5] teardown\n");
	for (int i = 0; i < 2; ++i)
		if (walkers[i].ch)
		{
			walkers[i].ch->Destroy();
			xr_delete(walkers[i].ch);
		}
	for (int i = 0; i < 4; ++i)
		if (bodies[i].shell)
			destroy_physics_shell(bodies[i].shell);
	destroy_physics_world();
	destroy_object_space(space);
	check(physics_world() == 0, "world destroyed");

	if (digest)
	{
		printf("digest %016llx  (box A %08x, box B %08x, sphere %08x, chain %08x, actor %08x, ai %08x)\n",
		       (unsigned long long)dg.h, (unsigned)per[0].h, (unsigned)per[1].h, (unsigned)per[2].h, (unsigned)per[3].h,
		       (unsigned)per[4].h, (unsigned)per[5].h);
	}
	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "all checks passed", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}

// ----------------------------------------------------------------------------------------------------------------
// Symbols xrPhysics expects from the rest of the engine (see docs/ios-port/XRPHYSICS.md)
// ----------------------------------------------------------------------------------------------------------------
CGameMtlLibrary* PGMLib = NULL; // xrAPI
bool g_actor_allow_ladder = true; // xrGame (level_script.cpp)
u32 g_dead_body_collision = 1; // xrGame (console_commands.cpp)
IRender_interface* Render = NULL; // xrAPI: needed by xrCDB's ISpatial.cpp, only used for the sector of renderable objects
class CObject // xrEngine: smart_cast<CObject*> in CPHSimpleCharacter; the stub holder is never a CObject, so it yields null
{
public:
	virtual ~CObject();
};
CObject::~CObject() {}
