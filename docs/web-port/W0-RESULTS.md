# W0 results: go

Decision of 2026-11-10 on the six spikes of [PLAN.md](PLAN.md). **Go: build the port as planned, as
32-bit WebAssembly with a WebGPU renderer.** No spike found a blocker. Three parts of the plan change
(input, audio, the data cache), and the spikes found risks the plan did not list (saves, storage
quota, request limits, GPU memory). `PLAN.md` is revised to match.

Each spike's document has the numbers, the method and its own list of what was not verified:
[LUA51.md](LUA51.md) (S1a), [WASM-BUILD.md](WASM-BUILD.md) (S1b), [SHADERS-WGSL.md](SHADERS-WGSL.md)
(S2), [MEMORY.md](MEMORY.md) (S3), [HOSTING.md](HOSTING.md) (S4a), [BROWSER-LAB.md](BROWSER-LAB.md)
(S4b).

## Answers

| | Question | Answer | Key numbers |
|---|---|---|---|
| S1a | Does the game run on plain Lua, and how slow is it? | Yes, on a patched Lua 5.1.5, and no slower per frame on the Mac | 0.74 to 2.04 ms of Lua a frame, against 0.86 to 2.01 for LuaJIT's interpreter and 1.05 to 1.72 with its JIT. The Lua heap is 25 to 30 % larger |
| S1b | Does the core build for wasm? | Yes | xrCore and five modules pass 8 of 8 tests under Node and read the real archives. Wall time is 1.1 to 1.8 times native, 2.9 for physics, on a shared machine |
| S2 | Do the shaders reach WGSL? | Yes, with three SPIR-V rewrites | 1478 of 1486 jobs (99.5 %) over four profiles. Of the 253 shaders a default run requests, 251 pass; the other two belong to 3D fluid, which is off on web |
| S3 | Does the heap fit in 4 GB? | Yes, and not in 2 GB | 1.2 GB in play, 1.4 to 1.8 GB at a level load, 2.5 GB if the allocator keeps what it frees |
| S4a | Does the hosting work? | Yes | Worker, private bucket and Access are deployed. All of `db/` is published: 67 archives, 15.97 GiB, 4,128 chunks, 31.5 minutes, verified |
| S4b | Does the browser environment work? | Yes for rendering, storage and audio. Input needs a page bridge. DualSense is not tried on a pad | 4,000 draws cost 1.0 ms of CPU a frame. OPFS reads 7.4 to 7.9 GiB/s. The storage quota was 10.0 GiB |

## The plan's risks after W0

| Risk in the plan | Now | Why |
|---|---|---|
| 1. Shader translation to WGSL | Low | Tint accepts everything the web build runs once `OpIsNan`, non-finite constants and the HDAO storage format are rewritten. No WGSL has run on a GPU yet |
| 2. Lua speed | Low on native code, open in wasm | Bytecode is a third of the game's Lua time; the rest is engine functions and the collector. No script yields across `pcall`. The interpreter and C++ exceptions are not measured in wasm |
| 3. Memory | Bounded | The heap leaves 1.6 GB of the 4 GB free in the pessimistic case. Memory64 is not needed |
| 4. Asynchronous GPU results | Confirmed | Occlusion results are zero or non-zero only and arrive one frame later. Sun flares, which fade by visible fraction, need another method |
| 5. Worker rendering and input | Low | WebGPU on a worker canvas works from wasm with threads. SDL3's input on a worker cannot cancel browser keys and gives no event when pointer lock is lost, so the page handles input |
| 6. Binary size | Open | The core is 0.6 MB. The whole engine is not built |
| 7. Renderer defects inherited from M2 | Unchanged | |

New risks:

- **The engine has not run in a browser.** Only xrCore and five modules run as wasm, under Node. The
  frame cost of the whole engine in wasm, on an x86 machine, on Windows, in Edge and on a GPU that
  is not Apple's is unknown.
- **GPU memory.** Textures, render targets and geometry take 2.7 to 3.4 GB on the GPU at texture
  level 1. The web graphics profile has to choose by device.
- **Storage quota.** Chrome offered 4.8 to 13.9 GiB of OPFS in the profiles measured, less than the
  16 GB of data. The browser copy becomes a capped cache, not a mirror.
- **Saves are tied to the Lua VM.** A save holds bytecode, so a web save does not load on the Mac or
  the iPad, and the other way round.
- **Request limit.** Every request runs the Worker. A full download is about 4,100 requests, and a
  free Workers plan allows 100,000 a day.

## Changes to the plan

All are made in `PLAN.md`:

1. **Input:** a page bridge (shared ring) carries keyboard, mouse, pointer lock, fullscreen and
   Keyboard Lock. SDL3's Emscripten input is not used.
2. **Audio:** OpenAL Soft renders through its loopback device on a worker thread and an AudioWorklet
   plays it. SDL3's audio backend would mix on the page's main thread.
3. **Data cache:** OPFS holds the boot set, the current level and what it touches, with eviction.
4. **Occlusion:** boolean results; a sample-count pass replaces pixel counts for flares.
5. **Allocator:** mimalloc is required, not an optimisation: the engine assumes 16-byte alignment.
6. **Platform split:** rename `XR_PLATFORM_APPLE` to `XR_PLATFORM_POSIX` where it means POSIX, then
   move code. The spike build defines both on web as a shortcut.
7. **Scripts:** only the vendored Lua 5.1 is supported; stock 5.1 freezes every new game. The
   coroutine item is closed.
8. **Shaders:** the pack also carries a patched pixel variant per shader pair that would not link,
   and explicit bind group layouts. The browser runs neither vkd3d nor Tint.
9. **Renderer:** a null backend comes first, so the engine can run without a GPU on the Mac, under
   Node and in a worker. W1 is cut into eleven pieces around it.
10. **Hosting:** the Worker is deployed with `wrangler`; the rest stays on `cf`. Edge-cache hits are
    not confirmed and get their own header.
11. **Web graphics profile:** the iPad's memory settings: a 64 MB sound cache and no CPU copies of
    textures, which would cost 1.0 GB of heap.

## Not known yet

| Question | Answered by |
|---|---|
| Lua 5.1 and C++ exceptions in wasm | W1c |
| Frame CPU, heap and binary size of the whole engine in wasm | W1j |
| mimalloc's retained memory at the largest level load | W1j |
| Initial heap size and the cost of growing it | W1j, W1k |
| Requests and time for a first boot from the deployed origin; edge-cache hits | W1k |
| Any WGSL on a GPU; NaN bit patterns and `discard` against Metal | W2 |
| Denormal cost and behaviour on x86 (audio, particles) | first run on an x86 machine, W3 |
| Windows, Edge, GPUs that are not Apple's | W3 |

## Needs the owner

1. **DualSense on a real pad**, USB then Bluetooth: the checklist in `BROWSER-LAB.md`, section 4.
   Everything about the pad is untested until then.
2. **Esc with a real keyboard:** steps 3 to 5 of "To run it in your own Chrome" in `BROWSER-LAB.md`.
3. **Storage quota in a normal Chrome profile:** step 6 of the same list.
4. **The hosting account's Workers plan,** for the daily request limit.
5. **The players' machines:** operating system, GPU and memory. They decide the web graphics profile
   and where the first x86 run happens.
