// First-run / launcher screen drawn with SDL_Renderer debug text. Shows the game-data status, the
// device memory budget, thermal state, free storage and (optionally) the live lifecycle events.
// Works with touch, mouse, keyboard and gamepad; has no dependency beyond SDL3 and the shell.
#pragma once

#include "Shell.h"

#include <string>
#include <vector>

struct SDL_Renderer;
union SDL_Event;

namespace AppleShell
{
class Launcher
{
public:
    enum class Action
    {
        None,
        StartGame, // the user pressed Start with valid game data
        Quit,      // macOS only
        Custom,    // one of the buttons added with AddButton(); see CustomButtonId()
    };

    struct Config
    {
        std::string title = "STALKER Anomaly 1.5.3";
        std::string subtitle;       // e.g. build name
        bool show_event_log = true; // live lifecycle / memory / thermal / power events
        bool allow_quit = false;    // adds a Quit button (macOS)
    };

    Launcher(SDL_Window* window, SDL_Renderer* renderer, Config config);
    ~Launcher();

    // Extra button next to Start/Rescan. Returns its id, reported by CustomButtonId().
    int AddButton(const std::string& label);
    // A line shown under the device block (e.g. "Allocated: 512 MB"). Replaces the previous text.
    void SetExtraText(const std::string& text);

    // Feed every SDL event. Returns the action the event triggered, or None.
    Action HandleEvent(const SDL_Event& event);
    int CustomButtonId() const { return custom_id_; }

    // Re-checks the game data and device info (also done automatically every 2 s while the data is not valid).
    void Rescan();
    const GameDataStatus& Data() const { return data_; }

    // Draws and presents one frame.
    void Render();

    // Saves the next rendered frame as a PNG (before it is presented). For checking the layout on a device.
    void SaveScreenshotNextFrame(const std::string& path) { screenshot_path_ = path; }

private:
    struct Button
    {
        std::string label;
        int id;
        float x = 0, y = 0, w = 0, h = 0;
        bool enabled = true;
    };

    Action Press(int id);

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    Config config_;
    GameDataStatus data_;
    DeviceInfo device_;
    DisplayInfo display_;
    std::vector<Button> buttons_;
    std::string extra_;
    int custom_id_ = 0;
    int pressed_ = -1;
    float unit_ = 1.0f; // points per layout unit
    uint64_t last_scan_ms_ = 0;
    uint64_t last_device_ms_ = 0;
    std::vector<void*> gamepads_;
    std::string screenshot_path_;
};
} // namespace AppleShell
