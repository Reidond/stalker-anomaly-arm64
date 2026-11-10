// Item 2: keyboard and mouse on a worker, two ways at once.
//  - SDL3's Emscripten backend, with the app on a pthread (what the engine does today).
//  - A thin bridge: page listeners write events into a ring in shared wasm memory and the worker
//    drains it each frame.
// Plus the DualSense shared-memory block (pad_shared.h) read on the worker.
#pragma once

#include <SDL3/SDL.h>

namespace lab::input
{
void Init(SDL_Window* window);
void OnSdlEvent(const SDL_Event& e);
void Frame();
// Page commands (see lab_main.cpp kCmd*).
void Command(int cmd, int arg);
} // namespace lab::input
