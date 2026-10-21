# xrPhysics on Apple platforms

`src/xrPhysics` (plus `xrServerEntities/PHNetState.cpp`, which its vcxproj compiles) builds as the static library
`xrPhysics` for macOS and iOS/iPadOS, arm64. It links the Anomaly-patched ODE (`ode`) and, through xrCore, the shim.

```bash
cmake --preset macos-arm64 -DANOMALY_BUILD_XRPHYSICS=ON -DANOMALY_BUILD_XRCDB=ON
cmake --build --preset macos-arm64 --target xrPhysics xrphysics_smoke
build/macos-arm64/tools/xrphysics_smoke/xrphysics_smoke                  # or: ctest --test-dir build/macos-arm64 -R xrphysics_smoke
cmake --preset ios-arm64 -DANOMALY_BUILD_XRPHYSICS=ON && cmake --build --preset ios-arm64 --target xrPhysics
```

The syntax probe is clean for all 58 translation units of the module (and `PHNetState.cpp`), with and without
`ANOMALY_MS_COMPAT`.

## Source changes

Everything else the P0 probe found in this module (`__stdcall`, `#pragma managed`, `_copysign`, `offsetof(dContact, geom)`,
`D3DCOLOR_XRGB`) is covered by the shim and the engine flags. Five sites needed a source change, all valid on MSVC too except
the enum, which is guarded by `XR_PLATFORM_APPLE`:

| File | Problem | Change |
|---|---|---|
| `BlockAllocator.h`, `PHGeometryOwner.h` | `xr_vector<T*>::iterator` in a template without `typename` | added `typename` |
| `tri-colliderknoopc/dTriBox.cpp` | `spoint##[0]` pastes an identifier and a bracket (MSVC accepts it) | `spoint[0]` (what OpenXRay has) |
| `tri-colliderknoopc/dSortTriPrimitive.h` | local `CDB::TRI* T` inside `template <class T>` | renamed to `Tr` (as OpenXRay) |
| `PHCharacter.h`, `PHSimpleCharacter.h` | `namespace ALife { enum EHitType; }`: an enum without a fixed type cannot be forward declared | on Apple include `alife_space.h` instead |

`xrGame/PHMovementControl.h:11` has the same forward declaration; the xrGame session needs the same treatment (or a fixed
underlying type on `ALife::EHitType`, as OpenXRay does with `: u32`).

## What xrPhysics needs from the rest of the engine

`nm -u` on `libxrPhysics.a` minus what xrPhysics, xrCore, ODE and the C/C++ runtime define. Nothing else: no xrEngine
function, no xrSound, no xrGame function.

| Symbol | Defined in |
|---|---|
| `XRC` (`xrXRC`), `g_SpatialSpace`, `g_SpatialSpacePhysic` | xrCDB (`xrXRC.cpp`, `ISpatial.cpp`) |
| `ISpatial::ISpatial/~ISpatial/spatial_register/spatial_unregister/spatial_move`, `ISpatial_DB::ISpatial_DB/q_box/q_ray`, typeinfo for `ISpatial` | xrCDB (`ISpatial.cpp`) |
| `CObjectSpace::CObjectSpace/~CObjectSpace/Load/Create/RayPick/RayQuery` | xrCDB (`xr_area*.cpp`) |
| `CDB::COLLIDER::box_query` | xrCDB (`xrCDB_box.cpp`) |
| `PGMLib` (`CGameMtlLibrary*`) | xrAPI (`Layers/xrAPI/xrAPI.cpp`; xrEngine's `GameMtlLib.cpp` only defines it `#ifdef _EDITOR`) |
| `g_actor_allow_ladder` (`bool`) | xrGame (`level_script.cpp`) |
| `g_dead_body_collision` (`u32`) | xrGame (`console_commands.cpp`) |
| typeinfo for `CObject` (`dynamic_cast` in `CPHSimpleCharacter`) | xrEngine (`xr_object.cpp`) |

Not a link-time dependency but required at run time:

- `DllMainXrPhysics(NULL, DLL_PROCESS_ATTACH, NULL)` installs the ODE allocation hooks. `xrEngine/x_ray.cpp` calls it, as
  before; whoever runs xrPhysics must call it before the first world.
- A `CRenderDeviceBase` that implements `AddSeqFrame`, `RemoveSeqFrame` and `StatPhysics` (the world registers itself as a
  `pureFrame` and is stepped from `OnFrame`), a `CGameMtlLibrary` with at least the materials named `objects\small_box`
  (and `materials\earth_slide` for the actor) behind `set_mtl_lib`, and a `CObjectSpace`.
- xrCDB itself needs the `Render` global (`ISpatial.cpp`, sector lookup of renderable objects), which xrAPI defines. Physics
  objects are never renderable, so a null pointer is enough without a renderer.

## `tools/xrphysics_smoke`

macOS only. It links the real xrPhysics, xrCDB and ODE and supplies the engine side itself (a stub device and shell holder, a
hand-made material library, the globals above), so no xrEngine or xrGame code is needed. It builds the world through
`mesh_create_object_space` and `create_physics_world`, then drops:

- two boxes (one onto the other), a sphere,
- a three-link chain with ball joints, tilted so it folds (`SetPrefereExactIntegration`, so it goes through `dWorldStep` like
  the ragdolls of single player),
- an actor and an AI character (`create_actor_character`, `create_ai_character`),

steps it for 6 s at 30 fps through `OnFrame` (100 physics steps per second) and checks: finite, inside the level, never through
the ground, at rest at the expected heights (a box on a box rests at three half extents), joints held, characters standing
at foot level 0. `--stress N --seed S` drops a random heap instead (N shells of every kind, 4 characters, some islands forced to
exact integration) and counts NaN, escaped and sunken bodies. The default scene (30 runs) and 48 stress runs (6 seeds, 8 times) pass in Debug and
RelWithDebInfo.

The runs are not repeatable, and that is upstream behaviour: X-Ray's `quickstep.cpp` shuffles constraint rows with
`std::mt19937(std::random_device()())`, so even the first contact step differs by about 1e-6 between two runs of the same
binary. (`stepfast.cpp` has a second shuffle with `rand()`, only used if `dWorldStepFast1` is called, which xrPhysics does
not.) Do not compare trajectories bit for bit, and keep thresholds loose.

## Floating point

xrPhysics contains no x87/SSE control-word code: no `_controlfp`, `_control87`, `_fpreset`, inline assembly, SSE intrinsics or
denormal handling (`grep` over the module, `PHNetState.cpp` and `PHSynchronize.h`). The rounding mode and flush-to-zero are xrCore's
(`FPU::m24r/m64r`, FPCR). `_valid()` and `_copysign` come from xrCore and the shim. ODE has no precision-dependent code beyond
`dSINGLE`.

**Decision: xrPhysics keeps `-ffp-contract=off`.** It already inherits it from `Anomaly::xrCore` (PUBLIC); the target now sets it
itself so the module does not depend on that. ODE keeps its own (`engine-3rdparty` found that fusing broke the `dWorldStep` LCP
solver on a box on a plane).

The test (`tools/xrphysics_smoke/fp_contract_experiment.py`) builds four variants of xrPhysics + ODE, `<xrPhysics>_<ODE>`, with
`-ffp-contract=on` (clang's default on arm64: `a*b+c` inside one expression becomes an FMA) or `off`, and runs the same smoke
scene and the stress heap against each. ODE's `quickstep.cpp` is compiled from a copy with a fixed shuffle seed so a variant is
repeatable. Results, RelWithDebInfo:

- Same scene, same seed: every variant gives a different digest of all positions and velocities. Fusing changes the result of
  the physics at the bit level in both libraries; xrPhysics' contact generation and ODE's solver each contribute.
- Stress heap, 12 seeds of 60 shells and 4 characters, 12 s each: no variant produced a NaN, an escaped or a sunken body
  (0 failed checks in all four). The chain link spacing error stays in the same range, 0.1 to 0.5 m at worst in a crushed pile (the joints are soft, links are 0.3 m apart; the check fails above 1 m).
  `ODE Message 3: LCP internal error` appears in all four, including `off_off`: 39 times with both off, 47 with xrPhysics fused,
  30 with ODE fused, 30 with both, over the 12 seeds. That is the degenerate-contact message of `dWorldStep`, not an FMA artefact,
  and the counts are within the run-to-run noise.

So fusing is not *required* for stability in xrPhysics, as it is in ODE's solver in isolation. It stays off because the contact and
solver tolerances (`CFM/ERP`, `fis_zero`, the `dTri*` depth tests) were tuned on the non-fused arithmetic of the Windows build,
because bit-level parity makes any later A/B check against the Windows engine meaningful, and because the stress shows no
price in stability to pay. I did not get a trustworthy speed number: the other sessions were loading the machine, and the
trajectories (so the work per run) differ between variants.

## For the sessions that link xrPhysics

- `xrPhysics` links `ode`, `xrCDB`, `xrAPI` and `xrSound` PUBLIC (absent targets are skipped), so a consumer only names `xrPhysics`.
- `CPhysicsShell::Activate(const Fmatrix&, bool)` is an empty inline in `PHShell.h`; the engine uses
  `Activate(form, dt01, form2, disable)` (what `P_build_SimpleShell` does).
- `GetPosition()` of a `CPHCharacter` is the foot centre; the box body is half its height above that.
- Keep `-ffp-contract=off` on ODE (required) and on xrPhysics (policy, see above).
- Include paths such as `"../xrserverentities/alife_space.h"` rely on a case-insensitive file system at compile time (the
  default APFS volume); a case-sensitive checkout breaks the build, not only xrPhysics.
