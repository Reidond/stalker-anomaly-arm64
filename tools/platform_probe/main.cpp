// Platform probe: the first thing the port runs on macOS and iPadOS.
//
// Checks, before any engine code is involved, that the toolchain, signing and the libraries the
// port depends on work on the device: SDL3 window + Metal renderer, LuaJIT (and whether its JIT is
// on), OpenAL Soft, the iOS memory budget, and full DualSense input (buttons, sticks, triggers,
// gyro, touchpad, rumble, light bar).

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <AL/al.h>
#include <AL/alc.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <os/proc.h>
#endif

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
struct ProbeState
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Gamepad* gamepad = nullptr;

    std::string lua_report;
    std::string openal_report;
    std::string memory_report;

    float gyro[3] = {};
    bool has_gyro = false;
};

std::string ProbeLuaJIT()
{
    lua_State* L = luaL_newstate();
    if (!L)
        return "LuaJIT: luaL_newstate failed";
    luaL_openlibs(L);

    const char* script =
        "local status = { jit.status() }\n"
        "return string.format('%s on %s/%s, JIT %s', jit.version, jit.os, jit.arch, status[1] and 'ON' or 'OFF')";
    std::string report;
    if (luaL_dostring(L, script) == 0 && lua_isstring(L, -1))
        report = lua_tostring(L, -1);
    else
        report = std::string("LuaJIT error: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "unknown");
    lua_close(L);
    return report;
}

std::string ProbeOpenAL()
{
    ALCdevice* device = alcOpenDevice(nullptr);
    if (!device)
        return "OpenAL: no output device";
    std::string report = "OpenAL: ";
    const ALCchar* name = alcGetString(device, ALC_ALL_DEVICES_SPECIFIER);
    report += name ? name : "(unnamed device)";
    if (ALCcontext* context = alcCreateContext(device, nullptr))
    {
        alcMakeContextCurrent(context);
        const ALchar* renderer = alGetString(AL_RENDERER);
        report += std::string(" / ") + (renderer ? renderer : "?");
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
    }
    alcCloseDevice(device);
    return report;
}

std::string ProbeMemory()
{
    char buffer[128];
#if TARGET_OS_IOS
    // Remaining jetsam budget for this process; shows whether increased-memory-limit took effect.
    std::snprintf(buffer, sizeof(buffer), "Memory: %.2f GB available to the app (os_proc_available_memory)",
        double(os_proc_available_memory()) / (1024.0 * 1024.0 * 1024.0));
#else
    std::snprintf(buffer, sizeof(buffer), "Memory: %d MB system RAM", SDL_GetSystemRAM());
#endif
    return buffer;
}

void OpenGamepad(ProbeState& state, SDL_JoystickID id)
{
    if (state.gamepad)
        return;
    state.gamepad = SDL_OpenGamepad(id);
    if (!state.gamepad)
    {
        SDL_Log("SDL_OpenGamepad failed: %s", SDL_GetError());
        return;
    }
    state.has_gyro = SDL_GamepadHasSensor(state.gamepad, SDL_SENSOR_GYRO) &&
        SDL_SetGamepadSensorEnabled(state.gamepad, SDL_SENSOR_GYRO, true);
    SDL_SetGamepadLED(state.gamepad, 255, 140, 0);
    SDL_Log("Gamepad connected: %s (type %d, gyro %s)", SDL_GetGamepadName(state.gamepad),
        int(SDL_GetGamepadType(state.gamepad)), state.has_gyro ? "yes" : "no");
}

void CloseGamepad(ProbeState& state, SDL_JoystickID id)
{
    if (state.gamepad && SDL_GetGamepadID(state.gamepad) == id)
    {
        SDL_CloseGamepad(state.gamepad);
        state.gamepad = nullptr;
        state.has_gyro = false;
        SDL_Log("Gamepad disconnected");
    }
}

std::vector<std::string> DescribeGamepad(const ProbeState& state)
{
    std::vector<std::string> lines;
    SDL_Gamepad* pad = state.gamepad;
    if (!pad)
    {
        lines.emplace_back("Gamepad: none - connect the DualSense over Bluetooth");
        return lines;
    }

    char buffer[256];
    int battery = -1;
    SDL_PowerState power = SDL_GetGamepadPowerInfo(pad, &battery);
    std::snprintf(buffer, sizeof(buffer), "Gamepad: %s  type=%s  battery=%d%% (state %d)", SDL_GetGamepadName(pad),
        SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)), battery, int(power));
    lines.emplace_back(buffer);

    std::string pressed = "Buttons:";
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    {
        auto button = SDL_GamepadButton(b);
        if (SDL_GetGamepadButton(pad, button))
        {
            pressed += ' ';
            pressed += SDL_GetGamepadStringForButton(button);
        }
    }
    lines.push_back(pressed);

    auto axis = [pad](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0f; };
    std::snprintf(buffer, sizeof(buffer), "Left stick % .2f % .2f   Right stick % .2f % .2f   L2 %.2f  R2 %.2f",
        axis(SDL_GAMEPAD_AXIS_LEFTX), axis(SDL_GAMEPAD_AXIS_LEFTY), axis(SDL_GAMEPAD_AXIS_RIGHTX),
        axis(SDL_GAMEPAD_AXIS_RIGHTY), axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    lines.emplace_back(buffer);

    if (state.has_gyro)
        std::snprintf(buffer, sizeof(buffer), "Gyro rad/s % .2f % .2f % .2f", state.gyro[0], state.gyro[1], state.gyro[2]);
    else
        std::snprintf(buffer, sizeof(buffer), "Gyro: not available");
    lines.emplace_back(buffer);

    if (SDL_GetNumGamepadTouchpads(pad) > 0)
    {
        bool down = false;
        float x = 0, y = 0, pressure = 0;
        SDL_GetGamepadTouchpadFinger(pad, 0, 0, &down, &x, &y, &pressure);
        std::snprintf(buffer, sizeof(buffer), "Touchpad: %s  x=%.2f y=%.2f", down ? "down" : "up", x, y);
    }
    else
        std::snprintf(buffer, sizeof(buffer), "Touchpad: not available");
    lines.emplace_back(buffer);

    lines.emplace_back("Cross = rumble, triangle = trigger rumble, left stick tints the light bar");
    return lines;
}
} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int /*argc*/, char* /*argv*/[])
{
    SDL_SetAppMetadata("Anomaly platform probe", "0.1", "com.andriishafar.anomaly.probe");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    auto* state = new ProbeState;
    *appstate = state;

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
#if TARGET_OS_IOS
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    if (!SDL_CreateWindowAndRenderer("Anomaly platform probe", 1280, 800, flags, &state->window, &state->renderer))
    {
        SDL_Log("Window/renderer creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(state->renderer, 1);

    state->lua_report = ProbeLuaJIT();
    state->openal_report = ProbeOpenAL();
    state->memory_report = ProbeMemory();
    SDL_Log("Renderer: %s", SDL_GetRendererName(state->renderer));
    SDL_Log("%s", state->lua_report.c_str());
    SDL_Log("%s", state->openal_report.c_str());
    SDL_Log("%s", state->memory_report.c_str());

    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
    {
        if (count > 0)
            OpenGamepad(*state, ids[0]);
        SDL_free(ids);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    auto& state = *static_cast<ProbeState*>(appstate);
    switch (event->type)
    {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key == SDLK_ESCAPE)
            return SDL_APP_SUCCESS;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        OpenGamepad(state, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        CloseGamepad(state, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (event->gsensor.sensor == SDL_SENSOR_GYRO)
            SDL_memcpy(state.gyro, event->gsensor.data, sizeof(state.gyro));
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (!state.gamepad)
            break;
        if (event->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
            SDL_RumbleGamepad(state.gamepad, 0x4000, 0xC000, 250);
        else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_NORTH && !SDL_RumbleGamepadTriggers(state.gamepad, 0xFFFF, 0xFFFF, 250))
            SDL_Log("Trigger rumble unsupported: %s", SDL_GetError());
        SDL_Log("Button down: %s", SDL_GetGamepadStringForButton(SDL_GamepadButton(event->gbutton.button)));
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto& state = *static_cast<ProbeState*>(appstate);

    float lx = 0, ly = 0;
    if (state.gamepad)
    {
        lx = SDL_GetGamepadAxis(state.gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        ly = SDL_GetGamepadAxis(state.gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        auto channel = [](float v) { return Uint8(127.5f + 127.5f * SDL_clamp(v, -1.0f, 1.0f)); };
        SDL_SetGamepadLED(state.gamepad, channel(lx), channel(ly), channel(-lx));
    }

    SDL_SetRenderDrawColor(state.renderer, Uint8(20 + 30 * std::fabs(lx)), 24, Uint8(28 + 30 * std::fabs(ly)), 255);
    SDL_RenderClear(state.renderer);

    std::vector<std::string> lines = {
        std::string("Anomaly platform probe (") + SDL_GetPlatform() + ")",
        std::string("Renderer: ") + SDL_GetRendererName(state.renderer),
        state.lua_report,
        state.openal_report,
        state.memory_report,
        "",
    };
    for (auto& line : DescribeGamepad(state))
        lines.push_back(std::move(line));

    int out_w = 0, out_h = 0;
    SDL_GetCurrentRenderOutputSize(state.renderer, &out_w, &out_h);
    const float scale = out_w >= 2000 ? 3.0f : 2.0f;
    SDL_SetRenderScale(state.renderer, scale, scale);
    SDL_SetRenderDrawColor(state.renderer, 235, 235, 235, 255);
    float y = 16.0f;
    for (const auto& line : lines)
    {
        SDL_RenderDebugText(state.renderer, 16.0f, y, line.c_str());
        y += SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 6.0f;
    }
    SDL_SetRenderScale(state.renderer, 1.0f, 1.0f);

    SDL_RenderPresent(state.renderer);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/)
{
    auto* state = static_cast<ProbeState*>(appstate);
    if (!state)
        return;
    if (state->gamepad)
        SDL_CloseGamepad(state->gamepad);
    delete state;
}
