// Input lab: shows the controller layer working, live, on macOS and iPadOS.
//
// Everything on screen comes from GameInput's output, not from raw SDL state: the mapped actions,
// movement and look, the virtual menu cursor (with a small drag-and-drop inventory to try it on),
// the radial menus, rumble, light bar and adaptive trigger profiles.
//
// Lab commands: hold Create and press another button (or tap the buttons at the top of the screen).
//     Create + Cross      switch Gameplay / Menu context
//     Create + Square     cycle the equipped weapon class (trigger profile)
//     Create + Circle     cycle weapon state: ready / empty / jammed / reloading
//     Create + Triangle   cycle gyro aim: off / ADS only / always
//     Create + D-pad L/R  health -/+ 10% (light bar)
//     Create + D-pad Up   test hit rumble       Create + D-pad Down  toggle blowout rumble
//     Create + L1 / R1    select previous / next tunable;  Create + L3 / R3  value - / +
//
// Run with --selftest to run the library's checks and a trigger-effect sweep, then exit (used for
// device verification through `devicectl --console`). Run with --shot <file.bmp> [gameplay|radial|menu]
// to render one scene from synthetic input, save it as a BMP and exit (layout checks without a pad).

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GameInput/DualSenseTriggers.h"
#include "GameInput/GameInput.h"
#include "GameInput/Profile.h"
#include "GameInput/SdlPadSource.h"
#include "SelfTest.h"

using namespace gameinput;

namespace
{
constexpr float kLogicalW = 1280.0f;
constexpr float kLogicalH = 960.0f;
constexpr float kText = 1.5f;
constexpr float kLine = 8.0f * kText + 4.0f;

struct Color
{
    Uint8 r, g, b;
};
constexpr Color kWhite = {235, 235, 235};
constexpr Color kDim = {140, 145, 155};
constexpr Color kAccent = {255, 190, 70};
constexpr Color kGood = {110, 220, 130};
constexpr Color kBad = {240, 100, 90};
constexpr Color kPanel = {34, 38, 46};

struct Button
{
    SDL_FRect rect;
    const char* label;
    void (*action)(struct Lab&);
};

struct Slot
{
    SDL_FRect ui; // in UI space (1024x768)
    int item = -1;
};

struct Lab
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    GameInput input;
    SdlPadSource* source = nullptr;
    ProfileStore store;
    std::string store_path;
    std::string loaded_key;

    Uint64 last_ns = 0;
    float fps = 0.0f;
    float fps_timer = 0.0f;
    int fps_frames = 0;

    PadSnapshot raw;
    FrameOutput frame;
    std::vector<std::string> log;
    std::vector<Button> buttons;

    // Simulated game state that drives feedback.
    WeaponClass weapon = WeaponClass::AssaultRifle;
    int weapon_state = 0; // 0 ready, 1 empty, 2 jammed, 3 reloading
    int ammo = 30;
    float reload_timer = 0.0f;
    float shot_timer = 0.0f;
    bool prev_fire = false;
    float health = 1.0f;
    bool blowout = false;

    // Look visualisation.
    Vec2 view;

    // Tunable editor.
    int tunable = 0;
    bool prev_create_chord[16] = {};
    bool chord_used = false;

    // Menu playground.
    std::vector<Slot> slots;
    int dragging = -1;
    int drag_from = -1;
    float last_click = -10.0f;
    int equipped = -1;
    float context_menu_timer = 0.0f;
    Vec2 context_menu_at;
    bool prev_mouse1 = false;
    bool prev_mouse2 = false;
    bool prev_escape = false;
    float time = 0.0f;

    TriggerReadback readback;
    float readback_timer = 0.0f;

    // --shot mode.
    std::string shot_path;
    std::string shot_scene = "gameplay";
    int shot_frame = 0;
};

void Log(Lab& lab, const char* fmt, ...)
{
    char line[200];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    SDL_Log("%s", line);
    lab.log.emplace_back(line);
    if (lab.log.size() > 9)
        lab.log.erase(lab.log.begin());
}

void Text(Lab& lab, float x, float y, Color c, const char* fmt, ...)
{
    char line[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    SDL_SetRenderScale(lab.renderer, kText, kText);
    SDL_SetRenderDrawColor(lab.renderer, c.r, c.g, c.b, 255);
    SDL_RenderDebugText(lab.renderer, x / kText, y / kText, line);
    SDL_SetRenderScale(lab.renderer, 1.0f, 1.0f);
}

void Fill(Lab& lab, SDL_FRect r, Color c, Uint8 a = 255)
{
    SDL_SetRenderDrawColor(lab.renderer, c.r, c.g, c.b, a);
    SDL_RenderFillRect(lab.renderer, &r);
}

void Outline(Lab& lab, SDL_FRect r, Color c)
{
    SDL_SetRenderDrawColor(lab.renderer, c.r, c.g, c.b, 255);
    SDL_RenderRect(lab.renderer, &r);
}

void Circle(Lab& lab, float cx, float cy, float radius, Color c, int segments = 40)
{
    std::vector<SDL_FPoint> pts;
    for (int i = 0; i <= segments; ++i)
    {
        const float a = float(i) / float(segments) * 6.2831853f;
        pts.push_back({cx + std::cos(a) * radius, cy + std::sin(a) * radius});
    }
    SDL_SetRenderDrawColor(lab.renderer, c.r, c.g, c.b, 255);
    SDL_RenderLines(lab.renderer, pts.data(), int(pts.size()));
}

void Line(Lab& lab, float x0, float y0, float x1, float y1, Color c)
{
    SDL_SetRenderDrawColor(lab.renderer, c.r, c.g, c.b, 255);
    SDL_RenderLine(lab.renderer, x0, y0, x1, y1);
}

const char* GyroName(GyroMode m)
{
    return m == GyroMode::Off ? "off" : (m == GyroMode::AdsOnly ? "ADS only" : "always");
}

const char* StateName(int s)
{
    static const char* names[] = {"ready", "empty", "jammed", "reloading"};
    return names[s & 3];
}

bool IsAutomatic(WeaponClass c)
{
    return c == WeaponClass::Smg || c == WeaponClass::AssaultRifle || c == WeaponClass::Lmg;
}

std::string KeyName(KeyCode k)
{
    namespace ek = engine_key;
    if (IsPadKey(k))
        return PadKeyName(k);
    switch (k)
    {
    case ek::kMouse1: return "MOUSE1 (click)";
    case ek::kMouse2: return "MOUSE2 (right click)";
    case ek::kMouse3: return "MOUSE3";
    case ek::kEscape: return "ESC";
    case ek::kLControl: return "CTRL";
    case ek::kLShift: return "SHIFT";
    default: return "dik " + std::to_string(k);
    }
}

// ---- lab commands ---------------------------------------------------------------------------

void ApplyWeapon(Lab& lab)
{
    lab.input.feedback().SetWeapon(lab.weapon, IsAutomatic(lab.weapon));
    lab.input.feedback().SetWeaponStatus(lab.weapon_state == 1, lab.weapon_state == 2, lab.weapon_state == 3);
}

void CmdContext(Lab& lab)
{
    const bool to_menu = lab.input.context() == InputContext::Gameplay;
    lab.input.SetContext(to_menu ? InputContext::Menu : InputContext::Gameplay);
    Log(lab, "context -> %s", to_menu ? "MENU" : "GAMEPLAY");
}

void CmdWeapon(Lab& lab)
{
    static const WeaponClass order[] = {WeaponClass::None, WeaponClass::Pistol, WeaponClass::Smg, WeaponClass::AssaultRifle,
        WeaponClass::Lmg, WeaponClass::Shotgun, WeaponClass::Sniper, WeaponClass::Launcher, WeaponClass::Melee,
        WeaponClass::Throwable};
    size_t i = 0;
    while (i < std::size(order) && order[i] != lab.weapon)
        ++i;
    lab.weapon = order[(i + 1) % std::size(order)];
    lab.weapon_state = 0;
    lab.ammo = 30;
    ApplyWeapon(lab);
    Log(lab, "weapon -> %s", WeaponClassName(lab.weapon));
}

void CmdState(Lab& lab)
{
    lab.weapon_state = (lab.weapon_state + 1) & 3;
    lab.reload_timer = lab.weapon_state == 3 ? 3.0f : 0.0f;
    ApplyWeapon(lab);
    Log(lab, "weapon state -> %s", StateName(lab.weapon_state));
}

void CmdGyro(Lab& lab)
{
    GyroConfig& g = lab.input.config().gyro;
    g.mode = float((int(g.Mode()) + 1) % 3);
    Log(lab, "gyro -> %s", GyroName(g.Mode()));
}

void CmdHealth(Lab& lab, float delta)
{
    lab.health = std::min(1.0f, std::max(0.0f, lab.health + delta));
    lab.input.feedback().SetHealth(lab.health);
    Log(lab, "health -> %.0f%%", lab.health * 100.0f);
}

void CmdHit(Lab& lab)
{
    lab.input.feedback().OnHit(HitType::Bullet, 0.35f);
    Log(lab, "test hit");
}

void CmdBlowout(Lab& lab)
{
    lab.blowout = !lab.blowout;
    lab.input.feedback().SetBlowout(lab.blowout ? 1.0f : 0.0f);
    Log(lab, "blowout %s", lab.blowout ? "ON" : "off");
}

void CmdSelectTunable(Lab& lab, int delta)
{
    const int n = int(Tunables(lab.input.config()).size());
    lab.tunable = (lab.tunable + delta + n) % n;
}

void CmdAdjustTunable(Lab& lab, int sign)
{
    const Tunable t = Tunables(lab.input.config())[size_t(lab.tunable)];
    *t.value = std::min(t.max, std::max(t.min, *t.value + t.step * float(sign)));
    Log(lab, "%s = %g", t.key, double(*t.value));
}

// ---- profile persistence (stable device identity) -------------------------------------------

void SaveProfile(Lab& lab)
{
    const DeviceIdentity* dev = lab.input.devices().Active();
    if (!dev || lab.store_path.empty())
        return;
    Profile p;
    p.config = lab.input.config();
    p.bindings = lab.input.bindings();
    lab.store.Save(dev->stable_key, p);
    if (FILE* f = std::fopen(lab.store_path.c_str(), "wb"))
    {
        const std::string text = lab.store.Serialize();
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

void LoadStore(Lab& lab)
{
    const char* pref = SDL_GetPrefPath("andriishafar", "inputlab");
    if (!pref)
        return;
    lab.store_path = std::string(pref) + "profiles.txt";
    if (FILE* f = std::fopen(lab.store_path.c_str(), "rb"))
    {
        std::string text;
        char buffer[4096];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
            text.append(buffer, n);
        std::fclose(f);
        lab.store.Deserialize(text);
    }
}

void OnActiveDevice(Lab& lab, const DeviceIdentity& dev)
{
    if (dev.stable_key == lab.loaded_key)
        return;
    lab.loaded_key = dev.stable_key;
    Profile p;
    if (lab.store.Load(dev.stable_key, p))
    {
        lab.input.config() = p.config;
        lab.input.bindings() = p.bindings;
        Log(lab, "loaded saved profile for %s", dev.stable_key.c_str());
    }
    else
    {
        Log(lab, "no saved profile for %s (using defaults)", dev.stable_key.c_str());
    }
}

// ---- simulation -----------------------------------------------------------------------------

void SimulateGame(Lab& lab, float dt)
{
    GameInput& gi = lab.input;
    Feedback& fb = gi.feedback();

    for (const std::string& a : lab.frame.actions_pressed)
    {
        if (a == "wpn_reload" && lab.weapon_state == 0 && lab.weapon != WeaponClass::None)
        {
            lab.weapon_state = 3;
            lab.reload_timer = 1.6f;
            Log(lab, "reloading...");
        }
    }
    if (lab.weapon_state == 3)
    {
        lab.reload_timer -= dt;
        if (lab.reload_timer <= 0.0f)
        {
            lab.weapon_state = 0;
            lab.ammo = 30;
            Log(lab, "reload done");
        }
    }
    if (lab.weapon_state == 0 && lab.ammo == 0)
        lab.weapon_state = 1;
    if (lab.weapon_state == 1 && lab.ammo > 0)
        lab.weapon_state = 0;

    const bool gun = lab.weapon != WeaponClass::None && lab.weapon != WeaponClass::Melee && lab.weapon != WeaponClass::Throwable;
    const bool fire_down = gi.IsKeyDown(PK_R2) && gi.context() == InputContext::Gameplay;
    lab.shot_timer -= dt;
    const bool edge = fire_down && !lab.prev_fire;
    if (fire_down && lab.weapon != WeaponClass::None)
    {
        if (lab.weapon_state == 0)
        {
            const bool auto_fire = IsAutomatic(lab.weapon);
            if ((edge || (auto_fire && lab.shot_timer <= 0.0f)) && (gun ? lab.ammo > 0 : true))
            {
                fb.OnShot();
                if (gun)
                    --lab.ammo;
                lab.shot_timer = lab.weapon == WeaponClass::Lmg ? 0.075f : 0.095f;
            }
        }
        else if (edge && lab.weapon_state == 1)
        {
            fb.OnDryFire();
        }
    }
    lab.prev_fire = fire_down;
    ApplyWeapon(lab);
    fb.SetFiring(fire_down && lab.weapon_state == 0 && (lab.ammo > 0 || !gun));
}

// ---- chords ---------------------------------------------------------------------------------

// Reads the raw pad for lab commands and returns the snapshot to feed the layer: while Create is
// held, every other button is masked so a chord does not also act in the game.
PadSnapshot HandleChords(Lab& lab, PadSnapshot raw)
{
    if (!raw.connected)
        return raw;
    const bool create = raw.Down(PadButton::Back);
    bool now[16] = {};
    const PadButton chord_buttons[] = {PadButton::South, PadButton::West, PadButton::East, PadButton::North,
        PadButton::DpadLeft, PadButton::DpadRight, PadButton::DpadUp, PadButton::DpadDown, PadButton::LeftShoulder,
        PadButton::RightShoulder, PadButton::LeftStick, PadButton::RightStick};
    for (size_t i = 0; i < std::size(chord_buttons); ++i)
        now[i] = create && raw.Down(chord_buttons[i]);

    auto pressed = [&](size_t i) { return now[i] && !lab.prev_create_chord[i]; };
    if (pressed(0)) CmdContext(lab);
    if (pressed(1)) CmdWeapon(lab);
    if (pressed(2)) CmdState(lab);
    if (pressed(3)) CmdGyro(lab);
    if (pressed(4)) CmdHealth(lab, -0.1f);
    if (pressed(5)) CmdHealth(lab, +0.1f);
    if (pressed(6)) CmdHit(lab);
    if (pressed(7)) CmdBlowout(lab);
    if (pressed(8)) CmdSelectTunable(lab, -1);
    if (pressed(9)) CmdSelectTunable(lab, +1);
    if (pressed(10)) CmdAdjustTunable(lab, -1);
    if (pressed(11)) CmdAdjustTunable(lab, +1);
    bool any = false;
    for (size_t i = 0; i < std::size(chord_buttons); ++i)
    {
        lab.prev_create_chord[i] = now[i];
        any = any || now[i];
    }
    if (any)
        SaveProfile(lab);

    if (create)
    {
        const bool keep_create = true;
        raw.buttons = 0;
        raw.Set(PadButton::Back, keep_create);
        raw.left = {};
        raw.right = {};
        raw.l2 = raw.r2 = 0.0f;
    }
    return raw;
}

// ---- drawing --------------------------------------------------------------------------------

void DrawStick(Lab& lab, float cx, float cy, Vec2 raw, float deadzone, const char* label, Color c)
{
    const float r = 70.0f;
    Fill(lab, {cx - r - 6, cy - r - 6, 2 * r + 12, 2 * r + 12}, kPanel);
    Circle(lab, cx, cy, r, kDim);
    Circle(lab, cx, cy, r * deadzone, {80, 85, 95});
    Line(lab, cx - r, cy, cx + r, cy, {60, 64, 72});
    Line(lab, cx, cy - r, cx, cy + r, {60, 64, 72});
    Fill(lab, {cx + raw.x * r - 5, cy + raw.y * r - 5, 10, 10}, c);
    Text(lab, cx - r, cy + r + 10, kDim, "%s %+.2f %+.2f", label, double(raw.x), double(raw.y));
}

void DrawBar(Lab& lab, float x, float y, float w, float value, Color c, const char* label)
{
    Fill(lab, {x, y, w, 14}, kPanel);
    Fill(lab, {x, y, w * std::min(1.0f, std::max(0.0f, value)), 14}, c);
    Outline(lab, {x, y, w, 14}, kDim);
    Text(lab, x + w + 8, y - 2, kDim, "%s", label);
}

void DrawRadial(Lab& lab, SDL_FRect area)
{
    const float cx = area.x + area.w * 0.5f, cy = area.y + area.h * 0.5f - 14.0f;
    const RadialView& v = lab.frame.radial;
    if (!v.open && !v.pending)
        return;
    const auto& wheels = lab.input.radial().Wheels();
    if (v.wheel < 0 || v.wheel >= int(wheels.size()))
        return;
    const RadialWheel& wheel = wheels[size_t(v.wheel)];
    const float R = 150.0f;
    Fill(lab, area, {18, 20, 26}, 240);
    if (v.pending)
    {
        Text(lab, cx - 100, cy - 8, kAccent, "hold... (%s)", wheel.name.c_str());
        return;
    }
    const RadialPage& page = wheel.pages[size_t(v.page)];
    const int n = int(page.slots.size());
    Circle(lab, cx, cy, R, kDim, 64);
    Circle(lab, cx, cy, R * lab.input.config().radial.deadzone * 0.5f, {80, 85, 95});
    for (int i = 0; i < n; ++i)
    {
        const float a = float(i) / float(n) * 6.2831853f;
        const float mid = a;
        const float sx = std::sin(mid), sy = -std::cos(mid);
        const bool sel = i == v.selected;
        const std::string* action = lab.input.bindings().ActionForKey(page.slots[size_t(i)].key);
        const std::string label = action ? ActionLabel(*action) : page.slots[size_t(i)].label;
        const float lx = cx + sx * R * 0.74f;
        const float ly = cy + sy * R * 0.74f;
        Fill(lab, {lx - 62, ly - 14, 124, 28}, sel ? kAccent : kPanel);
        const float tw = float(label.size()) * 8.0f * kText;
        Text(lab, lx - std::min(tw, 120.0f) * 0.5f, ly - 6, sel ? Color{20, 20, 20} : kWhite, "%.10s", label.c_str());
    }
    // Stick indicator.
    Line(lab, cx, cy, cx + v.stick.x * R * 0.5f, cy + v.stick.y * R * 0.5f, kAccent);
    Text(lab, area.x + 12, area.y + area.h - 2 * kLine - 8, kAccent, "%s - page %d/%d: %s", wheel.name.c_str(), v.page + 1,
        v.page_count, page.title.c_str());
    Text(lab, area.x + 12, area.y + area.h - kLine - 6, kDim, "D-pad left/right flips page, release to fire");
}

void DrawMenuPlayground(Lab& lab, SDL_FRect area)
{
    Fill(lab, area, {22, 24, 30});
    Outline(lab, area, kDim);
    const float sx = area.w / 1024.0f, sy = area.h / 768.0f;
    auto to_screen = [&](SDL_FRect ui) { return SDL_FRect{area.x + ui.x * sx, area.y + ui.y * sy, ui.w * sx, ui.h * sy}; };

    const Vec2 cur = lab.input.cursor_position();
    for (size_t i = 0; i < lab.slots.size(); ++i)
    {
        const Slot& s = lab.slots[i];
        const bool hover = cur.x >= s.ui.x && cur.x < s.ui.x + s.ui.w && cur.y >= s.ui.y && cur.y < s.ui.y + s.ui.h;
        SDL_FRect r = to_screen(s.ui);
        Fill(lab, r, hover ? Color{52, 58, 70} : Color{34, 38, 46});
        Outline(lab, r, hover ? kAccent : kDim);
        if (s.item >= 0 && int(i) != lab.dragging)
        {
            SDL_FRect item = {r.x + r.w * 0.2f, r.y + r.h * 0.2f, r.w * 0.6f, r.h * 0.6f};
            Fill(lab, item, s.item == lab.equipped ? kGood : Color{90, 130, 200});
            Text(lab, item.x + 8, item.y + 8, {10, 10, 10}, "item %c%s", 'A' + s.item, s.item == lab.equipped ? " *" : "");
        }
    }
    if (lab.dragging >= 0)
    {
        const float x = area.x + cur.x * sx, y = area.y + cur.y * sy;
        Fill(lab, {x - 20, y - 14, 70, 40}, {90, 130, 200}, 200);
        Text(lab, x - 14, y - 4, {10, 10, 10}, "item %c", 'A' + lab.slots[size_t(lab.drag_from)].item);
    }
    if (lab.context_menu_timer > 0.0f)
    {
        const float x = area.x + lab.context_menu_at.x * sx, y = area.y + lab.context_menu_at.y * sy;
        Fill(lab, {x, y, 150, 62}, {50, 54, 64});
        Outline(lab, {x, y, 150, 62}, kAccent);
        Text(lab, x + 6, y + 6, kWhite, "Use");
        Text(lab, x + 6, y + 6 + kLine, kWhite, "Drop");
        Text(lab, x + 6, y + 6 + 2 * kLine, kWhite, "Info");
    }
    if (lab.frame.cursor_active)
    {
        const float x = area.x + cur.x * sx, y = area.y + cur.y * sy;
        const bool down = lab.input.IsKeyDown(engine_key::kMouse1);
        SDL_FPoint arrow[4] = {{x, y}, {x + 14, y + 22}, {x + 5, y + 20}, {x, y}};
        SDL_SetRenderDrawColor(lab.renderer, down ? kBad.r : 255, down ? kBad.g : 255, down ? kBad.b : 255, 255);
        SDL_RenderLines(lab.renderer, arrow, 4);
        Line(lab, x - 8, y, x + 8, y, kAccent);
        Line(lab, x, y - 8, x, y + 8, kAccent);
    }
    Text(lab, area.x + 6, area.y + area.h + 6, kDim, "cursor (UI units) %.0f, %.0f   CTRL:%s SHIFT:%s", double(cur.x), double(cur.y),
        lab.input.IsKeyDown(engine_key::kLControl) ? "on" : "-", lab.input.IsKeyDown(engine_key::kLShift) ? "on" : "-");
}

void DrawLookPlayground(Lab& lab, SDL_FRect area)
{
    Fill(lab, area, {22, 24, 30});
    Outline(lab, area, kDim);
    // A scrolling dot grid: turning right moves the world left, like a real camera.
    const float px_per_deg = 6.0f;
    const float spacing = 60.0f;
    const float ox = std::fmod(-lab.view.x * px_per_deg, spacing);
    const float oy = std::fmod(-lab.view.y * px_per_deg, spacing);
    SDL_SetRenderDrawColor(lab.renderer, 90, 96, 110, 255);
    for (float x = area.x + (ox < 0 ? ox + spacing : ox); x < area.x + area.w; x += spacing)
        for (float y = area.y + (oy < 0 ? oy + spacing : oy); y < area.y + area.h; y += spacing)
            SDL_RenderPoint(lab.renderer, x, y), SDL_RenderPoint(lab.renderer, x + 1, y), SDL_RenderPoint(lab.renderer, x, y + 1),
                SDL_RenderPoint(lab.renderer, x + 1, y + 1);
    const float cx = area.x + area.w * 0.5f, cy = area.y + area.h * 0.5f;
    const bool aim = lab.input.aiming();
    Circle(lab, cx, cy, aim ? 10.0f : 22.0f, aim ? kGood : kWhite, 24);
    Line(lab, cx - 6, cy, cx + 6, cy, kWhite);
    Line(lab, cx, cy - 6, cx, cy + 6, kWhite);
    Text(lab, area.x + 6, area.y + area.h + 6, kDim, "view yaw %+.1f  pitch %+.1f deg", double(lab.view.x), double(lab.view.y));
}

void SaveShot(Lab& lab);

void Draw(Lab& lab)
{
    SDL_SetRenderDrawColor(lab.renderer, 16, 18, 22, 255);
    SDL_RenderClear(lab.renderer);

    const GameInput& gi = lab.input;
    const FrameOutput& f = lab.frame;
    const DeviceIdentity* dev = gi.devices().Active();
    const bool menu = gi.context() == InputContext::Menu;

    // Header and touch buttons.
    Text(lab, 16, 10, kAccent, "ANOMALY INPUT LAB  (%s)  %.0f fps", SDL_GetPlatform(), double(lab.fps));
    float bx = 16;
    for (Button& b : lab.buttons)
    {
        b.rect = {bx, 34, float(std::strlen(b.label)) * 8.0f * kText + 16, 26};
        Fill(lab, b.rect, kPanel);
        Outline(lab, b.rect, kAccent);
        Text(lab, b.rect.x + 8, b.rect.y + 7, kWhite, "%s", b.label);
        bx += b.rect.w + 10;
    }

    float y = 76;
    // The left column is 52 characters wide; longer lines would run into the right column.
    auto line = [&](Color c, const char* fmt, auto... args) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), fmt, args...);
        Text(lab, 16, y, c, "%.52s", buffer);
        y += kLine;
    };

    if (dev)
    {
        line(kGood, "%s", dev->name.c_str());
        line(kDim, "model %s  family %d  key %.34s", PadModelName(dev->model), int(dev->family), dev->stable_key.c_str());
        line(kDim, "gyro %d accel %d touch %d rumble %d trigRumble %d led %d adaptive %d", dev->caps.gyro, dev->caps.accel,
            dev->caps.touchpad, dev->caps.rumble, dev->caps.trigger_rumble, dev->caps.rgb_led, dev->caps.adaptive_triggers);
        line(kDim, "battery %s%.0f%%%s", lab.raw.battery < 0 ? "?" : "", lab.raw.battery < 0 ? 0.0 : double(lab.raw.battery) * 100.0,
            lab.raw.charging ? " charging" : "");
    }
    else
    {
        line(kBad, "No controller. Connect the DualSense (PS + Create) over Bluetooth.");
        y += 3 * kLine;
    }
    y += 6;

    line(menu ? kAccent : kGood, "context: %s   aiming: %s", menu ? "MENU (cursor)" : "GAMEPLAY", gi.aiming() ? "yes" : "no");
    line(kWhite, "weapon: %s (%s)  state: %s  ammo %d", WeaponClassName(lab.weapon), IsAutomatic(lab.weapon) ? "auto" : "single",
        StateName(lab.weapon_state), lab.ammo);
    line(kWhite, "health %.0f%%  gyro: %s  blowout: %s", double(lab.health) * 100.0, GyroName(gi.config().gyro.Mode()),
        lab.blowout ? "ON" : "off");
    y += 6;

    std::string held;
    for (KeyCode k : f.held)
    {
        held += (IsPadKey(k) ? KeyName(k).substr(3) : KeyName(k)) + " ";
    }
    line(kDim, "keys down:");
    line(kWhite, "%.52s", held.empty() ? "-" : held.c_str());
    std::string actions;
    for (const std::string& a : f.actions_down)
        actions += a + " ";
    line(kDim, "actions down:");
    line(kAccent, "%.52s", actions.empty() ? "-" : actions.c_str());
    line(kAccent, "%.52s", actions.size() > 52 ? actions.c_str() + 52 : "");
    y += 6;

    line(kDim, "move x %+.2f y %+.2f |%.2f| %s", double(f.move.x), double(f.move.y), double(f.move_magnitude),
        f.walking ? "WALK" : (f.move_magnitude > 0.05f ? "RUN" : "idle"));
    line(kDim, "look stick %+.2f %+.2f  gyro %+.2f %+.2f deg/frame", double(f.look_stick_deg.x), double(f.look_stick_deg.y),
        double(f.look_gyro_deg.x), double(f.look_gyro_deg.y));
    line(kDim, "gyro %s bias %+.3f %+.3f %+.3f", lab.input.gyro().Calibrated() ? "calibrated" : "calibrating",
        double(gi.gyro().Bias()[0]), double(gi.gyro().Bias()[1]), double(gi.gyro().Bias()[2]));
    line(kDim, "look ramp %.2f", double(gi.look_filter().Ramp()));
    y += 6;

    const FeedbackOutput& fo = gi.feedback().Current();
    line(kDim, "R2 trigger : %s s=%.2f e=%.2f str=%.2f", TriggerEffectName(fo.triggers.right), double(fo.triggers.right.start),
        double(fo.triggers.right.end), double(fo.triggers.right.strength));
    line(kDim, "L2 trigger : %s s=%.2f e=%.2f str=%.2f", TriggerEffectName(fo.triggers.left), double(fo.triggers.left.start),
        double(fo.triggers.left.end), double(fo.triggers.left.strength));
    if (lab.readback.valid)
        line(kDim, "GC status  : L mode %d st %d arm %.2f | R mode %d st %d arm %.2f", lab.readback.left_mode, lab.readback.left_status,
            double(lab.readback.left_arm), lab.readback.right_mode, lab.readback.right_status, double(lab.readback.right_arm));
    else
        line(kDim, "GC status  : (no DualSense via GameController)");
    line(kDim, "rumble low %.2f high %.2f   led %d,%d,%d", double(fo.motors.low), double(fo.motors.high), fo.led.r, fo.led.g, fo.led.b);
    Fill(lab, {16, y, 40, 16}, {Uint8(fo.led.r), Uint8(fo.led.g), Uint8(fo.led.b)});
    Outline(lab, {16, y, 40, 16}, kDim);
    y += kLine + 6;

    {
        const Tunable t = Tunables(lab.input.config())[size_t(lab.tunable)];
        line(kAccent, "tune: %s = %g", t.key, double(*t.value));
        line(kDim, "Create+L1/R1 select, Create+L3/R3 change");
    }
    y += 6;
    line(kDim, "log:");
    for (const std::string& l : lab.log)
        line(kWhite, "%.52s", l.c_str());

    // Right column: sticks, triggers, touchpad.
    const float rx = 680;
    DrawStick(lab, rx + 80, 150, lab.raw.left, gi.config().move.deadzone, "L", kGood);
    DrawStick(lab, rx + 260, 150, lab.raw.right, gi.config().look.deadzone, "R", kAccent);
    DrawBar(lab, rx + 370, 90, 140, lab.raw.l2, kGood, "L2");
    DrawBar(lab, rx + 370, 114, 140, lab.raw.r2, kBad, "R2");
    {
        std::string btn = "buttons:";
        static const char* names[] = {"X", "O", "[]", "^", "L1", "R1", "L3", "R3", "up", "down", "left", "right", "create", "options",
            "ps", "pad", "mic", "pL1", "pL2", "pR1", "pR2"};
        for (int i = 0; i < int(PadButton::Count); ++i)
            if (lab.raw.Down(PadButton(i)))
                btn += std::string(" ") + names[i];
        Text(lab, rx + 370, 146, kDim, "%.30s", btn.c_str());
    }

    const SDL_FRect pad_rect = {rx, 262, 300, 110};
    Fill(lab, pad_rect, kPanel);
    Outline(lab, pad_rect, kDim);
    Text(lab, rx + 310, 262, kDim, "touchpad");
    for (int i = 0; i < 2; ++i)
    {
        if (!lab.raw.finger[i].down)
            continue;
        Fill(lab, {pad_rect.x + lab.raw.finger[i].pos.x * pad_rect.w - 6, pad_rect.y + lab.raw.finger[i].pos.y * pad_rect.h - 6, 12, 12},
            i == 0 ? kAccent : kGood);
    }
    Text(lab, rx + 310, 262 + kLine, kDim, "%s", lab.raw.has_touchpad ? "available" : "none");

    // Playground.
    const SDL_FRect area = {rx, 410, 584, 438};
    if (menu)
        DrawMenuPlayground(lab, area);
    else
        DrawLookPlayground(lab, area);
    DrawRadial(lab, area);

    if (!lab.raw.connected)
        Text(lab, rx, 880, kBad, "controller disconnected - input released");

    if (!lab.shot_path.empty() && lab.shot_frame == 90)
        SaveShot(lab);
    SDL_RenderPresent(lab.renderer);
}

// ---- menu playground logic --------------------------------------------------------------------

int SlotAt(const Lab& lab, Vec2 p)
{
    for (size_t i = 0; i < lab.slots.size(); ++i)
    {
        const SDL_FRect& r = lab.slots[i].ui;
        if (p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h)
            return int(i);
    }
    return -1;
}

void UpdateMenuPlayground(Lab& lab, float dt)
{
    lab.context_menu_timer = std::max(0.0f, lab.context_menu_timer - dt);
    const GameInput& gi = lab.input;
    const bool m1 = gi.IsKeyDown(engine_key::kMouse1);
    const bool m2 = gi.IsKeyDown(engine_key::kMouse2);
    const bool esc = gi.IsKeyDown(engine_key::kEscape);
    const Vec2 cur = gi.cursor_position();
    const bool menu = gi.context() == InputContext::Menu;

    if (menu)
    {
        if (m1 && !lab.prev_mouse1)
        {
            const int s = SlotAt(lab, cur);
            if (lab.time - lab.last_click < 0.45f && s >= 0 && lab.slots[size_t(s)].item >= 0)
            {
                lab.equipped = lab.equipped == lab.slots[size_t(s)].item ? -1 : lab.slots[size_t(s)].item;
                Log(lab, "double click -> item %c %s", 'A' + lab.slots[size_t(s)].item, lab.equipped >= 0 ? "equipped" : "unequipped");
            }
            else if (s >= 0 && lab.slots[size_t(s)].item >= 0)
            {
                lab.dragging = s;
                lab.drag_from = s;
            }
            lab.last_click = lab.time;
        }
        if (!m1 && lab.prev_mouse1 && lab.dragging >= 0)
        {
            const int to = SlotAt(lab, cur);
            if (to >= 0 && to != lab.drag_from)
            {
                std::swap(lab.slots[size_t(to)].item, lab.slots[size_t(lab.drag_from)].item);
                Log(lab, "dragged item to slot %d", to);
            }
            lab.dragging = -1;
        }
        if (m2 && !lab.prev_mouse2)
        {
            lab.context_menu_timer = 2.5f;
            lab.context_menu_at = cur;
            Log(lab, "right click (context menu)");
        }
        if (esc && !lab.prev_escape)
        {
            Log(lab, "ESC from Circle -> back to gameplay");
            lab.input.SetContext(InputContext::Gameplay);
        }
    }
    else
    {
        lab.dragging = -1;
    }
    lab.prev_mouse1 = m1;
    lab.prev_mouse2 = m2;
    lab.prev_escape = esc;
}

// ---- logging of layer output ----------------------------------------------------------------

void LogFrame(Lab& lab)
{
    for (const KeyEvent& e : lab.frame.keys)
    {
        const std::string name = KeyName(e.key);
        const std::string* action = IsPadKey(e.key) ? lab.input.bindings().ActionForKey(e.key) : nullptr;
        if (e.down && action)
            Log(lab, "%s down -> %s", name.c_str(), action->c_str());
        else
            Log(lab, "%s %s", name.c_str(), e.down ? "down" : "up");
    }
    for (const RadialEvent& e : lab.frame.radial_events)
    {
        static const char* types[] = {"tap", "opened", "confirmed", "cancelled", "page"};
        Log(lab, "radial %s wheel %d page %d", types[int(e.type)], e.wheel, e.page);
    }
    for (const UiCommand c : lab.frame.ui)
        Log(lab, "ui command %s", c == UiCommand::PagePrev ? "page prev" : "page next");
    for (const DeviceEvent& e : lab.frame.device_events)
    {
        static const char* types[] = {"connected", "disconnected", "active changed"};
        Log(lab, "device %s: %s", types[int(e.type)], e.device.name.c_str());
    }
}

// ---- --shot mode ----------------------------------------------------------------------------

PadSnapshot SyntheticPad(Lab& lab)
{
    PadSnapshot p;
    p.connected = true;
    p.has_touchpad = true;
    p.has_gyro = p.has_accel = true;
    p.accel[1] = 9.81f;
    p.battery = 0.8f;
    p.finger[0] = {true, {0.62f, 0.4f}, 1.0f};
    const int f = lab.shot_frame;
    if (lab.shot_scene == "radial")
    {
        p.Set(PadButton::LeftShoulder, f >= 3);
        p.right = f > 40 ? Vec2{0.7f, -0.7f} : Vec2{};
        p.left = {0.0f, -0.3f};
    }
    else if (lab.shot_scene == "menu")
    {
        lab.input.SetContext(InputContext::Menu);
        p.left = f < 40 ? Vec2{0.6f, 0.4f} : Vec2{};
        p.Set(PadButton::South, f >= 40 && f < 60);
        p.Set(PadButton::West, f == 70);
        p.finger[0].down = false;
    }
    else
    {
        p.left = {0.3f, -0.8f};
        p.right = {0.5f, 0.3f};
        p.l2 = 0.9f;
        p.r2 = 0.7f;
        p.Set(PadButton::South, f >= 3);
        p.Set(PadButton::DpadLeft, f >= 3);
        p.gyro[1] = -0.3f;
    }
    return p;
}

void SaveShot(Lab& lab)
{
    SDL_Surface* surface = SDL_RenderReadPixels(lab.renderer, nullptr);
    if (!surface)
    {
        SDL_Log("shot: SDL_RenderReadPixels failed: %s", SDL_GetError());
        return;
    }
    SDL_Log("shot: saving %dx%d to %s: %s", surface->w, surface->h, lab.shot_path.c_str(),
        SDL_SaveBMP(surface, lab.shot_path.c_str()) ? "ok" : SDL_GetError());
    SDL_DestroySurface(surface);
}

// ---- self test ------------------------------------------------------------------------------

int RunDeviceSelfTest()
{
    int failures = gameinput::RunSelfTests([](const char* line, void*) { SDL_Log("selftest: %s", line); }, nullptr);

    if (!SDL_Init(SDL_INIT_GAMEPAD))
    {
        SDL_Log("selftest: SDL_Init failed: %s", SDL_GetError());
        return failures + 1;
    }
    GameInput gi;
    SdlPadSource source(gi);
    source.Init();
    SDL_PumpEvents();
    SDL_Delay(500);
    SDL_PumpEvents();
    for (const std::string& l : source.DescribeCapabilities())
        SDL_Log("selftest: %s", l.c_str());

    if (const DeviceIdentity* dev = gi.devices().Active(); dev && dev->IsDualSense())
    {
        // Sweep every trigger effect and read back what the framework reports.
        struct Step
        {
            const char* name;
            TriggerPair pair;
        };
        const Step steps[] = {
            {"off", {}},
            {"feedback L / weapon R (pistol)", {TriggerEffect::MakeFeedback(0.3f, 0.6f), RestingFireEffect(WeaponClass::Pistol)}},
            {"weapon R (shotgun)", {{}, RestingFireEffect(WeaponClass::Shotgun)}},
            {"vibration R (auto rifle)", {{}, TriggerEffect::MakeVibration(0.4f, 0.8f, 0.6f)}},
            {"slope L", {TriggerEffect::MakeSlope(0.2f, 0.9f, 0.1f, 0.9f), {}}},
            {"off", {}},
        };
        for (const Step& s : steps)
        {
            const bool ok = DualSenseTriggers::Apply(-1, s.pair);
            SDL_Delay(400);
            const TriggerReadback r = DualSenseTriggers::Read(-1);
            SDL_Log("selftest: trigger '%s' applied=%d L(mode=%d status=%d) R(mode=%d status=%d)", s.name, ok, r.left_mode,
                r.left_status, r.right_mode, r.right_status);
            if (!ok)
                ++failures;
        }
    }
    else
    {
        SDL_Log("selftest: no DualSense visible, trigger sweep skipped");
    }
    SDL_Log("selftest: done, %d failure(s)", failures);
    return failures;
}
} // namespace

// ---- SDL app callbacks ----------------------------------------------------------------------

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    SDL_SetAppMetadata("Anomaly input lab", "0.1", "com.andriishafar.anomaly.inputlab");
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--selftest") == 0)
        {
            const int failures = RunDeviceSelfTest();
            return failures == 0 ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    auto* lab = new Lab;
    *appstate = lab;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--shot") == 0)
        {
            lab->shot_path = argv[i + 1];
            if (i + 2 < argc && argv[i + 2][0] != '-')
                lab->shot_scene = argv[i + 2];
        }
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
#if defined(SDL_PLATFORM_IOS)
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    if (!SDL_CreateWindowAndRenderer("Anomaly input lab", 1024, 768, flags, &lab->window, &lab->renderer))
    {
        SDL_Log("Window/renderer creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(lab->renderer, 1);
    SDL_SetRenderLogicalPresentation(lab->renderer, int(kLogicalW), int(kLogicalH), SDL_LOGICAL_PRESENTATION_LETTERBOX);

    LoadStore(*lab);
    lab->source = new SdlPadSource(lab->input);
    lab->source->Init();
    lab->input.feedback().SetWeapon(lab->weapon, IsAutomatic(lab->weapon));

    lab->buttons = {
        {{}, "Menu/Game", [](Lab& l) { CmdContext(l); }},
        {{}, "Weapon", [](Lab& l) { CmdWeapon(l); }},
        {{}, "State", [](Lab& l) { CmdState(l); }},
        {{}, "Gyro", [](Lab& l) { CmdGyro(l); }},
        {{}, "Hit", [](Lab& l) { CmdHit(l); }},
        {{}, "Blowout", [](Lab& l) { CmdBlowout(l); }},
    };

    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
        {
            Slot s;
            s.ui = {70.0f + float(col) * 320.0f, 70.0f + float(row) * 230.0f, 280.0f, 190.0f};
            if (row == 0)
                s.item = col;
            lab->slots.push_back(s);
        }

    if (!lab->shot_path.empty())
    {
        DeviceIdentity fake;
        fake.instance_id = 1;
        fake.name = "DualSense Wireless Controller (synthetic)";
        fake.guid = "030000004c050000e60c000000000000";
        fake.model = PadModel::DualSense;
        fake.caps = {true, true, true, true, true, true, true};
        lab->input.OnDeviceConnected(fake);
        lab->weapon = WeaponClass::Shotgun;
        ApplyWeapon(*lab);
        lab->input.config().gyro.mode = float(GyroMode::Always);
    }
    for (const std::string& l : lab->source->DescribeCapabilities())
        SDL_Log("%s", l.c_str());
    lab->last_ns = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    auto& lab = *static_cast<Lab*>(appstate);
    switch (event->type)
    {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key == SDLK_ESCAPE)
            return SDL_APP_SUCCESS;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        SDL_ConvertEventToRenderCoordinates(lab.renderer, event);
        const float x = event->button.x, y = event->button.y;
        for (Button& b : lab.buttons)
            if (x >= b.rect.x && x < b.rect.x + b.rect.w && y >= b.rect.y && y < b.rect.y + b.rect.h)
                b.action(lab);
        break;
    }
    default:
        lab.source->HandleEvent(*event);
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto& lab = *static_cast<Lab*>(appstate);

    const Uint64 now = SDL_GetTicksNS();
    const float dt = float(double(now - lab.last_ns) / 1e9);
    lab.last_ns = now;
    lab.time += dt;
    lab.fps_timer += dt;
    ++lab.fps_frames;
    if (lab.fps_timer >= 0.5f)
    {
        lab.fps = float(lab.fps_frames) / lab.fps_timer;
        lab.fps_timer = 0.0f;
        lab.fps_frames = 0;
    }

    lab.raw = lab.shot_path.empty() ? lab.source->Poll() : SyntheticPad(lab);
    const PadSnapshot masked = HandleChords(lab, lab.raw);
    lab.frame = lab.input.Update(dt, masked);
    LogFrame(lab);

    if (const DeviceIdentity* dev = lab.input.devices().Active())
        OnActiveDevice(lab, *dev);

    lab.view = lab.view + lab.frame.look_deg;
    SimulateGame(lab, dt);
    UpdateMenuPlayground(lab, dt);

    lab.readback_timer -= dt;
    if (lab.readback_timer <= 0.0f)
    {
        lab.readback_timer = 0.1f;
        const DeviceIdentity* dev = lab.input.devices().Active();
        lab.readback = dev && dev->IsDualSense() ? DualSenseTriggers::Read(-1) : TriggerReadback();
    }

    Draw(lab);
    if (!lab.shot_path.empty() && ++lab.shot_frame > 90)
        return SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/)
{
    auto* lab = static_cast<Lab*>(appstate);
    if (!lab)
        return;
    SaveProfile(*lab);
    lab->input.feedback().Reset();
    if (lab->source)
    {
        lab->source->Silence();
        delete lab->source;
    }
    delete lab;
}
