# Common/compat: stand-ins for Windows and compiler headers

`Anomaly::EngineConfig` (cmake/AnomalyModules.cmake) puts this directory first on the include path of every engine
module on Apple platforms. Each file here is named like a header the engine, or code it was written against,
includes by name, and forwards to the shim:

| Header(s) | Forwards to |
|---|---|
| `windows.h`, `windowsx.h`, `objbase.h`, `shellapi.h`, `shlwapi.h`, `mmsystem.h`, `mmreg.h`, `process.h`, `io.h`, `direct.h`, `tchar.h`, `malloc.h` | `Common/Platform.h` (Win32 types and calls, MSVC CRT; see `PlatformApple.inl`) |
| `d3d9.h`, `d3d9types.h`, `d3d9caps.h`, `d3dx9*.h`, `dxerr.h` | `Common/d3d9compat.h` (D3D9 enums/flags/structs, D3DX9 math and declaration helpers) |
| `dinput.h` | `Common/dinput_compat.h` (`DIK_*` with the DirectInput numbering, DirectInput types as declarations) |
| `DPlay/dplay8.h`, `dplay8.h` | DirectPlay 8 subset (`DPNID`, `DPNSEND_*`, `DPN_APPLICATION_DESC`, ...) |
| `xmmintrin.h`, `emmintrin.h`, `pmmintrin.h`, `tmmintrin.h`, `smmintrin.h`, `nmmintrin.h`, `immintrin.h`, `intrin.h` | `external/sse2neon` (SSE intrinsics on NEON) |

These directories are never on the include path of xrCore itself or of non-engine targets, and never on Windows.
To add a stand-in, drop a file here (keep it a one-line forwarder if you can) and mention it in
docs/ios-port/XRCORE.md.
