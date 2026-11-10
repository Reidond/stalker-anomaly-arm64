# Anomaly in the browser: porting plan

Target: STALKER Anomaly 1.5.3 playable in a desktop browser tab, built from this repo's engine as
WebAssembly with a WebGPU renderer, hosted on the owner's Cloudflare account. Written on 2026-11-10
and revised the same day after the six W0 spikes; the decision is go
([W0-RESULTS.md](W0-RESULTS.md)). The spikes built the engine core for wasm, the shader path, the
hosting and a browser test page. No engine frame has run in a browser yet.

Items marked **(verify)** come from memory or secondary sources, or were not measured, and must be
confirmed by the work that first touches them. Where a spike settled an item, its document is named.

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

- Emscripten 7.0.0, `wasm32`, pthreads, `-msimd128 -msse4.2`, `-fwasm-exceptions`, RTTI on, `-O3`.
  The CMake preset `web-wasm32` holds the flags (`WASM-BUILD.md`). The engine's SSE code compiles
  through Emscripten's own intrinsics headers. LTO is a switch, off by default: it shrank one tool
  by 44% and another by 6%, and building those two took longer than the whole default build.
- 32-bit memory, up to 4 GB. The heap has to hold 1.2 GB in play, up to 1.8 GB at a level load and
  about 2.5 GB if the allocator keeps what it frees, as a wasm heap does (`MEMORY.md`). A 2 GB
  build is not possible, and Memory64 is not needed. Growing a shared memory is slow, so the heap
  starts large; 2.5 GB is the proposal **(verify at W1, together with `-sGROWABLE_ARRAYBUFFERS`)**.
- The engine runs on a worker (`-sPROXY_TO_PTHREAD`), not the page's main thread. The main thread
  cannot block, cannot use synchronous file handles and cannot wait on a mutex; the engine does all
  three. The page keeps the DOM, input capture, WebHID and audio output. The canvas is handed to
  the worker as an `OffscreenCanvas` (`-sOFFSCREENCANVAS_SUPPORT`,
  `-sOFFSCREENCANVASES_TO_PTHREAD`), which works with emdawnwebgpu (`BROWSER-LAB.md`).
- A frame reaches the screen when the worker returns to its event loop. The engine returns from
  `SDL_AppIterate` once per frame and never calls `wgpuSurfacePresent`, which aborts on the web.
- Threads: main, the secondary thread, the level prefetcher, the audio mixer, an I/O thread that
  owns OPFS and the network, and a fixed pool of 4 that replaces GCD (`xrThreadPool_web`, built in
  S1b). All are created up front. The count (`PTHREAD_POOL_SIZE`, 12 in the spike) is set at W1
  from a table with one line per thread. Stacks are 2 MB and no larger, because the heap counts
  them in full. The freeze watchdog and the busy-wait frame limiter are compiled out.
- Allocator: mimalloc, and it is required. The engine assumes 16-byte aligned blocks; Emscripten's
  default allocator returns 8-byte ones and fails the core's own test.
- Threads need a cross-origin isolated page (COOP and COEP headers); see Hosting.
- Headless tests run under Node. `ctest` in the web preset runs each tool as wasm with direct disk
  access, so a module is tested for wasm before any browser is involved.

### Platform layer

The spike build takes a shortcut: on web, `Platform.h` defines `XR_PLATFORM_APPLE` as well as
`XR_PLATFORM_WEB` and `XR_PLATFORM_POSIX`, because the code uses the Apple macro to mean "the
non-Windows path" (122 uses in 36 files of xrCore, 99 more files in the other modules). W1 undoes
that, in this order:

1. Rename the macro to `XR_PLATFORM_POSIX` wherever it means POSIX. Uses that call an Apple API
   keep it and get a web branch. This is mechanical and the Apple build alone checks it.
2. Divide `PlatformApple.*` into POSIX-common, Apple and web files.
3. Remove the Apple define from the web branch of `Platform.h`.

Already in place from S1b: the thread pool, memory queries, clocks, a software CRC32C, the CPU
description, and portable branches where the code fell through to MSVC x87 assembly. There is no
crash handler on web: a fault is a wasm trap, and the shell shows the error and offers the log.

- wasm has no flush-to-zero control. Measured: web equals native with flushing off, bit for bit,
  so values the native builds see as 0 stay tiny non-zero numbers and decaying loops never reach 0.
  This costs nothing on Apple silicon and can cost a lot on x86, which is not measured. Watch audio
  and particles on the first x86 run.
- wasm has only round-to-nearest, which is all the engine uses. Its math library is musl's, so the
  last bit of `sinf` and friends can differ from native.

The split touches shared files, so it lands as one PR before other work fans out.

### Scripts

- Lua 5.1.5 with LuaBitOp replaces LuaJIT, behind `ANOMALY_LUA=lua51`, which also works on the Mac.
  It is the copy in `src/3rd party/lua-5.1.5`, compiled as C++ so errors unwind as exceptions. It
  carries eight changes that reproduce LuaJIT behaviour the scripts depend on (`README.anomaly.md`
  there). Stock Lua 5.1 freezes every new game, so no other 5.1 is supported.
- On the Mac it costs no more Lua time per frame than LuaJIT (`LUA51.md`), because bytecode is only
  a third of that time. Its cost in wasm, and the cost of C++ exceptions there, is not measured
  **(verify at W1)**.
- Plain Lua cannot yield a coroutine across `pcall` or a C call. That does not affect the game: no
  stock script yields and no script thread runs. Only the sweep test driver would need a `copcall`.
- **A save loads only on the Lua VM that wrote it.** Every save holds `game.CTime` closures as
  bytecode. Web saves do not load on the Mac or the iPad, nor the other way round. Making saves
  portable (CTime stored as data, a version tag in the save) is a task of its own, outside the
  milestones below.
- `luabind`, `lfs` and `marshal` are Lua 5.1 API and need no change. Exceptions through luabind are
  untested in wasm **(verify at W1)**.
- A-Life and the collector keep running on the secondary thread.

### Renderer

A second implementation of the same D3D11 headers, written against the C `webgpu.h` API.

- **A null backend comes first.** W1 adds an implementation of the headers that keeps object state
  and draws nothing. With it the whole engine loads a level and ticks without a GPU: on the Mac,
  under Node and in a browser worker. The same work turns the two files with no Metal calls
  (`mtl_shader.mm`, `mtl_d3dx11.mm`: shader container, reflection, DDS parser) into shared C++.
- **The WebGPU backend also runs natively on the Mac through Dawn.** That gives frame-for-frame
  comparison against the Metal backend with the existing `tools/render_harness` scenes, without a
  browser or wasm in the loop. The harness has no image diff today; one is added.
- The Metal shim stays as it is while M2 is open.
- Device features, checked at start: `texture-compression-bc` and `clip-distances` (volumetric
  lights use it) are required. `texture-formats-tier1` and `rg11b10ufloat-renderable` are requested
  for render-target formats **(verify which formats R4 needs them for)**. All four are present in
  Chrome 157 on Apple GPUs **(verify on Windows and other GPUs)**.
- Differences the backend has to absorb:

| D3D11 as R4 uses it | WebGPU | Approach |
|---|---|---|
| Persistent mapped dynamic buffers (`DISCARD`, `NO_OVERWRITE`) | No CPU-visible GPU memory | CPU shadow rings in the wasm heap, one batched `writeBuffer` per ring before each submit. Measured: 1.0 ms of CPU for 4,000 draws; a `writeBuffer` per draw costs 2.4 to 3.1 times more |
| Textures filled from the CPU | The data passes through the heap | Submit uploads often, at most about 64 MB at a time, so a level load does not hold every decoded texture at once |
| Rasteriser, depth and blend state set per draw | Part of the pipeline | Wider pipeline key; pipelines pre-warmed with `createRenderPipelineAsync` from a recorded list. Creating them synchronously stalls the first draw (24 to 357 ms for 64 small pipelines) |
| Slot-based binding, separate register spaces per stage | Bind groups with explicit layouts, one space per pipeline | Group 0 for the vertex stage, group 1 for the pixel stage; binding is the register number plus a base per class (`b` 0, `t` 16, `s` 48, `u` 64). Layouts come from the pack's reflection, never `layout: "auto"`. Bind groups are cached by bound resources |
| Occlusion query read back in the same frame (`occq_get` spins up to 500 ms) | The result is readable one frame later and says only zero or non-zero | Engine uses the latest finished result and never waits. Callers that need a pixel count (sun flares fade by visible fraction) get a small pass that counts samples into a storage buffer |
| `Map(READ)` on staging textures (screenshots, save thumbnails) | Asynchronous only | Deliver the capture a few frames later |
| Border sampler address mode | Does not exist | Only the sun's far cascade uses it: a bounds test in the two `accum_sun_far*` shaders |
| Depth textures sampled as float with any sampler | No linear filtering on depth | No stock shader filters depth. The shadow map binds as `depth` with a comparison sampler, or as `unfilterable-float` with a non-filtering sampler, chosen from the reflection |
| `R8G8B8A8_SNORM`, `R11G11B10_FLOAT`, `R16_FLOAT` storage, `A8_UNORM` | Optional features or missing | The features above; substitute formats where they are absent |
| Fragment inputs the vertex shader does not write, or writes with another width (patched in MSL text today) | Validation error; the component count must match exactly | A patched pixel variant per (vertex, pixel) pair, built into the shader pack. Depending on the profile, up to 9 pairs read unwritten inputs and up to 4 differ in width |
| BC1 to BC7 textures | `texture-compression-bc`, desktop only | Required feature |
| MetalFX upscaling | None | The existing bilinear and sharpen pass |
| MSAA 2x/8x, wireframe, HDAO compute, 3D fluid, tessellation | Missing or restricted | Locked off in the web graphics profile. HDAO would also need 1,024 invocations per workgroup and `r16float` storage |

- Loading screens are drawn as a page overlay fed by progress messages. A blocked worker presents
  nothing, while the page's own animation keeps its frame rate (both measured in S4b).
- The web graphics profile starts from the iPad's memory settings: `snd_cache_size 32` (a 64 MB
  block, not 512 MB), `r__no_ram_textures` on and locked (off keeps a CPU copy of every texture,
  1.0 GB of heap) and no MSAA. `texture_lod` is chosen by GPU memory: textures, render targets and
  geometry took 2.7 to 3.4 GB on the GPU at level 1 across four levels under the iPad profile, and
  level 2 took about 0.7 GB less in the Cordon.

### Shaders

- With no addons, shaders are translated ahead of time on the Mac and shipped as a pack: HLSL →
  vkd3d → DXBC → SPIR-V → patch → Tint → WGSL, plus the reflection data, keyed like the existing
  cache. The browser runs neither vkd3d nor Tint.
- Measured in S2 (`SHADERS-WGSL.md`): 99.5% of all jobs give valid WGSL, and so do 251 of the 253
  shaders a default run requests. The other two are 3D fluid. Three SPIR-V rewrites make that
  possible: `OpIsNan` and `OpIsInf` become bit tests, non-finite constants become run-time values,
  and the HDAO storage texture gets its format. Naga and DXC are not needed.
- Tint is pinned to the Dawn revision of the Chrome stable release the build targets. A Chrome
  update means a new pin and a rerun of the sweep, which takes about ten minutes.
- The pack also holds the patched pixel variants for shader pairs that would not link (see the
  table above), found per graphics profile. A pair the pack lacks is refused and logged.
- 105 jobs take derivatives in non-uniform control flow, which WGSL rejects by default; their WGSL
  carries `diagnostic(off, derivative_uniformity)`. D3D leaves the same cases undefined.
- A shader missing from the pack is logged and replaced by the stub the engine already uses.
- W1 builds the first version of the pack with reflection only, which is all the null backend
  needs. W2 adds the WGSL and the variants.
- Not done yet: no WGSL has run on a GPU. The NaN bit patterns vkd3d keeps in float registers and
  `discard` need a pixel check against Metal on each Dawn backend.

### Files and game data

- The five `mmap` sites become a windowed read interface with two backends. The POSIX one maps as
  today. The other reads into reused buffers and is what the web uses, because Emscripten's own
  `mmap` copies each window into the heap and wasm memory never shrinks. Save files, which are
  written through a shared mapping today, become a read and one write on close.
- Published data is cut into 4 MiB content-addressed chunks with one index file (`HOSTING.md`). The
  index carries every archive's decoded table of contents, so the engine registers all archives
  from it without opening one. For all of `db/` it is 4.47 MiB, about 1 MiB compressed; whether to
  split it per archive is decided at W1. File names in it are byte strings: two stock archives use
  Cyrillic names.
- The web backend runs on the I/O thread: it keeps chunks in the browser's private file system
  (OPFS) and fetches missing ones while the engine thread waits. Measured in S4b: a stored chunk
  arrives in under 1 ms, a missing one in 10 to 17 ms from a local server.
- **OPFS is a capped cache with eviction, not a full copy.** Chrome offered 4.8 to 13.9 GiB in the
  profiles measured, less than the data. The cache holds the boot set, the current level and the
  parts of the shared archives it touches. Chunks are not also cached in the heap.
- The stock data is 67 archives under `db/`, 15.97 GiB: levels 6.9 GB, textures 7.0 GB, meshes
  1.4 GB, sounds 1.0 GB, configs 24 MB. All of it is published. A few small loose files
  (`fsgame.ltx`, `gamedata/`) and the repo's own `gamedata` overlay are not yet; they join the boot
  set at W1. Levels are separate archives, so a player needs the boot set, one level, and the parts
  of the shared archives that level touches.
- Per-level prefetch lists come from read traces recorded on the native build. Everything on a list
  is downloaded on the loading screen, so play does not stall on the network.
- Saves, `user.ltx`, logs and caches live in OPFS. The page asks for persistent storage, which
  Chrome did not grant to a fresh page, and offers save export and import, since clearing site data
  deletes them.

### Audio and video

- OpenAL Soft is kept for its software EFX reverb and HRTF, which the engine requires and
  Emscripten's built-in OpenAL lacks. The pinned 1.25.2 builds for wasm with a one-line patch and
  mixes HRTF and reverb in real time (`BROWSER-LAB.md`).
- It does not output through SDL3, whose Emscripten backend would run the mixer on the page's main
  thread. OpenAL Soft renders through its loopback device on a worker thread into a shared ring
  buffer, and an AudioWorklet plays the ring.
- Audio starts on the Play click, as browsers require a user gesture.
- Ogg, Vorbis and Theora are plain C and build as they are.

### Input

- **Mouse and keyboard go through a page bridge.** Page listeners write fixed-size events into a
  ring in the shared memory and the worker drains it each frame. The page cancels browser keys,
  requests pointer lock with unadjusted movement, puts the stage (canvas plus overlays) in
  fullscreen and holds Keyboard Lock so the game receives Esc there. It also sees the browser drop
  pointer lock or fullscreen, which opens the game menu. Latency matches SDL3's path (6 to 10 ms
  median in S4b). Esc with a real keyboard is not checked yet **(verify)**.
- SDL3's own Emscripten input is not registered. From a worker it cannot cancel a browser key, it
  gets no event when pointer lock is lost, and it runs SDL code on the page thread.
- **DualSense:** the page reads input reports into shared memory and sends output reports; a new
  `WebHidPadSource` fills `PadSnapshot` and implements `IFeedbackSink` on top. The report parsing,
  calibration, trigger effects and shared layout exist as lab code (`web/lab/page/dualsense.js`,
  `web/lab/src/pad_shared.h`) and pass their hardware-free checks. **Nothing has been tried on a
  real pad.** `xr_input_sdl.cpp:113` holds `SdlPadSource` concretely and needs a small seam.
- **Other pads and other browsers:** SDL3 over the Gamepad API, with sticks, buttons and basic
  rumble **(verify from a worker)**.

### Web shell

Replaces the Apple shell and the SwiftUI launcher with a page:

- capability check (WebGPU features, shared memory, OPFS, storage space) with clear failure text;
- Play button, download progress, Quality or Performance mode, controller pairing, fullscreen;
- error screen with log download.

A hidden tab stops the worker's frame callbacks, so the game pauses on tab hide by itself; threads
that are not driven by frames keep running.

It follows the launcher's visual direction (Anomaly theme, `better-ui` and `emil-design-eng` skills).

### Hosting on Cloudflare

Deployed in S4a (`HOSTING.md`):

- One Worker serves everything from one origin, so the isolation headers apply and no CORS setup is
  needed. Static assets hold the page (files are capped at 25 MiB each). A private R2 bucket holds
  the data chunks, and later the wasm binary and the shader pack.
- The whole hostname sits behind a Cloudflare Access application with an allow-list of emails. The
  bucket has no public access, and the `workers.dev` and preview addresses are switched off so
  nothing bypasses Access. Access was in place before any data was uploaded.
- Chunks are immutable and the Worker stores them in the edge cache. Whether repeated reads hit
  that cache is not confirmed; W1 adds a header that shows it.
- Publishing all of `db/` took 31.5 minutes with 8 parallel `cf r2 objects put`; a rerun uploads
  only chunks that are missing. `cf` accepts at most 300 MB per object, which chunking avoids.
  Chunks no index refers to are not pruned yet.
- Cost: about $0.10 a month for the storage above the free 10 GB, and egress is free. Requests are
  the limit to watch: every request runs the Worker, a full download is about 4,100 of them, and a
  free Workers plan allows 100,000 a day. If that binds: a paid plan, larger chunks, or Cloudflare's
  Workers Cache in front of the Worker.
- The `cf` CLI is a beta, pinned at 1.0.0-beta.28. Its `deploy` ignores the Worker's configuration,
  so the Worker is deployed with `wrangler`; everything else goes through `cf`.
- The address, account and allow-list are deployment settings and stay out of the repo.

### Bring your own copy

The data source is a loader setting. A deployment without a data bucket shows three steps instead:
open the ModDB page, download the one archive, drop it on the page. It unpacks into OPFS with a
progress bar, verifies files against the existing game-data manifest, and resumes after a reload.
The storage quota above limits this too.

## Risks, largest first

1. **The engine has not run in a browser.** Only xrCore and five modules run as wasm, at 1.1 to 1.8
   times native wall time (2.9 for physics). The frame cost of the whole engine, Lua 5.1 and C++
   exceptions in wasm, and the binary size are unmeasured. Measured in W1.
2. **Machines not tested.** Every measurement comes from one Apple-silicon Mac. x86 (where
   denormals cost time), Windows, Edge and GPUs that are not Apple's have not been run.
3. **GPU memory.** 2.7 to 3.4 GB at texture level 1. The graphics profile must fit the device.
4. **Asynchronous GPU results.** Occlusion is boolean and one frame late; readbacks are
   asynchronous. Flares and light visibility need work to look the same.
5. **Storage quota.** The browser may offer less than a level set needs, and does not grant
   persistence to a fresh page. The cache must evict and re-fetch.
6. **Renderer defects inherited from M2,** plus the x86-only undefined behaviour class
   (`RENDER-QA.md`), which a third architecture may expose again.
7. **Saves are tied to the Lua VM,** so nothing moves between the web and the native builds.
8. **Request limit of the hosting plan,** about 24 full downloads a day on a free plan.
9. **DualSense over WebHID is untested on hardware.**

Measured and now low: shader translation (S2), the heap size (S3), Lua speed on native code and
the coroutine limit (S1a), worker rendering and storage speed (S4b).

## Milestones

| | Goal | Done when |
|---|---|---|
| W0 | Six spikes (below) | Done on 2026-11-10: go ([W0-RESULTS.md](W0-RESULTS.md)) |
| W1 | Platform split; every module links for wasm with a null renderer | The engine boots in a browser worker against chunked data and ticks A-Life |
| W2 | WebGPU backend on native Dawn; shader pack with WGSL | The Cordon renders on the Mac through Dawn; diff against Metal is within threshold |
| W3 | Browser integration: renderer, audio through the AudioWorklet, mouse and keyboard through the bridge | A new game is playable in Chrome, on the Mac and on an x86 Windows machine |
| W4 | DualSense over WebHID; web shell; saves export | Full controller feature set works over USB and Bluetooth |
| W5 | Performance: prefetch lists, pipeline pre-warm, profiling | Frame time and load time targets agreed after W3 are met |
| W6 | Deploy | Published on Cloudflare behind Access, with the full data set |

### W1 pieces

Each piece is one pull request. "After" names the pieces that must be merged into its base first.
Every piece keeps the Apple build and its tests unchanged.

| | Piece | After | Done when |
|---|---|---|---|
| W1a | **Platform split**: the macro rename and the `PlatformApple.*` division described under Platform layer | | Apple presets build and pass as before and a new game still starts on the Mac; the web preset passes its 8 tests with `XR_PLATFORM_APPLE` undefined on web |
| W1b | **File windows**: the windowed read interface, its POSIX and buffer backends, the save-file write path | W1a | The five `mmap` sites use it. Apple level-load time is unchanged. Under Node the largest level entry is read through the engine and the heap growth is reported |
| W1c | **Lua for wasm**: the vendored Lua 5.1, luabind and lua-extensions in the web preset | W1a | The three Lua smoke tools pass under Node. The `jit_lab` workloads and the cost of a caught error are reported against native |
| W1d | **Null renderer backend** and a headless run: the shared shader-container and DDS code, the null implementation of the shim headers, an entry point with no window and no audio device | W1a | On the Mac, without Metal: a new game in the Cordon loads, ticks for a minute, saves and loads again. The Metal app is unchanged |
| W1e | **xrSound and OpenAL Soft for wasm**, with the loopback device | W1a | A smoke tool under Node renders a positioned source with HRTF and reverb into a buffer and checks it |
| W1f | **xrEngine and the small modules for wasm** (xrNetServer, xrXMLParser) | W1a | They compile for wasm as libraries |
| W1g | **xrServerEntities and xrGame for wasm**, with the luabind bindings | W1a, W1c | They compile for wasm as libraries |
| W1h | **Chunk store**: the index reader, the I/O thread, the capped OPFS cache with eviction, as a backend of W1b's interface | W1b | In headless Chrome against a local Worker: all archives register from the index, a config file and a large level entry read correctly, and the cache stays under its cap while evicting |
| W1i | **Shader pack, first version**: a Mac tool writes reflection for the default request set; the null backend loads it instead of compiling | W1d | The headless run on the Mac passes with shader compilation switched off |
| W1j | **The whole engine under Node**: R4 on the null backend for wasm, the final link, the headless run | W1b to W1g, W1i | A new game in the Cordon loads from the real archives, ticks A-Life, saves and loads. Reported: frame CPU against the native headless run, heap at the Cordon and at the largest level load, binary size, the thread table |
| W1k | **Browser boot**: a minimal page (capability check, progress overlay, log), the engine on a worker with the chunk store, the cache-hit header in the Worker | W1h, W1j | In Chrome the engine reaches the Cordon and ticks A-Life, from a local Worker and from the deployed origin. Reported: time and requests for a first and a second boot, heap, and the initial-memory choice |

They run in five rounds: W1a alone; then W1b, W1c, W1d and W1e; then W1f, W1g, W1h and W1i; then
W1j; then W1k. W2 does not depend on the wasm work and can start once W1d has landed.

### W0 spikes

All six ran in parallel on different files.

| | Question | Result |
|---|---|---|
| S1a | How slow is plain Lua, and does the game run on it? | It runs on the vendored Lua 5.1 and costs no more per frame than LuaJIT on the Mac. `LUA51.md` |
| S1b | Does the core build for wasm? | Yes: xrCore and five modules pass their tests under Node. `WASM-BUILD.md` |
| S2 | Do the shaders reach WGSL? | Yes: 99.5% through Tint with three rewrites. `SHADERS-WGSL.md` |
| S3 | Does the heap fit in 4 GB? | Yes: 1.2 GB in play, 1.8 GB at a load, 2.5 GB pessimistic. `MEMORY.md` |
| S4a | Does the hosting work? | Yes: deployed behind Access, all of `db/` published. `HOSTING.md` |
| S4b | Does the browser environment work? | Yes, with a page bridge for input; DualSense is untested on hardware. `BROWSER-LAB.md` |

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
