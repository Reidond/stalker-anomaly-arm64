// Drops a 1 m box onto a plane for 2 s with each of ODE's three steppers and prints where it ends up.
// Exits non-zero if the box did not come to rest on the plane. Y is up, as in X-Ray.
//
// This follows how xrPhysics drives X-Ray's ODE (see CPHIsland): bodies and contact joints are created
// without a world and added to the dxWorld explicitly, and the contacts are unlinked again after the step.
#include <cmath>
#include <cstdio>
#include <vector>
#include <ode/ode.h>

namespace
{
enum class Stepper { Step, QuickStep, StepFast1 };

struct Sim
{
    dWorldID world;
    dJointGroupID contacts;
    std::vector<dJointID> joints;
};

void near_callback(void* data, dGeomID o1, dGeomID o2)
{
    Sim* sim = static_cast<Sim*>(data);
    dBodyID b1 = dGeomGetBody(o1);
    dBodyID b2 = dGeomGetBody(o2);

    dContact contact[8];
    const int n = dCollide(o1, o2, 8, &contact[0].geom, sizeof(dContact));
    for (int i = 0; i < n; ++i)
    {
        contact[i].surface.mode = 0;
        contact[i].surface.mu = 1.0f;
        dJointID joint = dJointCreateContact(nullptr, sim->contacts, &contact[i]);
        dJointAttach(joint, b1, b2);
        dWorldAddJoint(sim->world, joint);
        sim->joints.push_back(joint);
    }
}

struct Result
{
    dReal pos[3];
    dReal speed;
    bool finite;
};

Result run(Stepper stepper)
{
    dWorldID world = dWorldCreate();
    dWorldSetGravity(world, 0, -9.81f, 0);
    dWorldSetERP(world, 0.2f);
    dWorldSetCFM(world, 1e-5f);

    dSpaceID space = dSimpleSpaceCreate(nullptr);
    dJointGroupID contacts = dJointGroupCreate(0);
    dCreatePlane(space, 0, 1, 0, 0);

    dBodyID body = dBodyCreate(nullptr);
    dWorldAddBody(world, body);
    dMass mass;
    dMassSetBox(&mass, 1.0f, 1.0f, 1.0f, 1.0f);
    dBodySetMass(body, &mass);
    dBodySetPosition(body, 0.0f, 5.0f, 0.0f);

    dGeomID box = dCreateBox(space, 1.0f, 1.0f, 1.0f);
    dGeomSetBody(box, body);

    Sim sim{world, contacts, {}};
    const dReal dt = 0.01f;
    for (int i = 0; i < 200; ++i) // 2 s
    {
        dSpaceCollide(space, &sim, near_callback);
        switch (stepper)
        {
        case Stepper::Step: dWorldStep(world, dt); break;
        case Stepper::QuickStep: dWorldQuickStep(world, dt); break;
        case Stepper::StepFast1: dWorldStepFast1(world, dt, 20); break;
        }
        // dJointGroupEmpty() neither unlinks joints from the world nor detaches joints that have no
        // world, so do both first.
        for (dJointID joint : sim.joints)
        {
            dJointAttach(joint, nullptr, nullptr);
            dWorldRemoveJoint(world, joint);
        }
        sim.joints.clear();
        dJointGroupEmpty(contacts);
    }

    Result r{};
    const dReal* p = dBodyGetPosition(body);
    const dReal* v = dBodyGetLinearVel(body);
    for (int i = 0; i < 3; ++i)
        r.pos[i] = p[i];
    r.speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    r.finite = std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::isfinite(r.speed);

    dJointGroupDestroy(contacts);
    dGeomDestroy(box);
    dBodyDestroy(body);
    dSpaceDestroy(space);
    dWorldDestroy(world);
    return r;
}
} // namespace

int main()
{
    struct Case { const char* name; Stepper stepper; };
    const Case cases[] = {
        {"dWorldStep", Stepper::Step},
        {"dWorldQuickStep", Stepper::QuickStep},
        {"dWorldStepFast1", Stepper::StepFast1},
    };

    int failures = 0;
    for (const Case& c : cases)
    {
        const Result r = run(c.stepper);
        // A 1 m box resting on y=0 has its centre at y=0.5; allow the contact penetration/ERP slack.
        const bool ok = r.finite && std::fabs(r.pos[1] - 0.5f) < 0.05f && std::fabs(r.pos[0]) < 1e-2f &&
                        std::fabs(r.pos[2]) < 1e-2f && r.speed < 0.1f;
        std::printf("%-16s final position = (%.4f, %.4f, %.4f), speed = %.4f  %s\n", c.name, r.pos[0], r.pos[1],
                    r.pos[2], r.speed, ok ? "ok" : "FAIL");
        if (!ok)
            ++failures;
    }

    dCloseODE();
    std::printf(failures ? "ode_smoke: FAILED\n" : "ode_smoke: passed\n");
    return failures ? 1 : 0;
}
