// The C interface of the SwiftUI launcher (src/Platform/Launcher/LauncherEntry.swift). The app shell opens it inside the window it
// created, feeds it the pad, polls for the answer and closes it before the engine creates its own window. Main thread only.
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

// Pad buttons in the launcher's numbering (not SDL's): the shell maps its events onto these.
enum AnomalyLauncherButton
{
    ANOMALY_LAUNCHER_UP = 0,
    ANOMALY_LAUNCHER_DOWN = 1,
    ANOMALY_LAUNCHER_LEFT = 2,
    ANOMALY_LAUNCHER_RIGHT = 3,
    ANOMALY_LAUNCHER_CONFIRM = 4,  // Cross, Return
    ANOMALY_LAUNCHER_BACK = 5,     // Circle, Escape
    ANOMALY_LAUNCHER_SQUARE = 6,   // held: reorder
    ANOMALY_LAUNCHER_TRIANGLE = 7, // details
    ANOMALY_LAUNCHER_OPTIONS = 8,  // Play
};

enum AnomalyLauncherResult
{
    ANOMALY_LAUNCHER_RUNNING = 0,
    ANOMALY_LAUNCHER_PLAY = 1,
    ANOMALY_LAUNCHER_QUIT = 2,
};

// Hosts the launcher in `native_window` (the UIWindow* / NSWindow* of an SDL window; SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER /
// SDL_PROP_WINDOW_COCOA_WINDOW_POINTER). `config_json` is a LauncherConfig (LauncherModel.swift): mods_root, downloads_dir,
// platform ("ios" | "macos"), allow_quit, version, device_line, notices, logo_path, auto_play_ms. `data_status` is called when the launcher
// wants the game data checked again and returns a malloc'd JSON string {state, summary, help, root, files_ok, files_expected}.
// Returns the launcher, or NULL when it could not be hosted.
void* anomaly_launcher_open(void* native_window, const char* config_json, char* (*data_status)(void* user), void* user);

// ANOMALY_LAUNCHER_RUNNING until the player pressed Play or Quit.
int anomaly_launcher_result(void* launcher);

// A pad button (keyboard != 0: a key on the iPad's hardware keyboard, which changes the hints shown).
void anomaly_launcher_button(void* launcher, int button, int down, int keyboard);
// The left stick: axis 0 is X, 1 is Y (down is positive), value -1..1.
void anomaly_launcher_axis(void* launcher, int axis, float value);

// Writes a PNG of the launcher's view. 1 on success.
int anomaly_launcher_snapshot(void* launcher, const char* png_path);

// Takes the launcher out of its window and releases it. Destroy the window afterwards.
void anomaly_launcher_close(void* launcher);

#ifdef __cplusplus
}
#endif
