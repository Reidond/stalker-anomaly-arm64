# xrSound on Apple platforms

`src/xrSound` builds as a static library (`Anomaly::xrSound`, `cmake/targets/xrSound.cmake`) for macOS and iOS/iPadOS,
arm64 only. The Windows build (`xrSound.vcxproj`) is unchanged; every port change sits behind `XR_PLATFORM_APPLE`.
The engine's sound code was already OpenAL; the port is the Windows plumbing around it.

```bash
cmake --preset macos-arm64 -DANOMALY_BUILD_XRSOUND=ON
cmake --build --preset macos-arm64 --target xrSound xrsound_smoke
build/macos-arm64/tools/xrsound_smoke/xrsound_smoke
```

## What changed

| Windows | Apple |
|---|---|
| `NotificationClient.cpp` (WASAPI `IMMNotificationClient`), `guids.cpp` (`dsound.h`) | not compiled. `SoundRender_Apple.cpp` implements `CNotificationClient` (declared in `NotificationClient.h` under `XR_PLATFORM_APPLE`) on top of OpenAL Soft's `ALC_SOFT_system_events` |
| `#include <msacm.h>` (unused) | skipped |
| `tbb::parallel_for_each` in `i_create_all_sources` | GCD (`xr_parallel_foreach`) over a vector copy of the file set |
| `bin\alsoft.ini` found by OpenAL Soft by itself | `xr_sound_apple::prepare()` points `ALSOFT_CONF` at a copy in `$app_data_root$` (see below) |
| the audio endpoint | iOS: `AVAudioSession` (`AudioSession_Apple.mm`) |
| `default_device_changed()` | also re-enumerates first, because `Default Device` resolves to the device that was default when the list was built |

`guids.cpp` and `NotificationClient.cpp` are dropped in `xrSound.cmake` (`EXCLUDE`), not by `#if`, so the Windows
project is untouched. New files, Apple only: `SoundRender_Apple.{h,cpp}`, `AudioSession_Apple.{h,mm}`. The `.mm` file
includes only Apple headers (Objective-C's `BOOL` and the Win32 shim's cannot share a translation unit); the engine side
talks to it through `AudioSession_Apple.h`.

## Device changes

`CSoundRender_Core::update()` already acts on two flags, `bPendingDefaultDeviceSwitch` and `bPendingDeviceListRefresh`.
Windows set them from the WASAPI client; here:

- **macOS and iOS:** `alcEventControlSOFT` for `DEFAULT_DEVICE_CHANGED`, `DEVICE_ADDED`, `DEVICE_REMOVED`. OpenAL Soft's
  CoreAudio backend raises them (it listens to `kAudioHardwarePropertyDefaultOutputDevice` on macOS). The callback only sets
  the flags.
- **iOS:** `AVAudioSessionRouteChangeNotification` (new / old device) sets both flags.

## iOS audio session

OpenAL Soft's CoreAudio backend opens a RemoteIO unit and never touches the audio session, so the port does:

- **Category:** `Ambient` (mixes with other apps' audio, yields to calls and Siri). `ANOMALY_AUDIO_CATEGORY=solo|playback`
  in the environment picks `SoloAmbient` (silences other apps) or `Playback` (keeps playing with the screen locked, needs
  `UIBackgroundModes=audio` to matter). The session is configured and activated in `xr_sound_apple::prepare()`, before the first
  `alc*` call.
- **Interruption began** (call, Siri, alarm): `alcDevicePauseSOFT`. **Ended**, or the app becoming active if the session could
  not be reactivated in the background: `setActive:YES`, then `alcDeviceResumeSOFT`.
- **Media services reset:** the session is configured again and the device is reopened through the default-device path.

The interruption path is written to Apple's documented sequence and is **not tested on a device** (it needs a real call, a
Siri invocation or an alarm during play).

## OpenAL Soft settings, HRTF and EFX

Anomaly ships its OpenAL Soft settings in `bin\alsoft.ini` (`stereo-mode = headphones`, `hrtf-mode = full`, `periods = 4`,
`sources = 1024`) and its HRTF data sets in the game root (`default-44100.mhr`, `default-48000.mhr`); `hrtf-paths = .` is relative
to the working directory, which is the game root on Windows. `xr_sound_apple::prepare()` writes a copy of the ini to
`$app_data_root$\alsoft.ini` with `hrtf-paths` set to `$fs_root$` and sets `ALSOFT_CONF` to it (an `ALSOFT_CONF` already in
the environment wins). The sample run below shows HRTF enabled with Anomaly's two data sets.

EFX is untouched: `ALC_EXT_EFX` 1.0 with two auxiliary sends, one EAX-reverb effect in one auxiliary slot, created by
`CSoundRender_CoreA::LoadEffect` exactly as on Windows.

## OpenAL Soft on iOS: `MAC_OS_X_VERSION_MIN_REQUIRED`

`AvailabilityMacros.h` defines `MAC_OS_X_VERSION_MIN_REQUIRED` as 10.5 on iOS, so OpenAL Soft's "old macOS" guards fire. In
`common/almalloc.cpp` (guard `< 101300`) the global aligned `operator new` is replaced by a `posix_memalign()` that rejects
alignments under `sizeof(void*)`; `al::Effect` is 4-aligned, so `alGenEffects` fails with `AL_OUT_OF_MEMORY` ("Failed to allocate
1 effect") and the engine's EAX reverb is never created (seen in the iOS Simulator; macOS is not affected). `atomic.h` and
`alformat.hpp` take their pre-macOS-11 / pre-13.3 fallbacks too. The fix is `MAC_OS_X_VERSION_MIN_REQUIRED=140000` on the `OpenAL`
target for iOS (AvailabilityMacros.h only defines the macro when it is unset). It belongs in `cmake/Dependencies.cmake`; until that
lands `cmake/targets/xrSound.cmake` carries the same line (`TODO(shim)`).

## Link dependencies

Beyond xrCore, OpenAL Soft (`OpenAL::OpenAL`), Vorbis (`Vorbis::vorbis`) and Ogg, xrSound needs:

- **xrEngine:** `CConsole* Console` and `CConsole::Execute(const char*)` (used by `CSoundRender_CoreA::_initialize` to lower
  `snd_targets` when the device runs out of sources). Nothing else: no `Device`, no timers, no input.
- **xrCDB:** `CDB::MODEL` (ctor, dtor, `build`), `CDB::COLLIDER` (ctor, dtor, `ray_query`), `CDB::Collector::add_face_packed_D`
  (occlusion and environment geometry). Optional in `xrSound.cmake`; `tools/xrsound_smoke/cdb_stubs.cpp` stands in until
  `xrCDB` is built.
- **Frameworks:** `Foundation`, `AVFoundation` (the module links them PUBLIC).

## `tools/xrsound_smoke`

macOS command-line tool, added from `xrSound.cmake` (so it exists when the module is on), also built as the iOS app
`com.andriishafar.anomaly.soundlab`.

```bash
xrsound_smoke [--game-dir <dir>] [--sound <name>] [--seconds <n>] [--device <name>]
              [--list [prefix]] [--dump-ogg <file>]
```

It initializes xrCore against the game directory (read only; logs, appdata and the generated `fsgame.ltx` go to the run
directory under the build tree), creates the sound manager like `x_ray.cpp` does (`_create(0)`, `_create(1)`), loads a
mono effect from the archives through `ref_sound::create`, times a separate full Ogg decode, plays the sound 2D and then 3D on
a source moving from x=-8 to x=+8, and checks the OpenAL source state every frame: `AL_PLAYING`, `AL_SOURCE_RELATIVE` as
expected, source position moving, play cursor moving, no underrun, no source left after stop. The report covers the device,
sample rate, EFX version and the engine's auxiliary slot, HRTF status and data sets, decode time and load time.

Sample run, macOS arm64 RelWithDebInfo, Anomaly 1.5.3 data (18,103 `.ogg` in the archives), default output AirPods Max:

```
device          : AirPods Max Andrii          output: 48000 Hz, 1023 mono + 1 stereo sources
EFX             : available (v1.0, 2 aux sends)   engine EFX slot : created (EAX reverb)
HRTF            : enabled (default-44100), 2 data set(s): 'default-44100', 'default-48000'
sound           : weapons\9a91\9a91_reload, 3.13 s, mono, 13,798 bytes of Ogg Vorbis
decode          : 1.28 ms for 3.13 s of 44100 Hz audio (2443x real time); engine load 3.7 ms; sound manager up in 697 ms
2D: 101 engine updates in 2005 ms, source cursor moved in 100, emitter clock 1985 ms
3D: source x -8 -> +8, OpenAL position follows (z negated by the engine), no underrun
```

Same tool on the iPad Air M3 (iPadOS 27, signed Debug app, bundled sample, default settings because there is no `alsoft.ini`):
device `CoreAudio Default`, 48 kHz, EFX 1.0 with 2 sends and the engine's EAX slot, 255 mono sources, decode 5.3 ms for 3.13 s,
2D and 3D phases with the source `AL_PLAYING`, cursor moving, no underrun, 3D position following the moving source; all checks
passed, exit code 0. The EAX effect is created on the device only with the `MAC_OS_X_VERSION_MIN_REQUIRED` workaround above.

A Debug build is much slower to initialize (4 to 7 s with HRTF on; the device list opens every device and each open builds the
HRTF filters), so judge start-up from a Release build.

On the iPad the archives are not available. The app bundles `sample.ogg` (written on the Mac with `--dump-ogg`, picked up by
CMake from `build/<dir>/tools/xrsound_smoke/sample.ogg` or `XRSOUND_SMOKE_BUNDLE_OGG`), lays out a one-file game directory in
its container and runs the same checks. The signing account is free: CMake takes the bundle suffix from
`XRSOUND_SMOKE_BUNDLE_SUFFIX` (default `.soundlab`), so a suffix whose profile Xcode already holds can be reused when it
cannot create a new one; the iPad run above used such a profile and the app was uninstalled afterwards.

## Paths inside the iOS container

Virtual paths are lower case, and the iOS sandbox does not let the resolver list the parents of the app container to find the
capitals again (`Containers`, the UUID), so `fsgame.ltx` inside the container could not be opened on the iPad. The first
iPad launch failed with "Cannot open file". `xr_native_path` now restores the spelling of `$HOME` (the container root) before
resolving the rest (`src/xrCore/Platform/PlatformApple.cpp`, commit "shim: restore the app container's spelling ...").
