# Browser lab (web port spike S4b)

A self-contained test page that proves the browser features the web port depends on, together:
WebGPU from a wasm pthread drawing to an `OffscreenCanvas`, SDL3 input on that worker plus a
page-side input bridge, pointer lock, fullscreen and Keyboard Lock, OPFS synchronous access handles
from wasm threads, and DualSense over WebHID. No engine code. Results and what they mean for the
plan: [`docs/web-port/BROWSER-LAB.md`](../../docs/web-port/BROWSER-LAB.md).

## Build and run

```bash
web/lab/setup_emsdk.sh      # once: Emscripten 7.0.0 into build/emsdk (nothing system-wide)
web/lab/build.sh            # SDL3 (first time), the lab, the audio page -> build/web-lab/site
web/lab/serve.py            # http://localhost:8642/ with COOP/COEP
```

Open <http://localhost:8642/> in desktop Chrome or Edge. The page runs items 1 and 3 on its own and
fills the results table; input, pointer lock, fullscreen and the DualSense need a person (or the
headless runner below). `audio.html` is the OpenAL Soft page (item 5).

`build/web-lab/site` is plain static files. Any host works if it sends
`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.

## Tests

```bash
node --test web/lab/test/dualsense.test.mjs     # DualSense parser and encoder, no hardware
node web/lab/tools/headless_run.mjs --fresh     # the whole page in headless Chrome over CDP (serve.py running)
```

The headless runner drives keyboard, mouse, pointer lock and fullscreen with DevTools input events,
so nothing grabs the desktop's mouse or screen. It writes `build/web-lab/headless-results.json`.

## Layout

| Path | What |
|---|---|
| `src/lab_main.cpp` | SDL3 main callbacks on the worker (`-sPROXY_TO_PTHREAD`), command ring from the page |
| `src/gpu_lab.cpp` | Item 1: WebGPU through `webgpu.h` (emdawnwebgpu) |
| `src/input_lab.cpp` | Item 2: SDL3 events and the bridge ring, read side of the DualSense block |
| `src/storage_lab.cpp`, `src/lab_lib.js` | Item 3: I/O thread, blocked reader thread, 1 GiB OPFS benchmark |
| `src/pad_shared.h` | Shared-memory layout of `PadSnapshot` and the feedback block |
| `page/index.html`, `page/page.js` | The page: results, input bridge, blocked-worker probe, storage, WebHID UI |
| `page/dualsense.js` | Item 4: standalone WebHID DualSense module |
| `audio/`, `page/audio.html` | Item 5: OpenAL Soft with its SDL3 backend |
| `tools/make_chunks.py` | Stand-in data chunks for the storage test |
| `tools/headless_run.mjs` | Headless Chrome runner |
