# Anomaly in the browser: porting plan

Target: STALKER Anomaly 1.5.3 playable in a desktop browser tab, built from this repo's engine as
WebAssembly with a WebGPU renderer, hosted on the owner's Cloudflare account. Draft of 2026-11-10;
nothing here is built yet.

Items marked **(verify)** come from memory or secondary sources and must be confirmed by the spike
that first touches them.

## Decisions already made

| Topic | Decision |
|---|---|
| Addons | None on web. No mod manager, no addon shaders or scripts. |
| Renderer | Port it as it stands today. The owner compares against native by hand and reports differences. Aim for maximum performance and correctness. |
| Input | Full DualSense support (gyro, touchpad, light bar, adaptive triggers, rumble) plus classic mouse and keyboard. |
| Game data | Served from the deployment's own storage, which the operator fills from their copy of the game files. Players assemble nothing by hand. The repo still holds source code only. |
| Access | Invite-only, through Cloudflare Access. |
| Tooling | Cloudflare work goes through the `cf` CLI. |
| Browsers | Desktop Chrome and Edge first. They are the only ones with WebHID, which the DualSense features need. Firefox and Safari are a later, reduced target; phones are out of scope. |

## What carries over from the iPad port

- **One frame per call.** The app already uses SDL3 main callbacks (`src/Platform/App/main.cpp:61`,
  `SDL_AppIterate` → `xr_engine::Frame()`), which is the shape a browser needs.
- **A narrow file seam.** All reads funnel through `CLocatorAPI::r_open_impl`; archives are read
  with `pread` plus five bounded `mmap` call sites (`LocatorAPI.cpp:1522`, `stream_reader.cpp:50`,
  `FS.cpp:577,599`), never a whole archive.
- **Single-player needs no sockets.** Client and server talk through direct calls (`NETSERVER.md`).
- **A backend-neutral D3D11 surface.** The shim's public headers (about 2,350 lines of plain C++)
  hold the whole object model. R4 uses a small feature set: 4 render targets, 16-bit indices, no
  instancing, no texture arrays, geometry and tessellation shaders already switched off.
- **Portable input core.** `src/Platform/GameInput` consumes a plain `PadSnapshot` and drives an
  `IFeedbackSink`; only `SdlPadSource` is tied to SDL.
- **A finite shader set.** Shader macros are a pure function of the graphics settings, and
  `tools/r4_shader_warmup` already enumerates the 334 shaders a default run requests.
- **Lua cost is known.** On the iPad the LuaJIT interpreter takes 1.0 to 3.6 ms a frame; 52% of
  that is C functions and 26% the collector, only 21% bytecode (`LUA-PERF.md`).

## Architecture

### Toolchain and process model

- Emscripten, `wasm32`, pthreads, `-msimd128 -msse4.2`, `-fwasm-exceptions`, RTTI on, `-O3` with LTO.
  The engine's SSE code compiles through Emscripten's own intrinsics headers, so the `sse2neon`
  forwarding headers in `src/Common/compat` become conditional.
- 32-bit memory first. Memory64 ships in Chrome and Firefox but not Safari, and costs 10% to over
  100% because every access gets a bounds check. It stays a build switch in case the heap does not fit.
- The engine runs on a worker, not the page's main thread. The main thread cannot block, cannot use
  synchronous file handles and cannot wait on a mutex; the engine does all three. The page keeps the
  DOM, input capture and WebHID. The canvas is handed to the worker as an `OffscreenCanvas` **(verify
  with emdawnwebgpu)**.
- Threads: main, the secondary thread, the level prefetcher, OpenAL's mixer, and a fixed pool that
  replaces GCD in `_thread_types.h`. All are created up front (`PTHREAD_POOL_SIZE`). Stacks are at
  least 2 MB, as on Apple. The freeze watchdog and the busy-wait frame limiter are compiled out.
- Allocator: mimalloc, since the default allocator takes a global lock.
- Threads need a cross-origin isolated page (COOP and COEP headers); see Hosting.

### Platform layer

The build has three hard stops today (`CMakeLists.txt:9`, `AnomalyPlatform.cmake:18`,
`Platform.h:30,40`) and non-Windows means Apple everywhere. The first change is a platform split:

- `XR_PLATFORM_WEB` and a `wasm32` architecture define; `PlatformApple.*` divided into POSIX-common,
  Apple and web parts.
- Web replacements for GCD, `malloc_size`, the mach and sysctl calls, the crash handler (signals and
  `backtrace` do not exist) and the arm64 inline assembly (`_math.h:38`, `_math.cpp:321`, `crc32.cpp`).
- Architecture `#if`s that fall through to MSVC x87 code when neither x64 nor ARM64 is defined
  (`_math.cpp:25`, `_bitwise.h:19`, `xrMemory_pso_*.cpp`, `crc32.cpp`) get a portable branch.
- wasm has no flush-to-zero control, so denormal behaviour differs from Windows and Apple. Watch
  audio and particles for slowdowns.

This split touches shared files, so it lands as one PR before other work fans out.

### Scripts

- Plain Lua 5.1.5 with LuaBitOp replaces LuaJIT, behind a build option that also works on the Mac.
  Lua is compiled as C++ so errors unwind through wasm exceptions.
- Known LuaJIT dependencies to patch: `luaJIT_setmode` (`script_storage.cpp:472`), the profiler
  (`script_perf.cpp`), the bytecode cache header check, `gamedata/scripts/global.lua:2`
  (`jit.opt.start`, unguarded) and `axr_main.script:21` (`jit.os`).
- `luabind`, `lfs`, `marshal` and the environment handling are Lua 5.1 API and need no change.
- Plain Lua 5.1 cannot yield a coroutine across `pcall` or a C call; LuaJIT can. Scripts that rely
  on it fail at runtime, so coroutine use is audited in spike S1a **(verify)**. Wrapping `pcall` in
  coroutines (coxpcall) is the fallback; native stack switching is not available in wasm.
- A-Life and the collector keep running on the secondary thread.

### Renderer

A second implementation of the same D3D11 headers, written against the C `webgpu.h` API.

- **It also runs natively on the Mac through Dawn.** That gives frame-for-frame comparison against
  the Metal backend with the existing `tools/render_harness` scenes, without a browser or wasm in
  the loop. The harness has no image diff today; one is added.
- The Metal shim stays as it is while M2 is open. Only the two files with no Metal calls
  (`mtl_shader.mm`, `mtl_d3dx11.mm`: shader container, reflection, DDS parser) become shared C++.
- Differences the backend has to absorb:

| D3D11 as R4 uses it | WebGPU | Approach |
|---|---|---|
| Persistent mapped dynamic buffers (`DISCARD`, `NO_OVERWRITE`) | No CPU-visible GPU memory | CPU shadow rings in the wasm heap, one batched `writeBuffer` per ring before each submit |
| Rasteriser, depth and blend state set per draw | Part of the pipeline | Wider pipeline key; pipelines pre-warmed with `createRenderPipelineAsync` from a recorded list |
| Slot-based binding | Bind groups with explicit layouts | Layouts from the existing reflection; bind groups cached by bound resources |
| Occlusion query read back in the same frame (`occq_get` spins up to 500 ms) | Results arrive asynchronously, and may be yes/no rather than a count **(verify)** | Engine uses the latest finished result and never waits; audit callers that need a pixel count (flares) |
| `Map(READ)` on staging textures (screenshots, save thumbnails) | Asynchronous only | Deliver the capture a few frames later |
| Border sampler address mode | Does not exist | Emulate in the shader where it matters (shadow maps) |
| Depth textures sampled as float with any sampler | No linear filtering on depth | Nearest sampler or comparison sampling |
| `R8G8B8A8_SNORM`, `R11G11B10_FLOAT`, `R16_FLOAT` storage, `A8_UNORM` | Optional features or missing | Require `texture-formats-tier1` and friends **(verify)**, else substitute formats |
| Fragment inputs the vertex shader does not write (patched in MSL text today) | Validation error | Same patch on SPIR-V or WGSL |
| BC1 to BC7 textures | `texture-compression-bc`, desktop only | Required feature; checked at start |
| MetalFX upscaling | None | The existing bilinear and sharpen pass |
| MSAA 2x/8x, wireframe, HDAO compute, 3D fluid, tessellation | Missing or restricted | Locked off in the web graphics profile |

- Loading screens are drawn as a page overlay fed by progress messages, because a blocked worker
  cannot present frames.

### Shaders

- With no addons, shaders are translated ahead of time on the Mac and shipped as a pack: HLSL →
  vkd3d → SPIR-V → WGSL, plus the reflection data, keyed like the existing cache. The browser never
  runs vkd3d (which also sidesteps its signal-guard code and the open heap-corruption bug).
- SPIR-V → WGSL uses Tint, which comes with Dawn. Whether Tint accepts vkd3d's SPIR-V for all 253
  runtime shaders is the largest renderer unknown, and the first thing to measure. Fallbacks: Naga,
  or DXC as the front end.
- A shader missing from the pack is logged and replaced by the stub the engine already uses.

### Files and game data

- The five `mmap` sites become a windowed read interface with a POSIX backend and a web backend.
- Published data is cut into fixed-size, content-addressed chunks (4 MiB to start) with one index
  file that includes every archive's table of contents, so start-up needs one request for it.
- The web backend keeps chunks in the browser's private file system (OPFS) and fetches missing ones
  on an I/O thread while the engine thread waits.
- The stock data is 96 files and 16 GB: levels 6.9 GB, textures 7.0 GB, meshes 1.4 GB, sounds
  1.0 GB, configs 24 MB. Levels are separate archives, so a player needs the boot set, one level,
  and the parts of the shared archives that level touches.
- Per-level prefetch lists come from read traces recorded on the native build. Everything on a list
  is downloaded on the loading screen, so play does not stall on the network.
- Saves, `user.ltx`, logs and caches live in OPFS. The page asks for persistent storage and offers
  save export and import, since clearing site data deletes them.

### Audio and video

- OpenAL Soft is kept for its software EFX reverb and HRTF, which the engine requires and
  Emscripten's built-in OpenAL lacks. It outputs through its SDL3 backend to Web Audio **(verify the
  pinned OpenAL Soft has that backend)**.
- Audio starts on the Play click, as browsers require a user gesture.
- Ogg, Vorbis and Theora are plain C and build as they are.

### Input

- **Mouse and keyboard:** SDL3's relative mouse mode maps to pointer lock. In fullscreen, keyboard
  lock lets the game receive Esc. Windowed, the browser takes Esc and drops pointer lock, so losing
  pointer lock opens the game menu.
- **DualSense:** a new `WebHidPadSource` fills `PadSnapshot` and implements `IFeedbackSink`. The page
  reads input reports into shared memory and sends output reports. This is new code: the repo has no
  raw-report path today (adaptive triggers go through Apple's GameController, the rest through
  SDL). It covers USB and Bluetooth report layouts, sensor calibration and the trigger effect
  encodings. `xr_input_sdl.cpp:113` holds `SdlPadSource` concretely and needs a small seam.
- **Other pads and other browsers:** SDL3 over the Gamepad API, with sticks, buttons and basic rumble.

### Web shell

Replaces the Apple shell and the SwiftUI launcher with a page:

- capability check (WebGPU features, shared memory, OPFS, storage space) with clear failure text;
- Play button, download progress, Quality or Performance mode, controller pairing, fullscreen;
- error screen with log download; pause on tab hide.

It follows the launcher's visual direction (Anomaly theme, `better-ui` and `emil-design-eng` skills).

### Hosting on Cloudflare

- One Worker serves everything from one origin, so the isolation headers apply and no CORS setup is
  needed. Static assets hold the page (files are capped at 25 MiB each). An R2 bucket holds the
  wasm binary, the shader pack and the data chunks.
- Chunks are immutable and cached at the edge and in the browser.
- `cf r2 objects put` accepts at most 300 MB and 27 of the 96 files are larger, which the chunking
  already solves. A publish script uploads in parallel and skips chunks that exist.
- Cost: 16 GB in R2 is about 6 GB over the free 10 GB, roughly $0.10 a month; egress is free
  **(verify on the pricing page)**.
- The whole hostname sits behind a Cloudflare Access application with an allow-list of emails. The
  bucket has no public access, and the `workers.dev` and preview addresses are switched off so
  nothing bypasses Access. Access is in place before any data is uploaded.
- The address, account and allow-list are deployment settings and stay out of the repo.
- The `cf` CLI is a beta; pin one version. `wrangler` is the fallback.

### Bring your own copy

The data source is a loader setting. A deployment without a data bucket shows three steps instead:
open the ModDB page, download the one archive, drop it on the page. It unpacks into OPFS with a
progress bar, verifies files against the existing game-data manifest, and resumes after a reload.

## Risks, largest first

1. **Shader translation to WGSL.** Unknown pass rate through Tint. Measured in spike S2.
2. **Lua speed.** Plain Lua is slower than LuaJIT's interpreter on bytecode and collection, which
   are about half the measured script time. Scripts may also depend on yielding across `pcall`.
   Measured in spike S1a.
3. **Memory.** The docs put non-GPU memory at about 1.5 to 2 GB, under the 4 GB cap, but the load
   peak is not broken down. Measured in spike S3.
4. **Asynchronous GPU results.** Occlusion queries and readbacks change timing; flares and light
   visibility may behave differently.
5. **Worker rendering and input.** `OffscreenCanvas` WebGPU, SDL3 event proxying and pointer lock
   from a worker are each documented but not proven together. Proven in spike S4b.
6. **Binary size.** No build exists to measure. It decides start-up time.
7. **Renderer defects inherited from M2,** plus the x86-only undefined behaviour class
   (`RENDER-QA.md`), which a third architecture may expose again.

## Milestones

| | Goal | Done when |
|---|---|---|
| W0 | Six spikes (below) | Each reports a number or a working demo; go or no-go on the risks above |
| W1 | Platform split; every module links for wasm with a null renderer | The engine boots in a browser worker against chunked data and ticks A-Life |
| W2 | WebGPU backend on native Dawn | The Cordon renders on the Mac through Dawn; diff against Metal is within threshold |
| W3 | Browser integration: renderer, audio, mouse and keyboard | A new game is playable in Chrome |
| W4 | DualSense over WebHID; web shell; saves export | Full controller feature set works over USB and Bluetooth |
| W5 | Performance: prefetch lists, pipeline pre-warm, profiling | Frame time and load time targets agreed after W3 are met |
| W6 | Deploy | Published on Cloudflare behind Access, with the full data set |

### W0 spikes

All six are independent and touch different files.

| | Question | Work |
|---|---|---|
| S1a | How slow is plain Lua, and does the game run on it? | Add the Lua 5.1 build option on the Mac, audit coroutine use, rerun the `LUA-PERF.md` scenes. |
| S1b | Does the core build for wasm? | Emscripten preset and platform branch; xrCore and the headless smoke tests built for wasm and run under Node. |
| S2 | Do the shaders reach WGSL? | Add a Tint back end to `tools/shader_batch`, run all profiles, validate the output, report the pass rate and failure classes. |
| S3 | Does the heap fit in 4 GB? | Record non-Metal heap on the Mac build in the menu, at load peak and in the Cordon, at the iPad preset. |
| S4a | Does the hosting work? | Worker plus R2 with isolation headers behind Access, and the chunk publisher, proven with a small sample of data. |
| S4b | Does the browser environment work? | A test page: WebGPU on a worker canvas from wasm with threads, synchronous OPFS, pointer and keyboard lock, DualSense over WebHID with adaptive triggers. |

## How the work lands

Every piece of work is its own pull request, stacked on this plan with `gh stack`. Pieces that run
in parallel are appended to the stack in the order they finish.

## References

- Memory64 status and cost: <https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html>, <https://bugs.webkit.org/show_bug.cgi?id=300538>
- WebGPU availability: <https://web.dev/blog/webgpu-supported-major-browsers>
- WebGPU in Emscripten (emdawnwebgpu): <https://emscripten.org/docs/porting/multimedia_and_graphics/WebGPU-support.html>
- Workers static asset limits: <https://developers.cloudflare.com/workers/platform/limits>
- R2 CORS: <https://developers.cloudflare.com/r2/buckets/cors>
- WebHID: <https://developer.chrome.com/docs/capabilities/hid>
