# Browser lab: spike S4b results

Spike S4b of the [web port plan](PLAN.md): do the browser features the port depends on work
together? The answer comes from one self-contained test page, [`web/lab`](../../web/lab/README.md),
with no engine code in it. Run on 2026-11-10.

## Verdicts

| # | Item | Verdict | Key numbers |
|---|---|---|---|
| 1 | Rendering from a worker | **pass** | wasm pthread draws to an `OffscreenCanvas` through emdawnwebgpu's `webgpu.h`. The 4-target G-buffer, BC1/BC3 and async pipelines all pass. Occlusion results are **zero/non-zero only** and readable the next frame. 4,000 draws with ring uploads cost **1.0 ms** CPU a frame. **No frame is presented while the worker is blocked.** |
| 2 | Input from a worker | **pass** with caveats | SDL3 on the worker gets keys, buttons, motion and wheel (6–11 ms median from the page event). Its relative mode gives pointer lock and its fullscreen works. It **cannot `preventDefault`** and **gets no event when the browser drops pointer lock**. The page bridge does all of it. Real-keyboard Esc behaviour is **unverified**. |
| 3 | Storage | **pass**, quota open | OPFS sync handle from a wasm thread takes the shared wasm memory directly: 1 GiB write **1.9–4.6 GiB/s**, read **7.4–7.9 GiB/s** (0.64 and 3.3 GiB/s in the Claude app). A blocked reader thread gets a missing 4 MiB chunk in **10–17 ms** (fetch + store, localhost), a stored one in **0.6–0.8 ms**. The quota measured **10.0 GiB** (fresh Chrome profile) and **4.8 GiB** (Claude app), below the 16 GB data set. |
| 4 | DualSense over WebHID | **unverified** (no hardware) | Module, shared-memory layout and checklist done. 20 hardware-free checks pass (USB and Bluetooth parsing, calibration, CRC32, all five trigger modes), and synthetic reports reach the worker's `PadSnapshot`. |
| 5 | OpenAL Soft (stretch) | **pass**, with a plan change | The pinned 1.25.2 has an SDL3 backend and builds with `emcmake`, but needs a one-line patch. HRTF and EFX reverb play at 48 kHz in real time. SDL3's backend **mixes on the page's main thread** (ScriptProcessorNode). |

Browsers: **Google Chrome 157.0.8103.50**, headless and driven over the DevTools protocol (three
runs, each on a fresh profile), and the Claude desktop app's built-in **Chromium 152.0.7977.167**
(one interactive run). Machine: Apple M2 Pro (12 cores, 32 GiB), macOS 27.0.1, 120 Hz built-in
display, Dawn's Metal backend. During all Chrome runs, **one game process was running on another
core**: other sessions' `anomaly` reproduction runs. Edge, Windows and non-Apple GPUs were not run.

## How to reproduce

```bash
web/lab/setup_emsdk.sh                       # Emscripten 7.0.0 into build/emsdk
web/lab/build.sh                             # -> build/web-lab/site (static files)
web/lab/serve.py                             # http://localhost:8642/ with COOP/COEP
node --test web/lab/test/dualsense.test.mjs  # DualSense module, no hardware
node web/lab/tools/headless_run.mjs --fresh  # whole page in headless Chrome; JSON in build/web-lab/
```

The headless runner uses its own profile in `build/web-lab/chrome-profile`, with extensions
switched off. Extensions that this Mac installs into every new profile open tabs of their own,
which hid the lab's tab and stopped it.

Toolchain: emsdk 7.0.0 (emsdk `1693ce7`), emdawnwebgpu `v20260423.175430` (Dawn `31e25af`,
fetched by `--use-port=emdawnwebgpu`), SDL 3.4.18 (submodule `829a65d`), OpenAL Soft 1.25.2
(submodule `b2c48f7`). The program is linked with `-pthread -sPROXY_TO_PTHREAD
-sOFFSCREENCANVAS_SUPPORT -sOFFSCREENCANVASES_TO_PTHREAD=#canvas`, 512 MB fixed memory, a pool of
6 threads with 2 MB stacks.

**To run it in your own Chrome** (needed for the unverified rows): build and serve as above and
open <http://localhost:8642/>. Items 1 and 3 fill in by themselves within about 15 s. Then:

1. Click the canvas, type, move the mouse and scroll over it.
2. Untick *Suppress browser keys* and press Tab once, then click the canvas and tick it again.
3. Press <kbd>L</kbd>, move the mouse, then press <kbd>Esc</kbd>. The *lock loss* row should
   fill in. Click the canvas (SDL takes the lock back), then press <kbd>U</kbd>.
4. Click *Fullscreen + Esc lock* and press <kbd>Esc</kbd> once. You should stay in fullscreen.
   Then hold <kbd>Esc</kbd> to leave.
5. Press <kbd>F</kbd>, then <kbd>Esc</kbd>.
6. Read *Storage quota estimate*, then *Copy as JSON*.
7. Work through the DualSense checklist on the page, over USB and then over Bluetooth.
8. Open `audio.html` and click *Start audio*. You should hear plucks circling your head with
   hangar reverb (use headphones for HRTF).

## 1. Rendering from a worker: pass

The SDL3 main callbacks run on a pthread (`-sPROXY_TO_PTHREAD`). Emscripten transfers `#canvas` to
that thread as an `OffscreenCanvas`, and emdawnwebgpu finds it by selector
(`wgpuInstanceCreateSurface` looks in `GL.offscreenCanvases`). Everything else is plain `webgpu.h`
on that thread. Adapter, device, buffer maps and async pipelines all arrive as
`WGPUCallbackMode_AllowSpontaneous` callbacks between frames, and nothing waits on the GPU.

Chrome columns give the range over the three runs; every value is a median unless marked.

| Check | Chrome 157 (headless, 3 runs) | Chromium 152 (Claude app) |
|---|---|---|
| Device on a worker, surface `bgra8unorm` 960×540 | pass | pass |
| 4 targets (`rgba16float` ×2, `rgba8unorm`, `rg11b10ufloat`) plus a composite; quadrant pixels read back from the canvas texture | pass, 0 WebGPU errors | pass, 0 errors |
| BC1 and BC3 decode (colour and alpha), 8×8 textures read with `textureLoad` | pass | pass |
| Occlusion values for 4096 / 256 / 0 / 1 passing samples | **1 / 1 / 0 / 1** | **1 / 1 / 0 / 1** |
| Occlusion result readable after (60 queries a run) | 1 frame (max 1), 1.2–1.3 ms | 1 frame (max 3), 1.0 ms |
| 64 `createRenderPipelineAsync` (distinct modules): all callbacks after | 13–53 ms (4–8 frames), frames kept rendering | 71 ms |
| Same count created synchronously: first draw waits | 24–32 ms | **357 ms** |
| Upload, CPU ms a frame (fill + encode + submit), one `writeBuffer` per ring: 1,000 / 2,000 / 4,000 / 8,000 draws | 0.29 / 0.53–0.55 / 1.00–1.09 / 1.97–2.04 (p95 ≤ 2.2) | 0.28 / 0.52 / 1.01 / 1.98 |
| Same, 4,000 draws with a `writeBuffer` per draw | 2.61–2.65 | 3.16 |
| Frame presented while the worker is blocked 1.6 s | **no**: the canvas kept the old frame for the whole block (197–198 samples); the first new frame showed 1.5–10 ms after the worker returned | **no** (same) |
| Page `requestAnimationFrame` during that block | 60 fps (headless runs at 60 Hz) | 120 fps |

Adapter (both browsers): all ten listed features are present: `texture-compression-bc`,
`texture-compression-bc-sliced-3d`, `texture-formats-tier1`, `texture-formats-tier2`,
`rg11b10ufloat-renderable`, `float32-filterable`, `clip-distances`, `depth-clip-control`,
`depth32float-stencil8` and `timestamp-query`. The adapter also has
`dual-source-blending`, `float32-blendable`, `shader-f16`, `subgroups` and the ETC2/ASTC formats.
Key limits: 8 colour attachments, 128 colour bytes per sample, 48 sampled textures and 16 samplers
per stage, 12 uniform buffers per stage, 64 KiB uniform bindings, 4 GiB storage bindings and
buffers, 256-byte uniform offset alignment, 30 vertex attributes, 28 inter-stage variables, 4 bind
groups. The full list is in the page's *data* cell.

The upload test mirrors the engine's dynamic buffers. Each draw has a 32-byte constant block in a
256-byte slot (dynamic offset) and 6 vertices. Both are filled into CPU rings and uploaded with one
`writeBuffer` per ring per frame. The cost is linear at about 0.24 µs a draw, almost all of it
`setBindGroup` + `draw` crossing into JS. Filling the rings is under 0.1 ms even at 8,000 draws.

What it means for the plan:

- The **(verify with emdawnwebgpu)** on `OffscreenCanvas` is resolved: it works with
  `-sOFFSCREENCANVAS_SUPPORT` and `-sOFFSCREENCANVASES_TO_PTHREAD`. emdawnwebgpu's
  `wgpuSurfacePresent` **aborts** on the web. A frame is presented when the worker returns to its
  event loop, so the engine must never call it and must return from `SDL_AppIterate` to show a
  frame.
- Occlusion queries on this backend are **boolean**. The plan's "(verify)" lands on the bad side.
  Whatever uses a pixel count (sun flares fade by visible fraction) needs another method, such as
  a small pass that counts samples into a storage buffer. The latency is good: the result is
  readable the next frame, so "use the latest finished result" costs one frame.
- Synchronous pipeline creation moves the compile to the first draw (24–357 ms for 64 small
  pipelines). Pre-warming with `createRenderPipelineAsync`, as planned, is needed.
- CPU-side shadow rings with one `writeBuffer` per ring per frame are cheap. A `writeBuffer` per
  draw costs 2.4–3.1× more. Per-draw `setBindGroup` + `draw` is the cost to watch: about 1 ms per
  4,000 draws on an M2 Pro.
- The plan's assumption that a blocked worker cannot present holds. The page's own animation keeps
  running at the display rate, so a page-drawn loading overlay works: the page shows one during
  the block test.
- A hidden tab stops `requestAnimationFrame` on the worker too, so SDL's main callbacks stop and
  the app thread stops iterating. This showed up when a headless run left the tab in the
  background. That is the plan's "pause on tab hide" for free. Threads that do not run on rAF,
  like the I/O thread, keep going.

## 2. Input from a worker: pass, the bridge is the better path

Both paths run at once on the same canvas. One is **SDL3** (the repo's 3.4.18, `emscripten` video
driver, window created on the worker). The other is a **bridge**: page listeners write fixed-size
events into a ring in the shared wasm memory, and the worker drains it each frame (single producer,
single consumer, no locks).

| Check | Chrome 157 (headless, CDP input, 3 runs) | Chromium 152 (Claude app) |
|---|---|---|
| Keys reach SDL3 on the worker | pass, 13 of 13; page event → SDL event 6.2–10.5 ms median, p95 15–26 | pass, 5 of 5; 3.8 ms median |
| Mouse buttons, motion and wheel reach SDL3 | pass | pass |
| Keys and mouse through the bridge | pass; page event → worker 5.9–10.3 ms median, 0 dropped | pass; 5.4 ms median |
| SDL3 relative mode (`SDL_SetWindowRelativeMouseMode` on the worker) → pointer lock | pass: locked about 12–20 ms after the L key; 20 relative motion events matching the page's `movementX/Y` exactly | refused by the browser (`pointerlockerror`; this embedded browser) |
| Pointer lock requested by the page in a click handler | pass, 3–9 ms | not tried (it would take the app's real mouse) |
| SDL3 `preventDefault` on browser keys (Tab, Space, arrows, F-keys, Backspace) | **0 of 2** | **0 of 2**: Tab moved focus to a button, Space clicked it |
| Bridge `preventDefault` | 2 of 2 | 3 of 3 |
| Fullscreen | pass: page on the `#stage` container, SDL3 on `#canvas` | not tried |
| `navigator.keyboard.lock(['Escape'])` | resolves; Esc reaches the page in fullscreen. **Unverified**: CDP Esc never leaves fullscreen even without the lock (control run), so it cannot show the lock's effect | not tried |
| Browser drops the pointer lock while SDL holds it | page gets `pointerlockchange`; **SDL3 sends no event** and keeps relative mode on; its next click re-grabs | – |

The drop was simulated with `document.exitPointerLock()` from outside SDL, because a CDP Escape
does not make the browser drop the lock (control run). To SDL that looks the same as the browser
dropping it.

How SDL3 behaves on a worker, from its source (`src/video/emscripten`) and the runs:

- Keyboard and wheel callbacks are registered from the worker, so Emscripten proxies them to the
  worker asynchronously. The page's handler has already returned by then, so **the result of
  SDL's handler cannot cancel the browser's default action**. That is what the 0 counts above show.
- Pointer events are handled the other way round: SDL installs plain listeners on the page's main
  thread that **call into SDL there** (`SDL_SendMouseButton`, `SDL_SendMouseMotion`), concurrently
  with the app thread. SDL's mouse state is not meant for two threads, and the page thread takes
  SDL's event-queue lock.
- `SDL_SetWindowRelativeMouseMode` and `SDL_SetWindowFullscreen` from the worker work as long as
  the page still has transient user activation (about 5 s after a key press or click). Emscripten
  7.0 checks `navigator.userActivation.isActive`. Outside that window the request is deferred to
  the next input event Emscripten handles.
- SDL fullscreen targets the canvas element, so anything the page draws over it (loading overlay,
  menus) is hidden. SDL also sets `document.title`.

What it means for the plan:

- Use the **bridge** for keyboard, mouse, pointer lock, fullscreen and focus. The page decides
  `preventDefault` synchronously, puts the stage container (canvas plus overlays) in fullscreen,
  requests `{unadjustedMovement: true}` and holds Keyboard Lock. It also sees lock and fullscreen
  losses. Latency equals SDL's, because both are bounded by the worker's next frame. The engine
  side is a small `SDL_Event` producer or a direct feed into `xr_input`. SDL3 can stay for windowing
  glue, but its Emscripten HTML5 input should not be registered (or should be ignored).
- The plan's line "losing pointer lock opens the game menu" needs the bridge: SDL3 gives the game
  no signal.
- Keyboard Lock and Esc: the API part works. Whether a real Esc stays in the page needs one manual
  check (step 4 above).

## 3. Storage: pass, but the quota needs a decision

An **I/O pthread** owns OPFS and the network. It never blocks: it waits for requests with
`Atomics.waitAsync`, fetches missing chunks, writes them with a `FileSystemSyncAccessHandle` and
copies them to the requester. A **reader pthread** stands in for the engine thread and blocks on a
futex until its chunk arrives. Afterwards C++ on the I/O thread writes and reads 1 GiB in 4 MiB
chunks through a sync handle, called through `EM_JS`.

| Check | Chrome 157 (headless, 3 runs) | Chromium 152 (Claude app) |
|---|---|---|
| Sync handle reads and writes views of the shared wasm memory (no copy) | yes | yes |
| 1 GiB write + flush, 4 MiB chunks | 1.87, 1.94, 4.59 GiB/s | 0.64 GiB/s |
| 1 GiB read back | 7.40, 7.70, 7.95 GiB/s | 3.28 GiB/s |
| Blocked reader, missing chunk (fetch from localhost + OPFS write + copy), median of 8 | 10.0–16.8 ms (fetch 7.8–11.1, write 2.1–3.3) | 10.7 ms (fetch 4.3, write 6.1) |
| Blocked reader, stored chunk (OPFS read), median of 8 | 0.59–0.83 ms | 1.3 ms |
| Checksums (CRC32 against the build's manifest) | 16 of 16 each run | 16 of 16 |
| `navigator.storage.persist()` | false (no prompt) | false |
| Quota (`estimate()`) | **10.0 GiB** (fresh profile; 13.9 GiB in an older automation profile) | **4.8 GiB** |

The read figures are from the OS cache, because the file was just written; a cold read from disk is
slower. The spread in write speed is the OS and SSD, not the browser.

What it means for the plan:

- The engine-thread-blocks, I/O-thread-fetches pattern works with plain pthreads, `Atomics.waitAsync`
  and futexes. A handle must be opened asynchronously by the thread that will use it, so the I/O
  thread opens them at start and keeps them.
- Throughput is far above what the game reads (16 GB total), so OPFS is not the bottleneck; the
  network is.
- **Quota is the open risk.** Every measurement is below the 16 GB data set, on a disk with
  358 GB free. Chrome documents up to 60% of the disk per origin, so a normal profile may differ:
  the headless runs use fresh automation profiles and the Claude app is an embedded browser.
  Check with step 6 in the owner's Chrome. Until then, plan the chunk store as a capped cache with
  eviction (boot set plus current level plus the shared parts it touches), not a full mirror.
  `persist()` is not granted without engagement; an installed web app usually gets it.

## 4. DualSense over WebHID: unverified (needs hardware)

[`web/lab/page/dualsense.js`](../../web/lab/page/dualsense.js) is a standalone ES module for the
page's main thread:

- Input: USB report `0x01` and Bluetooth `0x31` (CRC-checked), plus the reduced Bluetooth `0x01`
  report sent before full reports are on. Reading feature report `0x05` (IMU calibration) switches
  a Bluetooth pad to full reports. Sticks, buttons (DualSense Edge Fn and back buttons included,
  mapped like SDL), triggers, two touch points, gyro and accelerometer are converted to the units
  and axes of `gameinput::PadSnapshot`, with calibration applied. Battery and charging too.
- Output: USB report `0x02` and Bluetooth `0x31` with CRC32 over `A2 31 …`. It carries rumble,
  light bar, player LEDs, the Bluetooth light-bar release, and the adaptive trigger modes the port
  models: Off `0x05`, Feedback `0x21`, Weapon `0x25`, Vibration `0x26`, and Slope as multi-position
  feedback `0x21`. Positions and strengths map from `TriggerEffect`'s 0..1 values to the pad's 10
  zones and 8 levels, with the same clamping as `DualSenseTriggers.mm`. Vibration frequency maps
  0..1 to 0..40 Hz (`VIBRATION_MAX_HZ`); that scale is a guess to tune by feel.
- Shared memory: [`web/lab/src/pad_shared.h`](../../web/lab/src/pad_shared.h) defines `PadShared`
  (112 bytes) and `PadFeedbackShared` (76 bytes) with sequence locks, `static_assert`ed offsets,
  `ReadPad()` into a `PadSnapshot` and `WriteFeedback()` from `MotorState`, `Rgb` and
  `TriggerPair`. It includes the real `GameInput/Types.h` and `Feedback.h`. A later
  `WebHidPadSource`/`IFeedbackSink` is a thin wrapper around these.

Verified without hardware:

- `node --test web/lab/test/dualsense.test.mjs` passes 4/4. It runs the 20-check self-test the page
  also runs: synthetic USB and Bluetooth reports, calibration, a corrupted Bluetooth CRC, the
  reduced report, the five trigger encodings against hand-computed bytes, and both output layouts.
  It also cross-checks CRC32 and the Bluetooth input and output CRCs with Node's zlib.
- On the page, 60 synthetic reports go page → `PadShared` → worker, and the worker's `PadSnapshot`
  matches.
- The worker's feedback presets (key T) go worker → `PadFeedbackShared` → page, which builds the
  output report and shows its bytes.

Not verified: everything with a real pad. That covers the report layouts as Chrome delivers them
(including whether `receiveFeatureReport` includes the report id; the module accepts both), the
trigger feel, rumble strength, Bluetooth CRC acceptance by the pad, and the pairing light
animation.

**Needs the owner.** Open the lab in Chrome or Edge (see *How to reproduce*) and scroll to
*DualSense over WebHID*. The page keeps the ticks in local storage. The *real DualSense* row stays
unverified until every box is ticked. Do the steps first with a USB cable, then again over
Bluetooth (pair the pad in the OS Bluetooth settings first):

1. Click *Connect DualSense* and pick the pad. The status line names the transport (USB or
   Bluetooth).
2. Press every button. Each one lights its name, including PS, touchpad click, mute, Create and
   Options. On an Edge, also the Fn and back buttons.
3. Push both sticks to full travel. They read ±1.00, and pushing down gives a positive y.
4. Pull L2 and R2. Both run smoothly from 0.00 to 1.00.
5. Put two fingers on the touchpad. Two points show, 0..1 from the top-left corner.
6. Check that calibration says "applied". At rest the gyro reads near 0 rad/s, and the
   accelerometer reads about 9.8 on the axis pointing up.
7. Check that the battery shows a percentage, plus "charging" on a charging cable.
8. Click *Rumble 1 s*. Both motors run.
9. Click *Light bar red*, *Green* and *Blue*. The light bar follows. Click *Player LEDs*: lights 1,
   3 and 5 come on.
10. Click each trigger button and pull the triggers. *Feedback* resists from about a third of the
    travel. *Weapon* clicks between 40% and 62%. *Vibration* buzzes from 40%. *Slope* ramps up.
    *Off* is free.
11. With *Follow worker feedback* ticked, click the canvas and press <kbd>T</kbd> a few times. The
    worker's presets reach the pad.
12. Check that the *Worker's PadSnapshot* card above the results follows the sticks and buttons.
13. Bluetooth only: *bad CRC* stays at 0, and the blue pairing pulse stops once the pad is
    connected.

If a step fails, *Copy as JSON*, together with the *Output* panel's report bytes, gives what is
needed to fix it.

Sources for the formats: SDL's `SDL_hidapi_ps5.c` and `testcontroller.c` in `external/SDL` (zlib
licence) for the report structures, calibration and effect flags. The trigger mode byte layouts
follow the community documentation that SDL's test program links to (controllers.fandom.com,
"Sony DualSense", FFB trigger modes). The encodings were written from that description; no code
was copied from it or from any other project.

## 5. OpenAL Soft (stretch): works, but not through SDL3 in the shipped port

- The pinned OpenAL Soft 1.25.2 **has an SDL3 backend** (`alc/backends/sdl3.cpp`), and `emcmake`
  configures it with backends "SDL3, WaveFile, Null".
- It **builds** with two changes. The first is the same `-Wno-error=function-effects -w` the Apple
  build uses (`cmake/Dependencies.cmake`). The second is a one-line patch,
  [`web/lab/audio/openal-soft-emscripten.patch`](../../web/lab/audio/openal-soft-emscripten.patch):
  `alc.cpp` takes `&std::unique_ptr::operator*`, which Emscripten 7.0's libc++ rejects. The build
  applies it to a copy in `build/`; the submodule is untouched.
- [`audio.html`](../../web/lab/page/audio.html) runs on a worker: a looping sound circling the
  listener with HRTF on and an EFX EAX reverb (hangar preset) on an auxiliary send. In headless
  Chrome (output muted) the device opened with "Built-In HRTF" enabled, 48 kHz, EFX with 2 sends and
  no AL errors. In each of the three runs the source played 342,016 samples in 7.05–7.1 s, i.e.
  real time.
- But SDL3's Emscripten audio backend pulls audio from a **ScriptProcessorNode handler on the page's
  main thread**, so OpenAL's mixer (HRTF, reverb) runs there. That costs 1.0 ms (median) per
  2048-frame callback, about 23 times a second (111–113 callbacks counted per run).
  ScriptProcessorNode is deprecated. Audio glitches whenever the page's main thread is busy, and the
  main thread takes OpenAL's and SDL's locks.

What it means for the plan: keep OpenAL Soft for EFX and HRTF, but skip SDL3's audio backend.
Render with OpenAL Soft's **loopback device** (`ALC_SOFT_loopback`, `alcRenderSamplesSOFT`) on a
worker thread into a `SharedArrayBuffer` ring, and play it from an **AudioWorklet**. That keeps the
mixer off the main thread and gives the "OpenAL's mixer" thread the plan lists. Carry the one-line
patch (or upstream it).

## Proposed changes to the plan

1. *Toolchain*: Emscripten 7.0.0 with emdawnwebgpu works. Link with `-sOFFSCREENCANVAS_SUPPORT
   -sOFFSCREENCANVASES_TO_PTHREAD=#canvas` and `-sPROXY_TO_PTHREAD`. Never call
   `wgpuSurfacePresent`.
2. *Renderer table, occlusion row*: results are boolean on Chrome/Metal, readable one frame later.
   Plan a sample-count replacement for flares.
3. *Input*: replace "SDL3's relative mouse mode maps to pointer lock" with the page bridge for
   keyboard, mouse, pointer lock, fullscreen (of the stage container, not the canvas) and Keyboard
   Lock. SDL3's Emscripten input cannot `preventDefault`, gives no lock-loss event, and runs SDL
   code on the page thread.
4. *Audio*: OpenAL Soft loopback on a worker plus an AudioWorklet, instead of the SDL3 backend.
   Note the one-line OpenAL Soft patch for Emscripten's libc++.
5. *Files and game data*: treat OPFS as a capped cache until the quota in a normal Chrome profile is
   known; the measured quotas (4.8, 10.0 and 13.9 GiB) do not hold 16 GB.
6. *Risk 5* ("Worker rendering and input") can drop to low. *Risk 4* (asynchronous GPU results) is
   confirmed: occlusion is boolean.

## Not verified

- A real DualSense, over USB or Bluetooth (item 4).
- Real-keyboard Esc: that Keyboard Lock keeps fullscreen, and that Esc drops the pointer lock. CDP
  key events do not trigger the browser's own Esc handling (control runs), and the built-in
  browser refuses pointer lock.
- The storage quota in a normal Chrome profile; `persist()` after engagement or install.
- Edge, Windows, Linux, and non-Apple GPUs, where the feature list and occlusion behaviour may
  differ.
- `timestamp-query` beyond its presence; audio heard by a person (headless output was muted, and
  progress was measured instead); SDL3's Gamepad API path from a worker.
- Input latencies come from synthetic input; GPU and storage timings come from one machine that
  other sessions shared.
