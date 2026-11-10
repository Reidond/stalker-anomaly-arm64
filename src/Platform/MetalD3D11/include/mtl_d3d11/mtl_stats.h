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
    uint64_t renderTargetBytes = 0; // the part of it that is render target, depth or UAV
    uint64_t bufferBytes = 0;    // live buffer memory
    uint64_t deviceAllocatedBytes = 0; // MTLDevice.currentAllocatedSize (textures, buffers, pipelines, ...)
    // CPU-visible memory by class, for the browser-heap study (docs/web-port/MEMORY.md). Here all of it is shared storage outside the
    // malloc heap. "Heap" marks what a WebGPU backend would hold in the wasm heap instead; "GPU" what it would hold only on the GPU.
    uint64_t bufferStaticBytes = 0;        // IMMUTABLE and DEFAULT buffers: GPU after the first upload
    uint64_t bufferDynamicBytes = 0;       // DYNAMIC buffers, one backing each: heap (a CPU shadow written before each submit)
    uint64_t bufferStagingBytes = 0;       // STAGING buffers: heap
    uint64_t textureSharedStaticBytes = 0; // textures filled once with replaceRegion (IMMUTABLE, DEFAULT with data): GPU after the upload
    uint64_t textureDynamicBytes = 0;      // DYNAMIC textures, all three rename copies counted: GPU, plus one transient CPU shadow at Map
    uint64_t textureStagingBytes = 0;      // STAGING textures (screenshots, readbacks, no_ram_textures off): heap
    uint64_t texturePrivateBytes = 0;      // render targets, depth, UAV and DEFAULT without data (private storage): GPU
    uint64_t ringAllocatedBytes = 0;       // constant + upload rings, every in-flight slab
    uint64_t ringFramePeakBytes = 0;       // the most one frame took from both rings: what a CPU shadow ring needs
    uint64_t ringConstantPeakBytes = 0;    // ... from the constant ring alone
    uint64_t ringUploadPeakBytes = 0;      // ... from the upload ring alone
};
// Totals since the device was created, for the most recent device; false before one exists.
bool GetRenderStats(RenderStats* out);
} // namespace mtl_d3d11
