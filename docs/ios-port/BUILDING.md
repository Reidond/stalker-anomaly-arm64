# Building the Apple port

The CMake build in the repo root covers macOS and iPadOS (arm64 only). The Windows build still
uses `src/engine-vs2022.sln`.

## Prerequisites

- Xcode 27, CMake 3.28+ and Ninja.
- Submodules:

  ```bash
  git submodule update --init --depth 1 external/SDL external/openal-soft external/LuaJIT external/ogg external/vorbis external/theora external/SPIRV-Cross external/vkd3d external/sse2neon
  ```

## macOS

```bash
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --target platform_probe
```

The probe app is written to `build/macos-arm64/tools/platform_probe/platform_probe.app`.

## iPadOS

The signing team comes from `CMakeUserPresets.json`, which is gitignored. Copy the `ios-arm64-local`
preset there and put your team ID in `ANOMALY_IOS_TEAM_ID`. Xcode also needs your Apple ID under
Settings → Accounts.

```bash
cmake --preset ios-arm64-local
xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme platform_probe \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
```

To check that everything compiles and links without signing, add `CODE_SIGNING_ALLOWED=NO`.

## App icon

The game apps (`anomaly` and `shell_lab`, everything configured with `anomaly_configure_game_app()`) carry an
icon that CMake generates at configure time with `tools/make_app_icon.sh`, into `<build dir>/app_icon/`. The
Anomaly logo is GSC artwork, so it is never committed:

- With the game data present (`game/Anomaly`, or `-DANOMALY_GAME_DIR=<install>`), the icon is the logo embedded in
  `AnomalyLauncher.exe` (`tools/app_icon/extract_launcher_icon.py` pulls the largest PNG from its icon resources)
  composed by `tools/app_icon/compose_icon.swift` on a dark background with margins. The source is only 256 px, so
  the badge fills about 60% of the 1024 px canvas instead of being blown up to the edges.
- Without it, the icon is `platform/icon/fallback.png`, an original drawing (anomaly rings, no Anomaly artwork).
  `swift tools/app_icon/compose_icon.swift fallback platform/icon/fallback.png` regenerates it.

The configure step prints which one it used (`app icon: ...`). A configure that changes none of the inputs keeps
the previous output, because the Swift step takes about ten seconds. A git worktree has no `game/` folder of its
own, so pass `-DANOMALY_GAME_DIR` there or the build gets the fallback icon.

- **iOS** (Xcode generator): an `Assets.xcassets` with one 1024 px `AppIcon` (iOS 17+), added as a bundle resource
  with `ASSETCATALOG_COMPILER_APPICON_NAME=AppIcon`; `platform/ios/Info.plist.in` has `CFBundleIcons` and
  `CFBundleIconName`. Check a build with `xcrun assetutil --info <app>/Assets.car`.
- **macOS** (Ninja): `AppIcon.icns` made with `iconutil` (rounded 824 px body with a shadow, as the macOS icon grid
  asks), copied to `Contents/Resources` and named by `CFBundleIconFile` in `platform/macos/Info.plist.in`.

Run `tools/make_app_icon.sh <out-dir> [game-dir]` by hand to look at the result (`icon-1024.png`,
`icon-mac-1024.png`). If the Mac bundle shows a generic icon after a rebuild, Finder and the Dock cache icons by
path; `touch` the `.app` or run `killall Dock`.

## Platform probe

`tools/platform_probe` is a small SDL3 app that checks the port's foundations on each device. It
shows:

- Metal rendering.
- The LuaJIT version, and whether its JIT is on.
- The OpenAL Soft output device.
- On iPad, the memory budget, which shows whether the memory entitlements are active.
- Live DualSense state: buttons, sticks, triggers, gyro and touchpad.

On the controller, Cross triggers rumble and the left stick tints the light bar.

## JIT Lab and the iPad JIT

`tools/jit_lab` is an SDL3 app that runs the LuaJIT benchmarks and a stability run and shows the log
on screen. It is also how the iPad JIT is tested; see [IPAD-JIT.md](IPAD-JIT.md) for the mechanism and
how to run it under lldb or StikDebug.

```bash
# iPad: LuaJIT patched to run its JIT from a debugger-authorized region (off by default)
cmake --preset ios-arm64-local -DANOMALY_IOS_LUAJIT_JIT=ON
xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme jit_lab \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
tools/jit_lab/run_ipad.sh detach --exit

# Mac: stock LuaJIT, JIT on. -DANOMALY_LUAJIT_TXM_EMULATE=ON runs the iPad code path (dual-mapped
# code writer) on the Mac without a debugger, for testing.
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --target jit_lab
build/macos-arm64/tools/jit_lab/jit_lab.app/Contents/MacOS/jit_lab --headless
```

After changing `ANOMALY_IOS_LUAJIT_JIT` the LuaJIT build copy is recreated from the submodule, so the
next build recompiles LuaJIT and relinks the apps.

## Shader tools

`xrdb_unpack`, `shader_batch` and the `ShaderXlat` library build with the normal presets and need no
extra tools (vkd3d's generated parsers are checked in):

```bash
cmake --build --preset macos-arm64 --target xrdb_unpack shader_batch shaderxlat_selftest
ctest --test-dir build/macos-arm64
```

Usage and results are in [SHADERS.md](SHADERS.md). The shader submodules are
`external/vkd3d` (tag `vkd3d-2.1`) and `external/SPIRV-Cross` (`vulkan-sdk-1.4.363.0`); initialise
them with the other submodules.
