// CInput on SDL3 for Apple platforms (macOS, iPadOS). The Windows build keeps DirectInput in Xr_input.cpp.
//
// What it does, per frame (CInput::OnFrame):
//   keyboard   SDL key events -> DIK_* codes (xr_input_keymap) -> IR_OnKeyboardPress/Release, then a Hold for every
//              key that is down, like the DirectInput loop
//   mouse      relative motion while the pointer is captured (look, and the UI cursor, which integrates deltas),
//              absolute position for IR_GetMousePos*, buttons, wheel (120 per notch, DirectInput's units)
//   text       SDL text input events -> IR_OnTextInput (Windows-1251), with the on-screen keyboard on iPad
//   controller the GameInput layer (src/Platform/GameInput) through gameinput/Adapter.h: pad keys, analog look as
//              extra mouse deltas, the virtual cursor, rumble and adaptive triggers
//
// Event ownership: in the engine, xr_window::PumpEvents() is the single consumer of the SDL queue and passes every event
// to the handler CInput registers (xr_window.h); CInput never polls then. Without it (tools, tests) CInput peeks keyboard,
// mouse, text and gamepad events from the queue itself, or takes what the host gives HandleEvent().
#include "stdafx.h"
#pragma hdrstop

#ifdef XR_PLATFORM_APPLE

#include "xr_input.h"
#include "IInputReceiver.h"
#include "xr_input_keymap.h"

#include <SDL3/SDL.h>

#if !defined(XR_INPUT_STANDALONE) && __has_include("xr_window.h")
# include "xr_window.h" // engine-core's window layer: AddEventHandler/RemoveEventHandler
# define XR_INPUT_USE_XR_WINDOW 1
#endif

#include <algorithm>
#include <cmath>

#include "GameInput/Adapter.h"
#include "GameInput/SdlPadSource.h"

// Defined in Xr_input.cpp
extern bool g_exclusive;
extern BOOL b_altF4;
extern float stop_vibration_time;
extern IInputReceiver dummyController;

namespace
{
constexpr int kWheelDelta = 120; // WHEEL_DELTA: what DirectInput reports for one notch
constexpr float kUiBaseWidth = 1024.f; // the UI is laid out in 1024x768 (xrGame/ui_defs.h)
constexpr float kUiBaseHeight = 768.f;
constexpr int kMouseButtons = CInput::COUNT_MOUSE_BUTTONS;
constexpr int kMaxPeek = 256;

// SDL button (1 left, 2 middle, 3 right, 4 X1, 5 X2) -> engine mouse index (0 left, 1 right, 2 middle, 3+ extra).
int MouseIndex(Uint8 sdl_button)
{
    switch (sdl_button)
    {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
    }
}

bool IsTouchLike(SDL_MouseID id) { return id == SDL_TOUCH_MOUSEID || id == SDL_PEN_MOUSEID; }
} // namespace

// ---------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------
struct XrInputSdl : public gameinput::IEngineInputSink
{
    explicit XrInputSdl(CInput& owner)
        : input(owner), pad_source(pad)
    {
    }

    CInput& input;
    SDL_Window* window = nullptr;
    bool app_active = true;
    bool captured = true;    // wants the pointer captured (SetPointerCaptured)
    bool suspended = false;  // unacquire() without acquire()
    bool relative_on = false; // what SDL was last told
    bool ui_context = false;

    // Pointer in window points, tracked from events so it is the same in relative mode and testable.
    float px = 0.f, py = 0.f;
    bool have_pos = false;
    // Whole counts leave a fraction behind; it is carried to the next frame so slow motion isn't rounded away.
    float carry_x = 0.f, carry_y = 0.f;
    float wheel_carry = 0.f;

    // Events read from SDL (or handed to HandleEvent), kept in arrival order until KeyUpdate/MouseUpdate.
    std::vector<SDL_Event> kb_events;
    std::vector<SDL_Event> mouse_events;

    // Text input
    std::vector<const void*> text_owners;
    bool text_started = false;
    SDL_Rect text_area = {0, 0, 0, 0};
    bool has_text_area = false;

    // Controller layer
    gameinput::GameInput pad;
    gameinput::SdlPadSource pad_source;
    bool pad_ready = false;
    Uint64 pad_last_ns = 0;
    float look_radians_per_count = 0.f;
    bool inited_events = false;
    // The host feeds events (HandleEvent) and CInput leaves the SDL queue alone.
    bool external_events = false;

    static void EventThunk(const SDL_Event& e, void* user) { static_cast<XrInputSdl*>(user)->Accept(e); }

    // ---- window / pointer ----
    SDL_Window* Window() const
    {
        if (window)
            return window;
        if (RDEVICE.m_hWnd)
            return static_cast<SDL_Window*>(RDEVICE.m_hWnd);
        return SDL_GetKeyboardFocus();
    }

    void WindowSize(float& w, float& h) const
    {
        w = h = 0.f;
        if (SDL_Window* win = Window())
        {
            int iw = 0, ih = 0;
            SDL_GetWindowSize(win, &iw, &ih);
            w = float(iw);
            h = float(ih);
        }
    }

    // Client size in pixels, as the engine uses it (render resolution).
    void ClientSize(float& w, float& h) const
    {
        WindowSize(w, h);
        if (RDEVICE.clientWidth && RDEVICE.clientHeight)
        {
            w = float(RDEVICE.clientWidth);
            h = float(RDEVICE.clientHeight);
        }
    }

    void ApplyCapture()
    {
        SDL_Window* win = Window();
        const bool want = captured && app_active && !suspended;
        if (!win)
        {
            relative_on = false;
            return;
        }
        if (relative_on != want)
        {
            if (!SDL_SetWindowRelativeMouseMode(win, want))
                Msg("! INPUT: SDL_SetWindowRelativeMouseMode(%d) failed: %s", int(want), SDL_GetError());
            relative_on = want;
        }
        if (want)
            SDL_HideCursor();
        else
            SDL_ShowCursor();
    }

    void ClampPointer()
    {
        float w, h;
        WindowSize(w, h);
        if (w > 0.f)
            px = std::min(std::max(px, 0.f), std::max(w - 1.f, 0.f));
        if (h > 0.f)
            py = std::min(std::max(py, 0.f), std::max(h - 1.f, 0.f));
    }

    // ---- text input ----
    bool TextWanted() const { return !text_owners.empty(); }

    void SyncTextInput()
    {
        SDL_Window* win = Window();
        if (!win)
            return;
        const bool want = TextWanted();
        if (want && !text_started)
        {
            if (has_text_area)
                SDL_SetTextInputArea(win, &text_area, 0);
            if (!SDL_StartTextInput(win))
                Msg("! INPUT: SDL_StartTextInput failed: %s", SDL_GetError());
            text_started = true;
        }
        else if (want && has_text_area)
        {
            SDL_SetTextInputArea(win, &text_area, 0);
        }
        else if (!want && text_started)
        {
            SDL_StopTextInput(win);
            text_started = false;
        }
    }

    // ---- event intake ----
    static bool IsKeyboardEvent(Uint32 t) { return t >= SDL_EVENT_KEY_DOWN && t <= SDL_EVENT_SCREEN_KEYBOARD_HIDDEN; }
    static bool IsMouseEvent(Uint32 t) { return t >= SDL_EVENT_MOUSE_MOTION && t <= SDL_EVENT_MOUSE_REMOVED; }
    static bool IsPadEvent(Uint32 t) { return t >= SDL_EVENT_JOYSTICK_AXIS_MOTION && t <= SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED; }

    bool Accept(const SDL_Event& e)
    {
        if (IsKeyboardEvent(e.type))
        {
            // A level load or a stall can leave seconds of events behind; keep the newest.
            if (kb_events.size() >= 4096)
                kb_events.erase(kb_events.begin(), kb_events.begin() + 2048);
            kb_events.push_back(e);
            return true;
        }
        if (IsMouseEvent(e.type))
        {
            // Consecutive motion from one device is one motion (a high-rate mouse over a slow frame).
            if (e.type == SDL_EVENT_MOUSE_MOTION && !mouse_events.empty())
            {
                SDL_Event& last = mouse_events.back();
                if (last.type == SDL_EVENT_MOUSE_MOTION && last.motion.which == e.motion.which)
                {
                    last.motion.x = e.motion.x;
                    last.motion.y = e.motion.y;
                    last.motion.xrel += e.motion.xrel;
                    last.motion.yrel += e.motion.yrel;
                    return true;
                }
            }
            if (mouse_events.size() >= 4096)
                mouse_events.erase(mouse_events.begin(), mouse_events.begin() + 2048);
            mouse_events.push_back(e);
            return true;
        }
        if (IsPadEvent(e.type))
        {
            pad_source.HandleEvent(e);
            return true;
        }
        return false;
    }

    void Gather()
    {
        if (external_events)
            return;
        SDL_PumpEvents();
        SDL_Event buf[kMaxPeek];
        int n;
        while ((n = SDL_PeepEvents(buf, kMaxPeek, SDL_GETEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_SCREEN_KEYBOARD_HIDDEN)) > 0)
            kb_events.insert(kb_events.end(), buf, buf + n);
        while ((n = SDL_PeepEvents(buf, kMaxPeek, SDL_GETEVENT, SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_REMOVED)) > 0)
            mouse_events.insert(mouse_events.end(), buf, buf + n);
        while ((n = SDL_PeepEvents(buf, kMaxPeek, SDL_GETEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)) > 0)
            for (int i = 0; i < n; ++i)
                pad_source.HandleEvent(buf[i]);
    }

    // ---- IEngineInputSink: controller layer -> receiver stack ----
    void KeyPress(gameinput::KeyCode key) override
    {
        if (IInputReceiver* r = input.CurrentIR())
            r->IR_OnKeyboardPress(key);
    }
    void KeyRelease(gameinput::KeyCode key) override
    {
        if (IInputReceiver* r = input.CurrentIR())
            r->IR_OnKeyboardRelease(key);
    }
    void KeyHold(gameinput::KeyCode key) override
    {
        if (IInputReceiver* r = input.CurrentIR())
            r->IR_OnKeyboardHold(key);
    }
    void MouseMove(int dx, int dy) override
    {
        if (IInputReceiver* r = input.CurrentIR())
            r->IR_OnMouseMove(dx, dy);
    }
    void MouseWheel(int notches) override
    {
        if (IInputReceiver* r = input.CurrentIR())
            r->IR_OnMouseWheel(notches * kWheelDelta);
    }
    void CursorJump(float ui_x, float ui_y) override
    {
        if (input.pad_hooks.cursor_jump)
            input.pad_hooks.cursor_jump(ui_x, ui_y);
    }
    void UiAction(gameinput::UiCommand command) override
    {
        if (input.pad_hooks.ui_action)
            input.pad_hooks.ui_action(command == gameinput::UiCommand::PagePrev ? 0 : 1);
    }
    void PadConnected(const gameinput::DeviceIdentity&) override
    {
        if (input.pad_hooks.connection)
            input.pad_hooks.connection(true);
    }
    void PadDisconnected(const gameinput::DeviceIdentity&) override
    {
        if (input.pad_hooks.connection)
            input.pad_hooks.connection(false);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// CInput
// ---------------------------------------------------------------------------------------------------------------
static void on_error_dialog(bool before)
{
    if (!pInput || !g_exclusive)
        return;
    if (before)
        pInput->unacquire();
    else
        pInput->acquire(true);
}

CInput::CInput(BOOL bExclusive, int deviceForInit)
{
    g_exclusive = !!bExclusive;

    Log("Starting INPUT device (SDL3)...");

    sdl = new XrInputSdl(*this);

    mouse_property.mouse_dt = 25;

    ZeroMemory(mouseState, sizeof(mouseState));
    ZeroMemory(KBState, sizeof(KBState));
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeStamp));
    ZeroMemory(offs, sizeof(offs));

    if (!SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
        Msg("! INPUT: SDL_InitSubSystem failed: %s", SDL_GetError());
    else
        sdl->inited_events = true;

#ifdef XR_INPUT_USE_XR_WINDOW
    xr_window::AddEventHandler(&XrInputSdl::EventThunk, sdl);
    sdl->external_events = true;
#endif

    sdl->pad_source.Init();
    sdl->pad_ready = true;
    sdl->pad_last_ns = SDL_GetTicksNS();

    // Dummy pack: the receiver of last resort, so cbStack is never empty
    iCapture(&dummyController);

    sdl->ApplyCapture();

    Debug.set_on_dialog(&on_error_dialog);

#ifdef ENGINE_BUILD
    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this, REG_PRIORITY_HIGH);
    Device.seqFrame.Add(this, REG_PRIORITY_HIGH);
#endif
}

CInput::~CInput()
{
#ifdef ENGINE_BUILD
    Device.seqFrame.Remove(this);
    Device.seqAppDeactivate.Remove(this);
    Device.seqAppActivate.Remove(this);
#endif
    if (sdl)
    {
#ifdef XR_INPUT_USE_XR_WINDOW
        xr_window::RemoveEventHandler(&XrInputSdl::EventThunk, sdl);
#endif
        sdl->text_owners.clear();
        sdl->SyncTextInput();
        sdl->captured = false;
        sdl->ApplyCapture();
        sdl->pad.feedback().Reset();
        sdl->pad_source.Silence();
        const bool release_sdl = sdl->inited_events;
        delete sdl;
        sdl = nullptr;
        if (release_sdl)
            SDL_QuitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
    }
}

void CInput::SetAllAcquire(BOOL bAcquire)
{
    sdl->suspended = !bAcquire;
    sdl->ApplyCapture();
}

void CInput::SetMouseAcquire(BOOL bAcquire) { SetAllAcquire(bAcquire); }

void CInput::SetKBDAcquire(BOOL bAcquire)
{
    // The keyboard is always delivered while the window has focus; nothing to acquire.
}

void CInput::SetWindow(SDL_Window* window)
{
    sdl->window = window;
    sdl->have_pos = false;
    sdl->ApplyCapture();
    sdl->SyncTextInput();
}

SDL_Window* CInput::Window() const { return sdl->Window(); }

bool CInput::HandleEvent(const SDL_Event& event) { return sdl->Accept(event); }

void CInput::SetEventSourceExternal(bool external) { sdl->external_events = external; }

void CInput::SetPointerCaptured(bool captured)
{
    sdl->captured = captured;
    sdl->ApplyCapture();
}

bool CInput::IsPointerCaptured() const { return sdl->relative_on; }

void CInput::SetUiContext(bool ui)
{
    sdl->ui_context = ui;
    sdl->pad.SetContext(ui ? gameinput::InputContext::Menu : gameinput::InputContext::Gameplay);
}

void CInput::SetLookRadiansPerCount(float radians) { sdl->look_radians_per_count = radians; }

gameinput::GameInput* CInput::Pad() { return sdl && sdl->pad_ready ? &sdl->pad : nullptr; }
gameinput::SdlPadSource* CInput::PadSource() { return sdl && sdl->pad_ready ? &sdl->pad_source : nullptr; }

// ---- keyboard ---------------------------------------------------------------------------------------------

void CInput::KeyUpdate()
{
    if (b_altF4)
        return;

    sdl->Gather();

    struct Edge
    {
        int dik;     // key, or -1 for a text event
        bool down;
        char text[32]; // Windows-1251, for text events
    };
    std::vector<Edge> edges;
    edges.reserve(sdl->kb_events.size());

    for (const SDL_Event& e : sdl->kb_events)
    {
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            if (e.key.repeat)
                break; // the engine makes its own repeats from Hold
            const int dik = xr_input::ScancodeToDik(e.key.scancode);
            if (!dik)
                break;
            const bool down = e.key.down;
            if (!!KBState[dik] == down)
                break; // two keyboards, or a press we already have
            KBState[dik] = down ? TRUE : FALSE;
            edges.push_back({dik, down, {0}});
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
        {
            Edge text = {-1, true, {0}};
            xr_input::Utf8ToCp1251(e.text.text, text.text, sizeof(text.text));
            if (text.text[0])
                edges.push_back(text);
            break;
        }
        default:
            break;
        }
    }
    sdl->kb_events.clear();

    if (!b_altF4 && KBState[DIK_F4] && (KBState[DIK_RMENU] || KBState[DIK_LMENU]))
    {
        b_altF4 = TRUE;
        Engine.Event.Defer("KERNEL:disconnect");
        Engine.Event.Defer("KERNEL:quit");
    }
    if (b_altF4)
        return;

    if (Device.dwPrecacheFrame == 0)
    {
        for (const Edge& edge : edges)
        {
            IInputReceiver* ir = cbStack.back();
            if (edge.dik < 0)
                ir->IR_OnTextInput(edge.text);
            else if (edge.down)
                ir->IR_OnKeyboardPress(edge.dik);
            else
                ir->IR_OnKeyboardRelease(edge.dik);
        }

        for (u32 i = 0; i < COUNT_KB_BUTTONS; i++)
            if (KBState[i])
                cbStack.back()->IR_OnKeyboardHold(i);
    }
}

void CInput::resetMouseState()
{
    for (int i = 0; i < COUNT_MOUSE_BUTTONS; i++)
        mouseState[i] = 0;
}

bool CInput::get_dik_name(int dik, LPSTR dest_str, int dest_sz)
{
    return xr_input::DikName(dik, dest_str, dest_sz);
}

bool CInput::dik_to_text(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, LPSTR dest, int dest_sz)
{
    if (dest && dest_sz > 0)
        dest[0] = 0;
    // While text input is on, characters arrive as text events (layout, dead keys and the on-screen keyboard
    // included); producing them from the key as well would type everything twice.
    if (sdl->TextWanted())
        return false;
    return xr_input::DikToText(dik, shift, caps, ctrl, alt, altgr, dest, dest_sz);
}

#define MOUSE_1 (0xED + 100)
#define MOUSE_8 (0xED + 107)

BOOL CInput::iGetAsyncKeyState(int dik)
{
    if (dik < COUNT_KB_BUTTONS)
        return !!KBState[dik];
    if (dik >= MOUSE_1 && dik <= MOUSE_8)
        return iGetAsyncBtnState(dik - MOUSE_1);
    if (sdl->pad_ready && gameinput::IsPadKey(dik))
        return sdl->pad.IsKeyDown(dik);
    return FALSE; // unknown key ???
}

BOOL CInput::iGetAsyncBtnState(int btn)
{
    if (btn < 0 || btn >= COUNT_MOUSE_BUTTONS)
        return FALSE;
    return !!mouseState[btn];
}

bool CInput::iGetKeyToggleState(int dik) const
{
    const SDL_Keymod mods = SDL_GetModState();
    if (dik == DIK_CAPITAL)
        return (mods & SDL_KMOD_CAPS) != 0;
    if (dik == DIK_NUMLOCK)
        return (mods & SDL_KMOD_NUM) != 0;
    if (dik == DIK_SCROLL)
        return (mods & SDL_KMOD_SCROLL) != 0;
    return false;
}

// ---- mouse ------------------------------------------------------------------------------------------------

bool CInput::iGetAsyncMousePos(Ivector2& p, bool global) const
{
    SDL_Window* win = sdl->Window();
    if (!win)
        return false;

    float ww, wh, cw, ch;
    sdl->WindowSize(ww, wh);
    sdl->ClientSize(cw, ch);
    if (ww <= 0.f || wh <= 0.f)
        return false;

    if (!sdl->have_pos)
    {
        float x = 0.f, y = 0.f;
        SDL_GetMouseState(&x, &y);
        sdl->px = x;
        sdl->py = y;
        sdl->have_pos = true;
    }
    float x = sdl->px, y = sdl->py;
    if (global)
    {
        int wx = 0, wy = 0;
        SDL_GetWindowPosition(win, &wx, &wy);
        x += float(wx);
        y += float(wy);
        ww = cw = 1.f; // screen coordinates stay in points
        wh = ch = 1.f;
    }
    p.x = iFloor(x * cw / ww);
    p.y = iFloor(y * ch / wh);
    return true;
}

void CInput::iSetMousePos(const Ivector2& p)
{
    SDL_Window* win = sdl->Window();
    if (!win)
        return;
    float ww, wh, cw, ch;
    sdl->WindowSize(ww, wh);
    sdl->ClientSize(cw, ch);
    if (cw <= 0.f || ch <= 0.f)
        return;
    sdl->px = float(p.x) * ww / cw;
    sdl->py = float(p.y) * wh / ch;
    sdl->have_pos = true;
    sdl->ClampPointer();
    if (!sdl->relative_on)
        SDL_WarpMouseInWindow(win, sdl->px, sdl->py);
}

void CInput::MouseUpdate()
{
    sdl->Gather();

    if (Device.dwPrecacheFrame)
    {
        sdl->mouse_events.clear();
        return;
    }

    BOOL mouse_prev[COUNT_MOUSE_BUTTONS];
    for (int i = 0; i < COUNT_MOUSE_BUTTONS; ++i)
        mouse_prev[i] = mouseState[i];

    float ww, wh;
    sdl->WindowSize(ww, wh);
    if (!sdl->have_pos && sdl->Window())
    {
        float x = 0.f, y = 0.f;
        SDL_GetMouseState(&x, &y);
        sdl->px = x;
        sdl->py = y;
        sdl->have_pos = true;
    }

    // Counts are SDL's own units (points) for look. In a dialog they are UI units (1024x768), so the cursor follows
    // the pointer one to one across the window.
    const float scale_x = (sdl->ui_context && ww > 0.f) ? kUiBaseWidth / ww : 1.f;
    const float scale_y = (sdl->ui_context && wh > 0.f) ? kUiBaseHeight / wh : 1.f;

    float fdx = 0.f, fdy = 0.f, fwheel = 0.f;
    bool moved = false;
    offs[0] = offs[1] = offs[2] = 0;

    for (const SDL_Event& e : sdl->mouse_events)
    {
        switch (e.type)
        {
        case SDL_EVENT_MOUSE_MOTION:
        {
            const bool touch = IsTouchLike(e.motion.which);
            float dx = e.motion.xrel;
            float dy = e.motion.yrel;
            if (touch)
            {
                // A touch is an absolute position. In a dialog the cursor goes to the finger; in the world a tap
                // must not turn the view.
                dx = sdl->ui_context ? e.motion.x - sdl->px : 0.f;
                dy = sdl->ui_context ? e.motion.y - sdl->py : 0.f;
                sdl->px = e.motion.x;
                sdl->py = e.motion.y;
            }
            else if (sdl->relative_on)
            {
                sdl->px += e.motion.xrel;
                sdl->py += e.motion.yrel;
            }
            else
            {
                sdl->px = e.motion.x;
                sdl->py = e.motion.y;
            }
            sdl->ClampPointer();
            fdx += dx;
            fdy += dy;
            if (dx != 0.f)
                timeStamp[0] = dwCurTime;
            if (dy != 0.f)
                timeStamp[1] = dwCurTime;
            moved = true;
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            fwheel += e.wheel.y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            if (IsTouchLike(e.button.which) && !sdl->ui_context)
                break; // a tap in the world doesn't fire
            const int idx = MouseIndex(e.button.button);
            if (idx < 0 || idx >= COUNT_MOUSE_BUTTONS)
                break;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                if (!mouseState[idx])
                {
                    mouseState[idx] = TRUE;
                    cbStack.back()->IR_OnMousePress(idx);
                }
            }
            else if (mouseState[idx])
            {
                mouseState[idx] = FALSE;
                cbStack.back()->IR_OnMouseRelease(idx);
            }
            break;
        }
        default:
            break;
        }
    }
    sdl->mouse_events.clear();

    for (int i = 0; i < COUNT_MOUSE_BUTTONS; ++i)
        if (mouseState[i] && mouse_prev[i])
            cbStack.back()->IR_OnMouseHold(i);

    // Whole counts out, the fraction stays for the next frame.
    sdl->carry_x += fdx * scale_x;
    sdl->carry_y += fdy * scale_y;
    const int cx = int(std::trunc(sdl->carry_x));
    const int cy = int(std::trunc(sdl->carry_y));
    sdl->carry_x -= float(cx);
    sdl->carry_y -= float(cy);
    offs[0] = cx;
    offs[1] = cy;

    sdl->wheel_carry += fwheel * float(kWheelDelta);
    const int wheel = int(std::trunc(sdl->wheel_carry));
    sdl->wheel_carry -= float(wheel);
    offs[2] = wheel;

    if (moved || offs[0] || offs[1] || offs[2])
    {
        if (offs[0] || offs[1])
            cbStack.back()->IR_OnMouseMove(offs[0], offs[1]);
        if (offs[2])
            cbStack.back()->IR_OnMouseWheel(offs[2]);
    }
    else
    {
        if (timeStamp[1] && ((dwCurTime - timeStamp[1]) >= mouse_property.mouse_dt))
            cbStack.back()->IR_OnMouseStop(DIMOFS_Y, timeStamp[1] = 0);
        if (timeStamp[0] && ((dwCurTime - timeStamp[0]) >= mouse_property.mouse_dt))
            cbStack.back()->IR_OnMouseStop(DIMOFS_X, timeStamp[0] = 0);
    }
}

// ---- controller layer -------------------------------------------------------------------------------------

void CInput::PadUpdate()
{
    if (!sdl->pad_ready)
        return;

    const Uint64 now = SDL_GetTicksNS();
    float dt = float(double(now - sdl->pad_last_ns) * 1e-9);
    sdl->pad_last_ns = now;
    dt = std::min(std::max(dt, 0.f), 0.1f);

    gameinput::FrameOutput out = sdl->pad_source.Update(dt);

    // The actor turns counts into view rotation with `scale` radians per count horizontally and
    // scale * 3/4 * psMouseSensVerticalK vertically, and flips the vertical sign for "invert mouse". Convert the
    // layer's degrees with the horizontal figure and undo the vertical differences, so the layer's own tuning
    // (degrees per second, invert) is what the player gets.
    float rad_per_count = sdl->look_radians_per_count;
    if (rad_per_count <= 0.f)
        rad_per_count = std::max(psMouseSens * psMouseSensScale, 0.001f) / 50.f;
    const float counts_per_degree = (3.14159265f / 180.f) / rad_per_count;

    float vertical = 0.75f * psMouseSensVerticalK;
    if (vertical < 0.01f)
        vertical = 0.01f;
    out.look_deg.y /= vertical;
    if (psMouseInvert.test(1))
        out.look_deg.y = -out.look_deg.y;

    gameinput::DispatchFrame(sdl->pad, out, *sdl, counts_per_degree);
}

void CInput::feedback(u16 s1, u16 s2, float time)
{
    stop_vibration_time = RDEVICE.fTimeGlobal + time;
    // HUDManager::HitMarked reports a hit with power 0..1 as both motors; the controller layer does better with a
    // typed hit (CActor::Hit), so this is only the fallback.
    if (sdl && sdl->pad_ready)
        sdl->pad.feedback().OnHit(gameinput::HitType::Bullet, float(std::max(s1, s2)) / 65535.f);
}

// ---- text input -------------------------------------------------------------------------------------------

void CInput::EnableTextInput(const void* owner, const Irect* area)
{
    auto& owners = sdl->text_owners;
    if (std::find(owners.begin(), owners.end(), owner) == owners.end())
        owners.push_back(owner);
    if (area)
    {
        float ww, wh, cw, ch;
        sdl->WindowSize(ww, wh);
        sdl->ClientSize(cw, ch);
        const float sx = cw > 0.f ? ww / cw : 1.f;
        const float sy = ch > 0.f ? wh / ch : 1.f;
        sdl->text_area.x = int(area->x1 * sx);
        sdl->text_area.y = int(area->y1 * sy);
        sdl->text_area.w = std::max(1, int((area->x2 - area->x1) * sx));
        sdl->text_area.h = std::max(1, int((area->y2 - area->y1) * sy));
        sdl->has_text_area = true;
    }
    sdl->SyncTextInput();
}

void CInput::DisableTextInput(const void* owner)
{
    auto& owners = sdl->text_owners;
    owners.erase(std::remove(owners.begin(), owners.end(), owner), owners.end());
    if (owners.empty())
        sdl->has_text_area = false;
    sdl->SyncTextInput();
}

bool CInput::IsTextInputEnabled() const { return sdl->TextWanted(); }

// ---- receivers --------------------------------------------------------------------------------------------

void CInput::iCapture(IInputReceiver* p)
{
    VERIFY(p);
    MouseUpdate();
    KeyUpdate();

    // change focus
    if (!cbStack.empty())
        cbStack.back()->IR_OnDeactivate();
    cbStack.push_back(p);
    cbStack.back()->IR_OnActivate();

    // prepare for _new_ controller
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeStamp));
    ZeroMemory(offs, sizeof(offs));
}

void CInput::iRelease(IInputReceiver* p)
{
    if (p == cbStack.back())
    {
        cbStack.back()->IR_OnDeactivate();
        cbStack.pop_back();
        IInputReceiver* ir = cbStack.back();
        ir->IR_OnActivate();
    }
    else
    {
        // we are not topmost receiver, so remove the nearest one
        u32 cnt = cbStack.size();
        for (; cnt > 0; --cnt)
            if (cbStack[cnt - 1] == p)
            {
                xr_vector<IInputReceiver*>::iterator it = cbStack.begin();
                std::advance(it, cnt - 1);
                cbStack.erase(it);
                break;
            }
    }
}

void CInput::OnAppActivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnActivate();

    sdl->app_active = true;
    sdl->ApplyCapture();
    ZeroMemory(mouseState, sizeof(mouseState));
    ZeroMemory(KBState, sizeof(KBState));
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeStamp));
    ZeroMemory(offs, sizeof(offs));
}

void CInput::OnAppDeactivate(void)
{
    if (CurrentIR())
        CurrentIR()->IR_OnDeactivate();

    sdl->app_active = false;
    sdl->ApplyCapture();
    ZeroMemory(mouseState, sizeof(mouseState));
    ZeroMemory(KBState, sizeof(KBState));
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeStamp));
    ZeroMemory(offs, sizeof(offs));
}

void CInput::DeactivateSoft()
{
    if (CurrentIR())
        CurrentIR()->IR_OnDeactivate();

    ZeroMemory(mouseState, sizeof(mouseState));
    ZeroMemory(KBState, sizeof(KBState));
    ZeroMemory(timeStamp, sizeof(timeStamp));
    ZeroMemory(timeSave, sizeof(timeStamp));
    ZeroMemory(offs, sizeof(offs));
}

void CInput::OnFrame(void)
{
    RDEVICE.Statistic->Input.Begin();
    dwCurTime = RDEVICE.TimerAsync_MMT();
    KeyUpdate();
    MouseUpdate();
    PadUpdate();
    RDEVICE.Statistic->Input.End();
}

IInputReceiver* CInput::CurrentIR()
{
    if (cbStack.size())
        return cbStack.back();
    else
        return NULL;
}

void CInput::unacquire()
{
    sdl->suspended = true;
    sdl->ApplyCapture();
}

void CInput::acquire(const bool& exclusive)
{
    g_exclusive = exclusive;
    sdl->suspended = false;
    sdl->ApplyCapture();
}

void CInput::exclusive_mode(const bool& exclusive)
{
    g_exclusive = exclusive;
    unacquire();
    acquire(exclusive);
}

bool CInput::get_exclusive_mode() { return g_exclusive; }

#endif // XR_PLATFORM_APPLE
