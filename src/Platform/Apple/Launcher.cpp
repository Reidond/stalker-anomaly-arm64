#include "Launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace AppleShell
{
namespace
{
constexpr int kStartId = 1, kRescanId = 2, kQuitId = 3, kFirstCustomId = 100;
constexpr float kLine = 12.0f;  // layout units per text line
constexpr float kChar = 8.0f;   // SDL debug font glyph size
constexpr float kLayoutWidth = 720.0f;

struct Color
{
    Uint8 r, g, b;
};
constexpr Color kText{228, 228, 224}, kDim{140, 144, 140}, kGreen{110, 214, 120}, kAmber{240, 190, 80}, kRed{240, 100, 90},
    kAccent{232, 168, 56};

void Text(SDL_Renderer* r, float x, float y, const std::string& text, Color c = kText)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderDebugText(r, x, y, text.c_str());
}

// Hard-wraps on spaces so long paths and help lines stay inside the screen.
std::vector<std::string> Wrap(const std::string& text, size_t columns)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (line.size() > columns)
        {
            size_t cut = line.rfind(' ', columns);
            if (cut == std::string::npos || cut < columns / 2)
                cut = columns;
            lines.push_back(line.substr(0, cut));
            line.erase(0, line.find_first_not_of(' ', cut));
        }
        lines.push_back(line);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

std::string Fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

// First wrapped line of `text`; the event log shows one line per event.
std::string First(const std::string& text, size_t columns)
{
    auto lines = Wrap(text, columns);
    return lines.empty() ? std::string() : lines[0];
}

Color ThermalColor(ThermalState s)
{
    switch (s)
    {
    case ThermalState::Nominal: return kGreen;
    case ThermalState::Fair: return kAmber;
    default: return kRed;
    }
}
} // namespace

Launcher::Launcher(SDL_Window* window, SDL_Renderer* renderer, Config config)
    : window_(window), renderer_(renderer), config_(std::move(config))
{
    buttons_.push_back({"Start game", kStartId});
    buttons_.push_back({"Rescan", kRescanId});
    if (config_.allow_quit)
        buttons_.push_back({"Quit", kQuitId});
    Rescan();
}

Launcher::~Launcher()
{
    for (void* pad : gamepads_)
        SDL_CloseGamepad(static_cast<SDL_Gamepad*>(pad));
}

int Launcher::AddButton(const std::string& label)
{
    const int id = kFirstCustomId + int(buttons_.size());
    buttons_.push_back({label, id});
    return id;
}

void Launcher::SetExtraText(const std::string& text)
{
    extra_ = text;
}

void Launcher::Rescan()
{
    data_ = ValidateGameData(GetPaths().data_root);
    device_ = QueryDeviceInfo();
    display_ = QueryDisplayInfo(window_);
    last_scan_ms_ = last_device_ms_ = SDL_GetTicks();
}

Launcher::Action Launcher::Press(int id)
{
    switch (id)
    {
    case kStartId: return data_.state == DataState::Ok ? Action::StartGame : Action::None;
    case kRescanId: Rescan(); return Action::None;
    case kQuitId: return Action::Quit;
    default: custom_id_ = id; return Action::Custom;
    }
}

Launcher::Action Launcher::HandleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        // Touch is delivered as mouse events too (SDL's default), in window points.
        const float x = event.button.x / unit_, y = event.button.y / unit_;
        int hit = -1;
        for (const Button& b : buttons_)
            if (b.enabled && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
                hit = b.id;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            pressed_ = hit;
            return Action::None;
        }
        const int released_on = pressed_;
        pressed_ = -1;
        return (hit != -1 && hit == released_on) ? Press(hit) : Action::None;
    }
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat)
            break;
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE)
            return Press(kStartId);
        if (event.key.key == SDLK_R)
            return Press(kRescanId);
        if (config_.allow_quit && (event.key.key == SDLK_Q || event.key.key == SDLK_ESCAPE))
            return Action::Quit;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which))
            gamepads_.push_back(pad);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (auto it = gamepads_.begin(); it != gamepads_.end(); ++it)
        {
            if (SDL_GetGamepadID(static_cast<SDL_Gamepad*>(*it)) == event.gdevice.which)
            {
                SDL_CloseGamepad(static_cast<SDL_Gamepad*>(*it));
                gamepads_.erase(it);
                break;
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
            return Press(kStartId);
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_WEST)
            return Press(kRescanId);
        break;
    default:
        break;
    }
    return Action::None;
}

void Launcher::Render()
{
    const uint64_t now = SDL_GetTicks();
    if (data_.state != DataState::Ok && now - last_scan_ms_ > 2000)
        Rescan(); // pick up a copy in progress
    if (now - last_device_ms_ > 1000)
    {
        device_ = QueryDeviceInfo();
        display_ = QueryDisplayInfo(window_);
        last_device_ms_ = now;
    }

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);
    unit_ = std::max(1.0f, float(win_w) / kLayoutWidth);
    const float density = SDL_GetWindowPixelDensity(window_);
    SDL_SetRenderScale(renderer_, density * unit_, density * unit_);

    const float width = float(win_w) / unit_, height = float(win_h) / unit_;
    const float left = display_.safe_area_points.left / unit_ + 16.0f;
    const float top = display_.safe_area_points.top / unit_ + 12.0f;
    const float right = width - display_.safe_area_points.right / unit_ - 16.0f;
    const float bottom = height - display_.safe_area_points.bottom / unit_ - 12.0f;
    const size_t columns = size_t((right - left) / kChar);

    SDL_SetRenderDrawColor(renderer_, 16, 18, 16, 255);
    SDL_RenderClear(renderer_);

    float y = top;
    Text(renderer_, left, y, config_.title, kAccent);
    y += kLine;
    Text(renderer_, left, y, config_.subtitle.empty() ? Fmt("%s  %s", device_.model.c_str(), device_.os.c_str()) : config_.subtitle, kDim);
    y += kLine * 2;

    // Buttons sit at the bottom; lay them out first so the text above knows how much room it has.
    const float button_h = 36.0f, gap = 10.0f;
    float button_w = std::min(150.0f, (right - left - gap * float(buttons_.size() - 1)) / float(buttons_.size()));
    const float button_y = bottom - button_h;
    float bx = left;
    for (Button& b : buttons_)
    {
        b.x = bx; b.y = button_y; b.w = button_w; b.h = button_h;
        b.enabled = !(b.id == kStartId && data_.state != DataState::Ok);
        bx += button_w + gap;
    }

    // --- game data ---
    Color state_color = data_.state == DataState::Ok ? kGreen : data_.state == DataState::Incomplete ? kAmber : kRed;
    Text(renderer_, left, y, "GAME DATA", kAccent);
    y += kLine;
    Text(renderer_, left, y, data_.summary, state_color);
    y += kLine;
    for (const std::string& line : Wrap("Folder: " + data_.root, columns))
    {
        Text(renderer_, left, y, line, kDim);
        y += kLine;
    }
    if (data_.state == DataState::Ok)
    {
        Text(renderer_, left, y, Fmt("gamedata/ addons folder: %s", data_.has_gamedata_dir ? "present" : "absent (optional)"), kDim);
        y += kLine;
    }
    else
    {
        for (size_t i = 0; i < data_.issues.size() && i < 3; ++i)
        {
            const DataIssue& issue = data_.issues[i];
            Text(renderer_, left, y, First(Fmt("%s: %s (%s)", issue.missing ? "missing" : "wrong size", issue.path.c_str(),
                FormatBytes(issue.expected).c_str()), columns), kAmber);
            y += kLine;
        }
        if (data_.issues.size() > 3)
        {
            Text(renderer_, left, y, Fmt("... and %u more", unsigned(data_.files_expected - data_.files_ok - 3)), kAmber);
            y += kLine;
        }
        y += 4.0f;
        for (const std::string& line : Wrap(data_.help, columns))
        {
            Text(renderer_, left, y, line);
            y += kLine;
        }
    }
    y += kLine;

    // --- device ---
    Text(renderer_, left, y, "DEVICE", kAccent);
    y += kLine;
    if (device_.has_memory_budget)
        Text(renderer_, left, y, Fmt("Memory budget: %s available to the app (RAM %s)", FormatBytes(device_.available_memory).c_str(),
            FormatBytes(device_.physical_memory).c_str()));
    else
        Text(renderer_, left, y, Fmt("Memory: %s RAM (no per-app budget on this platform)", FormatBytes(device_.physical_memory).c_str()));
    y += kLine;
    Text(renderer_, left, y, Fmt("Thermal: %s", ToString(device_.thermal)), ThermalColor(device_.thermal));
    Text(renderer_, left + 17 * kChar, y, Fmt("Low Power Mode: %s", device_.low_power_mode ? "ON" : "off"), device_.low_power_mode ? kAmber : kText);
    y += kLine;
    const bool storage_low = device_.storage_free_raw < data_.bytes_expected && data_.state != DataState::Ok;
    Text(renderer_, left, y, Fmt("Storage: %s free of %s (%s incl. purgeable)", FormatBytes(device_.storage_free_raw).c_str(),
        FormatBytes(device_.storage_total).c_str(), FormatBytes(device_.storage_free_important).c_str()),
        storage_low ? kRed : kText);
    y += kLine;
    Text(renderer_, left, y, Fmt("Display: %dx%d px native, %.0fx%.0f pt @%.1fx, %.0f Hz (max %.0f%s)", display_.native_width, display_.native_height,
        display_.window_width, display_.window_height, display_.scale, display_.refresh_hz, display_.max_refresh_hz,
        display_.promotion ? ", ProMotion" : ""));
    y += kLine;
    Text(renderer_, left, y, Fmt("Safe area (pt): L%.0f T%.0f R%.0f B%.0f   keep-awake: on", display_.safe_area_points.left, display_.safe_area_points.top,
        display_.safe_area_points.right, display_.safe_area_points.bottom), kDim);
    y += kLine;
    if (PreviousSessionEndedUnexpectedly())
    {
        Text(renderer_, left, y, "Previous session ended unexpectedly (crash, memory kill or force quit)", kAmber);
        y += kLine;
    }
    if (PreviousCrashLogExists())
    {
        Text(renderer_, left, y, "Crash report from an earlier run: anomaly-crash.log", kAmber);
        y += kLine;
    }
    if (!extra_.empty())
    {
        for (const std::string& line : Wrap(extra_, columns))
        {
            Text(renderer_, left, y, line, kAccent);
            y += kLine;
        }
    }
    y += kLine;

    // --- live events ---
    if (config_.show_event_log)
    {
        Text(renderer_, left, y, IsPaused() ? "EVENTS (paused)" : "EVENTS", kAccent);
        y += kLine;
        const size_t room = size_t(std::max(0.0f, (button_y - kLine - 14.0f - y) / kLine));
        for (const EventRecord& e : RecentEvents(room))
        {
            Text(renderer_, left, y, First(Fmt("%8.2f  %-9s %s", e.time_s, e.category.c_str(), e.text.c_str()), columns),
                e.category == "memory" ? kAmber : e.category == "lifecycle" ? kText : kDim);
            y += kLine;
        }
    }

    // --- buttons ---
    for (const Button& b : buttons_)
    {
        const bool down = pressed_ == b.id;
        SDL_SetRenderDrawColor(renderer_, b.enabled ? (down ? 90 : 52) : 30, b.enabled ? (down ? 78 : 46) : 32, b.enabled ? 30 : 30, 255);
        SDL_FRect rect{b.x, b.y, b.w, b.h};
        SDL_RenderFillRect(renderer_, &rect);
        SDL_SetRenderDrawColor(renderer_, b.enabled ? kAccent.r : 70, b.enabled ? kAccent.g : 70, b.enabled ? kAccent.b : 70, 255);
        SDL_RenderRect(renderer_, &rect);
        const float tw = float(b.label.size()) * kChar;
        Text(renderer_, b.x + (b.w - tw) / 2, b.y + (b.h - kChar) / 2, b.label, b.enabled ? kText : kDim);
    }
    Text(renderer_, left, button_y - kLine - 4.0f, "Gamepad: A start, X rescan.  Keyboard: Enter start, R rescan.", kDim);

    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    if (!screenshot_path_.empty())
    {
        if (SDL_Surface* frame = SDL_RenderReadPixels(renderer_, nullptr))
        {
            Log("screenshot %s: %s", screenshot_path_.c_str(), SDL_SavePNG(frame, screenshot_path_.c_str()) ? "saved" : SDL_GetError());
            SDL_DestroySurface(frame);
        }
        screenshot_path_.clear();
    }
    SDL_RenderPresent(renderer_);
}

} // namespace AppleShell
