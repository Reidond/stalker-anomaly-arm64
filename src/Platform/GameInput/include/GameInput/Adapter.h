// The thin seam between the controller layer and the engine.
//
// The engine side implements IEngineInputSink with a handful of one-line forwards to its existing
// input entry points (CInput's receiver stack, CUICursor), then calls DispatchFrame() once per
// frame from CInput::OnFrame. See docs/ios-port/CONTROLS.md for the exact mapping.
#pragma once

#include <string>
#include <vector>

#include "GameInput/GameInput.h"

namespace gameinput
{
class IEngineInputSink
{
public:
    virtual ~IEngineInputSink() = default;

    // -> cbStack.back()->IR_OnKeyboardPress / Release / Hold(key). Keys are pad keys (gp_*), radial
    // keys, or in menus the engine's own MOUSE_n / DIK codes.
    virtual void KeyPress(KeyCode key) = 0;
    virtual void KeyRelease(KeyCode key) = 0;
    virtual void KeyHold(KeyCode key) = 0;

    // -> IR_OnMouseMove(dx, dy). In gameplay: look. In menus: cursor movement in UI units.
    virtual void MouseMove(int dx, int dy) = 0;
    // -> IR_OnMouseWheel(direction); positive = up.
    virtual void MouseWheel(int notches) = 0;
    // -> GetUICursor().SetUICursorPosition(...): the cursor moved discontinuously (D-pad widget jump).
    virtual void CursorJump(float ui_x, float ui_y) = 0;

    virtual void UiAction(UiCommand /*command*/) {}
    virtual void PadConnected(const DeviceIdentity& /*device*/) {}
    // The game should pause here (Device.Pause) and show a "reconnect the controller" prompt.
    virtual void PadDisconnected(const DeviceIdentity& /*device*/) {}
};

// Translates one frame of layer output into sink calls, in the order CInput::KeyUpdate/MouseUpdate
// makes them: device events, key releases and presses, mouse motion, wheel, then a Hold for every
// key that is down. `look_counts_per_degree` converts view rotation to the engine's integer mouse
// counts (the engine derives it from its current sensitivity and FOV; see CONTROLS.md).
void DispatchFrame(GameInput& input, const FrameOutput& frame, IEngineInputSink& sink, float look_counts_per_degree);

// One entry per pad key, for appending to the engine's keyboards[] table so bind/unbind, the
// Controls menu and user.ltx can name and display them.
struct EngineKeyDef
{
    std::string name;  // key_name: "gp_cross"
    KeyCode code;      // dik: runtime key code
    std::string label; // key_local_name: "Cross"
};
std::vector<EngineKeyDef> EnginePadKeyTable(ControllerFamily family);
} // namespace gameinput
