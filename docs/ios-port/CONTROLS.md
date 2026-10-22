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
Slots are clockwise from the top. On the iPad build the game draws the wheel (`xr_pad_render_radial`,
IPAD.md): item icons and slot badges on the weapon and quick-use tiles, a hub with the picked name and the page dots.

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

## Engine input on SDL3 (`CInput`, Apple platforms)

`xrEngine`'s `CInput` has two implementations behind the same class: DirectInput in `Xr_input.cpp` (Windows, unchanged) and
SDL3 in `xr_input_sdl.cpp` (`XR_PLATFORM_APPLE`). Every caller of `pInput` and every `IInputReceiver` keeps compiling. The
controller layer above is plugged in here, so a pad press is a key event like any other.

| File | What |
|---|---|
| `xr_input_sdl.cpp` | `CInput` on SDL3: events, keyboard, mouse, text input, the controller pump |
| `xr_input_keymap.{h,cpp}` | `SDL_Scancode` <-> `DIK_*`, key names, `dik_to_text`, UTF-8 <-> Windows-1251. No engine dependencies |
| `IInputReceiver.{h,cpp}` | new `IR_OnTextInput(const char*)`; `IR_GetMousePos*` read `CInput`'s pointer instead of `GetCursorPos` |
| `line_edit_control.{h,cpp}` | new `on_text_input(text)`; caps/num lock from `CInput::iGetKeyToggleState`; no keyboard-layout switch |
| `imgui_base_input.cpp` | pointer released while ImGui has input; typed characters come from `dik_to_text`; code page 1251 |
| `MonitorList.cpp` | `SDL_GetDisplays` for the `vid_monitor` list; a monitor handle is the `SDL_DisplayID` carried in the pointer (`xr_display_from_monitor` in `xr_window.h`) |

### Frame order

`CInput::OnFrame` = `KeyUpdate()`, `MouseUpdate()`, `PadUpdate()`. Each of the first two reads its SDL events, then
calls the top receiver (`cbStack.back()`) in the order the DirectInput loop did: edges first, then `Hold` for every
key or button that is down, then the summed motion and wheel. `PadUpdate()` runs `SdlPadSource::Update(dt)` and
`gameinput::DispatchFrame` with `CInput` itself as the `IEngineInputSink`.

**Event ownership.** In the engine, `xr_window::PumpEvents()` (engine-core, `xr_window.h`) is the only consumer of the SDL queue. It passes every event to the
handler `CInput` registers in its constructor (`xr_window::AddEventHandler`, removed in the destructor), which keeps the keyboard, mouse, text and gamepad ones
and ignores the rest. `CInput` then never polls: `SetEventSourceExternal(true)`, set by that registration, keeps it off the queue. Without `xr_window.h` (tools,
tests: `XR_INPUT_STANDALONE`) it peeks those event types from the queue itself, or takes events from a host's `HandleEvent`. It owns the `SdlPadSource`, so no one else
should create one. `OnAppActivate()` / `OnAppDeactivate()` (window focus, from the window layer) reset key state and give the pointer back.
A level load can leave seconds of events behind: consecutive motion merges and each list keeps the newest 4096.

### Keyboard

SDL scancodes are physical positions, like the DIK codes, so the table (`kTable`, 134 rows) is layout independent and
translates to the DirectInput numbering, never the other way round: the Lua `DIK_keys`, MCM and `user.ltx` keep working. Every key
DirectInput names is mapped (letters, digits, function keys to F15, numpad, navigation, media and browser keys, the Japanese and ISO extras); a scancode
without a DIK number is dropped. Command (`LGUI`/`RGUI`) is `DIK_LWIN`/`DIK_RWIN`, Option is `DIK_LMENU`/`DIK_RMENU`. SDL auto-repeat is ignored: the
engine makes its own from `Hold`. A release for a key `CInput` never saw pressed (focus came back with the key down) is dropped.

`get_dik_name` names a key after the active layout ("A", ";", a Cyrillic letter) or SDL's name ("Left Shift"), in Windows-1251.
Alt+F4 still defers `KERNEL:disconnect` and `KERNEL:quit`.

### Mouse and pointer

| Mode | When | Behaviour |
|---|---|---|
| Captured (default) | app active, `SetPointerCaptured(true)`, not `unacquire()`d | `SDL_SetWindowRelativeMouseMode`, OS pointer hidden. `SDL_EVENT_MOUSE_MOTION` `xrel/yrel` are summed per frame into whole counts for `IR_OnMouseMove`; the remainder carries to the next frame. A virtual pointer follows the deltas, clamped to the window |
| Free | `SetPointerCaptured(false)` (ImGui while it has input), app inactive | the OS pointer is visible; `x/y` are the pointer; deltas still arrive |

- **Buttons:** SDL left, right, middle, X1, X2 become engine buttons 0, 1, 2, 3, 4 (`MOUSE_1..`), with `IR_OnMouseHold` each frame.
- **Wheel:** 120 per notch, DirectInput's unit (consumers use the sign; the HUD-tune mode uses the value). Fractional trackpad scrolls accumulate.
- **Absolute position:** `iGetAsyncMousePos(p)` returns client pixels, the space of `RDEVICE.clientWidth/Height` (window points scaled to it), so
  `IR_GetMousePosReal`, `IR_GetMousePosIndependent` and the ImGui code work unchanged. `iSetMousePos(p)` replaces `SetCursorPos`.
- **UI cursor:** `CUICursor` with `m_b_use_win_cursor == false` adds the deltas to a position in 1024x768 UI units. While
  `SetUiContext(true)`, `CInput` scales the deltas to UI units (1024 / window width), so the cursor follows the pointer one to one.
- **Touch** (iPad; SDL turns touches and the Pencil into mouse events): in a dialog (`SetUiContext(true)`) a touch moves the cursor to the finger
  and a tap is a left click; in the world touches are ignored, so a tap neither turns the view nor fires.

### Text input and the on-screen keyboard

`EnableTextInput(owner, area)` / `DisableTextInput(owner)` start and stop `SDL_StartTextInput` while any owner (a text field,
passed as `this`; a second field can't unbalance a counter) holds it. On iPad this shows and hides the on-screen keyboard
(`area`, in client pixels, keeps the field above it). While it is on, characters arrive as `IR_OnTextInput(text)` in Windows-1251 and `dik_to_text`
returns false, so a hardware keyboard doesn't type everything twice. While it is off, a key press types through
`dik_to_text` (`SDL_GetKeyFromScancode` with the modifiers, then Windows-1251), like `ToUnicodeEx` did.
`line_edit_control::on_text_input` inserts at the cursor over the selection and applies the editor mode: nothing when read only, digits and
signs when number only, no `/ \ : * ? " < > | ^ ( ) [ ] %` for file names, and the buffer size.

### Controller

Created with `CInput`: `pInput->Pad()` is the `gameinput::GameInput` (bindings, tuning, key capture, `feedback()`),
`pInput->PadSource()` the SDL backend. Per frame:

- Pad keys reach the receiver stack as `IR_OnKeyboardPress/Release/Hold(code)` with the codes from 1200; `iGetAsyncKeyState(code)` answers for them.
- Right stick and gyro look arrive as `IR_OnMouseMove(dx, dy)`. The layer produces degrees, converted with the actor's own formula,
  `scale = psMouseSens * psMouseSensScale / 50` radians per count by default (`SetLookRadiansPerCount` replaces it with the live value including FOV and the aim factor),
  and the vertical `3/4 * psMouseSensVerticalK` and the invert flag are undone, so the layer's degrees per second and invert option hold at any mouse sensitivity.
- In a dialog (`SetUiContext(true)` also sets the layer's `Menu` context) the stick and touchpad move the cursor as `IR_OnMouseMove` in UI units, buttons are
  `MOUSE_1/2/3` and modifiers, and the right stick is the wheel (`notches * 120`).
- `pad_hooks` carries what is not a key: `cursor_jump(ui_x, ui_y)` (D-pad widget jump), `ui_action(0|1)` (previous/next page), `connection(bool)` (pause with a
  "reconnect the controller" message).
- `CInput::feedback(s1, s2, t)`, which `CHUDManager::HitMarked` already calls, starts a rumble hit scaled by `max(s1, s2)`.

## Hooks still needed in the engine and the game

`CInput` (above) already does the pump, the look conversion, the pad key codes, the cursor deltas and text input. What is left is
in files outside the input session, with the call to make in each. The three xrGame symbols the engine's input code names
(`GetUICursor()`, `CUICursor::SetUICursorPosition`, `get_binded_action(int)`) are the game's own definitions and are in `libxrGame.a`.
The xrgame-rest session did the small ones, behind `XR_PLATFORM_APPLE` (helpers `xr_pad_*` in `xrGame/xr_level_controller.cpp`):

| # | Hook | Status |
|---|---|---|
| 1 | key names | **done**: `initialize_bindings()` appends `EnginePadKeyTable(PlayStation)` to `keyboards[]` (spare slots after the terminator), labels from the table |
| 2 | pad binding slot, `bind_pad`, Controls screen | **done except the Controls screen column**: `_binding::m_keyboard[3]`, `bind_pad`/`unbind_pad`, `default_controls` runs the default DualSense layout, every bind is mirrored into the layer's `BindingMap`, `bind_list` shows the gamepad key. Verified on the iPad with the script pad (`padon`, `padaxis ly -1` walks, `pad create` opens the inventory). **Open:** the Controls screen's Gamepad column |
| 3 | UI context | **done**: `CUICursor::Show/Hide` call `SetUiContext`, `Feedback::SetMenu` and `Pad()->SetCursorPosition` (`xr_pad_ui_context`); `CGamePersistent` installs `pad_hooks.cursor_jump` (`SetUICursorPosition`) and `pad_hooks.connection` (pauses the game in a level while no pad is connected, resumes when one is back; no "reconnect the controller" message yet). **Open:** `pad_hooks.ui_action` (tabs), `INavigationProvider` |
| 4 | look scale | **done**: `CActor::IR_OnMouseMove` passes its `scale` to `SetLookRadiansPerCount`. It runs on input, so the first stick motion after a zoom change uses the previous figure |
| 5 | text fields and the on-screen keyboard | open (`CUICustomEdit` focus, `IR_OnTextInput` through `CDialogHolder`/`CLevel`/`CMainMenu`, `CConsole`) |
| 6 | Win32 pointer calls | the shim's `SetCursorPos`/`ClientToScreen` keep `UICursor.cpp`/`Level_input.cpp` compiling; `m_b_use_win_cursor` is forced false on Apple (`CUICursor::InitInternal`) |
| 7 | the window | engine-core's, nothing in xrGame |
| 8 | feedback | **done:** `CActor::UpdateCL` -> `xr_pad_update_actor` (health, bleeding, radiation, aiming, weapon class by `ClassifyWeapon(section)`, automatic = fire mode other than 0/1, empty/jammed/reloading); `CActor::Hit` -> `OnHit` with the mapping in item 8 below and `power` clamped to 0..1; `CWeaponMagazined::OnShot`/`OnEmptyClick` -> `OnShot`/`OnDryFire`. **Open:** `SetBlowout` (script side), shots of weapons that are not `CWeaponMagazined` (missiles, knives). Only the controlled actor (`Level().CurrentControlEntity()`) feeds the layer |

None of the feedback calls were exercised against the real engine yet (no running game); they compile for both platforms.

**1. Key names.** Append `EnginePadKeyTable(...)` to `keyboards[]` before its `{NULL, 0}` terminator
(`xrGame/xr_level_controller.cpp`). `remap_keys()` fills `key_local_name` from `CInput::get_dik_name`; pad keys have no DIK name, so set it from the
table's `label`.

**2. A pad binding slot.** `_binding::m_keyboard[2]` becomes `[3]`, and the third slot is the pad
key:

- `get_binded_action`, `is_binded`, `get_action_dik` and `CCC_Bind`'s conflict loop cover slot 2.
- Register `CMD2(CCC_Bind, "bind_pad", 2)` and `CMD2(CCC_UnBind, "unbind_pad", 2)`; `CCC_Bind::Save`
  already writes per slot (`m_work_idx`).
- Mirror every pad bind into the layer: `pInput->Pad()->bindings().Bind(action, key)` (or `Unbind`). The layer only emits keys that are in
  its own `BindingMap`, which keeps unbound inputs from waking the Lua `level_input.on_key_press` callback; `ClearKeyFilter()` lifts that.
- `CCC_DefControls` also runs the default pad layout: execute the lines of
  `BindingMap::Default().Serialize()`.
- The Controls screen gets a third "Gamepad" column: one more `CUIEditKeyBind` per action with
  `m_bPrimary` replaced by a slot index. Call `pInput->Pad()->SetKeyCapture(true/false)` from
  `CUIEditKeyBind::SetEditMode`.

**3. UI focus and context.** `pInput->SetUiContext(bool)` each frame (or when a dialog opens or closes) from `CurrentGameUI()->TopInputReceiver()` needing the
cursor, or `GetUICursor().IsVisible()`. It switches the layer between Gameplay and Menu, and scales mouse deltas and touch to UI units.
When a dialog opens, call `pInput->Pad()->SetCursorPosition(GetUICursor().GetCursorPosition())`. Once, at game start, set
`pInput->pad_hooks`: `cursor_jump` -> `GetUICursor().SetUICursorPosition`, `ui_action` -> the dialog's previous/next tab, `connection(false)` -> `Device.Pause`
with a "reconnect the controller" message. `m_b_use_win_cursor` must be false on Apple platforms (`CUICursor` uses the deltas, not an OS cursor). For
D-pad widget navigation, implement `INavigationProvider::FindNeighbor` by walking the visible `CUIWindow` tree and pass it to `Pad()->SetNavigationProvider`.

**4. Look scale.** Without help the pad look uses `psMouseSens`. For the exact figure, call
`pInput->SetLookRadiansPerCount(scale)` every frame from `CActor` with the `scale` of `CActor::IR_OnMouseMove`
(`(f_fov / g_fov) * psMouseSens * sens_multiple * psMouseSensScale / 50 / LookFactor`), so the layer's degrees per second hold while zoomed.
Later, a `CActor::IR_OnPadLook(yaw_deg, pitch_deg)` that calls `cam_Active()->Move` directly would skip the integer rounding.

**5. Text fields and the on-screen keyboard.** Nothing calls `EnableTextInput` yet, so on iPad no keyboard appears and a field takes only a hardware keyboard.
- `CUICustomEdit`: where `m_bInputFocus` becomes true (`OnMouseAction`, `SetKeyboardCapture`) call `pInput->EnableTextInput(this, &rect)` (the field's rectangle in
  client pixels); where it becomes false (`SendMessage(WINDOW_KEYBOARD_CAPTURE_LOST)`, `press_commit`, `OnFocusLost`) call `DisableTextInput(this)`.
- `CConsole::Show`/`Hide`: the same, with `this`.
- Forward `IR_OnTextInput(text)`: `CLevel` and `CMainMenu` -> `CDialogHolder::IR_UIOnTextInput` -> the top dialog -> the window that holds keyboard capture
  -> `CUICustomEdit` -> `ec().on_text_input(text)`; `CConsole::IR_OnTextInput` -> `m_editor->on_text_input(text)`.
- ImGui text: `ide::OnFrame` can call `EnableTextInput(this)` while `io.WantTextInput`. Hardware typing works today (the key's character goes to ImGui from `IR_OnKeyboardPress`).

**6. Win32 pointer calls** in files outside the input list, to move to `CInput`:

| Now | Use |
|---|---|
| `GetCursorPos` + `ScreenToClient` (`XR_IOConsole.cpp:682`, `device.cpp:296`, `xr_ioc_cmd.cpp:871`) | `IInputReceiver::IR_GetMousePosReal(p)` |
| `SetCursorPos` (`XR_IOConsole.cpp:714`, `UICursor.cpp:135`, `imgui_base.cpp:173`, `Level_input.cpp:140`) | `pInput->iSetMousePos(p)`, or nothing: in captured mode the pointer needs no re-centring |
| `ShowCursor`, `ClipCursor` (`device.cpp`, `Device_create.cpp`, `Device_destroy.cpp`, `xr_ioc_cmd.cpp`, `MainMenu.cpp`) | `pInput->SetPointerCaptured(bool)`; the window grab is SDL's |
| `WM_CHAR` -> `ide::InputChar` (`Device_wndproc.cpp`) | not needed: text events reach `IR_OnTextInput` |
| `WM_ACTIVATE` -> `OnAppActivate/Deactivate` | the same calls from `SDL_EVENT_WINDOW_FOCUS_GAINED/LOST` |

**7. The window.** `RDEVICE.m_hWnd` is the `SDL_Window*` (engine-core, `xr_window.h`), which is where `CInput` takes its window from. Its Win32-named
`GetCursorPos`/`SetCursorPos`/`ClipCursor`/`ShowCursor` shims (drawable pixels, client == screen) keep the files in table 6 compiling; `CInput` owns the relative mouse mode
itself, so `ShowCursor`/`ClipCursor` calls can stay.

**8. Feedback and weapon trigger profiles.** Where the events already exist in `xrGame`:

| Layer call | Engine source |
|---|---|
| `SetHealth`, `SetBleeding`, `SetRadiation` | `CActor::conditions()`: `GetHealth()`, `BleedingSpeed()`, `GetRadiation()` |
| `OnHit(type, severity)` | `CActor::Hit(SHit*)`. Map `ALife::EHitType`: `eHitTypeFireWound`/`eHitTypeWound`/`eHitTypeWound_2` to Bullet, `eHitTypeStrike` to Melee, `eHitTypeExplosion`, `eHitTypeBurn`/`eHitTypeLightBurn` to Fire, `eHitTypeShock`, `eHitTypeChemicalBurn`, `eHitTypeRadiation`, `eHitTypeTelepatic` to Psy. Severity is the hit power over max health |
| `OnShot`, `OnDryFire` | Actor's `CWeapon` fire path; `CWeaponMagazined::OnEmptyClick` |
| `SetWeapon(class, automatic)`, `SetWeaponStatus` | Active item change; `IsMisfire()`, magazine count, reload state |
| `SetAiming` | `CActor::IsZoomAimingMode()` |
| `SetBlowout` | The emission is script driven (`surge_manager`); expose an engine export the script calls while the blowout runs |

## Verification

**`xrinput_smoke`** (macOS) drives the real `CInput` without the engine: it compiles `Xr_input.cpp`, `xr_input_sdl.cpp`, `IInputReceiver.cpp`,
`line_edit_control.cpp` and `xrTheora_Stream.cpp` into its own translation units against a stub `Device`/`Engine`, creates a window with SDL's dummy video
driver and feeds SDL events (`SDL_PushEvent`) and a virtual DualSense (`SDL_AttachVirtualJoystick`) to a recording `IInputReceiver`. It checks 42 keys
end to end and the whole scancode table, mouse deltas with their carry, the five buttons, the wheel, the absolute position, the UI-unit and touch behaviour,
text input and `line_edit_control` in each mode, the pad keys (Cross, the stick as strafe keys, analog look and its degrees per second at two sensitivities, Cross as
`MOUSE_1` and the stick as cursor in a dialog), rumble from `feedback()`, unplugging and Alt+F4. Then it mounts the real archives (like `xrcore_smoke`) and decodes
`ui\video_crow.ogm` (32x32, 25 fps, 334 frames) with `CTheoraStream`; the header of each of the 12 `.ogm` files (intro videos up to 1920x1080, 2417 frames) is
parsed. The shipped data has no `.avi` or `.gif` files.

```bash
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --target xrinput_smoke
build/macos-arm64/tools/xrinput_smoke/xrinput_smoke           # --game-dir <dir> or $ANOMALY_GAME_DIR; --no-video skips the archives
```

The input and media files also pass `-fsyntax-only` inside the real `xrEngine` target (`-DANOMALY_BUILD_XRENGINE=ON`) for `macos-arm64` and against the iPhoneOS SDK
(`arm64-apple-ios17.0`). Not checked: the soft keyboard, touch, pointer capture and trackpad on the iPad, and the whole `xrEngine` target (engine-core's files).

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
