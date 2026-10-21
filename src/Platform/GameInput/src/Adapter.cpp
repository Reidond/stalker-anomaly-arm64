#include "GameInput/Adapter.h"

namespace gameinput
{
void DispatchFrame(GameInput& input, const FrameOutput& frame, IEngineInputSink& sink, float look_counts_per_degree)
{
    for (const DeviceEvent& e : frame.device_events)
    {
        if (e.type == DeviceEvent::Type::Connected)
            sink.PadConnected(e.device);
        else if (e.type == DeviceEvent::Type::Disconnected)
            sink.PadDisconnected(e.device);
    }

    for (const KeyEvent& e : frame.keys)
    {
        if (e.down)
            sink.KeyPress(e.key);
        else
            sink.KeyRelease(e.key);
    }

    if (frame.context == InputContext::Gameplay)
    {
        int dx = 0, dy = 0;
        input.LookToCounts(frame.look_deg, look_counts_per_degree, dx, dy);
        if (dx || dy)
            sink.MouseMove(dx, dy);
    }
    else if (frame.cursor_active)
    {
        if (frame.cursor.jumped)
            sink.CursorJump(frame.cursor.pos.x, frame.cursor.pos.y);
        else
        {
            int dx = 0, dy = 0;
            input.CursorToCounts(frame.cursor.delta, dx, dy);
            if (dx || dy)
                sink.MouseMove(dx, dy);
        }
        if (frame.wheel)
            sink.MouseWheel(frame.wheel);
    }

    for (UiCommand c : frame.ui)
        sink.UiAction(c);

    for (KeyCode key : frame.held)
        sink.KeyHold(key);
}

std::vector<EngineKeyDef> EnginePadKeyTable(ControllerFamily family)
{
    std::vector<EngineKeyDef> table;
    table.reserve(size_t(PadKeyCount()));
    for (KeyCode code = kPadKeyBase; code < kPadKeyEnd; ++code)
        table.push_back({PadKeyName(code), code, PadKeyLabel(code, family)});
    return table;
}
} // namespace gameinput
