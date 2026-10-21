// Key-code space for the gamepad layer.
//
// The engine identifies every input by an int "key": DirectInput scan codes (0..0xED), MOUSE_1..8
// (0xED+100..107) and kCUSTOM0..99 (1000..1099, only reachable from scripts). Gamepad buttons get
// their own range above those, so the stock bind/Controls machinery can name, store and rebind them
// exactly like keyboard keys. Names are what ends up in user.ltx ("bind_pad jump gp_cross"); the
// numbers only live at runtime and may change between builds.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gameinput
{
using KeyCode = int;

// Subset of the engine's own key codes the layer needs to emulate keyboard/mouse input in menus.
// Values are DirectInput scan codes and the MOUSE_n defines from xr_level_controller.h.
namespace engine_key
{
constexpr KeyCode kNone = 0;
constexpr KeyCode kEscape = 0x01;
constexpr KeyCode kBackspace = 0x0E;
constexpr KeyCode kTab = 0x0F;
constexpr KeyCode kReturn = 0x1C;
constexpr KeyCode kLControl = 0x1D;
constexpr KeyCode kLShift = 0x2A;
constexpr KeyCode kSpace = 0x39;
constexpr KeyCode kUp = 0xC8;
constexpr KeyCode kLeft = 0xCB;
constexpr KeyCode kRight = 0xCD;
constexpr KeyCode kDown = 0xD0;
constexpr KeyCode kDelete = 0xD3;
constexpr KeyCode kMouse1 = 0xED + 100; // left button
constexpr KeyCode kMouse2 = 0xED + 101; // right button
constexpr KeyCode kMouse3 = 0xED + 102; // middle button
} // namespace engine_key

constexpr KeyCode kPadKeyBase = 1200;

// X(id, config name, PlayStation label, Xbox label)
#define GAMEINPUT_PAD_KEYS(X)                                                                                         \
    X(Cross, "gp_cross", "Cross", "A")                                                                                \
    X(Circle, "gp_circle", "Circle", "B")                                                                             \
    X(Square, "gp_square", "Square", "X")                                                                             \
    X(Triangle, "gp_triangle", "Triangle", "Y")                                                                       \
    X(L1, "gp_l1", "L1", "LB")                                                                                        \
    X(R1, "gp_r1", "R1", "RB")                                                                                        \
    X(L2, "gp_l2", "L2", "LT")                                                                                        \
    X(R2, "gp_r2", "R2", "RT")                                                                                        \
    X(L3, "gp_l3", "L3", "LS")                                                                                        \
    X(R3, "gp_r3", "R3", "RS")                                                                                        \
    X(DpadUp, "gp_dpad_up", "D-pad Up", "D-pad Up")                                                                   \
    X(DpadDown, "gp_dpad_down", "D-pad Down", "D-pad Down")                                                           \
    X(DpadLeft, "gp_dpad_left", "D-pad Left", "D-pad Left")                                                           \
    X(DpadRight, "gp_dpad_right", "D-pad Right", "D-pad Right")                                                       \
    X(Create, "gp_create", "Create", "View")                                                                          \
    X(Options, "gp_options", "Options", "Menu")                                                                       \
    X(Ps, "gp_ps", "PS", "Xbox")                                                                                      \
    X(Touchpad, "gp_touchpad", "Touchpad", "Share")                                                                   \
    X(Mic, "gp_mic", "Mute", "Share")                                                                                 \
    X(PaddleLeft1, "gp_paddle_l1", "Left Fn", "Left Paddle 1")                                                        \
    X(PaddleLeft2, "gp_paddle_l2", "Left Paddle", "Left Paddle 2")                                                    \
    X(PaddleRight1, "gp_paddle_r1", "Right Fn", "Right Paddle 1")                                                     \
    X(PaddleRight2, "gp_paddle_r2", "Right Paddle", "Right Paddle 2")                                                 \
    X(LStickUp, "gp_lstick_up", "L-Stick Up", "LS Up")                                                                \
    X(LStickDown, "gp_lstick_down", "L-Stick Down", "LS Down")                                                        \
    X(LStickLeft, "gp_lstick_left", "L-Stick Left", "LS Left")                                                        \
    X(LStickRight, "gp_lstick_right", "L-Stick Right", "LS Right")                                                    \
    X(RStickUp, "gp_rstick_up", "R-Stick Up", "RS Up")                                                                \
    X(RStickDown, "gp_rstick_down", "R-Stick Down", "RS Down")                                                        \
    X(RStickLeft, "gp_rstick_left", "R-Stick Left", "RS Left")                                                        \
    X(RStickRight, "gp_rstick_right", "R-Stick Right", "RS Right")                                                    \
    X(Walk, "gp_walk", "L-Stick Soft", "LS Soft")                                                                     \
    X(TouchSwipeUp, "gp_touch_up", "Touchpad Swipe Up", "Swipe Up")                                                   \
    X(TouchSwipeDown, "gp_touch_down", "Touchpad Swipe Down", "Swipe Down")                                           \
    X(TouchSwipeLeft, "gp_touch_left", "Touchpad Swipe Left", "Swipe Left")                                           \
    X(TouchSwipeRight, "gp_touch_right", "Touchpad Swipe Right", "Swipe Right")

enum PadKey : KeyCode
{
    PK_NONE = 0,
    PK__BEFORE_FIRST = kPadKeyBase - 1, // so the first real key (Cross) is exactly kPadKeyBase
#define GAMEINPUT_X(id, name, ps, xb) PK_##id,
    GAMEINPUT_PAD_KEYS(GAMEINPUT_X)
#undef GAMEINPUT_X
    PK__END,
};
constexpr KeyCode kPadButtonKeyEnd = PK__END;

// Virtual keys fired by the radial menus. Slot n of a wheel page is key PK_RADIAL_1 + n, so each
// slot can be bound in the Controls menu like any other key.
constexpr int kRadialKeyCount = 32;
constexpr KeyCode kRadialKeyBase = kPadButtonKeyEnd;
constexpr KeyCode PK_RADIAL_1 = kRadialKeyBase;
constexpr KeyCode kPadKeyEnd = kRadialKeyBase + kRadialKeyCount;

constexpr bool IsPadKey(KeyCode key) { return key >= kPadKeyBase && key < kPadKeyEnd; }
constexpr bool IsRadialKey(KeyCode key) { return key >= kRadialKeyBase && key < kPadKeyEnd; }

enum class ControllerFamily : uint8_t
{
    Generic,
    PlayStation, // DualShock 4, DualSense, DualSense Edge
    Xbox,
    Nintendo,
};

// Config name ("gp_cross", "gp_radial_3"); empty for codes outside the pad range.
std::string PadKeyName(KeyCode key);
// Inverse of PadKeyName, case-insensitive. Returns PK_NONE if unknown.
KeyCode PadKeyFromName(std::string_view name);
// Human-readable label for the Controls menu / on-screen hints ("Cross", "A", "Radial 3").
std::string PadKeyLabel(KeyCode key, ControllerFamily family = ControllerFamily::PlayStation);
// Every pad key code in order, for registering the whole range with the engine's key table.
constexpr int PadKeyCount() { return kPadKeyEnd - kPadKeyBase; }
} // namespace gameinput
