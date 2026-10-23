#include "SelfTest.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "GameInput/Adapter.h"
#include "GameInput/Profile.h"

using namespace gameinput;

namespace
{
struct Harness
{
    void (*log)(const char*, void*) = nullptr;
    void* user = nullptr;
    int checks = 0;
    int failed = 0;
    const char* section = "";
};
Harness* g_h = nullptr;

void Report(const char* fmt, const char* a, int line)
{
    char buffer[400];
    std::snprintf(buffer, sizeof(buffer), fmt, g_h->section, a, line);
    g_h->log(buffer, g_h->user);
}

void Check(bool ok, const char* expr, int line)
{
    ++g_h->checks;
    if (!ok)
    {
        ++g_h->failed;
        Report("FAIL [%s] %s (line %d)", expr, line);
    }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

void Section(const char* name)
{
    g_h->section = name;
}

bool Near(float a, float b, float eps = 0.01f)
{
    return std::fabs(a - b) <= eps;
}

// ---- helpers ------------------------------------------------------------------------------------

PadSnapshot IdlePad()
{
    PadSnapshot p;
    p.connected = true;
    return p;
}

bool HasKey(const FrameOutput& f, KeyCode key, bool down)
{
    for (const KeyEvent& e : f.keys)
        if (e.key == key && e.down == down)
            return true;
    return false;
}

bool HasAction(const std::vector<std::string>& v, const char* name)
{
    for (const std::string& s : v)
        if (s == name)
            return true;
    return false;
}

bool Holding(const FrameOutput& f, KeyCode key)
{
    for (KeyCode k : f.held)
        if (k == key)
            return true;
    return false;
}

constexpr float kDt = 1.0f / 60.0f;

FrameOutput Step(GameInput& gi, const PadSnapshot& pad, float dt = kDt)
{
    return gi.Update(dt, pad);
}

// A GameInput that has already seen one idle frame (so the reconnect latch is cleared).
void Warm(GameInput& gi)
{
    Step(gi, IdlePad());
}

struct RecordingFeedbackSink : IFeedbackSink
{
    int rumble = 0, led = 0, triggers = 0, silence = 0;
    MotorState last_motors;
    Rgb last_led;
    TriggerPair last_triggers;
    void SetRumble(const MotorState& m) override { ++rumble; last_motors = m; }
    void SetLed(Rgb c) override { ++led; last_led = c; }
    void SetTriggers(const TriggerPair& t) override { ++triggers; last_triggers = t; }
    void Silence() override { ++silence; }
};

struct RecordingEngineSink : IEngineInputSink
{
    std::vector<std::string> log;
    void KeyPress(KeyCode k) override { log.push_back("press " + PadKeyName(k)); }
    void KeyRelease(KeyCode k) override { log.push_back("release " + PadKeyName(k)); }
    void KeyHold(KeyCode k) override { log.push_back("hold " + PadKeyName(k)); }
    void MouseMove(int dx, int dy) override { log.push_back("move " + std::to_string(dx) + "," + std::to_string(dy)); }
    void MouseWheel(int n) override { log.push_back("wheel " + std::to_string(n)); }
    void CursorJump(float x, float y) override { log.push_back("jump " + std::to_string(int(x)) + "," + std::to_string(int(y))); }
    void PadConnected(const DeviceIdentity&) override { log.push_back("connected"); }
    void PadDisconnected(const DeviceIdentity&) override { log.push_back("disconnected"); }
};

struct FixedNav : INavigationProvider
{
    bool FindNeighbor(Vec2, NavDir, Vec2& out) override
    {
        out = {100.0f, 200.0f};
        return true;
    }
};

// ---- tests --------------------------------------------------------------------------------------

void TestKeys()
{
    Section("keys");
    for (KeyCode k = kPadKeyBase; k < kPadKeyEnd; ++k)
    {
        const std::string name = PadKeyName(k);
        CHECK(!name.empty());
        CHECK(PadKeyFromName(name) == k);
        CHECK(!PadKeyLabel(k).empty());
    }
    CHECK(PadKeyName(PK_Cross) == "gp_cross");
    CHECK(PadKeyFromName("GP_CROSS") == PK_Cross);
    CHECK(PadKeyFromName("gp_radial_3") == PK_RADIAL_1 + 2);
    CHECK(PadKeyFromName("gp_radial_33") == PK_NONE);
    CHECK(PadKeyFromName("kESCAPE") == PK_NONE);
    CHECK(PadKeyLabel(PK_Cross, ControllerFamily::PlayStation) == "Cross");
    CHECK(PadKeyLabel(PK_Cross, ControllerFamily::Xbox) == "A");
    // The pad range must not collide with the engine's own codes (DIK <= 0xED, mouse 337..344, custom 1000..1099).
    CHECK(kPadKeyBase > 1099);
    CHECK(PadKeyCount() == int(EnginePadKeyTable(ControllerFamily::PlayStation).size()));
}

void TestFilters()
{
    Section("filters");
    CHECK(ApplyRadialDeadzone({0.1f, 0.0f}, 0.18f, 0.97f).Length() == 0.0f);
    CHECK(Near(ApplyRadialDeadzone({0.97f, 0.0f}, 0.18f, 0.97f).x, 1.0f));
    CHECK(Near(ApplyRadialDeadzone({0.575f, 0.0f}, 0.18f, 0.97f).x, 0.5f));
    CHECK(Near(ResponseCurve(0.5f, 2.0f), 0.25f));

    CHECK(SnapToCardinal({0.05f, 1.0f}, 9.0f).x == 0.0f);
    CHECK(SnapToCardinal({0.5f, 0.9f}, 9.0f).x == 0.5f);

    Hysteresis h;
    CHECK(!h.Update(0.4f, 0.5f, 0.3f));
    CHECK(h.Update(0.6f, 0.5f, 0.3f));
    CHECK(h.Update(0.4f, 0.5f, 0.3f));
    CHECK(!h.Update(0.25f, 0.5f, 0.3f));

    CountAccumulator acc;
    CHECK(acc.Add(0.4f) == 0);
    CHECK(acc.Add(0.4f) == 0);
    CHECK(acc.Add(0.4f) == 1);
    CountAccumulator neg;
    CHECK(neg.Add(-0.7f) == 0);
    CHECK(neg.Add(-0.7f) == -1);

    LookConfig cfg;
    LookFilter look;
    CHECK(look.Update({0.05f, 0.05f}, kDt, false, cfg).Length() == 0.0f);
    const Vec2 full = look.Update({1.0f, 0.0f}, kDt, false, cfg);
    CHECK(full.x > cfg.yaw_speed * kDt * 0.99f && full.x < cfg.yaw_speed * kDt * 1.6f);
    LookFilter slow;
    CHECK(slow.Update({1.0f, 0.0f}, kDt, true, cfg).x < full.x * 0.7f);
    LookFilter down;
    CHECK(down.Update({0.0f, 0.8f}, kDt, false, cfg).y > 0.0f);
    cfg.invert_y = 1.0f;
    LookFilter inverted;
    CHECK(inverted.Update({0.0f, 0.8f}, kDt, false, cfg).y < 0.0f);

    // Holding the stick at the edge ramps up, releasing it resets.
    cfg = LookConfig();
    LookFilter ramp;
    float first = 0, last = 0;
    for (int i = 0; i < 60; ++i)
    {
        const float x = ramp.Update({1.0f, 0.0f}, kDt, false, cfg).x;
        if (i == 0) first = x;
        last = x;
    }
    CHECK(last > first * 1.3f);
}

void TestGyro()
{
    Section("gyro");
    GyroConfig cfg;
    cfg.mode = float(GyroMode::Always);
    cfg.smoothing = 0.0f;

    PadSnapshot pad;
    pad.connected = true;
    pad.has_gyro = pad.has_accel = true;
    pad.accel[1] = 9.81f; // lying flat: up is +y

    // Turning the pad clockwise seen from above (negative rotation about up) = look right.
    pad.gyro[1] = -1.0f;
    GyroAim aim;
    Vec2 out = aim.Update(pad, 0.1f, false, cfg);
    CHECK(out.x > 5.0f && out.x < 6.0f);
    CHECK(Near(out.y, 0.0f, 0.001f));

    // Rotation about the pad's x axis tips the top edge towards the player = look down.
    pad.gyro[1] = 0.0f;
    pad.gyro[0] = 1.0f;
    GyroAim pitch;
    out = pitch.Update(pad, 0.1f, false, cfg);
    CHECK(out.y > 5.0f && Near(out.x, 0.0f, 0.001f));

    // Held upright (gravity along -z in controller space), yaw comes from the z axis.
    PadSnapshot upright = pad;
    upright.gyro[0] = 0.0f;
    upright.accel[1] = 0.0f;
    upright.accel[2] = -9.81f;
    upright.gyro[2] = 1.0f; // +z rotation = clockwise from above when z points down = turn right
    GyroAim up_aim;
    out = up_aim.Update(upright, 0.1f, false, cfg);
    CHECK(out.x > 5.0f);

    // AdsOnly: nothing until aiming.
    cfg.mode = float(GyroMode::AdsOnly);
    pad.gyro[0] = 0.0f;
    pad.gyro[1] = -1.0f;
    GyroAim ads;
    CHECK(ads.Update(pad, 0.1f, false, cfg).Length() == 0.0f);
    CHECK(ads.Update(pad, 0.1f, true, cfg).x > 0.0f);
    cfg.mode = float(GyroMode::Off);
    CHECK(ads.Update(pad, 0.1f, true, cfg).Length() == 0.0f);

    // Constant offset is learned and then ignored.
    cfg.mode = float(GyroMode::Always);
    PadSnapshot drift;
    drift.connected = drift.has_gyro = true;
    drift.gyro[0] = 0.012f;
    drift.gyro[1] = 0.003f;
    drift.gyro[2] = -0.008f;
    GyroAim cal;
    for (int i = 0; i < 180; ++i)
        cal.Update(drift, kDt, false, cfg);
    CHECK(cal.Calibrated());
    CHECK(Near(cal.Bias()[0], 0.012f, 0.002f));
    CHECK(cal.Update(drift, kDt, false, cfg).Length() < 0.001f);
}

void TestBindings()
{
    Section("bindings");
    BindingMap m = BindingMap::Default();
    CHECK(m.KeyForAction("jump") == PK_Cross);
    CHECK(m.KeyForAction("wpn_fire") == PK_R2);
    CHECK(m.KeyForAction("wpn_zoom") == PK_L2);
    CHECK(m.ActionForKey(PK_Cross) && *m.ActionForKey(PK_Cross) == "jump");
    CHECK(m.ActionForKey(PK_Mic) == nullptr);
    CHECK(m.ActionForKey(PK_Ps) == nullptr); // iPadOS keeps the PS button for Home

    // Rebinding moves the key and drops the old owner.
    CHECK(m.Bind("crouch", PK_Cross));
    CHECK(m.KeyForAction("crouch") == PK_Cross);
    CHECK(m.KeyForAction("jump") == PK_NONE);
    CHECK(m.KeyForAction("crouch") != PK_Circle);
    CHECK(!m.Bind("not_an_action", PK_Cross));
    CHECK(!m.Bind("jump", engine_key::kMouse1));

    // Every default bind names a real stock action and a valid key.
    for (const ActionInfo& a : StockActions())
    {
        const KeyCode key = BindingMap::Default().KeyForAction(a.name);
        if (key != PK_NONE)
            CHECK(IsPadKey(key));
    }

    // Text round trip.
    BindingMap base = BindingMap::Default();
    BindingMap copy;
    std::vector<std::string> errors;
    CHECK(copy.Load(base.Serialize(), &errors) == int(base.Size()));
    CHECK(errors.empty());
    CHECK(copy.Serialize() == base.Serialize());

    errors.clear();
    copy.Load("bind_pad jump gp_square # comment\nbind_pad nonsense gp_l1\nbind_pad jump nokey\nfoo bar\n", &errors);
    CHECK(copy.KeyForAction("jump") == PK_Square);
    CHECK(errors.size() == 3);
    copy.Load("unbind_pad jump\n");
    CHECK(copy.KeyForAction("jump") == PK_NONE);
    copy.Load("unbindall_pad\n");
    CHECK(copy.Size() == 0);

    CHECK(ActionLabel("wpn_fire") == "Fire");
    CHECK(ActionLabel("whatever") == "whatever");
}

void TestRadial()
{
    Section("radial");
    RadialConfig cfg;
    auto stick_at = [](float deg) {
        const float r = deg * 3.14159265f / 180.0f;
        return Vec2{std::sin(r), -std::cos(r)};
    };
    CHECK(RadialPickSlot({0, -1}, 8, -1, cfg) == 0);
    CHECK(RadialPickSlot({1, 0}, 8, -1, cfg) == 2);
    CHECK(RadialPickSlot({0, 1}, 8, -1, cfg) == 4);
    CHECK(RadialPickSlot({-1, 0}, 8, -1, cfg) == 6);
    CHECK(RadialPickSlot(stick_at(45), 8, -1, cfg) == 1);
    CHECK(RadialPickSlot({0.1f, 0.1f}, 8, -1, cfg) == -1);
    // Sticky borders: 25 degrees is just past the half-sector but still reads as slot 0 when already there.
    CHECK(RadialPickSlot(stick_at(25), 8, -1, cfg) == 1);
    CHECK(RadialPickSlot(stick_at(25), 8, 0, cfg) == 0);
    CHECK(RadialPickSlot(stick_at(35), 8, 0, cfg) == 1);

    RadialMenu menu;
    CHECK(menu.Wheels().size() == 2);
    const std::vector<bool> none = {false, false};
    const std::vector<bool> l1 = {true, false};

    // Tap.
    auto ev = menu.Update(0.05f, l1, {}, 0, cfg);
    CHECK(ev.empty() && menu.View().pending);
    ev = menu.Update(0.05f, none, {}, 0, cfg);
    CHECK(ev.size() == 1 && ev[0].type == RadialEvent::Type::Tap && ev[0].key == PK_L1);
    CHECK(!menu.Active());

    // Hold, open, page flip, confirm.
    for (int i = 0; i < 4; ++i)
        ev = menu.Update(0.1f, l1, {}, 0, cfg);
    CHECK(menu.Capturing() && menu.View().wheel == 0 && menu.View().page_count == 2);
    ev = menu.Update(0.016f, l1, {0, -1}, 0, cfg);
    CHECK(menu.View().selected == 0);
    ev = menu.Update(0.016f, l1, {0, -1}, +1, cfg);
    CHECK(menu.View().page == 1);
    ev = menu.Update(0.016f, none, {0, -1}, 0, cfg);
    CHECK(ev.size() == 1 && ev[0].type == RadialEvent::Type::Confirmed && ev[0].key == PK_RADIAL_1 + 8);
    CHECK(!menu.Active());

    // Release with the stick centred and nothing picked cancels.
    for (int i = 0; i < 4; ++i)
        menu.Update(0.1f, l1, {}, 0, cfg);
    ev = menu.Update(0.016f, none, {}, 0, cfg);
    CHECK(ev.size() == 1 && ev[0].type == RadialEvent::Type::Cancelled);

    // Flick and let the stick spring back: the pick survives the grace period, then expires.
    for (int i = 0; i < 4; ++i)
        menu.Update(0.1f, l1, {}, 0, cfg);
    menu.Update(0.016f, l1, {1.0f, 0.0f}, 0, cfg);
    menu.Update(0.1f, l1, {}, 0, cfg);
    CHECK(menu.View().selected == 2);
    ev = menu.Update(0.016f, none, {}, 0, cfg);
    CHECK(ev.size() == 1 && ev[0].type == RadialEvent::Type::Confirmed && ev[0].key == PK_RADIAL_1 + 2);
    for (int i = 0; i < 4; ++i)
        menu.Update(0.1f, l1, {}, 0, cfg);
    menu.Update(0.016f, l1, {1.0f, 0.0f}, 0, cfg);
    for (int i = 0; i < 5; ++i)
        menu.Update(0.1f, l1, {}, 0, cfg);
    CHECK(menu.View().selected == -1);
    ev = menu.Update(0.016f, none, {}, 0, cfg);
    CHECK(ev.size() == 1 && ev[0].type == RadialEvent::Type::Cancelled);

    // Every default slot key is unique and inside the radial range.
    std::vector<bool> seen(kRadialKeyCount, false);
    for (const RadialWheel& w : RadialMenu::DefaultWheels())
        for (const RadialPage& p : w.pages)
            for (const RadialSlot& s : p.slots)
            {
                CHECK(IsRadialKey(s.key));
                CHECK(!seen[s.key - kRadialKeyBase]);
                seen[s.key - kRadialKeyBase] = true;
            }
    CHECK(std::count(seen.begin(), seen.end(), true) == kRadialKeyCount);
    // ...and each is bound to an action by default, so the wheels have labels.
    BindingMap defaults = BindingMap::Default();
    for (int i = 0; i < kRadialKeyCount; ++i)
        CHECK(defaults.ActionForKey(kRadialKeyBase + i) != nullptr);
}

void TestCursor()
{
    Section("cursor");
    CursorConfig cfg;
    PadSnapshot::Finger up;

    VirtualCursor c;
    CursorFrame f = c.Update(0.1f, {1.0f, 0.0f}, false, 0, up, false, cfg, nullptr);
    CHECK(Near(f.pos.x, 582.0f, 1.0f) && Near(f.delta.x, 70.0f, 1.0f) && f.pos.y == 384.0f);
    CursorFrame slow = VirtualCursor().Update(0.1f, {1.0f, 0.0f}, true, 0, up, false, cfg, nullptr);
    CHECK(slow.delta.x < f.delta.x * 0.4f);
    f = c.Update(10.0f, {1.0f, 1.0f}, false, 0, up, false, cfg, nullptr);
    CHECK(f.pos.x == cfg.ui_width && f.pos.y == cfg.ui_height);

    // D-pad: one nudge on press, repeat while held.
    VirtualCursor d;
    const unsigned right = 1u << unsigned(NavDir::Right);
    f = d.Update(0.016f, {}, false, right, up, false, cfg, nullptr);
    CHECK(Near(f.delta.x, cfg.dpad_step, 0.01f));
    f = d.Update(0.1f, {}, false, right, up, false, cfg, nullptr);
    CHECK(f.delta.x == 0.0f);
    float moved = 0;
    for (int i = 0; i < 12; ++i)
        moved += d.Update(0.1f, {}, false, right, up, false, cfg, nullptr).delta.x;
    CHECK(moved >= 2 * cfg.dpad_step);
    d.Update(0.016f, {}, false, 0, up, false, cfg, nullptr);

    // Widget navigation jumps.
    FixedNav nav;
    VirtualCursor n;
    f = n.Update(0.016f, {}, false, right, up, false, cfg, &nav);
    CHECK(f.jumped && f.pos.x == 100.0f && f.pos.y == 200.0f);

    // Touchpad as trackpad (relative).
    VirtualCursor t;
    PadSnapshot::Finger fin;
    fin.down = true;
    fin.pos = {0.5f, 0.5f};
    t.Update(0.016f, {}, false, 0, fin, true, cfg, nullptr);
    fin.pos = {0.6f, 0.5f};
    f = t.Update(0.016f, {}, false, 0, fin, true, cfg, nullptr);
    CHECK(f.delta.x > 100.0f && Near(f.delta.y, 0.0f, 0.01f));
    cfg.trackpad_enabled = 0.0f;
    fin.pos = {0.7f, 0.5f};
    f = t.Update(0.016f, {}, false, 0, fin, true, cfg, nullptr);
    CHECK(f.delta.x == 0.0f);

    // Tap to click.
    cfg = CursorConfig();
    cfg.tap_to_click = 1.0f;
    VirtualCursor tap;
    fin.pos = {0.5f, 0.5f};
    tap.Update(0.016f, {}, false, 0, fin, true, cfg, nullptr);
    tap.Update(0.05f, {}, false, 0, fin, true, cfg, nullptr);
    f = tap.Update(0.016f, {}, false, 0, up, true, cfg, nullptr);
    CHECK(f.tapped);

    WheelScroller scroll;
    CHECK(scroll.Update(0.1f, 0.1f, cfg) == 0);
    int total = 0;
    for (int i = 0; i < 10; ++i)
        total += scroll.Update(0.1f, -1.0f, cfg);
    CHECK(total >= 8 && total <= 10); // stick up scrolls up (positive)
}

void TestFeedback()
{
    Section("feedback");
    CHECK(HealthColor(1.0f).g > HealthColor(1.0f).r);
    CHECK(HealthColor(0.0f).r > HealthColor(0.0f).g && HealthColor(0.0f).r > HealthColor(0.0f).b);
    CHECK(HealthColor(0.5f).r > 200 && HealthColor(0.5f).g > 150);

    CHECK(ClassifyWeapon("wpn_ak74") == WeaponClass::AssaultRifle);
    CHECK(ClassifyWeapon("wpn_ak74u") == WeaponClass::Smg);
    CHECK(ClassifyWeapon("wpn_pm") == WeaponClass::Pistol);
    CHECK(ClassifyWeapon("wpn_pm_actor") == WeaponClass::Pistol);
    CHECK(ClassifyWeapon("wpn_pmm") == WeaponClass::AssaultRifle); // short names are exact, not prefixes
    CHECK(ClassifyWeapon("wpn_toz34") == WeaponClass::Shotgun);
    CHECK(ClassifyWeapon("wpn_svd") == WeaponClass::Sniper);
    CHECK(ClassifyWeapon("wpn_knife") == WeaponClass::Melee);
    CHECK(ClassifyWeapon("wpn_rpg7") == WeaponClass::Launcher);
    CHECK(ClassifyWeapon("wpn_grenade_f1") == WeaponClass::Throwable);
    CHECK(ClassifyWeapon("wpn_pkm") == WeaponClass::Lmg);
    CHECK(ClassifyWeapon("wpn_binoc") == WeaponClass::None);
    CHECK(ClassifyWeapon("") == WeaponClass::None);

    using T = TriggerEffect::Type;
    WeaponState s;
    s.weapon = WeaponClass::AssaultRifle;
    s.automatic = true;
    TriggerPair p = SelectTriggerEffects(s);
    CHECK(p.right.type == T::Weapon && p.left.type == T::Feedback);
    s.firing = true;
    p = SelectTriggerEffects(s);
    CHECK(p.right.type == T::Vibration);
    s.weapon = WeaponClass::Shotgun;
    p = SelectTriggerEffects(s);
    CHECK(p.right.type == T::Weapon && p.right.strength >= 0.99f);
    s.empty = true;
    CHECK(SelectTriggerEffects(s).right.type == T::Off);
    s.empty = false;
    s.jammed = true;
    p = SelectTriggerEffects(s);
    CHECK(p.right.type == T::Feedback && p.right.strength >= 0.99f);
    s.jammed = false;
    s.aiming = true;
    CHECK(SelectTriggerEffects(s).left.strength > TriggerEffect::MakeFeedback(0, 0.18f).strength);
    CHECK(SelectTriggerEffects(WeaponState()).left.type == T::Off && SelectTriggerEffects(WeaponState()).right.type == T::Off);
    // the log line shows what DualSenseTriggers.mm passes to GameController: clamped, and a weapon effect ends past its start
    CHECK(DescribeTriggerEffect(TriggerEffect::MakeOff()) == "off");
    CHECK(DescribeTriggerEffect(TriggerEffect::MakeWeapon(0.4f, 0.62f, 0.55f)) == "weapon start 0.40 end 0.62 strength 0.55");
    CHECK(DescribeTriggerEffect(TriggerEffect::MakeWeapon(1.5f, 0.1f, 2.0f)) == "weapon start 0.94 end 0.99 strength 1.00");
    CHECK(DescribeTriggerEffect(TriggerEffect::MakeVibration(0.4f, 0.55f, 0.55f)) == "vibration start 0.40 amplitude 0.55 frequency 0.55");
    for (WeaponClass c : {WeaponClass::Pistol, WeaponClass::Smg, WeaponClass::AssaultRifle, WeaponClass::Lmg,
             WeaponClass::Shotgun, WeaponClass::Sniper, WeaponClass::Launcher})
    {
        const TriggerEffect e = RestingFireEffect(c);
        CHECK(e.type == T::Weapon && e.start < e.end && e.end <= 1.0f && e.strength > 0.0f);
    }

    RumbleMixer rumble;
    rumble.PlayHit(HitType::Bullet, 1.0f);
    MotorState m = rumble.Update(0.01f);
    CHECK(m.low > 0.1f && m.high > 0.1f);
    for (int i = 0; i < 40; ++i)
        m = rumble.Update(0.016f);
    CHECK(m.low == 0.0f && m.high == 0.0f && !rumble.Active());
    rumble.SetBlowout(1.0f);
    float max_low = 0;
    for (int i = 0; i < 120; ++i)
        max_low = std::max(max_low, rumble.Update(0.016f).low);
    CHECK(max_low > 0.4f && rumble.Active());
    rumble.SetBlowout(0.0f);
    CHECK(rumble.Update(0.016f).low == 0.0f);
    // A stronger hit is stronger.
    RumbleMixer weak, strong;
    weak.PlayHit(HitType::Explosion, 0.1f);
    strong.PlayHit(HitType::Explosion, 1.0f);
    CHECK(strong.Update(0.01f).low > weak.Update(0.01f).low);

    // Feedback end to end, with rumble disabled and enabled.
    FeedbackConfig fc;
    Feedback fb;
    fb.SetWeapon(WeaponClass::Pistol, false);
    fb.OnShot();
    fb.Update(0.01f, fc);
    CHECK(fb.Current().motors.high > 0.0f);
    fc.rumble_scale = 0.0f;
    Feedback quiet;
    quiet.OnHit(HitType::Explosion, 1.0f);
    quiet.Update(0.01f, fc);
    CHECK(quiet.Current().motors.low == 0.0f);
    fc = FeedbackConfig();
    fc.trigger_effects = 0.0f;
    Feedback no_triggers;
    no_triggers.SetWeapon(WeaponClass::Pistol, false);
    no_triggers.Update(0.01f, fc);
    CHECK(no_triggers.Current().triggers.right.type == T::Off);
    fc = FeedbackConfig();
    Feedback in_menu;
    in_menu.SetWeapon(WeaponClass::Pistol, false);
    in_menu.Update(0.01f, fc);
    CHECK(in_menu.Current().triggers.right.type == T::Weapon);
    in_menu.SetMenu(true);
    in_menu.Update(0.01f, fc);
    CHECK(in_menu.Current().triggers.right.type == T::Off && in_menu.Current().triggers.left.type == T::Off);
    Feedback health;
    health.SetHealth(1.0f);
    health.Update(0.016f, fc);
    const Rgb full = health.Current().led;
    health.SetHealth(0.6f);
    health.Update(0.016f, fc);
    CHECK(health.Current().led != full);

    // The driver sends light bar and trigger changes once, and refreshes rumble while it plays.
    RecordingFeedbackSink sink;
    FeedbackDriver driver;
    FeedbackOutput out;
    out.led = {10, 200, 30};
    for (int i = 0; i < 20; ++i)
        driver.Apply(sink, out, 0.016f, fc);
    CHECK(sink.led == 1 && sink.triggers == 1 && sink.rumble == 0);
    out.led = {200, 20, 30};
    out.triggers.right = TriggerEffect::MakeWeapon(0.4f, 0.6f, 0.5f);
    driver.Apply(sink, out, 0.1f, fc);
    CHECK(sink.led == 2 && sink.triggers == 2);
    out.motors.low = 0.8f;
    for (int i = 0; i < 30; ++i)
        driver.Apply(sink, out, 0.016f, fc);
    CHECK(sink.rumble >= 2 && sink.rumble <= 6);
    out.motors = {};
    driver.Apply(sink, out, 0.05f, fc);
    CHECK(sink.last_motors.low == 0.0f);
    const int before = sink.rumble;
    driver.Apply(sink, out, 0.05f, fc);
    CHECK(sink.rumble == before);
}

void TestDevices()
{
    Section("devices");
    auto make = [](uint32_t id, const char* serial) {
        DeviceIdentity d;
        d.instance_id = id;
        d.name = "DualSense Wireless Controller";
        d.guid = "030000004c050000e60c000000000000";
        d.serial = serial;
        d.model = PadModel::DualSense;
        return d;
    };
    DeviceRegistry reg;
    const std::string first = reg.Connect(make(7, "")).stable_key;
    const std::string second = reg.Connect(make(8, "")).stable_key;
    CHECK(first != second && second == first + "#2");
    CHECK(reg.ActiveInstance() == 7);
    CHECK(reg.Active()->family == ControllerFamily::PlayStation);

    reg.NoteActivity(8);
    CHECK(reg.ActiveInstance() == 8);
    reg.NoteActivity(99); // unknown: ignored
    CHECK(reg.ActiveInstance() == 8);

    // Disconnecting the active pad promotes the other; reconnecting gets the same stable key back.
    reg.TakeEvents();
    CHECK(reg.Disconnect(8));
    CHECK(reg.ActiveInstance() == 7);
    // xrGame pauses on a Disconnected event only when AnyConnected() is false by then (the device is erased before the event is
    // delivered); the promotion arrives as ActiveChanged, which is not a disconnect.
    CHECK(reg.AnyConnected());
    const std::vector<DeviceEvent> promoted = reg.TakeEvents();
    CHECK(promoted.size() == 2 && promoted[0].type == DeviceEvent::Type::Disconnected && promoted[0].device.instance_id == 8 &&
          promoted[1].type == DeviceEvent::Type::ActiveChanged && promoted[1].device.instance_id == 7);
    CHECK(reg.Connect(make(9, "")).stable_key == second);
    CHECK(reg.Disconnect(7));
    CHECK(reg.AnyConnected()); // 9 is left
    CHECK(reg.Disconnect(9));
    CHECK(!reg.AnyConnected() && reg.Active() == nullptr && reg.ActiveInstance() == 0);
    CHECK(!reg.Disconnect(9));
    CHECK(reg.Connect(make(11, "")).stable_key == first);

    // A serial makes the key unique per pad on its own.
    DeviceRegistry serial;
    CHECK(serial.Connect(make(1, "AAA")).stable_key != serial.Connect(make(2, "BBB")).stable_key);

    const auto events = reg.TakeEvents();
    CHECK(!events.empty() && reg.TakeEvents().empty());
}

void TestConfig()
{
    Section("config");
    Config c;
    c.look.exponent = 2.3f;
    c.gyro.mode = float(GyroMode::AdsOnly);
    c.cursor.speed = 900.0f;
    Config back;
    std::vector<std::string> errors;
    CHECK(ParseConfig(back, SerializeConfig(c), &errors) == int(Tunables(c).size()));
    CHECK(errors.empty());
    CHECK(Near(back.look.exponent, 2.3f) && back.gyro.Mode() == GyroMode::AdsOnly && Near(back.cursor.speed, 900.0f));

    Config bad;
    ParseConfig(bad, "look.exponent = 99\nnope = 1\nlook.deadzone = abc\ncursor.speed=1\n", &errors);
    CHECK(Near(bad.look.exponent, 3.0f));
    CHECK(Near(bad.cursor.speed, 200.0f));
    CHECK(errors.size() == 2);

    // Per-device profiles.
    Profile profile;
    profile.config.look.invert_y = 1.0f;
    profile.bindings.Bind("jump", PK_Square);
    ProfileStore store;
    store.Save("dualsense:abc", profile);
    store.Save("xbox-one:def", Profile());
    ProfileStore reloaded;
    reloaded.Deserialize(store.Serialize());
    CHECK(reloaded.Has("dualsense:abc") && reloaded.Has("xbox-one:def") && !reloaded.Has("other"));
    Profile loaded;
    CHECK(reloaded.Load("dualsense:abc", loaded));
    CHECK(loaded.config.look.invert_y > 0.5f && loaded.bindings.KeyForAction("jump") == PK_Square);
    CHECK(loaded.bindings.KeyForAction("crouch") == PK_Circle);
    CHECK(!reloaded.Load("missing", loaded));

    // Settings-only text keeps the default layout.
    Profile settings_only;
    ParseProfile(settings_only, "look.exponent = 2\n");
    CHECK(settings_only.bindings.KeyForAction("jump") == PK_Cross);
}

void TestGameplayInput()
{
    Section("gameplay");
    GameInput gi;
    Warm(gi);

    // Face button -> pad key -> action.
    PadSnapshot pad = IdlePad();
    pad.Set(PadButton::South, true);
    FrameOutput f = Step(gi, pad);
    CHECK(HasKey(f, PK_Cross, true) && HasAction(f.actions_pressed, "jump") && Holding(f, PK_Cross));
    CHECK(gi.IsKeyDown(PK_Cross));
    f = Step(gi, pad);
    CHECK(f.keys.empty() && Holding(f, PK_Cross) && HasAction(f.actions_down, "jump"));
    pad.Set(PadButton::South, false);
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_Cross, false) && !gi.IsKeyDown(PK_Cross));

    // Triggers are digital with hysteresis.
    pad = IdlePad();
    pad.r2 = 0.5f;
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_R2, true) && HasAction(f.actions_pressed, "wpn_fire"));
    pad.r2 = 0.30f; // between release (0.25) and press (0.35): still down
    CHECK(Step(gi, pad).keys.empty());
    pad.r2 = 0.1f;
    CHECK(HasKey(Step(gi, pad), PK_R2, false));

    // Left stick: full push runs, a gentle push adds the walk key.
    pad = IdlePad();
    pad.left = {0.0f, -0.95f};
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_LStickUp, true) && HasAction(f.actions_pressed, "forward"));
    CHECK(!f.walking && f.move.y > 0.9f && Near(f.move.x, 0.0f, 0.001f));
    CHECK(!Holding(f, PK_Walk));
    pad.left = {0.0f, -0.55f};
    bool walk_started = false;
    for (int i = 0; i < 12; ++i)
    {
        f = Step(gi, pad);
        if (HasKey(f, PK_Walk, true) && HasAction(f.actions_pressed, "accel"))
            walk_started = true;
        if (i < 3)
            CHECK(!f.walking); // not immediately: the push has to stay gentle for walk_delay
    }
    CHECK(walk_started && f.walking);
    CHECK(Holding(f, PK_LStickUp));
    pad.left = {0.1f, 0.0f};
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_LStickUp, false) && HasKey(f, PK_Walk, false));
    // Ramping up from centre to full deflection in a few frames never blips into walk.
    pad.left = {};
    Step(gi, pad);
    bool blip = false;
    for (float v : {0.25f, 0.45f, 0.6f, 0.75f, 0.9f, 0.95f, 0.95f, 0.95f})
    {
        pad.left = {0.0f, -v};
        blip = blip || HasKey(Step(gi, pad), PK_Walk, true);
    }
    CHECK(!blip);
    // A slightly crooked push forward does not strafe.
    pad.left = {0.12f, -0.9f};
    f = Step(gi, pad);
    CHECK(Holding(f, PK_LStickUp) && !Holding(f, PK_LStickRight));
    // Diagonals work.
    pad.left = {0.8f, -0.8f};
    f = Step(gi, pad);
    CHECK(Holding(f, PK_LStickUp) && Holding(f, PK_LStickRight));
    pad.left = {};
    Step(gi, pad);

    // Look.
    pad = IdlePad();
    pad.right = {1.0f, 0.0f};
    f = Step(gi, pad);
    CHECK(f.look_stick_deg.x > 0.0f && Near(f.look_stick_deg.y, 0.0f, 0.001f) && f.look_deg.x == f.look_stick_deg.x);
    int dx = 0, dy = 0;
    gi.LookToCounts(f.look_deg, 8.0f, dx, dy);
    CHECK(dx >= 0);
    pad.right = {};
    Step(gi, pad);

    // L1 tap fires the key (prev_slot) as a one-frame pulse instead of opening the wheel.
    pad = IdlePad();
    pad.Set(PadButton::LeftShoulder, true);
    f = Step(gi, pad);
    CHECK(f.keys.empty());
    pad.Set(PadButton::LeftShoulder, false);
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_L1, true) && HasAction(f.actions_pressed, "prev_slot"));
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_L1, false));

    // L1 hold opens the wheel: no L1 key, look is captured, release fires the picked slot.
    pad = IdlePad();
    pad.Set(PadButton::LeftShoulder, true);
    bool opened = false;
    for (int i = 0; i < 25; ++i)
    {
        f = Step(gi, pad);
        CHECK(!HasKey(f, PK_L1, true));
        opened = opened || f.radial.open;
    }
    CHECK(opened && f.radial.open && f.radial.wheel == 0);
    pad.right = {0.0f, -1.0f};
    pad.Set(PadButton::DpadRight, true); // flips page, and is not forwarded as the lean key
    f = Step(gi, pad);
    CHECK(!HasKey(f, PK_DpadRight, true) && f.look_stick_deg.Length() == 0.0f && f.radial.page == 1);
    pad.Set(PadButton::DpadRight, false);
    pad.Set(PadButton::DpadLeft, true);
    f = Step(gi, pad);
    CHECK(f.radial.page == 0);
    pad.Set(PadButton::DpadLeft, false);
    Step(gi, pad);
    pad.Set(PadButton::LeftShoulder, false);
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_RADIAL_1, true) && HasAction(f.actions_pressed, "quick_use_1"));
    pad.right = {};
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_RADIAL_1, false) && !f.radial.open);

    // Unbound buttons stay quiet unless the filter is lifted.
    pad = IdlePad();
    pad.Set(PadButton::Misc1, true);
    CHECK(Step(gi, pad).keys.empty());
    gi.ClearKeyFilter();
    pad.Set(PadButton::Misc1, false);
    Step(gi, pad);
    pad.Set(PadButton::Misc1, true);
    CHECK(HasKey(Step(gi, pad), PK_Mic, true));
    pad.Set(PadButton::Misc1, false);
    Step(gi, pad);

    // Touchpad swipe.
    pad = IdlePad();
    pad.has_touchpad = true;
    gi.bindings().Bind("cam_1", PK_TouchSwipeLeft);
    pad.finger[0].down = true;
    pad.finger[0].pos = {0.8f, 0.5f};
    Step(gi, pad);
    pad.finger[0].pos = {0.3f, 0.5f};
    Step(gi, pad);
    pad.finger[0].down = false;
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_TouchSwipeLeft, true));

    // Disconnect releases everything and silences feedback.
    RecordingFeedbackSink sink;
    gi.SetFeedbackSink(&sink);
    pad = IdlePad();
    pad.Set(PadButton::South, true);
    pad.left = {0.0f, -0.95f};
    Step(gi, pad);
    CHECK(gi.IsKeyDown(PK_Cross) && gi.IsKeyDown(PK_LStickUp));
    f = Step(gi, PadSnapshot());
    CHECK(HasKey(f, PK_Cross, false) && HasKey(f, PK_LStickUp, false) && !f.pad_active);
    CHECK(!gi.IsKeyDown(PK_Cross) && sink.silence == 1);
    // On reconnect a button that is still held does nothing until it is released and pressed again.
    f = Step(gi, pad);
    CHECK(!HasKey(f, PK_Cross, true));
    pad.Set(PadButton::South, false);
    Step(gi, pad);
    pad.Set(PadButton::South, true);
    CHECK(HasKey(Step(gi, pad), PK_Cross, true));
}

void TestMenuInput()
{
    Section("menu");
    namespace ek = engine_key;
    GameInput gi;
    Warm(gi);

    // A held button does not leak across the context switch.
    PadSnapshot pad = IdlePad();
    pad.Set(PadButton::South, true);
    FrameOutput f = Step(gi, pad);
    CHECK(HasKey(f, PK_Cross, true));
    gi.SetContext(InputContext::Menu);
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_Cross, false) && !HasKey(f, ek::kMouse1, true) && f.context == InputContext::Menu);
    f = Step(gi, pad);
    CHECK(!HasKey(f, ek::kMouse1, true));
    pad.Set(PadButton::South, false);
    Step(gi, pad);

    // Cross clicks, Circle backs out, shoulders are modifiers, Square right-clicks.
    pad.Set(PadButton::South, true);
    f = Step(gi, pad);
    CHECK(HasKey(f, ek::kMouse1, true) && Holding(f, ek::kMouse1));
    pad.Set(PadButton::South, false);
    CHECK(HasKey(Step(gi, pad), ek::kMouse1, false));
    pad.Set(PadButton::East, true);
    CHECK(HasKey(Step(gi, pad), ek::kEscape, true));
    pad.Set(PadButton::East, false);
    Step(gi, pad);
    pad.Set(PadButton::West, true);
    pad.Set(PadButton::LeftShoulder, true);
    f = Step(gi, pad);
    CHECK(HasKey(f, ek::kMouse2, true) && HasKey(f, ek::kLControl, true));
    pad.Set(PadButton::West, false);
    pad.Set(PadButton::LeftShoulder, false);
    Step(gi, pad);

    // Two buttons on the same mouse button do not release each other early.
    pad.Set(PadButton::South, true);
    Step(gi, pad);
    pad.Set(PadButton::Touchpad, true);
    f = Step(gi, pad);
    CHECK(f.keys.empty());
    pad.Set(PadButton::South, false);
    CHECK(Step(gi, pad).keys.empty());
    pad.Set(PadButton::Touchpad, false);
    CHECK(HasKey(Step(gi, pad), ek::kMouse1, false));

    // Triangle double-clicks: down, up, down, up on four frames.
    pad.Set(PadButton::North, true);
    f = Step(gi, pad);
    CHECK(HasKey(f, ek::kMouse1, true));
    pad.Set(PadButton::North, false);
    f = Step(gi, pad);
    CHECK(HasKey(f, ek::kMouse1, false));
    f = Step(gi, pad);
    CHECK(HasKey(f, ek::kMouse1, true));
    f = Step(gi, pad);
    CHECK(HasKey(f, ek::kMouse1, false));
    CHECK(Step(gi, pad).keys.empty());

    // Stick moves the cursor; precision (L3) slows it; R3 recentres.
    gi.SetCursorPosition({100.0f, 100.0f});
    pad = IdlePad();
    pad.left = {1.0f, 0.0f};
    f = Step(gi, pad, 0.1f);
    CHECK(f.cursor_active && f.cursor.delta.x > 60.0f && f.cursor.pos.x > 160.0f);
    gi.SetCursorPosition({100.0f, 100.0f});
    pad.Set(PadButton::LeftStick, true);
    f = Step(gi, pad, 0.1f);
    CHECK(f.cursor.delta.x > 0.0f && f.cursor.delta.x < 30.0f);
    pad = IdlePad();
    Step(gi, pad);
    pad.Set(PadButton::RightStick, true);
    f = Step(gi, pad);
    CHECK(f.cursor.jumped && Near(f.cursor.pos.x, 512.0f, 1.0f) && Near(f.cursor.pos.y, 384.0f, 1.0f));
    pad = IdlePad();
    Step(gi, pad);

    // The gameplay-only outputs are quiet in menus.
    pad.right = {1.0f, 0.0f};
    f = Step(gi, pad);
    CHECK(f.look_deg.Length() == 0.0f && f.radial.open == false);

    // Right stick scrolls; L2/R2 flip pages; L1 hold does not open the wheel.
    pad = IdlePad();
    pad.right = {0.0f, -1.0f};
    int wheel = 0;
    for (int i = 0; i < 60; ++i)
        wheel += Step(gi, pad).wheel;
    CHECK(wheel > 4);
    pad = IdlePad();
    Step(gi, pad);
    pad.r2 = 1.0f;
    f = Step(gi, pad);
    CHECK(f.ui.size() == 1 && f.ui[0] == UiCommand::PageNext);
    pad.r2 = 0.0f;
    Step(gi, pad);
    pad.l2 = 1.0f;
    f = Step(gi, pad);
    CHECK(f.ui.size() == 1 && f.ui[0] == UiCommand::PagePrev);

    // Options is passed through so the engine's own "quit" binding closes the dialog.
    pad = IdlePad();
    Step(gi, pad);
    pad.Set(PadButton::Start, true);
    CHECK(HasKey(Step(gi, pad), PK_Options, true));
    pad.Set(PadButton::Start, false);
    Step(gi, pad);

    // Key capture (Controls menu waiting for a key): raw pad keys, not clicks. The press that started
    // the capture (still held) is ignored until released.
    pad = IdlePad();
    pad.Set(PadButton::South, true);
    Step(gi, pad);
    gi.SetKeyCapture(true);
    f = Step(gi, pad);
    CHECK(!HasKey(f, PK_Cross, true) && gi.key_capture());
    pad.Set(PadButton::South, false);
    Step(gi, pad);
    pad.Set(PadButton::South, true);
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_Cross, true) && !HasKey(f, ek::kMouse1, true));
    pad.Set(PadButton::South, false);
    Step(gi, pad);
    pad.right = {0.0f, 0.9f};
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_RStickDown, true));
    pad.right = {};
    pad.Set(PadButton::Guide, true);
    f = Step(gi, pad);
    CHECK(HasKey(f, PK_Ps, true)); // even keys that are not bound to anything
    pad.Set(PadButton::Guide, false);
    Step(gi, pad);
    gi.SetKeyCapture(false);
    Step(gi, pad);
    pad.Set(PadButton::South, true);
    CHECK(HasKey(Step(gi, pad), ek::kMouse1, true));
    pad.Set(PadButton::South, false);
    Step(gi, pad);

    // Back to gameplay: nothing menu-ish lingers.
    gi.SetContext(InputContext::Gameplay);
    f = Step(gi, pad);
    CHECK(!f.cursor_active && f.context == InputContext::Gameplay);
}

void TestAdapter()
{
    Section("adapter");
    GameInput gi;
    RecordingEngineSink sink;

    DeviceIdentity id;
    id.instance_id = 3;
    id.name = "Test pad";
    id.model = PadModel::DualSense;
    gi.OnDeviceConnected(id);
    FrameOutput f = Step(gi, IdlePad());
    DispatchFrame(gi, f, sink, 20.0f);

    PadSnapshot pad = IdlePad();
    pad.Set(PadButton::South, true);
    pad.right = {1.0f, 0.0f};
    f = Step(gi, pad, 0.05f);
    DispatchFrame(gi, f, sink, 20.0f);
    // Presses before motion before holds; the connect event goes first.
    CHECK(!sink.log.empty() && sink.log.front() == "connected");
    auto index_of = [&](const std::string& s) {
        for (size_t i = 0; i < sink.log.size(); ++i)
            if (sink.log[i] == s)
                return int(i);
        return -1;
    };
    const int press = index_of("press gp_cross");
    const int hold = index_of("hold gp_cross");
    CHECK(press > 0 && hold > press);
    bool moved = false;
    for (const std::string& s : sink.log)
        moved = moved || s.rfind("move ", 0) == 0;
    CHECK(moved);

    sink.log.clear();
    pad.Set(PadButton::South, false);
    f = Step(gi, pad, 0.05f);
    DispatchFrame(gi, f, sink, 20.0f);
    CHECK(index_of("release gp_cross") == 0);

    sink.log.clear();
    f = Step(gi, PadSnapshot());
    gi.OnDeviceDisconnected(3);
    f = Step(gi, PadSnapshot());
    DispatchFrame(gi, f, sink, 20.0f);
    CHECK(index_of("disconnected") >= 0);

    // Menu: cursor deltas become integer mouse moves, jumps become CursorJump.
    gi.OnDeviceConnected(id);
    gi.SetContext(InputContext::Menu);
    sink.log.clear();
    pad = IdlePad();
    Step(gi, pad);
    pad.left = {1.0f, 0.0f};
    int total_dx = 0;
    for (int i = 0; i < 10; ++i)
    {
        f = Step(gi, pad, 0.1f);
        DispatchFrame(gi, f, sink, 20.0f);
    }
    for (const std::string& s : sink.log)
        if (s.rfind("move ", 0) == 0)
            total_dx += std::atoi(s.c_str() + 5);
    CHECK(total_dx > 400);
    CHECK(index_of("jump 512,384") < 0);
    pad.left = {};
    pad.Set(PadButton::RightStick, true);
    sink.log.clear();
    f = Step(gi, pad);
    DispatchFrame(gi, f, sink, 20.0f);
    CHECK(index_of("jump 512,384") >= 0);

    const auto table = EnginePadKeyTable(ControllerFamily::PlayStation);
    CHECK(table.front().name == "gp_cross" && table.front().code == kPadKeyBase && table.front().label == "Cross");
}
} // namespace

namespace gameinput
{
int RunSelfTests(void (*log)(const char* line, void* user), void* user)
{
    Harness h;
    h.log = log;
    h.user = user;
    g_h = &h;

    TestKeys();
    TestFilters();
    TestGyro();
    TestBindings();
    TestRadial();
    TestCursor();
    TestFeedback();
    TestDevices();
    TestConfig();
    TestGameplayInput();
    TestMenuInput();
    TestAdapter();

    char summary[96];
    std::snprintf(summary, sizeof(summary), "%d checks, %d failed", h.checks, h.failed);
    log(summary, user);
    g_h = nullptr;
    return h.failed;
}
} // namespace gameinput
