# Building the Apple port

The CMake build in the repo root covers macOS and iPadOS (arm64 only). The Windows build still
uses `src/engine-vs2022.sln`.

## Prerequisites

- Xcode 27, CMake 3.28+ and Ninja.
- Submodules:

  ```bash
  git submodule update --init --depth 1 external/SDL external/openal-soft external/LuaJIT external/ogg external/vorbis external/theora
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

## Platform probe

`tools/platform_probe` is a small SDL3 app that checks the port's foundations on each device. It
shows:

- Metal rendering.
- The LuaJIT version, and whether its JIT is on.
- The OpenAL Soft output device.
- On iPad, the memory budget, which shows whether the memory entitlements are active.
- Live DualSense state: buttons, sticks, triggers, gyro and touchpad.

On the controller, Cross triggers rumble and the left stick tints the light bar.
