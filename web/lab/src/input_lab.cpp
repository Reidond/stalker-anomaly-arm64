#include "input_lab.h"

#include <emscripten.h>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "lab_report.h"
#include "pad_shared.h"

namespace lab::input
{
namespace
{
// Bridge ring: page.js writes, this thread reads. Layout mirrored in page/page.js (BRIDGE_*).
struct BridgeEvent
{
    int32_t type;
    int32_t code;    // key: index into page.js KEY_CODES; lock/fullscreen: 1 on, 0 off
    int32_t buttons; // pointer: event.buttons; key: modifier bits
    int32_t extra;   // pointer: event.button
    float dx, dy;    // movementX/Y, or wheel deltaX/Y
    double epoch_ms; // performance.timeOrigin + event.timeStamp on the page
};
static_assert(sizeof(BridgeEvent) == 32);
constexpr int kRingCapacity = 1024;
struct BridgeRing
{
    int32_t write;   // page: next slot to write (monotonic)
    int32_t read;    // worker: next slot to read (monotonic)
    int32_t capacity;
    int32_t dropped; // page: events lost because the ring was full
    BridgeEvent events[kRingCapacity];
};
static_assert(offsetof(BridgeRing, events) == 16);

enum BridgeType
{
    kKeyDown = 1,
    kKeyUp = 2,
    kPointerDown = 3,
    kPointerUp = 4,
    kPointerMove = 5,
    kWheel = 6,
    kPointerLock = 7,
    kFullscreen = 8,
    kKeyboardLock = 9,
    kBlur = 10,
    kFocus = 11,
    kVisibility = 12,
};

struct Counters
{
    int key_down = 0, key_up = 0, button_down = 0, button_up = 0, motion = 0, wheel = 0;
    double dx = 0, dy = 0;
    std::string last_key;

    std::string ToJson() const
    {
        return Json()
            .Int("key_down", key_down)
            .Int("key_up", key_up)
            .Int("button_down", button_down)
            .Int("button_up", button_up)
            .Int("motion", motion)
            .Int("wheel", wheel)
            .Num("dx", dx)
            .Num("dy", dy)
            .Str("last_key", last_key)
            .Done();
    }
};

SDL_Window* g_window = nullptr;
alignas(8) BridgeRing g_ring;
alignas(8) pad::PadShared g_pad;
alignas(8) pad::PadFeedbackShared g_feedback;
Counters g_sdl, g_bridge;
std::map<std::string, int> g_sdl_types;
std::vector<double> g_sdl_key_seen, g_bridge_key_page, g_bridge_latency, g_sdl_button_seen, g_bridge_button_page;
double g_last_report = 0;
int g_page_lock = 0, g_page_fullscreen = 0, g_keyboard_lock = -1, g_blur = 0, g_hidden = 0;
double g_lock_lost_at = 0;
int g_lock_losses = 0;
std::vector<std::string> g_after_loss;
bool g_loss_pending = false;
uint32_t g_pad_reports = 0;
gameinput::PadSnapshot g_snap;
int g_trigger_preset = 0;

std::string EventName(uint32_t type)
{
    switch (type)
    {
    case SDL_EVENT_KEY_DOWN: return "key_down";
    case SDL_EVENT_KEY_UP: return "key_up";
    case SDL_EVENT_TEXT_INPUT: return "text_input";
    case SDL_EVENT_MOUSE_MOTION: return "mouse_motion";
    case SDL_EVENT_MOUSE_BUTTON_DOWN: return "mouse_button_down";
    case SDL_EVENT_MOUSE_BUTTON_UP: return "mouse_button_up";
    case SDL_EVENT_MOUSE_WHEEL: return "mouse_wheel";
    case SDL_EVENT_WINDOW_MOUSE_ENTER: return "window_mouse_enter";
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return "window_mouse_leave";
    case SDL_EVENT_WINDOW_FOCUS_GAINED: return "window_focus_gained";
    case SDL_EVENT_WINDOW_FOCUS_LOST: return "window_focus_lost";
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: return "window_enter_fullscreen";
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: return "window_leave_fullscreen";
    case SDL_EVENT_WINDOW_RESIZED: return "window_resized";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return "window_pixel_size_changed";
    case SDL_EVENT_WINDOW_SHOWN: return "window_shown";
    case SDL_EVENT_WINDOW_HIDDEN: return "window_hidden";
    case SDL_EVENT_WINDOW_EXPOSED: return "window_exposed";
    case SDL_EVENT_WINDOW_OCCLUDED: return "window_occluded";
    case SDL_EVENT_KEYBOARD_ADDED: return "keyboard_added";
    case SDL_EVENT_MOUSE_ADDED: return "mouse_added";
    default: return Fmt("0x%x", type);
    }
}

void SetRelative(bool on)
{
    const bool ok = SDL_SetWindowRelativeMouseMode(g_window, on);
    Log("SDL_SetWindowRelativeMouseMode(%s) -> %s%s%s", on ? "true" : "false", ok ? "true" : "false", ok ? "" : ": ", ok ? "" : SDL_GetError());
}

void SetFullscreen(bool on)
{
    const bool ok = SDL_SetWindowFullscreen(g_window, on);
    Log("SDL_SetWindowFullscreen(%s) -> %s%s%s", on ? "true" : "false", ok ? "true" : "false", ok ? "" : ": ", ok ? "" : SDL_GetError());
}

std::string PairedLatency(const std::vector<double>& seen, const std::vector<double>& page)
{
    std::vector<double> d;
    for (size_t i = 0; i < seen.size() && i < page.size(); ++i)
        d.push_back(seen[i] - page[i]);
    return StatsJson(Summarize(d));
}

void CycleTriggers()
{
    using E = gameinput::TriggerEffect;
    auto make = [](E::Type t, float start, float end, float strength, float end_strength, float amplitude, float frequency) {
        E e;
        e.type = t;
        e.start = start;
        e.end = end;
        e.strength = strength;
        e.end_strength = end_strength;
        e.amplitude = amplitude;
        e.frequency = frequency;
        return e;
    };
    // The resting effects Feedback.cpp picks (aim on L2, an assault rifle on R2) and the other modes.
    struct Preset
    {
        const char* name;
        gameinput::TriggerPair pair;
        gameinput::MotorState motors;
        gameinput::Rgb led;
        uint8_t player_leds;
    };
    const Preset presets[] = {
        {"off", {}, {}, {0, 0, 0}, 0x00},
        {"feedback (aim 0.30/0.18, R2 0.40/0.30)",
         {make(E::Type::Feedback, 0.30f, 0, 0.18f, 0, 0, 0), make(E::Type::Feedback, 0.40f, 0, 0.30f, 0, 0, 0)},
         {0.2f, 0.0f},
         {40, 200, 40},
         0x04},
        {"weapon (assault rifle 0.40-0.62, 0.55)",
         {make(E::Type::Feedback, 0.30f, 0, 0.18f, 0, 0, 0), make(E::Type::Weapon, 0.40f, 0.62f, 0.55f, 0, 0, 0)},
         {0.0f, 0.4f},
         {220, 180, 20},
         0x0A},
        {"vibration (full-auto 0.40, amp 0.55, freq 0.55)",
         {make(E::Type::Feedback, 0.25f, 0, 0.40f, 0, 0, 0), make(E::Type::Vibration, 0.40f, 0, 0, 0, 0.55f, 0.55f)},
         {0.6f, 0.6f},
         {220, 40, 20},
         0x15},
        {"slope (0.20-0.80, 0.20 -> 1.00)",
         {make(E::Type::Slope, 0.20f, 0.80f, 0.20f, 1.00f, 0, 0), make(E::Type::Slope, 0.20f, 0.80f, 0.20f, 1.00f, 0, 0)},
         {0.0f, 0.0f},
         {40, 80, 220},
         0x1F},
    };
    g_trigger_preset = (g_trigger_preset + 1) % int(sizeof(presets) / sizeof(presets[0]));
    const Preset& p = presets[g_trigger_preset];
    pad::WriteFeedback(g_feedback, pad::kRumble | pad::kLed | pad::kTriggers | pad::kPlayerLeds, p.motors, p.led, p.pair, p.player_leds);
    Report("pad.feedback", "info", Fmt("worker published feedback preset %d: %s", g_trigger_preset, p.name),
           Json().Int("preset", g_trigger_preset).Str("name", p.name).Int("seq", g_feedback.seq).Done());
}

void Drain()
{
    const int32_t write = __atomic_load_n(&g_ring.write, __ATOMIC_ACQUIRE);
    int32_t read = g_ring.read;
    const double now = EpochMs();
    for (; read != write; ++read)
    {
        const BridgeEvent& e = g_ring.events[uint32_t(read) % kRingCapacity];
        switch (e.type)
        {
        case kKeyDown:
            ++g_bridge.key_down;
            g_bridge.last_key = Fmt("#%d", e.code);
            g_bridge_key_page.push_back(e.epoch_ms);
            g_bridge_latency.push_back(now - e.epoch_ms);
            break;
        case kKeyUp: ++g_bridge.key_up; break;
        case kPointerDown:
            ++g_bridge.button_down;
            g_bridge_button_page.push_back(e.epoch_ms);
            g_bridge_latency.push_back(now - e.epoch_ms);
            break;
        case kPointerUp: ++g_bridge.button_up; break;
        case kPointerMove:
            ++g_bridge.motion;
            g_bridge.dx += e.dx;
            g_bridge.dy += e.dy;
            break;
        case kWheel: ++g_bridge.wheel; break;
        case kPointerLock:
            g_page_lock = e.code;
            if (!e.code)
            {
                g_lock_lost_at = now;
                g_after_loss.clear();
                g_loss_pending = true;
                ++g_lock_losses;
            }
            break;
        case kFullscreen: g_page_fullscreen = e.code; break;
        case kKeyboardLock: g_keyboard_lock = e.code; break;
        case kBlur: ++g_blur; break;
        case kVisibility: g_hidden = e.code == 0; break;
        default: break;
        }
    }
    __atomic_store_n(&g_ring.read, read, __ATOMIC_RELEASE);
}
} // namespace

void Init(SDL_Window* window)
{
    g_window = window;
    g_ring.capacity = kRingCapacity;
    g_pad.battery = -1;
    MAIN_THREAD_ASYNC_EM_ASM({ if (globalThis.labAttach) globalThis.labAttach($0, $1, $2, $3); }, &g_ring, kRingCapacity, &g_pad, &g_feedback);
}

void OnSdlEvent(const SDL_Event& e)
{
    const std::string name = EventName(e.type);
    ++g_sdl_types[name];
    if (g_loss_pending)
        g_after_loss.push_back(name);
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat)
            break;
        ++g_sdl.key_down;
        g_sdl.last_key = SDL_GetKeyName(e.key.key);
        g_sdl_key_seen.push_back(EpochMs());
        if (e.key.key == SDLK_L)
            SetRelative(true);
        else if (e.key.key == SDLK_U)
            SetRelative(false);
        else if (e.key.key == SDLK_F)
            SetFullscreen(!(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
        else if (e.key.key == SDLK_T)
            CycleTriggers();
        break;
    case SDL_EVENT_KEY_UP: ++g_sdl.key_up; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        ++g_sdl.button_down;
        g_sdl_button_seen.push_back(EpochMs());
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP: ++g_sdl.button_up; break;
    case SDL_EVENT_MOUSE_MOTION:
        ++g_sdl.motion;
        g_sdl.dx += e.motion.xrel;
        g_sdl.dy += e.motion.yrel;
        break;
    case SDL_EVENT_MOUSE_WHEEL: ++g_sdl.wheel; break;
    default: break;
    }
}

void Command(int cmd, int)
{
    switch (cmd)
    {
    case 1: SetRelative(true); break;
    case 2: SetRelative(false); break;
    case 3: SetFullscreen(true); break;
    case 4: SetFullscreen(false); break;
    case 6:
        g_sdl = {};
        g_bridge = {};
        g_sdl_types.clear();
        g_sdl_key_seen.clear();
        g_bridge_key_page.clear();
        g_bridge_latency.clear();
        g_sdl_button_seen.clear();
        g_bridge_button_page.clear();
        break;
    case 7: CycleTriggers(); break;
    default: break;
    }
}

void Frame()
{
    Drain();
    gameinput::PadSnapshot snap;
    uint32_t reports = 0;
    if (pad::ReadPad(g_pad, snap, &reports))
    {
        g_snap = snap;
        g_pad_reports = reports;
    }

    const double now = EpochMs();
    if (g_loss_pending && now - g_lock_lost_at > 1000)
    {
        g_loss_pending = false;
        std::string seen;
        for (const std::string& s : g_after_loss)
            if (s != "mouse_motion")
                seen += (seen.empty() ? "" : ", ") + s;
        const bool rel = g_window && SDL_GetWindowRelativeMouseMode(g_window);
        Report("input.lock_loss", "info",
               Fmt("browser dropped pointer lock (loss %d): SDL relative mode still %s; SDL events in the next second: %s", g_lock_losses,
                   rel ? "on" : "off", seen.empty() ? "none" : seen.c_str()),
               Json().Bool("sdl_relative_after_loss", rel).Str("sdl_events_after_loss", seen).Int("losses", g_lock_losses).Done());
    }

    if (now - g_last_report < 250)
        return;
    g_last_report = now;
    std::string types = "{";
    for (const auto& [k, v] : g_sdl_types)
        types += Fmt("%s\"%s\":%d", types.size() > 1 ? "," : "", k.c_str(), v);
    types += "}";
    const bool rel = g_window && SDL_GetWindowRelativeMouseMode(g_window);
    const bool fs = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN);
    const std::string pad_json = Json()
                                     .Bool("connected", g_snap.connected)
                                     .Int("reports", g_pad_reports)
                                     .Int("buttons", g_snap.buttons)
                                     .Nums("left", {g_snap.left.x, g_snap.left.y})
                                     .Nums("right", {g_snap.right.x, g_snap.right.y})
                                     .Nums("triggers", {g_snap.l2, g_snap.r2})
                                     .Nums("gyro", {g_snap.gyro[0], g_snap.gyro[1], g_snap.gyro[2]})
                                     .Nums("accel", {g_snap.accel[0], g_snap.accel[1], g_snap.accel[2]})
                                     .Nums("touch0", {g_snap.finger[0].down ? 1.0 : 0.0, g_snap.finger[0].pos.x, g_snap.finger[0].pos.y})
                                     .Nums("touch1", {g_snap.finger[1].down ? 1.0 : 0.0, g_snap.finger[1].pos.x, g_snap.finger[1].pos.y})
                                     .Num("battery", g_snap.battery)
                                     .Done();
    Report("input.live", "info",
           Fmt("SDL keys %d/%d buttons %d motion %d wheel %d | bridge keys %d/%d buttons %d motion %d wheel %d | SDL relative %s",
               g_sdl.key_down, g_sdl.key_up, g_sdl.button_down, g_sdl.motion, g_sdl.wheel, g_bridge.key_down, g_bridge.key_up,
               g_bridge.button_down, g_bridge.motion, g_bridge.wheel, rel ? "on" : "off"),
           Json()
               .Raw("sdl", g_sdl.ToJson())
               .Raw("bridge", g_bridge.ToJson())
               .Raw("sdl_event_types", types)
               .Bool("sdl_relative", rel)
               .Bool("sdl_fullscreen_flag", fs)
               .Int("page_pointer_lock", g_page_lock)
               .Int("page_fullscreen", g_page_fullscreen)
               .Int("keyboard_lock", g_keyboard_lock)
               .Int("bridge_dropped", g_ring.dropped)
               .Raw("bridge_latency_ms", StatsJson(Summarize(g_bridge_latency)))
               .Raw("sdl_key_latency_ms", PairedLatency(g_sdl_key_seen, g_bridge_key_page))
               .Raw("sdl_button_latency_ms", PairedLatency(g_sdl_button_seen, g_bridge_button_page))
               .Raw("pad", pad_json)
               .Done());
}
} // namespace lab::input
