# Controls: DualSense layout and the controller layer

The controller layer lives in `src/Platform/GameInput` and has no dependency on the engine. The
engine reaches it through a thin adapter (`GameInput/Adapter.h`). `tools/input_lab` shows all of it
live, on the Mac and on the iPad. Build and run notes are in [BUILDING.md](BUILDING.md).

## How the engine takes input today

This is what the layer plugs into (all paths under `src/`):

- `xrEngine/Xr_input.cpp`: `CInput::OnFrame` polls DirectInput. Keys go to
  `cbStack.back()->IR_OnKeyboardPress/Release/Hold(dik)`, and every key that is down gets a `Hold`
  call each frame. Mouse motion arrives as **integer** counts through `IR_OnMouseMove(dx, dy)`.
- `xrGame/xr_level_controller.cpp`: every input is an `int` key: DIK scan codes (0..0xED),
  `MOUSE_1..8` (`0xED+100..107`) and `kCUSTOM0..99` (1000..1099, script only). `keyboards[]` names
  them, `g_key_bindings[]` binds an action to up to two of them (`m_keyboard[2]`), and
  `get_binded_action(key)` turns a key into an `EGameActions`. `bind <action> <key>` is the console
  command; `user.ltx` stores it by key *name*.
- `CLevel::IR_OnKeyboardPress(key)` resolves the action; `CActor::IR_OnKeyboardPress/Hold(action)`
  acts on it. Movement is digital: `kFWD/kBACK/kL_STRAFE/kR_STRAFE` set `mstate_wishful` bits while
  held, and `kACCEL` held means **walk** (`isActorAccelerated` returns false while `mcAccel` is set).
- `CActor::IR_OnMouseMove(dx, dy)` turns counts into radians with
  `scale = (fov / g_fov) * psMouseSens * sens_multiple * psMouseSensScale / 50 / LookFactor` and calls
  `cam_Active()->Move(kLEFT/kRIGHT/kUP/kDOWN, angle)`. dy > 0 looks down.
- Menus: `CDialogHolder::IR_UIOnKeyboardPress` turns `MOUSE_1/2/3` into `WINDOW_*BUTTON_DOWN` at
  `CUICursor` position. `CUICursor::UpdateCursorPosition(dx, dy)` adds the deltas straight to a
  position in **UI space (1024x768)** unless `m_b_use_win_cursor` is set, in which case it reads the OS
  cursor. Inventory, trade, PDA and dialogs are all mouse driven: click, drag and drop, right-click
  context menu, Ctrl/Shift+click, wheel scrolling.
- `xr_input_xinput.*` is commented out, so there is no gamepad code to reuse.

## Default layout (PlayStation DualSense)

Gameplay, with no menu open. "Tap" means pressed and released within 0.22 s.

| Input | Action (engine action name) | Notes |
|---|---|---|
| Left stick | Move: `forward` `back` `lstrafe` `rstrafe` | Becomes `gp_lstick_*` keys. 8 directions, near-axis pushes snap to the axis |
| Left stick, gentle push (below 62 %) | Walk: `accel` | `gp_walk` is held while the push stays gentle for 0.12 s, so ramping up to a run does not blip into walk. It can be rebound or removed (always run) |
| L3 (click) | Sprint: `sprint_toggle` | Hold or toggle follows the engine's sprint option |
| Right stick | Look | Deadzone, response curve, acceleration; slower while aiming |
| Gyro | Look (optional) | Off by default; "ADS only" or "always" |
| R3 (click) | Free look: `freelook` | |
| R2 | Fire: `wpn_fire` | Adaptive trigger profile per weapon |
| L2 | Aim down sights: `wpn_zoom` | Light adaptive resistance |
| Cross | Jump: `jump` | |
| Circle | Crouch: `crouch` | |
| Square | Reload: `wpn_reload` | |
| Triangle | Use / interact: `use` | |
| D-pad Up | Flashlight: `torch` | |
| D-pad Down | Night vision: `night_vision` | |
| D-pad Left / Right | Lean left / right: `llookout` `rlookout` | |
| L1 tap / R1 tap | Previous / next weapon: `prev_slot` `next_slot` | |
| L1 hold | Items wheel | See below |
| R1 hold | Combat wheel | See below |
| Create | Inventory: `inventory` | |
| Touchpad (press) | PDA: `active_jobs` | Anomaly's PDA entry. Check against the real default controls at integration |
| Options | Menu / back: `quit` | |
| PS | Unbound | iPadOS keeps the PS button for Home: a short press leaves the app and the game never sees it (verified on the iPad). `pause` is reachable from the menu |
| Touchpad swipes, Mic, paddles | Unbound | Swipes fire `gp_touch_*` keys, ready to bind |

### Radial menus

Hold L1 or R1 for 0.22 s: a wheel opens, the movement stick keeps working, the right stick picks a
slot (the look stick is captured while the wheel is open), D-pad left/right flips between the two
pages, and releasing the button fires the picked slot. A pick survives for 0.3 s after the stick
springs back to centre, so "flick and let go" works as well as holding the direction; releasing with
nothing picked cancels.
Slots are clockwise from the top.

| Wheel | Page | Slots (clockwise from the top) |
|---|---|---|
| Items (L1) | 1 Quick use | Quick 1, Quick 2, Quick 3, Quick 4, Artefact, Detector, Quick save, Drop |
| Items (L1) | 2 Custom | `custom1` to `custom8` (addon-defined actions) |
| Combat (R1) | 1 Weapons | Slot 1 to Slot 6, Safe mode, Weapon function |
| Combat (R1) | 2 Weapon tools | Ammo type, Fire mode next, Fire mode prev, Scope zoom +, Scope zoom -, `custom9` to `custom11` |

- **Heal and bandage** have no dedicated stock action: Anomaly uses *quick use* slots, which the
  player fills with a medkit, bandage, food and so on in the inventory. They sit at the top of the
  Items wheel, so Quick 1 and Quick 2 are the fast heal and bandage.
- **Prone** and other addon stances are not stock actions. Addons bind them to `custom1..25`; put
  that action's slot on the wheel by rebinding `gp_radial_N` (for example `gp_radial_9` is Custom 1).
- Each wheel slot is a virtual key `gp_radial_1` to `gp_radial_32`, so the Controls menu rebinds any
  slot to any action and the wheel labels follow.

### Menu context

When a dialog that needs the cursor is on top (inventory, trade, PDA, dialogs, the main menu), the
engine sets the Menu context and the same buttons change meaning:

| Input | Effect |
|---|---|
| Left stick | Moves the cursor (velocity, response curve) |
| Touchpad | Trackpad: relative motion with mild speed gain. Pressing the pad clicks. Tap-to-click is optional |
| D-pad | Nudges the cursor by 96 UI units, repeating while held. With a navigation provider it jumps to the nearest widget instead |
| L3 held | Precision mode (cursor at 30 % speed) |
| R3 | Recentre the cursor |
| Cross | Left click (`MOUSE_1`): click, drag and drop, hold to drag |
| Square | Right click (`MOUSE_2`): item context menu |
| Triangle | Left double click: quick use or equip |
| Circle | `ESC`: back, close the dialog |
| L1 / R1 held | `Ctrl` / `Shift` held: Ctrl+click moves an item to the other list |
| R2 / L2 | Next / previous page command (`UiCommand`), for tabs |
| Right stick up/down | Mouse wheel (lists, trade quantity) |
| Options, Create | Passed through as pad keys, so the engine's `quit` and `inventory` bindings close the dialog |

The Controls screen needs the raw buttons to bind them, so `SetKeyCapture(true)` turns the mapping
off while a field is waiting for a key: every button and stick direction arrives as its own `gp_*` key.

### Feedback

| Event | Rumble | Light bar | Adaptive triggers |
|---|---|---|---|
| Health | | Green to yellow to red; below 25 % a heartbeat pulse that quickens as health drops | |
| Damage taken | Per hit type (bullet, melee, explosion, fall, fire, shock, chemical, radiation, psy), scaled by severity | White flash | |
| Bleeding | | Slow red pulse | |
| Radiation | Soft pulse on hit | Yellow-green flicker | |
| Blowout / emission | Long rolling low-frequency rumble | Orange flicker | |
| Shot fired | Short kick per weapon class | | R2 buzzes while an automatic weapon fires |
| Dry fire | Light click | | R2 goes limp when the magazine is empty or while reloading |
| Jam | | | R2 becomes a wall |
| Aiming | | | L2 resistance firms up |
| Menu open | | Cool blue | Off |

Per-weapon R2 profiles (`GCDualSenseAdaptiveTrigger` weapon mode: resistance between *start* and
*end*, then a click). Positions and strengths are 0..1.

| Class | Start | End | Strength | While firing |
|---|---|---|---|---|
| Pistol | 0.50 | 0.65 | 0.65 | single |
| SMG | 0.42 | 0.58 | 0.45 | vibration 0.55 |
| Assault rifle | 0.40 | 0.62 | 0.55 | vibration 0.55 |
| LMG | 0.35 | 0.70 | 0.75 | vibration 0.80 |
| Shotgun | 0.30 | 0.78 | 1.00 | single |
| Sniper | 0.55 | 0.90 | 0.90 | single |
| Launcher | 0.30 | 0.85 | 1.00 | single |
| Throwable | feedback from 0.40, strength 0.30 | | | |
| Melee | feedback from 0.35, strength 0.20 | | | |

`ClassifyWeapon(section)` guesses the class from an Anomaly weapon section name (`wpn_ak74`,
`wpn_toz34`, `wpn_svd`). Names it does not know become assault rifles, so the engine side can pass
a class from the section's ini fields instead.

## How the layer works

```
SDL_Gamepad / GCController
   |  SdlPadSource::Poll()            PadSnapshot (buttons, sticks, triggers, touchpad, gyro, accel)
   v
GameInput::Update(dt, snapshot)       context, bindings, filters, radial, cursor, feedback
   |  FrameOutput                     key edges, held keys, move, look degrees, cursor, wheel, radial, device events
   v
DispatchFrame(output, IEngineInputSink)   -> IR_OnKeyboardPress/Release/Hold, IR_OnMouseMove, IR_OnMouseWheel, cursor jump
```

Feedback goes the other way: the engine calls `GameInput::feedback()` (`SetHealth`, `OnHit`, `OnShot`,
`SetWeapon`, `SetBlowout`, ...), and an `IFeedbackSink` carries the result out. `SdlPadSource` is that
sink: rumble and light bar through SDL, adaptive triggers through `DualSenseTriggers.mm`
(`GCDualSenseGamepad`). Rumble is limited to about 30 Hz and refreshed while it plays, the light
bar only on visible change, triggers only on change.

- **Pad keys.** `gp_cross` ... `gp_r3`, D-pad, `gp_create`, `gp_options`, `gp_ps`, `gp_touchpad`,
  `gp_mic`, paddles, `gp_lstick_*`, `gp_rstick_*`, `gp_walk`, `gp_touch_*` swipes and
  `gp_radial_1..32`. They start at 1200, clear of DIK codes, mouse buttons and `kCUSTOM`.
  `EnginePadKeyTable()` lists them with their names and labels (Cross/Circle for PlayStation, A/B for
  Xbox).
- **Bindings.** `BindingMap` maps pad keys to engine action names (the strings in `actions[]`), one pad
  key per action. Its text form is the console syntax: `bind_pad jump gp_cross`, `unbind_pad jump`,
  `unbindall_pad`. `BindingMap::Default().Serialize()` is the default layout above.
- **Only bound keys are emitted** by default, so an unbound input does not wake the engine's Lua
  `level_input.on_key_press` callback. `SetKeyCapture(true)` or `ClearKeyFilter()` lifts that.
- **Movement.** The engine moves digitally, so "analog" movement means: radial deadzone, rescale,
  snap near-axis pushes to the axis, then four direction keys with hysteresis (on at 0.42, off at
  0.32 of the rescaled push), plus `gp_walk` once the push has stayed below 62 % for 0.12 s. The
  analog vector is also in `FrameOutput.move` for later.
- **Look.** Right stick: deadzone, `pow(m, 1.7)`, 220 and 150 degrees per second at full yaw and
  pitch, 1.5x acceleration after 0.45 s pushed to the edge, 0.55x while aiming. Output is degrees
  per frame; `LookToCounts()` converts to integer mouse counts and carries the remainder, so slow
  motion is not rounded away.
- **Gyro.** With an accelerometer, yaw is the rotation rate about world-up, found from the gravity
  direction ("player space"), so it works whether the pad is held upright or tilted. Pitch comes from
  the pad's left-right axis. A zero-rate offset is learned while the pad is still (calibrated after
  one second), and a soft noise floor removes the rest. **On the iPad the DualSense reaches the app
  through GameController with a gyro but no accelerometer** (SDL reports `accel=0`), so yaw falls
  back to fixed axes that assume the pad is held upright, the normal grip. Verified on the iPad: turning
  the pad clockwise (to the right) gives positive yaw, and tipping the trigger edge away gives
  negative pitch (look up).
- **Radial menu.** `RadialMenu` is the model (wheels, pages, slot angles with sticky borders, hold
  versus tap); drawing is the host's job. A confirmed slot fires its `gp_radial_N` key as a one-frame
  pulse.
- **Devices.** `DeviceRegistry` gives every pad a stable key: model + SDL GUID (+ serial if the
  platform has one), with `#2`, `#3` for identical pads without serials. The active pad is the first
  connected, then whichever was last touched. Disconnecting releases every key, silences feedback
  and reports a `Disconnected` device event; on reconnect, buttons still held are ignored until
  released. `ProfileStore` keeps tuning and bindings per stable key as one text file.

### Tunables

Defaults, in `Config` (`GameInput/Config.h`). Each has a dotted name for profiles and sliders
(`look.exponent`).

| Group | Settings |
|---|---|
| `move` | `deadzone` 0.18, `key_on` 0.42, `key_off` 0.32, `walk_threshold` 0.62, `walk_hysteresis` 0.05, `walk_delay` 0.12, `cardinal_snap_deg` 9, `walk_enabled` 1 |
| `look` | `deadzone` 0.10, `exponent` 1.7, `yaw_speed` 220, `pitch_speed` 150, `accel_gain` 1.5, `accel_time` 0.45, `ads_scale` 0.55, `invert_y` 0 |
| `gyro` | `mode` 0 (off), 1 (ADS only), 2 (always); `sensitivity` 1, `ads_scale` 0.6, `noise_floor` 0.01 rad/s, `smoothing` 0.45, `player_space` 1, `invert_x`, `invert_y` |
| `trigger` | `press` 0.35, `release` 0.25 (digital threshold for L2/R2) |
| `cursor` | `deadzone` 0.14, `speed` 700 UI units/s, `exponent` 1.9, `precision_scale` 0.3, `dpad_step` 96, `trackpad_gain` 1.35, `trackpad_enabled` 1, `tap_to_click` 0, `scroll_rate` 9 |
| `radial` | `hold_seconds` 0.22, `deadzone` 0.45, `sticky` 0.18, `release_grace` 0.30 |
| `feedback` | `rumble_scale` 1, `trigger_effects` 1, `lightbar` 1, `lightbar_brightness` 0.8 |

## Hooking the layer into the engine

None of this is committed to engine files yet: it belongs to the session that ports `xrEngine` input
to SDL3. The seam is small.

**1. Key names.** Append `EnginePadKeyTable(...)` to `keyboards[]` before its `{NULL, 0}` terminator.
`remap_keys()` fills `key_local_name` from DirectInput; pad keys have no DIK name, so set it from the
table's `label`.

**2. A pad binding slot.** `_binding::m_keyboard[2]` becomes `[3]`, and the third slot is the pad
key:

- `get_binded_action`, `is_binded`, `get_action_dik` and `CCC_Bind`'s conflict loop cover slot 2.
- Register `CMD2(CCC_Bind, "bind_pad", 2)` and `CMD2(CCC_UnBind, "unbind_pad", 2)`; `CCC_Bind::Save`
  already writes per slot (`m_work_idx`).
- `CCC_DefControls` also runs the default pad layout: execute the lines of
  `BindingMap::Default().Serialize()`.
- The Controls screen gets a third "Gamepad" column: one more `CUIEditKeyBind` per action with
  `m_bPrimary` replaced by a slot index. Call `GameInput::SetKeyCapture(true/false)` from
  `CUIEditKeyBind::SetEditMode`.

**3. The pump.** After `KeyUpdate()` and `MouseUpdate()` in `CInput::OnFrame`:

```cpp
FrameOutput out = g_pad_source.Update(dt);         // SdlPadSource: polls the active pad, runs GameInput
DispatchFrame(g_gameinput, out, g_engine_sink, counts_per_degree);
```

`g_engine_sink` implements `IEngineInputSink` with one-line forwards: `KeyPress/Release/Hold` call
`cbStack.back()->IR_OnKeyboardPress/Release/Hold`, `MouseMove` calls `IR_OnMouseMove`, `MouseWheel`
calls `IR_OnMouseWheel`, `CursorJump` calls `GetUICursor().SetUICursorPosition`, and `PadDisconnected`
pauses the game (`Device.Pause`) with a "reconnect the controller" message.
`CInput::iGetAsyncKeyState` returns `GameInput::IsKeyDown(key)` for pad keys, and `COUNT_KB_BUTTONS`
stays 256 because pad key `Hold` calls come from `DispatchFrame`, not from the `KBState` loop.

**4. Look scale.** The engine converts counts to radians with the `scale` formula above, so pass
`counts_per_degree = (pi / 180) / scale` each frame, with the live FOV and sensitivity. Later, a
`CActor::IR_OnPadLook(yaw_deg, pitch_deg)` that calls `cam_Active()->Move` directly would skip the
integer rounding.

**5. Context.** Each frame set `SetContext(Menu)` while `CurrentGameUI()->TopInputReceiver()` needs the
cursor (or `GetUICursor().IsVisible()`), else `Gameplay`. When a dialog opens, call
`SetCursorPosition(GetUICursor().GetCursorPosition())`. `m_b_use_win_cursor` must be false on Apple
platforms so `CUICursor` uses the deltas instead of an OS cursor. For D-pad widget navigation,
implement `INavigationProvider::FindNeighbor` by walking the visible `CUIWindow` tree.

**6. Feedback.** Where the events already exist in `xrGame`:

| Layer call | Engine source |
|---|---|
| `SetHealth`, `SetBleeding`, `SetRadiation` | `CActor::conditions()`: `GetHealth()`, `BleedingSpeed()`, `GetRadiation()` |
| `OnHit(type, severity)` | `CActor::Hit(SHit*)`. Map `ALife::EHitType`: `eHitTypeFireWound`/`eHitTypeWound`/`eHitTypeWound_2` to Bullet, `eHitTypeStrike` to Melee, `eHitTypeExplosion`, `eHitTypeBurn`/`eHitTypeLightBurn` to Fire, `eHitTypeShock`, `eHitTypeChemicalBurn`, `eHitTypeRadiation`, `eHitTypeTelepatic` to Psy. Severity is the hit power over max health |
| `OnShot`, `OnDryFire` | Actor's `CWeapon` fire path; `CWeaponMagazined::OnEmptyClick` |
| `SetWeapon(class, automatic)`, `SetWeaponStatus` | Active item change; `IsMisfire()`, magazine count, reload state |
| `SetAiming` | `CActor::IsZoomAimingMode()` |
| `SetBlowout` | The emission is script driven (`surge_manager`); expose an engine export the script calls while the blowout runs |

## Verification

Tests, run on both platforms: `gameinput_tests` (macOS, also built under ASan and UBSan) and
`input_lab --selftest` (the same 624 checks on the iPad; the trigger sweep that follows only finds a
pad when the app is in front, so launched headless through `devicectl` it reports "no controllers").
They cover key names, filters, gyro,
bindings, radial, cursor, feedback, devices, profiles, gameplay and menu input, key capture and the
adapter order.

`input_lab` shows the live state. Lab commands use Create as a modifier (the layer sees nothing else
while it is held): Create+Cross context, Create+Square weapon class, Create+Circle weapon state
(ready, empty, jammed, reloading), Create+Triangle gyro mode, Create+D-pad left/right health,
Create+D-pad up test hit, Create+D-pad down blowout rumble, Create+L1/R1 pick a tunable,
Create+L3/R3 change it. The six buttons at the top of the screen do the same by touch.
`input_lab --shot out.bmp [gameplay|radial|menu]` renders one scene from synthetic input for layout
checks.

Checked on the iPad Air (M3) with a DualSense over Bluetooth, from the lab's logs and the user's
hands-on checks (2026-10-21):

- Every default button, stick direction, walk and trigger reached the right action; L1/R1 taps and
  both wheels opened, flipped pages and cancelled (the first run exposed that flick-and-let-go
  cancelled, which the release grace fixes).
- Menu context: stick cursor, touchpad trackpad, click, click-and-drag, right click, double click and
  Circle to go back.
- Gyro direction (yaw, pitch) as described above.
- Adaptive triggers: GameController's own readback shows the modes switching (weapon, vibration while
  an automatic weapon fires, off when empty, feedback on L2) and the trigger status moving through
  ready, firing and fired as R2 is pulled. By hand: clearly different per weapon class and per state.
- Rumble (shots, hit, blowout) and the light bar (health colours, pulse when low, hit flash) were felt
  and seen.
- Disconnect: powering the pad off released every key and silenced feedback, and on reconnect the
  same stable device key came back, the trigger effects were re-applied and input resumed. The key
  was also identical after relaunching the app, and the saved per-device profile loaded.
- iPadOS takes the PS button (Home) and a long hold of Create (about a second, the system capture
  gesture): the game never sees PS, and during a long Create hold GameController reports the
  trigger modes as off until it ends. A short Create press arrives normally.

Not verified without the real engine: the exact default Anomaly PDA binding, the hit, shot and
weapon hooks in the adapter section, and the `surge_manager` blowout hook. Not verified on the
device: haptic trigger rumble (SDL reports none through GameController), several pads at once, and
Xbox or other non-DualSense pads (they use the same code path minus the DualSense features).
