// SDL3 backend: opens gamepads, tracks connect/disconnect, samples the active pad into a
// PadSnapshot, and carries out feedback (rumble and light bar through SDL, adaptive triggers through
// the native GameController layer).
//
// The host owns the SDL event loop and forwards every event to HandleEvent().
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "GameInput/DualSenseTriggers.h"
#include "GameInput/GameInput.h"

namespace gameinput
{
class SdlPadSource : public IFeedbackSink
{
public:
    explicit SdlPadSource(GameInput& input);
    ~SdlPadSource() override;

    SdlPadSource(const SdlPadSource&) = delete;
    SdlPadSource& operator=(const SdlPadSource&) = delete;

    // Opens pads that are already connected. Call after SDL_Init(SDL_INIT_GAMEPAD) and before the
    // first Update(). Also attaches itself as the input's feedback sink.
    void Init();
    // Gamepad added/removed events. Returns true if the event was handled here.
    bool HandleEvent(const SDL_Event& event);
    // Samples the active pad. connected == false if there is none.
    PadSnapshot Poll();
    // Poll() + GameInput::Update().
    FrameOutput Update(float dt);

    // Active pad's identity, or nullptr.
    const DeviceIdentity* ActiveDevice() const { return input_.devices().Active(); }
    // Describes native capabilities for logs: SDL view plus GameController view.
    std::vector<std::string> DescribeCapabilities() const;
    // Last time-stamped input, for "controller idle" decisions.
    float SecondsSinceInput() const;

    // IFeedbackSink
    void SetRumble(const MotorState& motors) override;
    void SetLed(Rgb color) override;
    void SetTriggers(const TriggerPair& triggers) override;
    void Silence() override;

private:
    struct Pad
    {
        SDL_Gamepad* handle = nullptr;
        SDL_JoystickID id = 0;
        bool has_touchpad = false;
    };

    void Open(SDL_JoystickID id);
    void Close(SDL_JoystickID id);
    Pad* Find(SDL_JoystickID id);
    Pad* ActivePad();
    void Sample(const Pad& pad, PadSnapshot& out) const;
    bool HasInput(const Pad& pad) const;

    GameInput& input_;
    std::vector<Pad> pads_;
    Uint64 last_input_ns_ = 0;
    int active_player_index_ = -1;
};
} // namespace gameinput
