// Renderer statistics without any D3D vocabulary, for apps that only want numbers (mtl_d3d11.h pulls in the D3D types).
#pragma once
#include <cstdint>

namespace mtl_d3d11
{
struct RenderStats
{
    uint64_t frames = 0;         // Present calls
    uint64_t draws = 0;
    uint64_t renderPasses = 0;
    uint64_t commandBuffers = 0;
    double gpuSeconds = 0;       // sum of (GPUEndTime - GPUStartTime) over all command buffers
    double blockedSeconds = 0;   // CPU time spent waiting for the GPU (frame throttle, ring reuse, read-backs)
    uint64_t textureBytes = 0;   // live texture memory
    uint64_t bufferBytes = 0;    // live buffer memory
    uint64_t deviceAllocatedBytes = 0; // MTLDevice.currentAllocatedSize (textures, buffers, pipelines, ...)
};
// Totals since the device was created, for the most recent device; false before one exists.
bool GetRenderStats(RenderStats* out);
} // namespace mtl_d3d11
