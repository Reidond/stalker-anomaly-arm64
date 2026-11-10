// Item 1: WebGPU from a worker through emdawnwebgpu's webgpu.h, drawing to the transferred
// OffscreenCanvas. Everything here runs on the app thread (the pthread SDL's main callbacks run on).
#pragma once

namespace lab::gpu
{
void Init(const char* canvas_selector, int width, int height);
// Once per SDL_AppIterate. Renders the scene and advances the test sequence.
void Frame();
// Starts the blocked-worker presentation test (also run automatically once).
void RequestBlockTest(int block_ms);
bool Done();
} // namespace lab::gpu
