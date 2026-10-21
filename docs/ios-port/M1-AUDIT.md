# M1 audit: porting the remaining modules

Read-only audit of `xrCDB`, `xrSound`, `xrParticles`, `xrPhysics`, `xrEngine`, `xrGame`, `xrServerEntities`,
`xrNetServer`, `xrXMLParser`, `xrCPU_Pipe`, `3rd party/lua-extensions` and the `Layers/xrRender*` directories of
this fork, compared with OpenXRay (`reference/xray-16`, commit `c62d9d3`). It plans the wave-2 sessions that port
these modules in parallel. No code was changed.

Method: line-oriented scans (ripgrep and small Python scripts), the project files (`*.vcxproj`,
`xrGame.vcxproj.filters`), and a clang probe on macOS (section 7.4). Numbers are heuristic counts
unless a section says "measured". Commands are in the appendix. Counts of "LOC" include headers and blank lines;
"effort" is in Sonnet-xhigh session-days (about 4 to 6 productive hours each) assuming the xrCore shim exists.

## 1. Headline findings

1. **The monolith is already fully static.** Every module is a `StaticLibrary` linked into `xrEngine.exe`.
   `EngineAPI.cpp` calls `DllMainXrRenderR4()` and `DllMainXrGame()` directly (`STATIC_RENDERER_R4`), and the
   `XR*_API` export macros are already empty. There is no DLL plugin boundary to reproduce on iOS.
2. **`xrServerEntities` is not a library.** It has no project of its own: 58 of its translation units compile
   into `xrGame.lib` (the script engine, server entities, object factory) and 1 into `xrPhysics.lib`. Plan it with xrGame.
3. **Direct Win32 use is tiny outside xrCore and xrEngine.** `xrGame` calls about 25 distinct Win32
   functions (plus about 10 MSVC CRT aliases) in about 15 files (anti-cheat events, registry, `ShellExecute`, `SetCapture`). `xrPhysics`,
   `xrParticles`, `xrXMLParser` and `xrCDB` call essentially none. The work is the C++ dialect and the xrCore
   shim, not API porting.
4. **No live x86 inline asm or SEH in the modules audited** (release configuration). All of it is behind `#if 0`
   or `#ifdef DEBUG`: `xrSkin2W_SSE.cpp` (53 `__asm`), `xrTheora_Surface_mmx.cpp`, `OPC_FPU.h`, the SEH in
   `xr_object_list.cpp`. The only live SEH is the stack-overflow filter in `x_ray.cpp`.
   SSE intrinsics (xmmintrin) appear in 7 files and map to `sse2neon`, as OpenXRay does.
5. **`xrCPU_Pipe` is almost entirely dead.** The bind table only wires the plain C++ `xrSkin*W_x86` functions
   and `PLC_calc3_x86`; the SSE path is `#if 0`. `PLC_calc3` is only called by the R1 renderer, which we do not
   build. OpenXRay folded the skinning code into `xrRender`; do the same.
6. **TBB has 3 real call sites** (`xrSound/SoundRender_Core_SourceManager.cpp:70`,
   `Layers/xrRender/ResourceManager.cpp:388` and `:398`, all `parallel_for_each`) and 3 dead includes.
   `xrCore/_thread_types.h` includes PPL (`<ppl.h>`), but none of `xr_parallel_*`, `xr_task_group`,
   `xr_concurrent_*` is used anywhere; only `xr_atomic_u32` is. A GCD-backed `parallel_for_each` is about 60 lines.
7. **Include hygiene is the main mechanical blocker:** 161 `#include` lines use backslashes (Apple clang rejects them unless
   `-fms-extensions` is on) and 2,626 differ from the on-disk case (fine on APFS case-insensitive, which is the default
   on macOS and iPadOS, but wrong for any case-sensitive volume or CI). The vcxproj paths also differ in case from disk
   (`Ai/Stalker/...` vs `ai/stalker/...`).
8. **Multiplayer is 11.7% of xrGame** (163 `.cpp`, 53.2K of 454.7K LOC, by project filter). The GameSpy SDK is not
   in this repo and its calls are already commented out, so there is no library to replace, only game-side code to
   compile out or stub. DirectPlay in `xrNetServer` is the only real transport; OpenXRay has a ready replacement.
9. **R4 compiles 227 `.cpp` (72K LOC)**: 121 from `xrRender`, 74 from `xrRenderPC_R4`, 30 from
   `xrRenderDX10`, plus 2 xrEngine Lua files. D3D11 contact is spread over about 80 files, but the window,
   swap chain and DXGI are concentrated in `xrRenderDX10/dx10HW.cpp`. See section 8.
10. **Compile flags (measured on 81 xrGame TUs, 7.4):** use `-fms-extensions -fdelayed-template-parsing -fno-operator-names` from day one, plus `-iquote src/xrCore` (two xrCore headers shadow system headers).
    `-fms-compatibility-version=19.30` is a useful bootstrap (20 of 81 TUs clean, 410 unique error sites instead of 1,510) but hides about 2,000 `-Wmicrosoft-*` diagnostics. Keep C++ exceptions on (2.8).
11. **Hard prerequisite:** every module includes `xrCore.h`, which includes `windows.h`. Nothing in wave 2 compiles
    without the xrCore compat header. At audit time the sessions `xrcore-port` and `engine-3rdparty` were
    reported as failed; verify and re-run them first.

### Proposed wave-2 sessions

| # | Session | Owns | LOC | Effort | Starts when |
|---|---|---|---|---|---|
| P0 | `xrcore-shim` (carry-over) | `src/xrCore/**`, compat header, container conformance (`xr_vector`, `xr_deque`, `resptr_core`, `intrusive_ptr`, `_type_traits`), pin `external/sse2neon`, CMake skeleton for all static libs | 46K | wave 1 | now |
| 1 | `leaf-libs` | `xrCDB`, `xrXMLParser`, `xrParticles`, `xrCPU_Pipe` | 30K | 2 d | P0 |
| 2 | `sound` | `xrSound` | 6K | 1.5 d | P0 |
| 3 | `physics` | `xrPhysics` (builds against the ODE session's target) | 32K | 2 d | P0 + ODE |
| 4 | `netserver` | `xrNetServer` (stub transport, OpenXRay `empty/`) | 5K | 1.5 d | P0 |
| 5 | `engine-core` | `xrEngine` except input and media (section 3.9) | 40K | 5 d | P0 + ios-shell |
| 6 | `engine-input` | `Xr_input`, key tables, imgui input, media decoders | 8K | 3 d | P0 + gamepad |
| 7 | `renderer-r4` | `Layers/{xrAPI,xrRender,xrRenderDX10,xrRenderPC_R4}` sources | 72K | 5 d | P0 + metal-d3d11 headers |
| 8 | `xrgame-triage` (serial, first) | `xrGame/stdafx.h`, `pch_script.h`, PCH and CMake, the ~30 shared template headers and FastDelegate/luabind patches (7.4), include-hygiene pass | headers | 3 d | P0 |
| 9 | `xrgame-ai-monsters` | `AI\AMonsters` (94K), split a/b | 94K | 6 d | 8 |
| 10 | `xrgame-ai-core` | `AI\{AComponents,AGroup,ACover,AEval,ADebug,ASpace,ASound}` | 40K | 4 d | 8 |
| 11 | `xrgame-nav-alife` | `AI\{ANavigation,ALife}` | 30K | 3 d | 8 |
| 12 | `xrgame-script-entities` | `AI\AScript` plus the 58 `xrServerEntities` TUs, `lua-extensions` | 20K + 36K | 5 d | 8 + luabind + LuaJIT |
| 13 | `xrgame-ui` | `UI\*` except Multiplayer | 54K | 4 d | 8 |
| 14 | `xrgame-objects-items` | `Core\Client\Objects\{items & weapons,inventory}` | 50K | 4 d | 8 |
| 15 | `xrgame-objects-world` | `Core\Client\Objects\*` other than items | 49K | 4 d | 8 |
| 16 | `xrgame-core-physics` | `Core\{Client other,Server SP,Common}`, `Physics\*` (IK, character) | 65K | 5 d | 8 |
| 17 | `xrgame-mp-stub` | MP/GameSpy exclusion and stubs (53K LOC of sources removed from the build) | 53K | 2 d | 8 |

Sessions 1 to 4 and 8 can run at once. Sessions 5 to 7 need the SDL3 shell, gamepad seam and Metal D3D11 headers
described in sections 3.9 and 8. Sessions 9 to 17 fan out after the triage session lands (section 7).

## 2. Cross-cutting findings

### 2.1 Link topology and build shape

| Project | Type | `.cpp` | Notes |
|---|---|---|---|
| `xrEngine` | Application | 105 | also compiles 14 `3rd party/discord` files |
| `xrGame` | StaticLibrary | 1246 (1188 in `xrGame/`, 58 from `xrServerEntities/`) | PCH `stdafx.h` + `pch_script.h` |
| `xrRender_R4` | StaticLibrary | 227 | 121 `xrRender`, 74 R4, 30 `xrRenderDX10`, 2 xrEngine Lua files |
| `xrPhysics`, `xrCDB`, `xrSound`, `xrParticles`, `xrNetServer`, `xrXMLParser`, `xrCPU_Pipe`, `xrCore` | StaticLibrary | 59/40/20/8/8/5/7/69 | |
| `lua_extensions`, `luabind`, `ode`, `imgui`, `OpenAL-new`, `cximage`, `crypto`, `giflib`, `DXERR`, `NVTT`, `optick`, `ReShadeCompat`, `xrAPI` | StaticLibrary | | third-party or glue |

R1, R2, R3 and DX9 (37K LOC) are separate projects that `EngineAPI.cpp` selects with `STATIC_RENDERER_R*`. We build only R4;
do not port the others. Both xrGame projects (`xrGame/xrGame.vcxproj` and `xrGame/vs2022/xrGame.vcxproj`) list nearly the same files.
Use `xrGame/xrGame.vcxproj` plus the `.filters` file as the source of truth for what compiles.

### 2.2 What the non-core modules demand of the xrCore shim (measured)

The shim header belongs to the xrCore session. These are the names the other modules actually use, so the shim
can be scoped by evidence:

- **Types:** `DWORD` must be `uint32_t` (not `unsigned long`, which is 64-bit on LP64), `BOOL`, `BYTE`, `WORD`,
  `LPCSTR`/`LPSTR`/`LPVOID`/`LPCWSTR`, `HANDLE`, `HWND`, `HINSTANCE`, `HMODULE`, `HMONITOR`, `WPARAM`/`LPARAM`/`LRESULT`,
  `RECT`, `POINT`, `LARGE_INTEGER`, `FILETIME`, `SYSTEMTIME`, `CRITICAL_SECTION`, `GUID`, `HRESULT`, `WAVEFORMATEX`.
- **CRT:** `ZeroMemory`/`CopyMemory`/`FillMemory` (about 250 uses), `_alloca` (about 110), `stricmp`/`_stricmp`/`strnicmp`
  (about 75), `strlwr`/`strupr`, `itoa`/`_itoa`, `_snprintf`/`_vsnprintf`, `_finite`, `_copysign`, `_splitpath`/`_makepath`,
  `_mkdir`/`_unlink`/`_access`/`_filelength`/`_fileno`, `_localtime64`/`_time64`, `strncpy_s`/`sscanf_s`/`sprintf_s`.
- **Win32:** `Sleep`, `SwitchToThread`, `GetTickCount`, `timeGetTime`, `Interlocked*`, `GetCurrentThreadId`/`GetCurrentProcessId`,
  `OutputDebugString`, `GetLastError`, `MultiByteToWideChar`/`WideCharToMultiByte`; events (`CreateEvent`, `SetEvent`,
  `WaitForSingleObject`, `CloseHandle`) only in MP and `xrServerEntities`.
- **D3D9-era types leaking into engine and game code:** `D3DCOLOR_*`, `D3DFVF_*`, `D3DPT_*`, `D3DVERTEXELEMENT9`, `D3DFORMAT`,
  `D3DXGetFVFVertexSize`, `D3DXGetDeclLength` in 157 files (425 `D3DCOLOR_*` uses in 83 files, mostly `#ifdef DEBUG` debug drawing).
  OpenXRay solved this with `src/Common/d3d9compat.hpp` (602 lines) and `src/Common/_d3d_extensions.h`; reuse them.
- **Input:** `DIK_*` constants (about 650 uses in 12+ files); see 2.7 for why the numbering must stay.
- **DirectPlay types** (`DPNID`, `DPN_APPLICATION_DESC`, `DPNSEND_*`): 9 uses in 6 xrGame files (`xrServer.h`, `Level.h`,
  `GameObject.h`, `game_sv_base.h`, `game_sv_mp.cpp`, `xrGameSpyServer.h`), plus `DPlay/dplay8.h` included from `xrGame/stdafx.h`. Provide a tiny stub header (3.8).

OpenXRay references for the shim: `src/Common/Platform.hpp`, `PlatformApple.inl`, `PlatformLinux.inl`,
`Compiler.inl`, `d3d9compat.hpp`, `_d3d_extensions.h`.

### 2.3 LP64 and dialect hazards (counts are heuristic)

| Hazard | Where | Note |
|---|---|---|
| `long`/`unsigned long` in source | xrCore 111, xrGame 21 (15 files), lua-extensions 25, xrRender 11 | 8 bytes on arm64 Apple; fix where it meets a file format, a struct, or `DWORD` |
| `%ld`/`%lu`/`%I64u` format strings | xrCore 24, xrGame 17 (7 files), xrEngine 6 | wrong width or unknown conversion |
| pointer to 32-bit casts | xrGame 26 (12 files), xrCore 12, xrRender 8, lua-extensions 4 | `(u32)ptr`, `(DWORD)this`; truncate on 64-bit. MSVC x64 has the same problem, so many are already fixed; verify each |
| `wchar_t` is 32-bit on Apple | xrServerEntities 64 (2 files), xrEngine 15, xrNetServer 11, xrCore 13, xrSound 10 | do not serialize `wchar_t`; keep UTF-8 internally |
| `__int64`, `__w64`, `__int32` | `OPC_Types.h`, `ISpatial.h:137` | `int64_t`, plain `unsigned` |
| `__declspec(...)` (align, dllimport, dllexport, noinline, thread) | xrCDB 9, xrEngine 8, xrSound/xrNetServer/xrParticles/xrPhysics/xrCPU_Pipe 2-3 each | macro to `alignas`/empty (`-fdeclspec` also accepts `align`) |
| `__stdcall`/`__cdecl` | xrServerEntities 127 (`xrEProps.h` 57), xrEngine 35, xrPhysics 22, xrCPU_Pipe 18 | ignored by clang on arm64 with a warning; macro to empty |
| `#pragma comment(lib, ...)` | xrEngine 42, others | ignored; remove or `-Wno-unknown-pragmas` |
| `#pragma hdrstop` | 286 files outside `3rd party` | ignored by clang (use CMake PCH) |
| `#pragma optimize("s", on)` | 155 xrGame files | ignored by clang |
| `/permissive` reliance (dependent-base lookup, template-parameter shadowing, forward-declared enums, missing `typename`) | xrCore containers, about 30 template headers in xrGame/xrServerEntities | measured in 7.4 |

### 2.4 Include hygiene (measured)

| Module | `#include` lines | Backslash | Case mismatch |
|---|---|---|---|
| xrGame | 13,601 | 91 | 2,023 |
| Layers/xrRender | 710 | 39 | 157 |
| xrPhysics | 621 | 3 | 169 |
| xrEngine | 589 | 7 | 136 |
| Layers/xrRenderPC_R4 | 267 | 3 | 64 |
| xrCDB / xrServerEntities / xrNetServer / xrSound / xrRenderDX10 / xrCore | 145 / 778 / 53 / 112 / 152 / 467 | 0 / 4 / 1 / 2 / 2 / 9 | 19 / 14 / 14 / 11 / 8 / 10 |
| **Total** | | **161** | **2,626** |

Backslash includes break the Apple build and must be fixed (a mechanical `sed`, one owner per module).
Measured in 7.4: `-fms-extensions` makes clang accept them, so the hygiene pass is optional if that flag is on.
Case mismatches build on case-insensitive APFS. Do not mass-fix them in wave 2 (churn, merge conflicts with upstream);
revisit only if CI runs on a case-sensitive volume. The CMake source lists must be generated with case-insensitive path matching.

### 2.5 Intrinsics, asm, SEH (measured, comments and `#if 0` stripped)

| Item | Live occurrences | Action |
|---|---|---|
| x86 inline asm | `NET_Compressor.cpp:467` (`int 3`, debug path), `xrCore` (separate session) | `__builtin_trap()` |
| Dead asm | `xrSkin2W_SSE.cpp` (53), `OPC_FPU.h` (24 macro lines), `xrTheora_Surface_mmx.cpp`, `Level_Bullet_Manager.cpp:1322`, `doug_lea_memory_allocator.c` (`__asm__` bsr/bsf, file is not compiled) | ignore |
| SSE intrinsics | `xrCDB_ray.cpp` (44), `ISpatial_q_ray.cpp` (41), `particle_actions_collection.cpp` (81), `noise.cpp` (2), `Layers/xrRender/ParticleEffect.cpp` (72), `DetailManager.cpp` (1), `xrCPU_Pipe/PLC.cpp` (9, dead for us) | `sse2neon` |
| SEH (`__try/__except`) | `x_ray.cpp` (stack-overflow filter); the rest is `#ifdef DEBUG` or `#if 0`: `physics_game.cpp:180`, `script_game_object.cpp:1388`, `xr_object_list.cpp:133` | drop |
| `_control87`/`_controlfp`/`_clearfp` | xrCore only (`_math.cpp`), plus `OPC_Settings.h`/`OPC_TriBoxOverlap.h` | xrCore session |

Define `NDEBUG`, not `DEBUG`, for the first builds. It removes most `D3DCOLOR` debug draws, the SEH wrappers and debug-only code.

`sse2neon` is a submodule of OpenXRay (`https://github.com/DLTcollab/sse2neon.git`, `Externals/sse2neon`), but **the local reference checkout has that directory empty**. Pin it as `external/sse2neon` (single header) in P0, like the other deps in `cmake/Dependencies.cmake`. OpenXRay includes it per file
(`#include "sse2neon/sse2neon.h"`) in `xrCDB_ray.cpp`, `ISpatial_q_ray.cpp`, `particle_actions_collection.cpp`,
`noise.cpp`, `Layers/xrRender/ParticleEffect.cpp`, `DetailManager.cpp`: the same six files.
Note the local `__forceinline __m128 _mm_load_fvector(...)` helpers in `particle_actions_collection.cpp:1683` compile unchanged on top of sse2neon.

### 2.6 Threading and TBB

| Use | Where | Replacement |
|---|---|---|
| `tbb::parallel_for_each(flist, processFile)` | `xrSound/SoundRender_Core_SourceManager.cpp:70` | `xr_parallel_for_each` |
| `tbb::parallel_for_each(m_textures, ...)` x2 | `Layers/xrRender/ResourceManager.cpp:388, 398` | same; **iterates a `map`**, so copy to a vector first for GCD. The body calls `CTexture::Load()` which creates D3D11 resources: the Metal D3D11 shim must allow concurrent `Create*` calls |
| dead `<tbb/...>` includes | `xrParticles/particle_actions_collection.cpp:7-8`, `Layers/xrRender/ParticleEffect.cpp:5-6`, `Layers/xrRender/HOM.cpp:12-13` | delete |
| PPL in `xrCore/_thread_types.h` | `xr_task_group`, `xr_concurrent_*`, `xr_parallel_*` unused; `xr_atomic_u32` used by `xr_resource.h`, `xrsharedmem.h`, `intrusive_ptr.h` | keep the atomics, delete the rest |
| `ttapi` spin pool | `xrCPU_Pipe/ttapi.cpp`, used only for threaded 4-weight skinning | `dispatch_apply`, or drop MT |
| `thread_spawn()` | 10 call sites (`xrCDB` build thread, FS-notify and checksum threads, 3 `slowdown`, freeze, secondary, Discord, network time sync) | xrCore session maps to `std::thread` |

Reusable from OpenXRay: `src/xrCore/Threading/ParallelFor.hpp`, `ParallelForEach.hpp`, `TaskManager.{hpp,cpp}`
(a worker pool; no TBB). Either use that or a GCD wrapper. Ownership: the helper lives in xrCore (shim session), the
three call sites in the owning module sessions.

### 2.7 Things that look portable but are not

- **`DIK_*` numbering is an Anomaly gamedata contract.** Lua (`DIK_keys`, MCM, `bind_stalker`) and `user.ltx` use
  the DirectInput scancode values. OpenXRay replaced them with SDL scancodes everywhere (`xrEngine/xr_input.h`,
  `key_binding_registrator_script.cpp`), which would break Anomaly scripts. Keep `DIK_*` as an enum and put a
  table `SDL_Scancode -> DIK` in the input layer (3.9).
- **`fast_dynamic_cast`** (`3rd party/fast_dynamic_cast`, 40+ uses) reads the MSVC vtable layout, but the header
  guards it with `_MSC_VER >= 1800` and falls back to `#define fast_dynamic_cast dynamic_cast` (line 164). Safe on
  clang as is. Do not define `_MSC_VER`.
- **`xrCore/xrMemory_subst_msvc.h`** replaces global `new/delete` via MSVC CRT hooks (xrCore session).
- **`LuaJIT`:** `3rd party/luajit-2/src` is a modified in-tree copy (it has `lj_allocprof.c`); the CMake build uses a pinned
  upstream 2.1 in `external/LuaJIT`. `xrServerEntities/script_storage.cpp` calls `luaJIT_setmode(... LUAJIT_MODE_ENGINE ...)`
  and `luabind::set_allocator`. Diff the in-tree copy against upstream before assuming they are interchangeable (LuaJIT session).
- **ODE:** `xrPhysics` reaches into ODE internals (`ode/src/collision_std.h` in `dRayMotions.cpp`, `offsetof(dContact, geom)`,
  196 lines using `dx*` structs). Use the Anomaly-patched `3rd party/ode` (77 files, `dSINGLE`), not OpenXRay's `Externals/ode` and not upstream ODE, without a diff.

### 2.8 C++ exceptions, RTTI and floating point (project settings, measured)

- Every MSVC project (`xrGame`, `xrEngine`, `xrCore`, `xrPhysics`, R4, luabind) sets `<ExceptionHandling>false</ExceptionHandling>` and luabind defines
  `LUABIND_NO_EXCEPTIONS`; `XRAY_EXCEPTIONS` is 0 in release. RTTI stays on (`dynamic_cast` and `typeid` are used). `FloatingPointModel` is `Precise`.
- The source still contains `try`/`catch` (xrGame 38/50, xrServerEntities 24/28, xrEngine 13/14, Layers 9/9) and `throw` (xrGame 25). MSVC accepts these with
  exceptions "off" (warning C4530); **clang with `-fno-exceptions` rejects `try`**. Build with clang's default (`-fexceptions`).
  This is also likely required for correctness (not measured; the LuaJIT session should confirm): LuaJIT documents external frame unwinding on arm64 and x64, so C++ frames between a `lua_error` and its
  `lua_pcall` (luabind trampolines, script exports) need unwind tables and cleanups. Keep `LUABIND_NO_EXCEPTIONS`.
- Do not carry `/EHs-c-`, `/Zc:__cplusplus` or `_SECURE_SCL=0` over; `_HAS_ITERATOR_DEBUGGING=0` and `_SECURE_SCL=0` are MSVC STL switches that clang/libc++ ignores.
- Floating point: `/fp:precise` does not contract multiply-add, while Apple clang defaults to `-ffp-contract=on` on arm64. See 7.4 for the recommended flags.

## 3. Module by module

Format per module: size, Windows/DirectX/COM, MSVC-isms, asm and SSE, third-party libraries, dependencies,
effort, OpenXRay comparison. "Shim" means the xrCore compat header.

### 3.1 xrCDB (collision database, OPCODE) | 20.2K LOC, 86 files, 40 `.cpp` | effort S (0.5 d)

- **Windows/DX/COM:** `Opcode.h` includes `<windows.h>`, `<windowsx.h>`, `<float.h>`. `d3d9types.h` and `D3DCOLOR_XRGB` in debug drawing
  (`xr_area.cpp:160`, `xr_area_raypick.cpp:187`; `RCache.dbg_Draw*`). One `Sleep`. `thread_spawn` (xrCore) for the model build thread.
- **MSVC-isms:** `ALIGN(a) __declspec(align(a))` (`xrCDB.h:19`), `_MM_ALIGN16` (2 files), `typedef __w64 unsigned ptrt` (`ISpatial.h:137`),
  `signed __int64` (`OPC_Types.h:46`), `__declspec(dllimport)` (`ISpatial.h:166`), `#pragma inline_depth(255)`, `#pragma message`
  (`OPC_Preprocessor.h`), `#pragma comment(lib, ...)` x4 (`StdAfx.cpp`), 31 `#pragma hdrstop`, `__stdcall build_callback` (`xrCDB.h:58`).
- **asm/SSE:** no live asm (all `_asm` is in comments or unused macros). SSE in `xrCDB_ray.cpp`, `ISpatial_q_ray.cpp` -> `sse2neon`.
- **Third-party:** none; OPCODE is vendored as `OPC_*.cpp`.
- **Deps:** xrCore, `xrAPI` (render debug calls, `#ifdef DEBUG`).
- **OpenXRay:** `src/xrCDB/xrCDB_ray.cpp`, `ISpatial_q_ray.cpp` (same files, `sse2neon` include). It keeps OPCODE in `Externals/OPCODE`
  instead of inlined `OPC_*` files; do not swap implementations.

### 3.2 xrXMLParser (TinyXML wrapper `CXml`) | 4.2K LOC, 8 files | effort S (0.25 d)

- Windows: `<windows.h>` + `OutputDebugString` in `tinyxmlparser.cpp`; `LPCSTR` in the API.
- MSVC-isms: `_MSC_VER` branches and `_snprintf_s`/`sscanf_s` in `tinyxml.h`; `#pragma comment(lib)` x3; dllexport macro (already empty).
- Third-party: TinyXML (vendored). Deps: xrCore `FS`.
- OpenXRay: moved into xrCore as `src/xrCore/XML/` (reference only).

### 3.3 xrParticles | 4.4K LOC, 16 files, 8 `.cpp` | effort S (0.5 d)

- SSE: `particle_actions_collection.cpp:1679` onward (81 hits) and `noise.cpp` -> `sse2neon` (OpenXRay: same file at line 1620).
- `#include "../xrCPU_Pipe/ttapi.h"` and `#pragma comment(lib,"xrCPU_Pipe.lib")` (`particle_actions_collection.cpp:1680`) are
  leftovers: no `ttapi_*` call exists in xrParticles. Dead `<tbb/...>` includes at lines 7-8.
- Other: `__forceinline` x3, `__declspec` export (empty), `xr_new`/`xr_delete` only. No third-party.
- OpenXRay: `src/xrParticles/particle_actions_collection.cpp`, `noise.cpp`.

### 3.4 xrCPU_Pipe (skinning and light-calc dispatch) | 1.3K LOC, 11 files | effort S (0.5 d)

- Live code: `xrCPU_Pipe.cpp:xrBind_PSGP` binds `xrSkin{1,2,3,4}W_x86` (plain C++ in `xrSkin2W.cpp`), `PLC_calc3_x86`, and a threaded
  `xrSkin4W_thread` through `ttapi`. The SSE block is `#if 0`; `xrSkin2W_SSE.cpp` is wholly `#if 0` (53 `__asm` lines).
- `ttapi.cpp`: hand-rolled spin pool (`CreateThread`, `WaitForMultipleObjects`, `_mm_pause`, `SwitchToThread`, a `__try/__except` for thread naming).
- Consumers: `Engine.cpp` (`bindCPU(&PSGP, ...)`, `ttapi_Done`), `Layers/xrRender/SkeletonX.cpp:183-208` (`PSGP.skin1W..4W`),
  `FSkinned.cpp`. `PLC_calc3` is used only by `Layers/xrRenderPC_R1/LightShadows.cpp:550`.
- **Plan:** do not port the library. Move `xrSkin2W.cpp` into `Layers/xrRender` (as OpenXRay did), drop `PLC.cpp` and the SSE file,
  replace `ttapi` by `dispatch_apply` or run single-threaded first, delete `PSGP`. Optional: NEON skinning later.
- **OpenXRay:** `src/Layers/xrRender/SkeletonXSkinXW.h`, `SkeletonXSkinXW_CPP.cpp`, `SkeletonXSkinXW_SSE.cpp` (intrinsics, works with `sse2neon`).
- Ownership note: this edits `Engine.cpp` (engine-core) and `SkeletonX.cpp` (renderer). Land it as part of the renderer session
  (`SkeletonX*`) with a 3-line change in `Engine.cpp` agreed with engine-core.

### 3.5 xrSound | 6.4K LOC, 34 files, 20 `.cpp` | effort S (1.5 d)

- **Windows/COM:** `NotificationClient.{h,cpp}` (WASAPI `IMMNotificationClient`, `CoInitialize`, `__uuidof`, `Interlocked*`; it only sets
  `bPendingDefaultDeviceSwitch`). `guids.cpp` (`INITGUID`, `<dsound.h>`). `stdafx.h` includes `<mmsystem.h>`, `<mmreg.h>` for
  `WAVEFORMATEX` (`SoundRender_Source.h:20`, `SoundRender_Source_loader.cpp:94`, `SoundRender_Emitter_FSM.cpp:16`). `OpenALDeviceList.cpp:30` includes `<objbase.h>`.
- **MSVC-isms:** `typedef void __stdcall sound_event` (`Sound.h:383`), `#pragma comment(lib)` x6, `_stricmp`/`stricmp`/`strlwr`.
- **Third-party:** OpenAL (`<AL/efx.h>`, `<AL/efx-presets.h>`, 654 AL/EFX token hits, device enumeration with `ALC_ENUMERATE_ALL_EXT`),
  Ogg/Vorbis (`vorbis/vorbisfile.h`), TBB (3 lines).
- **Plan:** link `external/openal-soft` (EFX is supported on its CoreAudio backend) and `external/vorbis`. Delete `NotificationClient` and `guids.cpp`;
  on iOS, route changes come from `AVAudioSession` (call `bPendingDefaultDeviceSwitch = TRUE`). Provide a local `WAVEFORMATEX`.
- **Deps:** xrCore, `xrCDB.h` (occlusion), `xrServerEntities/ai_sounds.h`.
- **OpenXRay:** `src/xrSound/` (same OpenAL design; no WASAPI notification client; adds `SoundRender_EffectsA_EAX.*`).

### 3.6 xrPhysics | 31.6K LOC, 150 files, 59 `.cpp` (+ `PHNetState.cpp` from xrServerEntities) | effort M (2 d)

- **Windows/DX:** none beyond `d3d9types.h` and `D3DCOLOR_XRGB` in debug draws (`stdafx.h`, `tri-colliderknoopc/dSortTriPrimitive.h`).
- **MSVC-isms:** `__stdcall`/`_cdecl` x22, `#pragma warning` x60, `#pragma managed(push, off)` (`xrPhysics.cpp`), `_copysign` x3, `offsetof(dContact, geom)`.
- **Third-party:** ODE, 2,075 `d*` tokens in 91 files; includes `../3rd party/ode/include/ode/*.h` by relative path, and `ode/src/collision_std.h`.
- **Deps:** xrCore, xrCDB (`CDB::MODEL`), `xrServerEntities` (`alife_space.h`), xrEngine headers (`iphysicsshell.h`, a few `Device.*`), the ODE target.
- **Risk:** it is a compile-fix job with no API work; the risk is ODE struct layout and `dSINGLE`/`dDOUBLE` consistency with the ODE session.
- **OpenXRay:** `src/xrPhysics/` (58 of 59 `.cpp` names match). Use it to read portability diffs, not as a drop-in.

### 3.7 3rd party/lua-extensions | 10.4K LOC, 59 files | effort S (0.5 d)

- Contents: `lfs.c`, `lmarshal.c`, `LibStr/LibMath/LibTab`, `script_additional_libs.cpp` (an `xrLuaFix` port: bit ops, random), `luasocket/`, `luapanda.cpp` (debugger), `keyboard.cpp`, `Logger.cpp`.
- **Windows:** `keyboard.cpp` (`GetKeyState` polled from a `_beginthread` thread, exposed to Lua as `GetKB`) -> SDL keyboard state. `luapanda.cpp:969`
  (`GetProcAddress` x27 on a Lua DLL, behind `!USE_SOURCE_CODE && _WIN32`) -> exclude. `luasocket/wsocket.c` -> use `usocket.c` (already in the tree). `lfs.c` already has `_WIN32` and POSIX branches.
- **MSVC:** `__declspec(dllexport)` x3, `_itoa` in `script_additional_libs.cpp` (bit.tobit/tohex; also leaks: `new char[]`), `#pragma comment`.
- **OpenXRay:** has `Externals/xrLuaFix` and `src/xrScriptEngine` for the Lua glue; `luapanda` and `keyboard` have no counterpart.

### 3.8 xrNetServer | 4.7K LOC, 19 files, 8 `.cpp` | effort M (1.5 d)

- **Windows/COM:** DirectPlay 8 through COM: `CoCreateInstance(CLSID_DirectPlay8Client/Server/Address)`, `IDirectPlay8*`, `DPN_*`, `DPNSEND_*`
  (153 COM hits; `NET_Client.cpp` 86, `NET_Server.cpp` 56). `dxerr.h` / `DXGetErrorString9` (6), `gethostbyname` (`NET_Client.cpp:1219`), `#pragma comment(lib, "Ws2_32.lib")`.
  Live SEH x2 in `NET_Client.cpp`; `__asm { int 3 }` in `NET_Compressor.cpp:467`.
- **Why it cannot simply be dropped:** single-player runs a server and a client in one process. `CLevel` derives `IPureClient`, `xrServer` derives `IPureServer`,
  and `NET_Packet` is used by all game objects. `IPureServer::Connect` sets `psNET_direct_connect = TRUE` when the options contain `/single` (`NET_Server.cpp:250`); that is the in-process path SP uses. Only the transport is dead weight.
- **Plan:** use OpenXRay's `src/xrNetServer/empty/` backend (`NET_Client.{h,cpp}` 605+167 LOC, `NET_Server.{h,cpp}` 694+320 LOC), which keeps
  the direct-connect path and deletes DirectPlay; keep `NET_Common.cpp`, `NET_Compressor.cpp`, `NET_Log.cpp`, `NET_AuthCheck.cpp` after dropping `dxerr`.
  Provide a header with the few `DPN*` types xrGame still names (section 2.2). Diff Anomaly's `NET_Server.h` first: it has extra MP members that `xrServer`/`game_sv_*` use.
- **Deps:** xrCore only, plus `xrGame` consumers (`Level.h`, `xrServer.h`, `game_sv_base.h`).
- **OpenXRay:** `src/xrNetServer/empty/*` and its `CMakeLists.txt` (shows which files form the stub library).

### 3.9 xrEngine (Application) | 52.8K LOC, 187 files, 91 `.cpp` (+14 Discord files) | effort L (8 d, two sessions)

Windows/DirectX/COM hot spots, measured by real API tokens:

| File | LOC | What |
|---|---|---|
| `x_ray.cpp` | 2031 | `WinMain` (line 1251) + `WinMain_impl`; single-instance mutex, DPI awareness, `SystemParametersInfo` for screensaver/sticky keys, startup dialog `IDD_STARTUP`, `HeapSetInformation`, stack-overflow SEH, Discord Rich Presence (needs ICU `ucnv_*`), `slowdownthread` |
| `device.cpp`, `Device_Initialize.cpp`, `Device_create.cpp`, `Device_destroy.cpp`, `Device_wndproc.cpp`, `Device_Misc.cpp` | about 1.7K+ | `WNDCLASS`/`CreateWindowEx`, `WndProc`, `ShowCursor`/`ClipCursor`/`SetWindowPos`, monitor handling, D3D9 render-state leftovers (`Device_create.cpp` 33 D3D hits) |
| `Xr_input.cpp` (783), `IInputReceiver.cpp`, `xr_input_xinput.*` (128; the plan notes it is commented out) | | DirectInput 8 keyboard/mouse (`DirectInput8Create`, `IDirectInputDevice8`), `ToUnicodeEx`, `VK_*` |
| `line_edit_control.cpp` (865), `imgui_helper.h` (106 `DIK_`), `FDemoRecord.cpp` (48), `XR_IOConsole_callback.cpp` (37) | | `DIK_*` key tables; `GetKeyState` |
| `imgui_base_input.cpp`, `imgui_base.cpp` | | Win32 keyboard layout, clipboard, `WM_*`/`VK_*` mapping |
| `MonitorList.cpp` (425) | | `EnumDisplayMonitors`, dynamic `GetProcAddress` |
| `Text_Console*.cpp`, `mailSlot.cpp`, `xrSASH.*` (OpenAutomate) | | Win32 console window, mailslot IPC, automation hooks |
| `tntQAVI.cpp` (473) | | VFW/`mmio*` AVI player; only used by `Layers/xrRenderDX10/dx10SH_Texture.cpp:473` for `.avi` textures |
| `xrTheora_Surface*.cpp`, `xrTheora_Stream.cpp`, `GIFResource.cpp` | | libtheora CPU YUV->RGB (`_mmx.cpp` is `#if 0`), giflib |
| `EngineAPI.cpp` (443), `Engine.cpp` | | 19 `#pragma comment(lib)`, `LoadLibrary` strings for DLL renderers (R4 and xrGame are static), PSGP binding |
| `shader_bus.cpp`, `fmesh.cpp`, `Rain.cpp`, `Environment_render.cpp`, `thunderbolt.cpp`, `StatGraph.cpp`, `_d3d_extensions.h` | | D3D9 `D3DFVF_*`/`D3DPT_*` and `D3DXGetDecl*` |

- **Windows function inventory:** `ZeroMemory` x58, `CopyMemory` x28, `_alloca` x16, `stricmp` x14, `Sleep` x12, `GetClientRect` x12, `GetProcAddress` x11, `ShowCursor` x10, `ClipCursor` x9,
  `_finite` x8, `LoadLibrary` x4, `SetWindowPos`, `GetWindowRect`, `timeGetTime` and ~25 singletons.
- **MSVC-isms:** `WinMain`/`APIENTRY`/`WINAPI`, `__stdcall`/`__cdecl` x35, `__declspec` x8, `#pragma comment` x42, one `thread_local` (fine).
- **Third-party:** ImGui (`3rd party/imgui`, builds as is), Discord SDK + ICU, giflib, libtheora/ogg/vorbis, DirectInput/XInput, OpenAutomate, fastdelegate, luabind (3 files),
  `fast_dynamic_cast` (37 uses; falls back).
- **Deps:** xrCore, xrCDB, xrSound, xrPhysics headers, xrParticles, renderer through `Render` and `RenderFactory` (`src/Include/xrRender/*`, `Layers/xrAPI`).
- **OpenXRay:** `src/xr_3da/entry_point.cpp` (`int main`), `src/xrEngine/{Device_Initialize,Device_create,Device_destroy,Device_mode,Device_imgui}.cpp`,
  `xr_input.{h,cpp}`, `IInputReceiver.*`, `Text_Console.cpp`. It uses SDL for window and input: borrow the structure, not the key numbering.

**Split into two sessions** (disjoint files):

- `engine-core`: everything not listed under input or media. Replace `WinMain` with an SDL3 `main`/`SDL_main` entry point that the `ios-shell` session
  owns (the engine entry is `xrEngine_Main(argc, argv)`), window creation (`HWND` becomes an opaque pointer to the `SDL_Window`), delete the DLL-loading branches
  of `EngineAPI.cpp`, stub Discord/ReShade/SASH/mailslot/Text_Console, `Engine.cpp` PSGP removal.
- `engine-input`: `Xr_input.cpp`, `IInputReceiver.*`, `xr_input.h`, `xr_input_xinput.*` (delete), `line_edit_control.cpp`, `imgui_base_input.cpp`, `imgui_helper.h`, `FDemoRecord.cpp`
  key paths, `MonitorList.*`, plus media (`xrTheora_*`, `GIFResource`, delete `tntQAVI`). The gamepad session adds its own new files and hooks through one agreed seam in `CInput`.

### 3.10 xrGame | 454.7K LOC, 2,716 files, 1,188 `.cpp` | effort XL (about 35 session-days, parallel)

Section 7 is the strategy. In short: about 25 Win32 function names (listed in 2.2), 0 live asm, SEH only in debug paths, 301 files include luabind
(the script exports), 6 files reference Discord, 2 reference ReShade, 319 files (163 `.cpp`) are MP/GameSpy/anti-cheat by project filter, and `DPlay/dplay8.h` is in `stdafx.h`.
- **Third-party in xrGame:** luabind, LuaJIT, ImGui (`script_imgui_script.cpp`, 1,009 ImGui tokens in 8 files), `fastdelegate`, `sdk/include/sm_boost` (a tiny legacy boost subset),
  OpenAutomate (1 file: `smart_cover_animation_selector.cpp`), and for MP only: `crypto` (24 files), CxImage/JPEG (4 files), DirectPlay types.

### 3.11 xrServerEntities | 35.6K LOC, 165 files; 58 `.cpp` compiled into xrGame | effort M (3 d, inside session 12)

- **What it is:** server-side entity classes (`xrServer_Objects*`, `object_factory*`), the script engine (`script_engine.cpp`, `script_storage.cpp`, `script_thread.cpp`,
  `script_process.cpp`), the Lua bindings of math/ini/net-packet types (`script_*_script.cpp`), and debugging (`script_debugger*`, `lua_studio.cpp`).
- **Windows:** `lua_studio.cpp` + `script_engine.cpp:64` (`LoadLibrary(CS_LUA_STUDIO_BACKEND_FILE_NAME)`, `GetProcAddress("_cs_lua_studio_backend_create_world@12")`),
  `script_debugger.cpp` / `mslotutils.h` (mailslot `CreateFile`, `WM_USER`, `SendMessage`), `shlwapi.h` in 2 files, `MultiByteToWideChar` x1, `SetErrorMode`, `_stricmp` x12 (`alife_space.h`), `_alloca` x6.
  `doug_lea_memory_allocator.c` (dlmalloc with `VirtualAlloc`, `WIN32`) is not compiled and not used (every instantiation is commented out).
- **Plan:** define `USE_DEBUGGER` off (removes `lua_studio` and `script_debugger`); keep everything else. `setup_luabind_allocator()` and the LuaJIT open-libs sequence stay.
- **Third-party:** LuaJIT, luabind (45 files), fastdelegate. `smart_cast*.h` uses `fast_dynamic_cast`.
- **OpenXRay:** `src/xrScriptEngine/*` and `src/xrServerEntities/*` (53 of 61 names match); `script_engine.cpp`, `script_storage.cpp` show the Linux-safe form of the debugger and allocator guards.

### 3.12 Layers (renderers) | xrRender 61.4K, xrRenderDX10 14.8K, R4 21.6K, xrAPI 0.1K; R1 6.1K, R2 13.1K, R3 17.5K, DX9 0.6K

- **Port only** `xrAPI`, `xrRender`, `xrRenderDX10`, `xrRenderPC_R4` (R4 project list, section 8). R1/R2/R3/DX9 stay in the tree, unbuilt.
- **Windows/DirectX:** D3D11 interface use in 77 files (656 hits), DXGI in 5 files (`dx10HW.cpp` 60, `dxRenderDeviceRender.cpp` 6, `r2_test_hw.cpp` 5, `xr_effgamma.cpp` 3, `xrRender_R4.cpp` 1),
  D3DX11 in 6 files, `D3DCompile`/`D3DReflect` in `r4.cpp`, NvAPI in `HWCaps.cpp`, window in `dx10HW.cpp` (32 hits). Details in section 8.
- **MSVC-isms:** `#include <tbb\parallel_for_each.h>` (backslash, `ResourceManager.cpp:16`); `blenders\blender.h` in `R4/stdafx.h`; 39 backslash includes in `xrRender`; `__declspec(align)` x8; `__forceinline` x2.
- **asm/SSE:** `ParticleEffect.cpp` (72 intrinsics), `DetailManager.cpp` (1). `doug_lea_memory_allocator.c` appears under `xrRender` (not compiled).
- **Third-party:** ImGui (`dxImGuiRender.cpp`, uses `ImGui_ImplDX11_*`), luabind (blender/shader scripts: `ResourceManager_Scripting.cpp`, `dx10ResourceManager_Scripting.cpp`), D3DX11, NvAPI, TBB.
- **OpenXRay:** `src/Layers/xrRender` (105 of 129 `.cpp` names match), `xrRenderPC_R4` (32 of 74), `xrRenderDX10` has no counterpart (they use `xrRenderDX11`, `xrRenderGL`).
  Reusable pieces: `SkeletonXSkinXW_*.cpp`, `Layers/xrRender` shared code with GL guards, `Layers/xrRenderGL` as an example of a non-D3D backend behind the same `Render` interface.
- Effort: M to L only for the glue (5 d); the Metal backend itself is the metal-d3d11 session.

## 4. What can be excluded or stubbed on Apple

| Item | Where | Size | Action |
|---|---|---|---|
| Multiplayer game types, UI, award system, accounts, file transfer, anti-cheat, MP screenshots | xrGame (163 `.cpp`, 319 files) | 53.2K LOC | compile out leaf sources; keep base classes (`game_sv_GameState`, `game_cl_GameState`, `xrServer`) and the SP game type; stub factory registrations (session 17) |
| GameSpy | `xrGame/gamespy/CdkeyDecode/*.c`, `xrGameSpyServer.*`, `Level_GameSpy_Funcs.cpp`, `login_manager`, `ui/ServerList` | 2.7K LOC | the SDK is absent and calls are already commented out; exclude files, stub `CGameSpy_*` references |
| DirectPlay transport | `xrNetServer/NET_Client.cpp`, `NET_Server.cpp`, `sdk/include/DPlay/*` | 2.5K LOC | OpenXRay `empty/` backend |
| Discord Rich Presence | `x_ray.cpp:340-670`, `device.cpp` thread, `defines.h`, 6 xrGame files, `dxApplicationRender.cpp`, `LocatorAPI.cpp` (comment), `3rd party/discord` (14 `.cpp` compiled into xrEngine), SDK dll | 12 files + SDK | stub `updateDiscordPresence()`/`Init_Discord()`; keep `discord_gameinfo`, `discord_strings`, `rsDiscord` flag so scripts still link |
| ICU | `3rd party/icu` (119K LOC vendored), `x_ray.cpp` (`ucnv_*`, Discord only) | | drop with Discord |
| ReShade | `ReShadeCompat`, `3rd party/reshade`, `use_reshade` + 3 externs in `dx10HW.cpp`, `dxApplicationRender.cpp`, `MainMenu.cpp`, `Level.cpp`, `x_ray.cpp`, `Device_destroy.cpp` | 6 call-site files | define `bool use_reshade = false`, stub `init_reshade/unregister_reshade/render_reshade_effects` |
| optick | `xrCore/profiler.h` (10 macro uses) | | `OPTICK_*` to empty |
| stackwalker | `xrCore/xrDebugNew.cpp` (6) | 273 LOC | `backtrace()`; OpenXRay `xrCore/Debug/StackTrace.cpp` |
| DXERR | `3rd party/DXERR` (4K LOC), 15 refs in xrCore/xrEngine/xrNetServer | | OpenXRay `xrCore/Debug/dxerr.{h,cpp}` + `DXGetError*.inl`, or hex-only text |
| NvAPI | `Layers/xrRender/HWCaps.cpp`, pragma in `EngineAPI.cpp`, `sdk/include/nvapi` | 8 hits | `NvAPI_Initialize` returns failure |
| NVTT | `3rd party/NVTT` (36.8K LOC) | | no engine reference; do not build |
| crypto / DSA signing / SHA | `3rd party/crypto` (OpenSSL subset), used by `xr_dsa_*`, `stats_submitter*`, `configs_dump*`, `secure_messaging`, `screenshots_*` (24 xrGame files) | | MP only; goes with the MP exclusion |
| cximage + JPEG | `xrGame/screenshot*`, `screenshots_writer*` (MP), `UIServerInfo.cpp` | | MP only; in-engine screenshots use D3DX11 (see section 8) |
| VFW AVI player | `xrEngine/tntQAVI.*`, `dx10SH_Texture.cpp:473` | 0.6K LOC | stub `CAviPlayerCustom`; `.avi` textures unsupported |
| OpenAutomate SASH, mailslot, Text_Console, startup dialog | `xrSASH.*`, `mailSlot.cpp`, `Text_Console*.cpp`, `IDD_STARTUP` | | stub |
| Lua Studio, script debugger, luapanda | `xrServerEntities/lua_studio.cpp`, `script_debugger*.cpp`, `lua-extensions/luapanda.cpp`, `sdk/include/cs` | 4K LOC | `USE_DEBUGGER` off |
| `doug_lea_memory_allocator.c` | xrServerEntities, `Layers/xrRender` | 5K LOC | not compiled today; keep it so |
| Registry, CD key | `xrGame/RegistryFuncs.cpp`, `ui/UICDkey`, `gamespy/CdkeyDecode` | | MP only |
| `xrCPU_Pipe` SSE and `PLC_calc3`; R1, R2, R3, DX9 renderers | | 37K LOC + 1K | do not port |
| BugTrap / minidump | `NO_BUG_TRAP`, `USE_OWN_MINI_DUMP` in `build_config_defines.h`; `xrDebugNew.cpp` | | replace crash handler in xrCore (separate session) |

## 5. Replacements for libraries that are needed

| Library or API | Used by | Replacement on Apple |
|---|---|---|
| TBB (`parallel_for_each`) | xrSound, xrRender `ResourceManager` | `xr_parallel_for_each` on GCD `dispatch_apply` (copy map to vector first), or OpenXRay `xrCore/Threading/ParallelForEach.hpp` |
| PPL / `concurrency::` | `xrCore/_thread_types.h` | delete (unused) |
| `ttapi` thread pool | xrCPU_Pipe | `dispatch_apply` or single-thread |
| SSE intrinsics | 6 files (section 2.5) | `sse2neon` (`DLTcollab/sse2neon`, pin as `external/sse2neon`; the local OpenXRay copy is empty), included per file |
| DirectInput 8, XInput | `Xr_input.cpp`, `xr_input_xinput.cpp` | SDL3 keyboard/mouse; GameController (gamepad session); scancode -> `DIK_*` table |
| WASAPI, DirectSound, `mmsystem` | xrSound | OpenAL Soft on CoreAudio; `AVAudioSession` route notifications |
| Win32 window, monitors, console | xrEngine | SDL3 window + Metal view; `SDL_GetDisplays` replaces `MonitorList` |
| D3DX11 texture load/save/filter | `dx10Texture.cpp` (create from memory, image info), `r__screenshot.cpp` (save JPG/PNG/BMP/DDS), `r4_rendertarget.cpp:1286` (`FilterTexture`) | own DDS reader into BC1-BC7 shim textures; ImageIO or stb for screenshots; `GenerateMips` |
| `D3DCompile`, `D3DReflect`, `D3DDisassemble`, `ID3DInclude` | `r4.cpp` (shader compile at lines 2093, reflection at 1017-1149, disassembly 1225, `includer` class 1240) | shader-xlat session API; reflection from SPIRV-Cross or DXC |
| DXGI (factory, adapter, output, swap chain, gamma, HDR color space) | `dx10HW.cpp`, `xr_effgamma.cpp` | Metal D3D11 shim; `CAMetalLayer` from the SDL window |
| ImGui DX11 backend | `dxImGuiRender.cpp` | runs through the D3D11 shim, or a Metal backend (ImGui ships `imgui_impl_metal`) |
| libtheora, Ogg, Vorbis | xrEngine, xrSound | already pinned in `external/` and `cmake/deps/` |
| giflib | `GIFResource.cpp` | build `3rd party/giflib` as is (portable C) |
| `_beginthread`, `CreateThread` | `lua-extensions/keyboard.cpp`, `ttapi` | `std::thread` / `pthread` |

## 6. Order, grouping and file ownership for wave 2

### 6.1 Dependencies

```
P0 xrCore shim + CMake skeleton
 |
 +-- 1 leaf-libs ------+
 +-- 2 sound           |  (link-independent; only need the shim)
 +-- 3 physics  <-- ODE target (luabind-ode session)
 +-- 4 netserver
 +-- 8 xrgame-triage --> 9 .. 17 xrgame shards  (compile -fsyntax-only needs only the shim and headers)
 +-- 5 engine-core  <-- ios-shell (SDL3 entry, window)
 +-- 6 engine-input <-- gamepad-input (seam in CInput)
 +-- 7 renderer-r4  <-- metal-d3d11 (D3D11 headers/shim), shader-xlat (D3DCompile replacement)
 12 script-entities <-- luabind, LuaJIT targets
Integration (M1): link everything; needs 1..7 and 9..17.
```

xrGame sessions do not need the other libraries to build; they only need headers (`xrEngine/*.h`, `xrPhysics/*.h`, `xrServerEntities/*.h`).
A shard session verifies with `ninja -k0 <shard-target>` or `clang -fsyntax-only`, and the link happens at integration.

### 6.2 Ownership rules (to avoid merge conflicts)

1. **Directory ownership:** a session edits only the files it owns (tables in 1 and 6.3). A needed change outside the owned set goes into the owner's
   inbox (a note in the shared notes folder), not into a drive-by edit.
2. **Shared headers are single-owner.** `xrCore/**` and the compat header: xrcore-shim. `xrGame/stdafx.h`, `xrGame/pch_script.h`, `xrEngine/stdafx.h`,
   `build_config_defines.h`, `Include/**`: xrgame-triage (it stays alive as the header owner for the whole of wave 2).
3. **CMake:** one file per module, `cmake/targets/<module>.cmake`, owned by that module's session. Only P0 edits the top-level `CMakeLists.txt` and `cmake/AnomalyPlatform.cmake`.
4. **Mechanical repo-wide edits happen first and once:** the backslash-include fix (161 lines) can be applied per module by its owner (files do not overlap) as the first commit of each session.
5. **Platform `#if`s:** use one macro in the compat header (for example `XR_PLATFORM_APPLE`, `XR_PLATFORM_WINDOWS`) so the Windows build keeps working. Do not scatter `__APPLE__`.
6. **No reformatting.** Upstream merges (themrdemonized) must stay cheap; keep diffs minimal and avoid mass case fixes.

### 6.3 File ownership sets for the non-xrGame sessions

| Session | Files |
|---|---|
| leaf-libs | `src/xrCDB/**`, `src/xrXMLParser/**`, `src/xrParticles/**`, `src/xrCPU_Pipe/**` |
| sound | `src/xrSound/**` |
| physics | `src/xrPhysics/**` (not `3rd party/ode`) |
| netserver | `src/xrNetServer/**` (+ a new `dplay8` stub header there) |
| engine-core | `src/xrEngine/**` except the engine-input list; `src/Include/**` |
| engine-input | `Xr_input.*`, `xr_input.h`, `xr_input_xinput.*`, `IInputReceiver.*`, `line_edit_control.*`, `edit_actions.*`, `imgui_base_input.cpp`, `imgui_helper.*`, `MonitorList.*`, `xrTheora_*`, `GIFResource.*`, `tntQAVI.*`, `FDemoRecord.cpp` (key paths only) |
| renderer-r4 | `src/Layers/xrAPI/**`, `xrRender/**`, `xrRenderDX10/**`, `xrRenderPC_R4/**` (+ the 2 Lua files compiled into R4 are owned by xrgame-script-entities and only read here) |

## 7. xrGame strategy

### 7.1 What the code looks like

- About 1,185 `.cpp` in 4 trees (`xrGame/`, `ai/`, `ui/`, `ik/`) but a **flat** top level of 855 files. The project's own virtual folders
  (`xrGame.vcxproj.filters`) are the only real structure: they cover 2,712 of 2,721 files on disk when matched case-insensitively.
- Heaviest groups by LOC: `Core\Client` 123K, `AI\AMonsters` 94K, `Core\Server` 48K (40K of it MP game types), `AI\AComponents` 30K, `UI\Common` 28K, `AI\AScript` 20K, `UI\UIWindows` 20K.
- 301 files include luabind (script exports: `*_script*.cpp`, 105 files, 11.7K LOC at the top level, 21.7K more in `AScript\ScriptClasses`). They are template-heavy and slow to compile.
- The Win32 surface is small (about 25 functions); the volume is in the C++ dialect, in the `D3DCOLOR` debug code, in `DPlay` types, and in 2,023 case-mismatched and 91 backslash includes.
- Compile flags in `xrGame.vcxproj`: C++17, PCH (`stdafx.h`), `dSINGLE`, `MSVC`, `_SECURE_SCL=0`, `XRGAME_EXPORTS`.

### 7.2 Recommended split

**Step 0, one session, serial (`xrgame-triage`, about 3 days):**
- Build the PCH (`xrGame/stdafx.h` includes `DPlay/dplay8.h`, `../xrEngine/*`, `pch_script.h`) with CMake `target_precompile_headers`.
- Generate the CMake source list from `xrGame.vcxproj` (case-insensitive path matching; exclude the 5 `ExcludedFromBuild` entries and `ScriptDebugger`/`lua_studio`).
- Apply the backslash-include fix to `xrGame/` (91 lines).
- Build with `ninja -k0` and bucket the errors (the probe in 7.4 did this for a sample); fix the shared template headers listed in 7.4 first.
- Land all mechanical fixes that cut across shards (macros in the compat header, `dplay8` stub, `D3DCOLOR` header) and the shared template headers listed in 7.4 so that shard sessions start on a tree that parses.

**Step 1, in parallel, by project filter (disjoint file sets, from `xrGame.vcxproj.filters`):**

| Shard | Filter prefixes | `.cpp` | LOC |
|---|---|---|---|
| `xrgame-ai-monsters-a` | `AI\AMonsters\common` | 71 | 37.3K |
| `xrgame-ai-monsters-b` | `AI\AMonsters\{humans,monsters}` | 159 | 56.9K |
| `xrgame-ai-core` | `AI\{AComponents,AGroup,ACover,AEval,ADebug,ASpace,ASound}` | 99 | 39.7K |
| `xrgame-nav-alife` | `AI\{ANavigation,ALife}` | 68 | 29.9K |
| `xrgame-script-entities` | `AI\AScript` (+ the 58 `xrServerEntities` `.cpp`) | 60 + 58 | 20.3K + 17.6K (`.cpp` only; 35.6K with headers) |
| `xrgame-ui` | `UI\*` except `Multiplayer`, `GameTypes\{CaptureTheArtefact,Deathmatch,TeamDeathmatch}` | 142 | 53.8K |
| `xrgame-objects-items` | `Core\Client\Objects\{items & weapons,inventory}` | 146 | 49.9K |
| `xrgame-objects-world` | `Core\Client\Objects\*` (other) | 112 | 49.1K |
| `xrgame-core-physics` | `Core\{Client other,Server SP,Common}`, `Physics\*` | 167 | 64.6K |
| `xrgame-mp-stub` | MP/GameSpy filters (`Multiplayer`, `Games\{mp,deathmatch,...}`, `PlayerAccounts`, `anticheat`, `award_system`, `GameSpy`, `SecureMessages`, `filetransfer`) | 163 | 53.2K (excluded) |

The assignment is deterministic. The appendix has a 30-line script that reads the filters and prints each shard's file list, so a session
does not reconstruct it by hand. About 9 files that are not in the filters (for example `ai/monsters/chimera/chimera_attack_state_inline.h`, `graph_abstract*.h`,
`Explosive.h`) go to the shard of their directory or includer.

Rationale for splitting by project filter rather than by kind of fix: errors cascade through headers, so a "fix everything of kind X" session would touch every shard's files
and conflict with all of them. Mechanical, kind-based fixes are done once, up front, by the triage session. After that, the remaining errors are per-file and local, and ownership by directory is conflict-free.

**Step 2, integration:** the triage session re-runs the full `-k0` build, owns the header queue, and hands the link to the M1 integration session.

### 7.3 Use OpenXRay as an answer key

88% of xrGame `.cpp` names (1,045 of 1,182) exist in OpenXRay's `src/xrGame`. When a shard session hits a diagnostic at `file:line`, it should open the OpenXRay file of the same name
(`reference/xray-16/src/xrGame/<name>` or search by name: some moved to `xrAICore`, `xrUICore`, `xrScriptEngine`) and `diff` it. Most portability fixes (templates, `typename`, `this->`, `override`) were already made there; the gameplay differences
(Anomaly features) must not be copied. This is faster than rediscovering the fixes, but it is a reading aid only.

### 7.4 Clang flags and the compile probe (measured)

OpenXRay builds its Clang targets with `-fdelayed-template-parsing`, `-Wno-inconsistent-missing-override`, `-Wno-attributes`, `-Wno-unknown-pragmas` and others
(`reference/xray-16/cmake/XRay.Compiler.GNULike.cmake:41-45, 80, 121`), with a TODO to remove the first. It uses neither `-fms-extensions` nor `-fms-compatibility`.

**Probe setup.** Apple clang 21.0.0 (arm64-apple-darwin27, libc++), `-std=c++17 -fsyntax-only -ferror-limit=0 -Wno-everything`, the include dirs and defines from `xrGame.vcxproj`
(`dSINGLE`, `MSVC`, `XRGAME_EXPORTS`, `NDEBUG`), no `_WIN32`/`_MSC_VER`. The probe force-includes a 434-line prelude and about 30 stub headers that stand in for the xrCore shim, so xrGame's
own errors surface instead of "windows.h not found". Sample: 81 translation units (6.9% of xrGame, stratified: 6 anchors such as `Actor.cpp`/`Weapon.cpp`, 12 script exports, 6 alife, 14 other top-level, 8 `ai/stalker`,
8 `ai/monsters`, 4 other `ai/`, 10 `ui/`, 3 `ik/`, plus 5 `xrServerEntities` and 5 `xrPhysics`). A "port error" is a clang error located in `xrGame`, `xrServerEntities` or `xrPhysics`;
a "shim error" is one in the prelude, the stubs, `xrCore`, `xrEngine` or `Include`. Backslash includes were rewritten to `/` in a scratch mirror of the tree (the repo was not touched) and four xrCore dependent-base bugs
(`_stl_extensions.h` `xr_vector`/`xr_deque`, `xr_resource.h` `resptr_core`, `intrusive_ptr.h:186`, `_type_traits.h:77`) were patched there, because unpatched they produce about 6,000 bogus errors per run.

| Flags (all with `-fno-operator-names`) | TUs with 0 port errors (of 81) | Port errors | Port error sites (unique file:line:msg) | Shim sites |
|---|---|---|---|---|
| `-fdeclspec` only | 7 | 17,313 | 1,510 | 189 |
| `-fms-extensions` | 7 | 15,342 | 1,423 | 161 |
| `-fdelayed-template-parsing` | 10 | 5,589 | 691 | 48 |
| `-fdelayed-template-parsing -fms-extensions` | 14 | 3,650 | 614 | 23 |
| `-fms-compatibility -fms-compatibility-version=19.30 -fms-extensions -fdelayed-template-parsing` | **20** | **1,263** | **410** | 28 |

Totals are inflated because one bad header is re-reported in every TU that includes it; unique sites are the better proxy for work. No TU was fully clean because about 25 xrCore/xrEngine
shim sites fail in every TU, and the "first error in a port file" goal was not reached; the probe therefore says what the compiler thinks of xrGame, not how long the port will take.
Warnings (`-Wno-everything` hid them) were checked separately on the best flag set: no `-Wc++11-narrowing`, `-Wreturn-type` or `-Wint-conversion` in port files; one real 64-bit truncation,
`(u32)ISpatial_NODE*` in `xrCDB/ISpatial.h:151-154`; 7 `-Wwritable-strings`, 9 `-Wformat-security`, 1 `-Wnon-pod-varargs` (`ini_table_loader.h:123` passes a `shared_str` through varargs).
Not covered: link errors, inactive `#if` branches (xrGame has no `_MSC_VER` or `_WIN32` tokens, so little is hidden), the other 1,100 TUs, anything render-interface specific (no D3D type errors surfaced, but the D3D headers were empty stubs).

**Recommendation.**

Enable from day one:
1. `-fno-operator-names`. Required: `xrCore/_flags.h` has members named `or`, `and`, `xor` (`_flags.h:68-86`), called on 25 lines in xrGame, xrServerEntities and xrPhysics.
2. `-fms-extensions`. Without it 81 of 81 TUs die at `xrCore.h:163` (`#include <sys\utime.h>`): on this target the flag is what makes clang accept `\` in `#include` paths. It also supplies `__declspec` (implies `-fdeclspec`), `__super` (20 uses in
   xrCore and xrGame, including every `xr_*` container) and the `i64` literal suffix (19 lines). Rewriting the backslash includes (161) removes only the first need.
3. `-fdelayed-template-parsing`, as OpenXRay does. It is the biggest single lever (unique sites 1,510 to 691, totals down 68%). xrGame is built on header-only template frameworks
   (`operator_abstract`, `a_star`, `graph_*`, `vertex_manager_*`, `associative_vector`, `object_{saver,loader,cloner,comparer,destroyer}`). Treat what it hides as debt.
4. `-iquote src/xrCore` rather than `-I src/xrCore`, or rename the colliding files: `xrCore/_types.h` collides with the SDK `<_types.h>` and `xrCore/cpuid.h` with clang's `<cpuid.h>`, which breaks `<stdio.h>`
   (a scan of every header name in xrCore, xrGame, xrServerEntities, xrEngine, xrPhysics and `3rd party` found exactly these two collisions). Keep `src/xrGame` on `-I`: some files include project headers with angle brackets
   (`<Inventory.h>`, `<Weapon.h>` in `script_game_object2.cpp:724`, `<ai/monsters/poltergeist/poltergeist.h>` in `HUDRecon.cpp:32`).
5. Also set `-fno-strict-aliasing` and `-ffp-contract=off` (not measured; MSVC does not do type-based alias analysis, the code type-puns floats as integers in `OPC_FPU.h`, `ISpatial*`, `xrCore` math, and `/fp:precise`
   does not contract multiply-add while Apple clang does on arm64). Revisit after M1 with a profile.

Bootstrap only: `-fms-compatibility -fms-compatibility-version=19.30`. It is the first configuration where the sample is tractable (1,263 errors at 410 sites, median 2 to 7 errors per ordinary TU) because it accepts dependent-base lookup,
template-parameter shadowing and forward-declared enums. Costs: it hides about 2,000 `-Wmicrosoft-*` diagnostics (template-parameter shadowing 1,138 hits at 62 sites, enum forward references 632 at 47, dependent-base lookup 572 at 33,
unqualified friends 192 at 6, extra qualification 58 at 4, pure-virtual definitions 50 at 6), it undefines `__GNUC__` (an SDK `#warning`) and breaks `va_list&` parameters (334 errors at 5 xrCore sites: `xrDebug.h:83`,
`_std_extensions.h:263,271`, `xrstring.h:151,154`). Always pass the explicit `-version=`: without it libc++ fails with `use of undeclared identifier 'char16_t'`. It does not define `_MSC_VER`. Build with `-Wmicrosoft` visible
and burn those sites down (about 160 unique in the sample) before dropping it.

Avoid: `-fms-compatibility` without a version; `-I src/xrCore`; defining `_MSC_VER` or `_WIN32` (flips about 76 of 98 xrCore blocks onto the Windows path); depending on `-Wno-everything` in the real build
(it hides the `ISpatial.h` pointer truncation); treating `-fdeclspec` as a separate decision (`__declspec` is only 88 port errors at 13 sites, and 0 tokens in xrGame).

**Port-error classes (best flags, then the weakest):**

| Class | Best flags (total/sites) | `-fdeclspec` only |
|---|---|---|
| Undeclared identifier (mostly follow-on from the next class) | 358/187 | 9,167/807 |
| Missing or misplaced `typename`/`template`, dependent-base lookup | 181/37 | 1,733/109 |
| Syntax (`expected ';'`, `expected unqualified-id`) | 152/31 | 1,378/110 |
| Incomplete type (forward-declared enum or class used as a field) | 135/38 | 312/75 |
| Cascade from a broken base (`cannot initialize object parameter`) | 91/62 | 585/115 |
| Unknown type / no template named (FastDelegate) | 110/14 | 473/21 |
| Unresolved overloaded function, unqualified `&member` | 107/6 | 106/6 |
| Template-template argument, rvalue to non-const ref, casts | 68/24 | 76/22 |
| Forward-reference to enum, array without size | 31/1 | 663/48 |

By subtree (best flags, total/sites): xrGame top-level non-script 865/293, xrServerEntities 206/11, xrGame script exports 82/36, `ai/` 105/65, `ui/` 4/4, xrPhysics 0/0, `ik/` 1/1. Median errors per TU on the best flags: 42 for `ai/stalker`
(9 TUs, the heaviest), 21 for the anchors (`Actor`, `Weapon`, ...), 5-7 for script exports, alife and `ai/monsters`, 2 for `ui/`, `xrServerEntities` and 0 for `xrPhysics` and `ik/`. Of the 61 TUs with any port error, 45 start at
`xrGame/mixed_delegate.h:21` (FastDelegate). Cost is per shared header, not per TU.

**Concrete examples (fix in brackets):**
1. Stray or missing `typename`: `graph_edge_inline.h:27` (`typename const CSGraphEdge::...`, 92 reports at 3 sites; drop it), `xrServerEntities/xml_str_id_loader.h:143`, `ini_id_loader.h:161`; `operator_abstract_inline.h:425`
   (`xr_vector<...>::const_iterator` without `typename`, 27 sites; add it).
2. Forward-declared enums: `script_game_object.h:62` (`enum EPatrolStartType;`), `movement_manager.h:174` (`EPathType m_path_type;` forward-declared in `alife_monster_movement_manager.h:34`), `PHCharacter.h` (`enum EHitType;`),
   `script_sound_type.h` (give them a fixed underlying type at declaration and definition, or include the definition).
3. FastDelegate: `mixed_delegate.h:21` (`fastdelegate::FastDelegate<R (P1, P2)>` is "no template named"; 57 files use it). The library's compiler detection does not enable the function-type syntax for clang without `_MSC_VER`; patch `3rd party/fastdelegate`/`xrCore/fastdelegate.h` configuration.
4. `template` keyword: `object_destroyer.h:150`, `object_saver.h:191`, `object_loader.h:282` (`CHelper4<T>::template delete_data<...>(data)`).
5. Broken-base cascade rooted in `a_star.h:101` (`AStar::_Vertex<..>::_vertex` as a template-template argument) and `a_star.h:122` (`'CAStar::inherited' is not a class`).
6. Dependent-base lookup: `associative_vector_inline.h:299` (`this->operator()`), template-parameter shadowing in `associative_vector_compare_predicate.h:22-24`, `_matrix33.h:326`, `intrusive_ptr.h:186`.
7. Real bugs the MSVC accepted: `Weapon.cpp:1106` (rvalue `(u8)Zoom` to `r_u8(u8&)`), `game_cl_single.h:39` (`xr_token difficulty_type_token [ ];`), `level_script.cpp:2077` (`R_ASSERT(a, b)` with a comma),
   `WeaponStatMgun.cpp:919` (jump past an initialization in a `switch`), `Weapon.cpp:1780` (`dynamic_cast` away const), `WeaponMagazined.cpp:1257` (`cond ? void : value`), `xrServer_Objects_ALife_Items_script.cpp:49`
   (`&has_upgrade` unqualified member), `login_manager_script.cpp:46` (specialization without `template<>`).
8. luabind (every script TU): `detail/class_registry.hpp:52`, `class_rep.hpp:90-147` use `type_info` unqualified; `overload_rep_base.hpp:35` explicit instantiation outside the namespace enclosing libc++'s inline `__1`;
   72 `struct` specifier on a type alias. This belongs to the luabind session.

**Shim must provide (floor, from the probe prelude).** Beyond the lists in 2.2: macros `M_VISUAL` (xrCore `vector.h:20` defines `EPS`, `PI` and friends only under it), `_MT` (`xrCore.h:49` is an `#error` without it), `_WINDOWS_`
(`xrCore/_types.h` typedefs `BOOL` unless set), `interface` (= `struct`, `ph_shell_interface.h`); calling-convention and annotation keywords (`__stdcall`, `__cdecl`, `__fastcall`, `__thiscall`, `__vectorcall`, `__w64`, `__int8..64`, `__noop`,
`__assume`, `__pragma`, `CALLBACK`, `WINAPI`, `APIENTRY`, SAL `_In_`...); `SRWLOCK` with `InitializeSRWLock`/`Acquire`/`Release` (exclusive and shared); `<hash_map>`/`<hash_set>` (`stdext::hash_map`, `hash_set`, `hash_compare`; `xr_hash_map` derives
from them); `DPlay/dplay8.h` subset (2.2); DirectInput types for `xr_input.h` (`DIDEVCAPS`, `DIDEVICEINSTANCE`, `DIDEVICEOBJECTINSTANCE`, `DIDATAFORMAT`, `LPDIRECTINPUT8`, `LPDIRECTINPUTDEVICE8`); the secure CRT with
array-reference template overloads (`strncpy_s` 78 uses, `sscanf_s` 23, `_alloca` 127, `_control87` 29, `_stricmp` 28 in xrCore/xrEngine/xrGame/xrServerEntities/xrPhysics); `_aligned_malloc`/`_aligned_free`/`_msize`/`_heapmin`.
Source work in xrCore/xrEngine/Include that every TU hits: `_math.h:39-40` (x86 `_asm _emit`), `Include/xrRender/FactoryPtr.h:59-83` (specializations without `template<>`; 2,844 errors at 18 sites in the weakest configuration),
`_flags.h:68` (`or/and/xor`), `_matrix33.h:326`, `xr_ini.h:135` (extra qualification), `xrEngine/imgui_base.h:90` (`UINT32`), plus the four dependent-base fixes listed above.

**Include problems other than the Windows headers** (static scan of 3,031 files, 15,000 include lines, exact-case resolution): case mismatches 2,235 lines in xrGame/xrServerEntities/xrPhysics (519 spellings; top:
`level.h` 177, `actor.h` 117, `inventory.h` 78, `../xrphysics/PhysicsShell.h` 54, `weapon.h` 49, `BaseMonster/base_monster.h` 31); 41 lines that resolve nowhere and are not Windows headers (for example
`../../script_game_object.h` in `ExplosiveItem.cpp:11`, `opt.lua.h` in `script_storage.cpp:20`, VCL `mxPlacemnt.hpp`/`ElTree.hpp`, `lua/lua.h` in `pch_script.h:18`; some may sit in inactive `#if`); 64 Windows or CRT only lines
(`dinput.h` 38, `malloc.h` 21, `shellapi.h` 2, `shlwapi.h` 2, `windows.h` 1).

**What this changes in the plan.** (a) The xrcore-shim session's definition of done must include compile conformance of the xrCore containers (`xr_vector`, `xr_deque`, `resptr_core`, `intrusive_ptr`, `_type_traits`),
not just Windows names; otherwise every xrGame session drowns in the same 6,000 cascade errors. (b) The triage session owns the roughly 30 shared template headers (xrServerEntities `object_*`, `associative_vector*`,
`PropertiesListTypes.h`; xrGame `a_star*`, `operator_abstract_inline.h`, `graph_*`, `vertex_manager_*`, `dijkstra_inline.h`, `action_planner_inline.h`, `mixed_delegate.h`, `movement_manager.h`, `script_game_object.h`) and the
luabind and FastDelegate patches, and lands them before the shards start. (c) Do not size shards by file count: after the shared headers are fixed, the per-TU long tail is the kinds in the examples above.
Extrapolating 410 sampled sites to 1,179 TUs is not reliable (sites cluster in headers and a new sample finds new headers); expect low thousands of sites before the Microsoft-compatibility mode is turned off.

Reproduction material (the stubs, prelude, sample list, raw clang output and scripts) is in the audit session's scratch directory, not in the repo.

## 8. What R4 needs at the API level

Files compiled into the R4 renderer (227 `.cpp`, about 72K LOC): the project lists them; the groups are below. The Metal work happens elsewhere; this section only lists the contact points.

### 8.1 Files by role

| Role | Files |
|---|---|
| Device, swap chain, OS window, DXGI | `xrRenderDX10/dx10HW.cpp` (window/HWND 32 hits, DXGI 60, `D3D11CreateDevice` at 567, factory at 149-164, ReShade hooks 1079-1137), `xrRenderPC_R4/r2_test_hw.cpp` (loads `d3d11.dll`, `D3D11CreateDeviceAndSwapChain` probe), `xrRender/dxRenderDeviceRender.cpp`, `xrRender/xr_effgamma.cpp` (DXGI gamma ramp), `xrRender/HWCaps.cpp` (NvAPI), `xrRenderPC_R4/xrRender_R4.cpp` (static init) |
| Device state and backend | `xrRenderDX10/dx10R_Backend_Runtime.h` (60 device hits), `StateManager/dx10StateManager.cpp`, `dx10SamplerStateCache`, `dx10StateUtils.*`, `dx10BufferUtils.*`, `dx10ConstantBuffer*`, `dx10r_constants*.cpp`, `xrRender/R_Backend_Runtime.cpp`, `R_DStreams.cpp` |
| Resources and textures | `dx10SH_Texture.cpp` (35), `dx10SH_RT.cpp`, `dx10Texture.cpp` (D3DX11), `dx10TextureUtils.*`, `dx10ResourceManager_Resources.cpp`, `dx10ResourceManager_Scripting.cpp`, `xrRender/ResourceManager*.cpp` |
| Shader compile and reflection | `xrRenderPC_R4/r4.cpp` (44 device hits; `D3DCompile` 2093, `D3DReflect` 1017-1149, includer 1240), `CSCompiler.cpp`, `ComputeShader.cpp`, `xrRenderDX10/DXCommonTypes.h`, `xrRender/SH_Atomic.cpp` |
| Render targets and passes | `r4_rendertarget*.cpp` (about 20 files), `blender_*.cpp` (about 28), `r4_R_render.cpp`, `r4_R_rain.cpp`, `r4_R_sun_support.cpp`, `light_*.cpp`, `Light_Render_Direct*.cpp`, `dx11HDAOCSBlender`, `dx11MinMaxSMBlender`, `xrRenderDX10/{MSAA,3DFluid,DX10 Rain}` |
| Screenshots | `xrRender/r__screenshot.cpp` (D3DX11 29 hits, 25 device hits) |
| UI, ImGui, debug | `xrRender/dxUIRender.cpp`, `dxImGuiRender.cpp` (ImGui DX11 backend), `dxDebugRender.cpp`, `dxApplicationRender.cpp` (also ReShade and Discord hooks) |
| Engine-facing glue | `Layers/xrAPI/xrAPI.cpp` (the `Render`, `RenderFactory`, `DU`, `UIRender`, `DRender` globals), `src/Include/xrRender/*.h` (the 30 interfaces the engine calls), `xrRender/dxRenderFactory.*` |
| CPU-only or portable | 148 of 227 `.cpp` (38.8K LOC) have no D3D, DXGI, window or OS call: visuals, skinning, HOM, detail, particles, blenders' data, scripting glue |

### 8.2 D3D11 API actually used

- **Interfaces:** `ID3D11Device`, `ID3D11DeviceContext`, `ID3D11Buffer`, `ID3D11Texture{1D,2D,3D}`, `ID3D11{ShaderResource,RenderTarget,DepthStencil,UnorderedAccess}View`,
  `ID3D11{Vertex,Pixel,Geometry,Hull,Domain,Compute}Shader`, `ID3D11InputLayout`, `ID3D11{Blend,DepthStencil,Rasterizer,Sampler}State`, `ID3D11Query`,
  `ID3D11ShaderReflection*`, `ID3DBlob`, `ID3DInclude`; D3D10 aliases of all of these (`ID3D10*`) via `xrRenderDX10/DXCommonTypes.h`; `ID3D11Device1`/`ID3D11DeviceContext1` x1 each.
- **Calls (counts over 494 hits):** `ClearRenderTargetView` 49, `GetDesc` 47, `CopyResource` 38, `OMSetRenderTargets` 20, `Map` 18, `Unmap` 16, `CreateQuery` 14, `CreateTexture2D` 14, `RSSetViewports` 10,
  `ClearDepthStencilView` 9, `CreateShaderResourceView` 7, `CreateBuffer` 5, `ResolveSubresource` 3, `CopySubresourceRegion` 2, `Dispatch` 2, `CSSetUnorderedAccessViews` 3, `GS/HS/DSSetShader` 4/2/2,
  `CreateTexture3D` 4, `DrawIndexed`/`Draw` (the bulk of draws go through `dx10R_Backend_Runtime.h`).
- **Features beyond the basics:** geometry shaders (`r4_rendertarget_accum_*_geom.cpp`, omni-light cube), hull/domain shaders (tessellation), compute (`CSCompiler`, HDAO, `Light_Render_Direct_ComputeXFS`),
  UAVs, MSAA (`xrRenderDX10/MSAA`, `r4_rendertarget_mark_msaa_edges.cpp`, `ResolveSubresource`), 3D textures (`3DFluid`), dual-source blending (`D3D_BLEND_SRC1_*`), occlusion queries (`CreateQuery`/`GetData`), `CheckFormatSupport`,
  `CheckFeatureSupport`, `D3D_FEATURE_LEVEL_11_0/10_1`. The call scan found no stream-output (`SOSetTargets`), deferred contexts or indirect draws.
- **Formats:** about 30 DXGI formats incl. BC1/2/5/7 (+SRGB), `R16G16B16A16_FLOAT`, `R24G8_TYPELESS`, `D24_UNORM_S8_UINT`, `R32_TYPELESS`, `R10G10B10A2_UNORM`, HDR10 color spaces (`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`).
- **Constants:** 594 distinct `D3D*` enum and struct names (1,455 uses).

### 8.3 Calls outside Direct3D

| Need | Where | Note |
|---|---|---|
| OS window handle | `Device.m_hWnd` (`dx10HW.cpp`), `HWND` x17, `ShowWindow`, `SetWindowPos`, `GetClientRect`, `GetMonitorInfo`/`MonitorFromWindow`, `SetForegroundWindow`, `ShowCursor`, `ClipCursor` | `dx10HW.cpp` (32), `dxRenderDeviceRender.cpp` (7), `r2_test_hw.cpp` (2). Give the shim an opaque `HWND` = `SDL_Window*` |
| Dynamic loading of D3D | `LoadLibrary("d3d11.dll")`, `GetProcAddress("CreateDXGIFactory")`, `D3D11CreateDeviceAndSwapChain` | `dx10HW.cpp:149-151`, `r2_test_hw.cpp:8-91`; replace by direct calls |
| `D3DCompile` and friends | `r4.cpp:2093` (compile), `1017-1149` (`D3DReflect` x5), `1225` (`D3DDisassemble`), `1240` (`class includer : public ID3DInclude` reading shader files through `FS`), `D3D_SHADER_MACRO defines[128]` at 1287 | the entry point the shader-xlat session replaces |
| D3DX11 | `D3DX11CreateTextureFromMemory` (`dx10Texture.cpp:449`), `D3DX11GetImageInfoFromMemory` (404), `D3DX11SaveTextureToFile` (143), `D3DX11LoadTextureFromTexture`/`SaveTextureToMemory` (`r__screenshot.cpp`), `D3DX11FilterTexture` (`r4_rendertarget.cpp:1286`) | 6 files |
| NvAPI | `HWCaps.cpp:27` | stub |
| Misc Win32 | `LoadLibrary`, `GetProcAddress`, `MessageBox`, `Sleep`, `SwitchToThread` | `dx10HW.cpp` (8), `r2_test_hw.cpp` (3), `r4_R_render.cpp` (2), `r__occlusion.cpp` (2) |
| Lua | luabind in `dx10ResourceManager_Scripting.cpp` and `ResourceManager_Scripting.cpp` (blender and shader scripts) | needs the luabind port |
| Engine globals | `Device.dwWidth` x126, `dwHeight` x141, `mFullTransform` x86, `dwFrame` x73, `vCameraPosition` x67, `mView`, `mProject`, `Statistic`, `fTimeDelta`, `seqRender`, `b_is_Ready`; `FS.*`, `Core.Params`, `pSettings`, `g_pGamePersistent`, ~215 `ps_r*` console variables | xrEngine headers |
| Console variables | `ps_r2_*`, `ps_r4_hdr10_*`, `ps_r3_msaa*`, `psCurrentVidMode`, `vid_mode_token` filled by `HW` | `xrRender_initconsole()` |
| ImGui | `ImGui_ImplDX11_Init/NewFrame/RenderDrawData/Shutdown` | `dxImGuiRender.cpp` |
| Lua files compiled into R4 | `xrEngine/ai_script_lua_extension.cpp`, `ai_script_lua_debug.cpp` | in the vcxproj only; keep in the engine target |

## 9. OpenXRay reuse index

| Need | OpenXRay path (under `reference/xray-16`) |
|---|---|
| Compat headers | `src/Common/Platform.hpp`, `PlatformApple.inl`, `Compiler.inl`, `d3d9compat.hpp`, `_d3d_extensions.h` |
| NEON from SSE | include pattern in `src/xrCDB/xrCDB_ray.cpp`, `src/xrParticles/noise.cpp`; the header itself is the empty submodule `Externals/sse2neon` (see 2.5) |
| Parallel for | `src/xrCore/Threading/{ParallelFor,ParallelForEach,TaskManager}.{hpp,cpp}` |
| CPU skinning | `src/Layers/xrRender/SkeletonXSkinXW_{CPP,SSE}.cpp`, `SkeletonXSkinXW.h` |
| Network stub | `src/xrNetServer/empty/NET_{Client,Server}.{h,cpp}`, `src/xrNetServer/CMakeLists.txt` |
| SDL window, main, input | `src/xr_3da/entry_point.cpp`, `src/xrEngine/xr_input.{h,cpp}`, `Device_Initialize.cpp`, `Device_imgui.cpp` |
| Stack traces and DXERR | `src/xrCore/Debug/StackTrace.cpp`, `dxerr.{h,cpp}`, `DXGetError*.inl` |
| Script engine | `src/xrScriptEngine/`, `Externals/luabind`, `Externals/LuaJIT`, `Externals/xrLuaFix` |
| ODE, OPCODE, ImGui, zlib | `Externals/ode`, `Externals/OPCODE`, `Externals/imgui`, `Externals/zlib.vcxproj` |
| Compiler flags | `cmake/XRay.Compiler.GNULike.cmake` (lines 41-45, 80, 121, 181-197) |
| Answer key for xrGame | `src/xrGame` (88% name overlap), `src/xrAICore`, `src/xrUICore` |

OpenXRay is a different lineage: its gameplay code does not carry Anomaly's script exports, and its input uses SDL scancodes. Use it for portability patterns only.

## 10. Risks and open questions

1. **xrCore shim is the critical path.** Both wave-1 sessions that touch it were reported failed. Re-plan it before launching wave 2, or run `xrgame-triage` against a minimal stub shim first (the probe in 7.4 shows how small the stub can be).
2. **Memory-order and ABI traps** that compile cleanly: `long`/`DWORD` widths in file formats (level, `.ogf`, save games) and `wchar_t` width. xrCore owns the on-disk readers; module sessions should grep their own `sizeof` of structs.
3. **Thread safety of the Metal D3D11 shim** for `Create*` from `parallel_for_each` in `ResourceManager::DeferredUpload()`; otherwise run it serially (only a load-time cost).
4. **Compile time:** 1,188 xrGame TUs with PCH and luabind. Budget for ccache and `-O0` first builds; do not enable unity builds before the triage session decides.
5. **LuaJIT fork vs. upstream** (section 2.7): confirm before the script shard tries to run `script_storage.cpp`.
6. **Case-insensitive assumptions** (2,626 includes; gamedata paths in `xrCore` FS) are fine on APFS today. If the project later runs on a case-sensitive volume, plan a single mass-fix.
7. **Save compatibility:** unchanged from `PLAN.md` (LuaJIT 2.0 to 2.1).

## Appendix: commands and scripts used

Scripts live outside the repo (session scratch). The numbers can be reproduced with these checks:

```bash
# size
find src/xrGame -name '*.cpp' -o -name '*.h' | xargs cat | wc -l
# live MSVC-only constructs (strip comments and `#if 0` first, then count)
rg -n '\b(__asm|_asm|__try|__except)\b' src/xrGame src/xrEngine
# real TBB uses
rg -n 'tbb::' src -g '*.{cpp,h}' -g '!**/tbb/**' -g '!**/serial/**'
# backslash includes
rg -n '^\s*#\s*include\s*[<"][^>"]*\\' src -g '*.{cpp,h}'
# what the R4 project compiles
rg -o 'ClCompile Include="[^"]+"' src/Layers/xrRenderPC_R4/xrRender_R4.vcxproj | wc -l
```

xrGame shard file lists (reads `xrGame.vcxproj.filters`; run from the repo root):

```python
import re, os, html, collections
base = 'src/xrGame'
fx = open(base + '/xrGame.vcxproj.filters', encoding='utf-8-sig', errors='replace').read()
filt = {}
for kind, path, body in re.findall(r'<(ClCompile|ClInclude)\s+Include="([^"]+)"\s*(?:/>|>(.*?)</\1>)', fx, flags=re.S):
    m = re.search(r'<Filter>(.*?)</Filter>', body or '')
    filt[os.path.normpath(path.replace('\\', '/')).lower()] = html.unescape(m.group(1)) if m else ''
mp = re.compile(r'(\\mp(\\|$)|Multiplayer|Games\\server\\(mp|deathmatch|artefact_hunt|capture_the_artefact|team_deathmatch)'
                r'|Games\\client\\(mp|deathmatch|team_deathmatch|artefact_hunt|capture_the_artefact)|PlayerAccounts|filetransfer'
                r'|xrGameSpyServer|GameSpy|anticheat|screenshots|award_system|SecureMessages'
                r'|UI\\GameTypes\\(CaptureTheArtefact|Deathmatch|TeamDeathmatch))', re.I)
rules = [('mp-stub', lambda k: mp.search(k)),
         ('ai-monsters-a', lambda k: k.startswith('AI\\AMonsters\\common')),
         ('ai-monsters-b', lambda k: k.startswith('AI\\AMonsters')),
         ('ai-core', lambda k: k.startswith(('AI\\AComponents', 'AI\\AGroup', 'AI\\ACover', 'AI\\AEval', 'AI\\ADebug', 'AI\\ASpace', 'AI\\ASound')) or k == 'AI'),
         ('nav-alife', lambda k: k.startswith(('AI\\ANavigation', 'AI\\ALife'))),
         ('script-entities', lambda k: k.startswith('AI\\AScript')),
         ('ui', lambda k: k.startswith('UI')),
         ('objects-items', lambda k: k.startswith(('Core\\Client\\Objects\\items', 'Core\\Client\\Objects\\inventory'))),
         ('objects-world', lambda k: k.startswith('Core\\Client\\Objects')),
         ('core-physics', lambda k: k.startswith(('Core', 'Physics')))]
for dp, _, fn in os.walk(base):
    if 'vs2022' in dp: continue
    for f in fn:
        if f.endswith(('.cpp', '.c', '.h', '.inl', '.hpp')):
            rel = os.path.relpath(os.path.join(dp, f), base)
            k = filt.get(os.path.normpath(rel).lower(), 'Core\\Common')
            print(next(n for n, p in rules if p(k)), rel)
```
