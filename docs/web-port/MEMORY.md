# Spike S3: does the game fit in a 32-bit WebAssembly heap?

**Verdict: yes, with about 1.6 GB to spare on the 4 GB cap even at the pessimistic estimate, and not with 2 GB.** Measured on the Mac build under the iPad profile
(`ipad-8gb`: `texture_lod` 1, render scale 0.6), the memory a wasm heap would have to hold is **about 1.2 GB in play, 1.4 to 1.8 GB at the
peak of a level load, and about 2.5 GB if the allocator keeps what it has freed, as a wasm heap does**. Textures, render targets and level
geometry (2.7 to 3.4 GB on the GPU) are not heap. The numbers below are for the engine as it is today: 64-bit pointers, LuaJIT, the Mac's
allocator.

| What the heap has to hold (MB) | menu | Cordon, steady | worst level load | pessimistic* |
|---|---:|---:|---:|---:|
| Margin against 4 GB | 3,776 | 2,925 | 2,302 | 1,602 |
| Margin against 3 GB | 2,752 | 1,901 | 1,278 | 578 |
| Margin against 2 GB | 1,728 | 877 | 254 | −446 |
| The heap itself | 320 | 1,171 | 1,794 | 2,494 |

\*Pessimistic: the largest `malloc_allocated` (the blocks the zones hold from the OS: 2,199 MB at the Jupiter load of the 17 ms run, against
1,510 MB live) instead of the live bytes, plus Lua 47, windows 152 (the exact peak at any `mmap`, not the sampled 111), Metal CPU 28 and
stacks and static data 68. A wasm heap grows and never shrinks, so after a load spike it stays that big; the Mac allocator's retained
blocks stand in for the fragmentation a wasm allocator would add (1.3 to 1.5 times the live bytes at the peaks).

Recommendations (details in the last section):

1. Plan for a 4 GB heap (`MAXIMUM_MEMORY=4GB`), start it at 2.5 GB or less if growth stalls turn out to matter, and do not promise a 2 GB build.
2. Keep `r__no_ram_textures` on. Off, the engine keeps a CPU copy of every texture: **+1.0 GB of heap** (measured).
3. Give the web profile the iPad's `snd_cache_size 32` (a 64 MB block). The Mac profile's 512 MB one is a single `calloc` that a wasm heap would count whole.
4. Bound the transient at a level load. Every spike is 200 to 360 MB above the new level's steady state and lasts under a second, but it
   sets the heap's size for the rest of the session. The engine frees the old level before it loads the next, so the spike is not two
   levels at once.
5. Textures are not a heap problem, but 2.7 GB of them on the GPU is the next question for S2/S4b-style checks on integrated GPUs.

## What counts as heap, and how each part was measured

The numbers come from the Mac build, `RelWithDebInfo`, unattended, with a sampler thread (`--memstat <csv>`) that reads every 100 ms (one
row every 110 ms with the cost of a sample; 17 ms rows for the check below):

| Part | Source on the Mac | On the web |
|---|---|---|
| malloc | `malloc_zone_statistics`, all zones: bytes in live blocks (`malloc_in_use`) and bytes the zones hold from the OS (`malloc_allocated`) | the heap itself |
| Lua | `collectgarbage("count")` from `xr_harness.mem()`, step-held between samples. LuaJIT uses its own mmap allocator, so it is **not** in the malloc numbers | plain Lua 5.1 allocates through malloc: in the heap |
| mapped windows | counters at the five `mmap` sites (`LocatorAPI.cpp` archive entries, `stream_reader.cpp`, `FS.cpp` loose files) | the windows become heap buffers; the live total counts in full |
| CPU-visible Metal copies | counters in the Metal shim by class (below) | the part a WebGPU backend would keep in the heap |
| stacks and static data | 64 MB for stacks (about 32 threads of 2 MB; the Mac peaked at 22 threads) plus 4 MB of `__DATA` | the same order |
| *not heap* | Metal textures, render targets, static buffers: reported as "GPU-only" | GPU process |

`heap` in every table is the sum of the first five rows at the same sample. Lua can only be sampled while the game runs, not while a
level loads, so during the first load the Lua column carries the first value read afterwards (40 MB, a 2 % error on a peak).

## Results

### iPad profile (`--graphics-profile ipad`, 2732x2048, texture level 1, sound cache 32 MB)

All figures in MB. *Peak* is the heap's largest sample in the window with its parts; *steady* is the median of the window's last 10 s.
Level loads are the time from the `jump_to_level` command to the level being ready; the chain is Cordon → Garbage → Pripyat → Jupiter → Cordon.

| scenario | | heap | malloc | Lua | windows | Metal CPU | stacks+static | footprint | GPU-only |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| main menu, idle | steady | 320 | 217 | 0 | 8 | 27 | 68 | 721 | 405 |
| new game: load of the Cordon | peak | 1,388 | 1,169 | 40 | 84 | 27 | 68 | 2,854 | 1,576 |
| new game: 30 s after ready | steady | 1,157 | 989 | 43 | 28 | 28 | 68 | 4,204 | 2,795 |
| Cordon from a save: load | peak | 1,380 | 1,160 | 41 | 84 | 27 | 68 | 2,847 | 1,416 |
| Cordon after 5 min of play | steady | 1,171 | 1,010 | 41 | 24 | 29 | 68 | 4,275 | 2,748 |
| Cordon after 5 min of play | peak | 1,212 | 1,051 | 41 | 24 | 29 | 68 | 4,307 | 2,748 |
| change to Garbage: load | peak | 1,350 | 1,138 | 42 | 74 | 28 | 68 | 3,394 | 1,921 |
| Garbage, 40 s after ready | steady | 1,145 | 994 | 40 | 15 | 28 | 68 | 4,374 | 2,716 |
| change to Pripyat: load | peak | 1,753 | 1,514 | 40 | 102 | 28 | 68 | 3,789 | 1,990 |
| Pripyat, 40 s after ready | steady | 1,394 | 1,234 | 45 | 18 | 28 | 68 | 5,299 | 3,332 |
| change to Jupiter: load | peak | 1,763 | 1,510 | 46 | 111 | 28 | 68 | 4,125 | 2,337 |
| Jupiter, 40 s after ready | steady | 1,427 | 1,246 | 42 | 42 | 28 | 68 | 5,386 | 3,401 |
| change back to the Cordon: load | peak | 1,669 | 1,448 | 41 | 84 | 28 | 68 | 4,105 | 2,388 |
| Cordon again, 40 s after ready | steady | 1,315 | 1,146 | 40 | 32 | 29 | 68 | 5,133 | 3,215 |

The same chain sampled every 17 ms instead of 110 ms (`mem-fine`): Garbage load 1,454, Pripyat load **1,794**, Jupiter load 1,764, Cordon
load 1,641; steady states within 3 %. A 100 ms sampler undershoots a spike by up to 8 %, and a 17 ms one may still miss a little.

### Mac default profile (`mac-pro`, 1280x720, sound cache 512 MB)

| scenario | | heap | malloc | Lua | windows | Metal CPU | stacks+static | footprint | GPU-only |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| main menu, idle | steady | 750 | 666 | 0 | 8 | 8 | 68 | 440 | 184 |
| new game: load of the Cordon | peak | 1,838 | 1,618 | 40 | 84 | 27 | 68 | 3,371 | 2,136 |
| Cordon after 5 min of play | steady | 1,619 | 1,478 | 40 | 24 | 9 | 68 | 4,351 | 2,476 |
| change to Pripyat: load | peak | 2,190 | 1,968 | 42 | 102 | 9 | 68 | 3,319 | 1,723 |
| change to Jupiter: load | peak | 2,233 | 1,995 | 50 | 111 | 9 | 68 | 4,408 | 2,083 |
| Cordon again, 40 s after ready | steady | 1,783 | 1,636 | 40 | 31 | 9 | 68 | 5,519 | 2,957 |

It differs from the iPad profile by about 450 MB, the size difference of the sound cache (one `calloc` of 524,304 KB for the engine's default
setting against 65,552 KB for the iPad's `snd_cache_size 32`), which
is why the menu alone holds 666 MB of malloc: the cache is allocated at start and mostly never touched, so the footprint (440 MB) stays low.
A wasm heap counts it whole.

### Does the heap return after a level change?

No, and it does not need to for the numbers above to hold. The sampler's CSV (`mem-final/ipad/mem_levels/mem.csv`) shows the Garbage → Pripyat change second by second:

| time (s) | malloc in use (MB) | blocks held from the OS (MB) | what happens |
|---|---:|---:|---|
| 113 | 994 | 1,479 | Garbage is playing |
| 115 | **495** | 1,162 | the engine has freed Garbage: 500 MB gone |
| 117 to 118 | 897 → 1,227 | 1,432 → 1,495 | Pripyat loads: windows rise to 102 MB |
| 119.3 | **1,514** | 1,766 | a spike of less than 1 s, 280 MB above the steady state it settles to (1,235) |
| 121 to 124 | 1,099 → 1,219 | 1,734 → 1,766 | settling |
| 130 | 1,235 | 1,776 | Pripyat plays |

- The old level goes before the new one comes, so a change is not the sum of two levels; the spike is the loader's own temporary memory
  (decompression and parsing buffers: unconfirmed, see the last section).
- malloc in use drops by half and the blocks held from the OS drop by 20 %: macOS keeps most freed pages in its zones. A wasm heap would do the same, which is what the "pessimistic" column prices.
- Back in the Cordon after four loads, the steady state is 1,315 MB against 1,193 MB the first time: **122 MB (10 %) is not given back**,
  all of it in malloc (Lua is 40 MB both times). It was not chased further; one cycle of the chain adds it, so a long session with many
  changes needs a longer run to say whether it grows. In a 5-minute stay in one level malloc is flat (1,024 MB when the level is ready, 1,010 MB five minutes later).

### Settings that move the heap

Cordon load and 90 s of play, engine defaults (`_preset Default`, no graphics profile, 1280x720), one setting changed:

| setting | | heap | malloc | windows | Metal CPU | staging textures | GPU-only | footprint |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `texture_lod` 0 | load peak | 1,695 | 1,493 | 84 | 8 | 4 | 1,463 | 2,727 |
| | steady | 1,607 | 1,462 | 27 | 9 | 4 | 3,611 | 5,145 |
| `texture_lod` 1 | steady | 1,608 | 1,466 | 24 | 9 | 4 | 2,551 | 4,349 |
| `texture_lod` 2 | steady | 1,608 | 1,460 | 28 | 9 | 4 | 1,855 | 3,679 |
| `texture_lod` 3 | steady | 1,614 | 1,472 | 24 | 9 | 4 | 1,590 | 3,430 |
| `r__no_ram_textures` **off** (`texture_lod` 2) | load peak | **2,653** | 1,317 | 24 | **1,204** | **1,199** | 455 | 3,176 |
| | steady | **2,594** | 1,459 | 27 | **998** | **992** | 870 | 3,705 |

- **`texture_lod` does not move the heap** (the heap column is flat across levels 0 to 3) and moves the GPU a lot: 3.6 GB at level 0,
  2.6 GB at 1, 1.9 GB at 2. Textures are created from the file and uploaded; nothing keeps a CPU copy.
- **`r__no_ram_textures` off costs +1.0 GB of heap**: the engine then creates every texture as a `STAGING` copy in shared memory
  (a WebGPU backend would hold these in the heap) next to the GPU one. It is on by default for R4, and the graphics profiles lock it on; the web profile must too.

## The CPU-visible copies the Metal shim keeps

On Apple silicon every buffer is `MTLResourceStorageModeShared`, so the shim's CPU-visible memory sits outside the malloc heap. A WebGPU
backend has no CPU-visible GPU memory: what the engine writes from the CPU has to live in the wasm heap until `writeBuffer` or
`writeTexture` copies it. The shim now counts each class (`mtl_stats.h`, `RenderStats`). iPad profile, Cordon, 5 min (MB):

| class (shim code) | here, final / peak | On a WebGPU backend | Reasoning |
|---|---:|---|---|
| per-frame rings: constant ring and upload ring (`Ring`, 8 MB and 32 MB chunks per in-flight frame) | 32 allocated; **1.4 used per frame at most** (the upload ring is never used) | **heap**, one frame's worth (about 1.4 MB, say 8 MB with slack, and one batched `writeBuffer` per ring before each submit) | `DISCARD` maps of buffers up to 256 KB: constant buffers and small dynamic vertex data. The 32 MB is presumably four 8 MB slabs of the constant ring, one per frame in flight plus one, which `writeBuffer` makes unnecessary |
| `DYNAMIC` buffers (the first backing; rename-pool copies not counted) | 4.7 / 4.7 | **heap**, one shadow each | the engine maps them with `DISCARD` or `NO_OVERWRITE` and writes: a CPU shadow plus a copy into the GPU buffer |
| `STAGING` buffers | 0 / 0 | heap | none exist |
| `STAGING` textures (`CopyResource` readbacks) | 22.7 / 22.7 | heap, but only while a screenshot or a save thumbnail is being read | one back-buffer-sized staging texture that exists from start-up (22.7 MB at 2732x2048, 3.5 MB at 1280x720); most likely the screenshot readback, not confirmed. With `r__no_ram_textures` on no game texture is staged |
| `DYNAMIC` textures (three rename copies here) | 0 / 0 | GPU plus a transient shadow at `Map` | none are created in these scenes |
| static buffers (`IMMUTABLE`/`DEFAULT`, vertex and index data of the level) | 54 / 172 (chain: 121 / 289) | **GPU-only** after the upload | filled once; the source is an archive window or a `malloc` copy that is freed (counted in malloc) |
| shared textures filled once with `replaceRegion` (`IMMUTABLE`, `DEFAULT` with data) | 1,479 / 2,179 (chain: 1,897 / 2,729) | **GPU-only** after `writeTexture` | nearly all textures that are not render targets (1,479 of 1,899 MB); the engine keeps no CPU copy with `no_ram_textures` on. The data passes through the heap once, as the file window and its decoded copy |
| private storage (render targets, depth, UAV, `DEFAULT` without data) | 397 / 397 | GPU-only | the back buffer scale decides it (177 MB at 1280x720 and scale 1.0, 397 MB at the iPad profile's 2732x2048 and 0.6) |

The Metal shim's heap share is therefore about 6 MB at steady state, 28 MB with the readback staging copy, and the rest of its 2.1 to 3.6 GB is
GPU memory. One transient the numbers cannot show: **textures stream through the heap on the way up** (a Pripyat load uploads about
1.3 GB: the GPU-only total goes from 2.0 to 3.3 GB). That is safe if the backend submits `writeTexture` often. A backend that queues uploads and submits once per frame has to hold every
one of the frame's file windows and decoded copies; cap it at about 64 MB per submit.

## What dominates the heap

One run of the Cordon under the iPad profile with `MallocStackLogging=1`, 170 s after launch (the level has been up for about two
minutes), `malloc_history -allBySize`. The log accounts for **1,023 MB of live malloc blocks, against 1,025 MB that the sampler read at the
same moment**, so nothing large is missing. `tools/render_harness/mem_top.py` names each stack's owner by its innermost engine frame
(allocator, container and file-layer frames skipped): a cheap attribution, and the categories are as rough as that sounds. The Lua heap (41 MB) and the Metal memory are not malloc and are not in it.

| owner | MB | share | what is in it |
|---|---:|---:|---|
| models: skeletons, visuals, shared geometry containers | 198 | 19 % | `CModelPool` / `CKinematics` / `smem_container::dock`: the CPU copy of every loaded model's vertex and index data (190,000 blocks; 40 MB for the entities spawned at load, 16 MB for the player's hands) |
| A-Life spawn registry, game graph, AI map, cover | 196 | 19 % | the whole spawn file read and kept (**93 MB, one block**), a cover-point quadtree (32 MB), the game graph build (21 MB), the level graph (15 MB) |
| collision model (xrCDB, Opcode) | 176 | 17 % | `CDB::MODEL::build` of the level: three Opcode trees of **112, 45 and 18 MB**, built at load from the level's geometry |
| game objects and entities | 121 | 12 % | `CSE_*` and `CObject` state of the squads and NPCs that A-Life keeps (300,000 blocks) |
| sound | 83 | 8 % | the decoded-audio cache (**64 MB**, one `calloc`: `snd_cache_size 32` allocates twice its value) and OpenAL |
| ini files | 77 | 7 % | `CInifile`: **three trees of 17.7 MB and 11,867 blocks each** from `InitSettings()` (`pSettings` and `pSettingsAuth`, both `system.ltx`, and `pGameIni`, `game.ltx`; the equal sizes say they hold the same data), plus the per-spawn ini objects (5.5 MB) |
| renderer | 47 | 5 % | blenders, shader objects, details (12 MB), the pipelines' CPU side |
| XML and UI | 44 | 4 % | TinyXML trees of the UI (49,000 blocks in one stack) |
| Lua and luabind, C++ side | 32 | 3 % | proxies and class registrations; the VM's own heap is the 41 MB outside malloc |
| engine core, file tables, physics, other | 49 | 5 % | shared strings, the archive directories (6.5 MB for 50,000 files), ODE |

- Nothing here depends on the GPU, so it carries to the web unchanged except for pointer width.
- Four items stand out as saving candidates, none needed for the verdict: the spawn file kept whole (93 MB; a `CStreamReader` would not need
  it), the three equally sized ini trees (up to 35 MB; `pSettingsAuth` is a second parse of `system.ltx` for the auth check), the collision trees (176 MB, rebuilt at every load) and the model geometry that also exists
  on the GPU (198 MB; the CPU copy matters only if the model is read back).
- The textures are created on GCD worker threads (`dispatch_apply` in `CTexture::Load`): the web needs a thread pool with room for several
  decoded textures at once, which is part of the 111 to 158 MB of windows and of the load spike.

## What this changes in the plan

- **Risk 3 (memory) drops from open to bounded.** `PLAN.md` puts non-GPU memory at about 1.5 to 2 GB and says the load peak is not broken down;
  the measured figures are 1.2 GB in play, 1.4 to 1.8 GB at a load, 2.5 GB pessimistic. The 32-bit build is the right first target
  and Memory64 is not needed. Keep it as a build switch for the sound cache and texture experiments only.
- **Web graphics profile:** copy the iPad's memory-relevant lines (`snd_cache_size 32`, `texture_lod 1` or 2, `r__no_ram_textures` on and locked, no MSAA). The GPU
  share (2.7 to 3.4 GB at level 1, Cordon to Jupiter) is the thing that profile has to pick by device.
- **Heap sizing:** `MAXIMUM_MEMORY=4GB`, `ALLOW_MEMORY_GROWTH`. Growth of a shared memory under pthreads makes every typed-array view stale and is slow; an initial size near
  2.5 GB (above the pessimistic peak) avoids growing during a load **(verify in S1b/S4b)**. A 2 GB build cannot be promised: the load peak
  alone is 1.8 GB and the retained blocks push the heap past 2 GB.
- **File windows:** the live window total peaks at 111 MB sampled and 152 to 158 MB exact during a load: 144 MB of it archive entries read
  through `LocatorAPI`, 2 MB stream readers, 1.5 MB loose files. Whole entries become heap buffers on the web; do not also cache chunks in the heap
  (an OPFS-backed chunk cache is not in these numbers).
- **Allocator:** mimalloc as planned. Test its retained-to-live ratio at the Pripyat load; the Mac's was 1.3 to 1.5, and a worse ratio is the main
  way this verdict can slip.
- **Thread stacks** (64 MB here) are small next to the rest; keep them at 2 MB, not larger, because the heap counts them in full.

## Reproduce

Build the release app once (with every module; the default preset does not build the app):

```bash
git submodule update --init --depth 1 external/SDL external/openal-soft external/LuaJIT external/ogg external/vorbis external/theora external/SPIRV-Cross external/vkd3d external/sse2neon external/libarchive external/xz
cmake --preset macos-arm64-release -DANOMALY_BUILD_ALL_MODULES=ON
cmake --build build/macos-arm64-release --target anomaly -j 4
```

Then one command (about 40 minutes; it waits while another `anomaly` process runs, and runs offscreen and silent):

```bash
tools/render_harness/mem_report.sh -t mem-final
```

It writes `build/render-harness/mem-final/mem_report.md` with the summary tables above and one table per run. The pieces:

| piece | what it does |
|---|---|
| `anomaly --memstat <csv> [--memstat-ms 100]` | a sampler thread writes one CSV row per interval (malloc, footprint, windows, the shim's classes), keeps the peaks, logs a `mem:` line every 10 s |
| `<t> mark <text>` (script step) | a labelled row in the CSV and a line in the shell log; `waitlevel` and the engine start mark themselves |
| `xr_harness.mem(tag)` | logs `collectgarbage("count")` (the Lua heap) with the level name |
| `HARNESS_MEMSTAT=1`, `HARNESS_MEMSNAP="60 240"` | `run_scene.sh` options: pass `--memstat`, and run `vmmap -summary` and `heap -s` at those seconds. With `HARNESS_ENV="MallocStackLogging=1"` it also runs `malloc_history -allBySize` |
| `mem_report.py`, `mem_top.py` | tables from the CSVs; the live heap by call stack and subsystem |
| scenes `mem_menu`, `_newgame` (clicks the new-game screen's Start button), `mem_cordon`, `mem_cordon_short`, `mem_levels` | the scenarios |

Single runs and variants:

```bash
MEM_PROFILES=ipad MEM_SCENES="levels" MEM_SWEEPS=0 tools/render_harness/mem_report.sh -t quick
HARNESS_MEMSTAT=1 HARNESS_ARGS="--graphics-profile ipad --memstat-ms 10" tools/render_harness/run_scene.sh -t fine -s 2732x2048 mem_levels
MEM_REPORT_ONLY=1 tools/render_harness/mem_report.sh -t mem-final        # tables of an earlier run
```

## What was not verified

- **No wasm build was run.** Everything is the Mac build: 64-bit pointers (structures with many pointers shrink in wasm32, perhaps 10 to 25 %
  of malloc: a likely gain, not counted), LuaJIT (plain Lua 5.1 is bigger: from memory, 16-byte values against 8 and hash nodes of 40 bytes against 24 on
  wasm32; a guess of 1.5 times the 41 MB, about +20 MB, which S1a measures), the Mac allocator's fragmentation (`malloc_allocated` / `malloc_in_use` stands in for mimalloc's),
  and the Metal driver's own CPU-side allocations, which are in `malloc_in_use` and do not exist on the web (not separated; an overestimate).
- **The load spike's cause is a hypothesis.** Decompression and parsing buffers of the level's large archive entries fit the
  timing and size, but a spike under a second cannot be caught by `malloc_history`, so no stack names it.
- **Sampling at 17 ms may still miss a short spike**; 110 ms missed up to 8 %. The peak is a lower bound.
- **Lua cannot be sampled during a load**, and not in the menu at all (`run_string` crashes there; the script engine is not up).
  The menu's Lua heap is assumed to be small.
- **Retention across many level changes** was seen for one chain (+122 MB); no long session was run.
- **Only four levels were loaded** (Cordon, Garbage, Pripyat, Jupiter). Pripyat (500 MB) and Jupiter (417 MB) have the two largest level
  archives; Zaton (404 MB) is third and was not loaded.
- **Combat and a busy A-Life** were not measured; the Cordon runs teleport the player across the level but fight nothing (`ai_ignore_actor on`).
- Footprint and GPU-only numbers come from the Mac under the iPad profile at 2732x2048 and say nothing about browser GPU limits.
- **Apple build and tests.** All engine changes are counters and a sampler that is off unless `--memstat` is given. Run: `cmake --preset macos-arm64`
  then `cmake --build --preset macos-arm64 -j 3` (Debug, default modules) and `ctest --test-dir build/macos-arm64`: 23 of 23 passed.
  `macos-arm64-release` with `ANOMALY_BUILD_ALL_MODULES=ON` builds the `anomaly` app, which is what every number here was measured with;
  the default preset does not compile `main.cpp` or the Metal shim, so only that build covers them. Not run: the iPad (no device, as the brief says), the ASan and TSan presets.
