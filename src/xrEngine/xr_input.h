#ifndef __XR_INPUT__
#define __XR_INPUT__

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#ifdef XR_PLATFORM_APPLE
# include <functional>
// SDL3 backend (xr_input_sdl.cpp). The SDL and GameInput headers stay out of this one: it is included everywhere.
struct SDL_Window;
union SDL_Event;
namespace gameinput
{
class GameInput;
class SdlPadSource;
} // namespace gameinput
struct XrInputSdl;
#endif

class ENGINE_API IInputReceiver;

//\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\
//описание класса
const int mouse_device_key = 1;
const int keyboard_device_key = 2;
const int all_device_key = mouse_device_key | keyboard_device_key;
const int default_key = mouse_device_key | keyboard_device_key;

class ENGINE_API CInput
#ifndef M_BORLAND
	:
	public pureFrame,
	public pureAppActivate,
	public pureAppDeactivate
#endif
{
public:
	enum
	{
		COUNT_MOUSE_BUTTONS = 8,
		COUNT_MOUSE_AXIS = 3,
		COUNT_KB_BUTTONS = 256
	};

	struct sxr_mouse
	{
		DIDEVCAPS capabilities;
		DIDEVICEINSTANCE deviceInfo;
		DIDEVICEOBJECTINSTANCE objectInfo;
		u32 mouse_dt;
	};

	struct sxr_key
	{
		DIDEVCAPS capabilities;
		DIDEVICEINSTANCE deviceInfo;
		DIDEVICEOBJECTINSTANCE objectInfo;
	};

private:
	BENCH_SEC_SCRAMBLEMEMBER1
#ifndef XR_PLATFORM_APPLE
	LPDIRECTINPUT8 pDI; // The DInput object
	LPDIRECTINPUTDEVICE8 pMouse; // The DIDevice7 interface
	LPDIRECTINPUTDEVICE8 pKeyboard; // The DIDevice7 interface
#endif
	//----------------------
	u32 timeStamp[COUNT_MOUSE_AXIS];
	u32 timeSave[COUNT_MOUSE_AXIS];
	int offs[COUNT_MOUSE_AXIS];
	BOOL mouseState[COUNT_MOUSE_BUTTONS];

	//----------------------
	BOOL KBState[COUNT_KB_BUTTONS];

#ifndef XR_PLATFORM_APPLE
	HRESULT CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice,
	                          const DIDATAFORMAT* pdidDataFormat, u32 dwFlags,
	                          u32 buf_size);
#endif

	// xr_stack<IInputReceiver*> cbStack;
	xr_vector<IInputReceiver*> cbStack;

	void MouseUpdate();
	void KeyUpdate();

#ifdef XR_PLATFORM_APPLE
	XrInputSdl* sdl; // SDL3 state: window, pointer, text input, the controller layer
	void PadUpdate();
#endif

public:
	void resetMouseState();
	sxr_mouse mouse_property;
	sxr_key key_property;
	u32 dwCurTime;

	void SetAllAcquire(BOOL bAcquire = TRUE);
	void SetMouseAcquire(BOOL bAcquire);
	void SetKBDAcquire(BOOL bAcquire);

	void iCapture(IInputReceiver* pc);
	void iRelease(IInputReceiver* pc);
	BOOL iGetAsyncKeyState(int dik);
	BOOL iGetAsyncBtnState(int btn);
	void iGetLastMouseDelta(Ivector2& p) { p.set(offs[0], offs[1]); }

	CInput(BOOL bExclusive = true, int deviceForInit = default_key);
	~CInput();

	virtual void _BCL OnFrame(void);
	virtual void OnAppActivate(void);
	virtual void OnAppDeactivate(void);
	void DeactivateSoft();

	IInputReceiver* CurrentIR();

public:
	void exclusive_mode(const bool& exclusive);
	bool get_exclusive_mode();
	void unacquire();
	void acquire(const bool& exclusive);
	bool get_dik_name(int dik, LPSTR dest, int dest_sz);
	bool dik_to_text(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, LPSTR dest, int dest_sz);

	void feedback(u16 s1, u16 s2, float time);

#ifdef XR_PLATFORM_APPLE
	// ---- Apple platforms (SDL3) ---------------------------------------------------------------------------
	// The window events are read from. Defaults to RDEVICE.m_hWnd, which is the SDL_Window* there.
	void SetWindow(SDL_Window* window);
	SDL_Window* Window() const;

	// Hosts that poll SDL events themselves hand the keyboard, mouse and text events to CInput instead of dropping
	// them (it reads the SDL queue itself every frame otherwise). True if the event was one of those.
	bool HandleEvent(const SDL_Event& event);

	// The pointer is captured (relative mode, hidden) while the application is active. ImGui turns it off to
	// get the OS pointer; the engine's own UI cursor works from the mouse deltas and needs it on.
	void SetPointerCaptured(bool captured);
	bool IsPointerCaptured() const;

	// Pointer position in client pixels (the space of RDEVICE.clientWidth/Height), also while the pointer is
	// captured for look: then it is the virtual position the mouse deltas have moved. `global` adds the
	// window origin. False when there is no window.
	bool iGetAsyncMousePos(Ivector2& p, bool global = false) const;
	// Moves the tracked pointer (client pixels), and the OS pointer too while it is not captured.
	void iSetMousePos(const Ivector2& p);
	// Caps lock / num lock latch state (GetKeyState(VK_CAPITAL) & 1 on Windows). dik: DIK_CAPITAL, DIK_NUMLOCK.
	bool iGetKeyToggleState(int dik) const;

	// Text input: while any text field owns it, characters arrive as IR_OnTextInput (Windows-1251) and, on
	// iPad, the on-screen keyboard is up. `owner` identifies the field, so several fields can't unbalance a
	// counter; pass the field's `this`. `area` is the field's rectangle in client pixels, to keep it above the
	// keyboard (may be null).
	void EnableTextInput(const void* owner = nullptr, const Irect* area = nullptr);
	void DisableTextInput(const void* owner = nullptr);
	bool IsTextInputEnabled() const;

	// UI/gameplay switch for touch: while a dialog with a cursor is on top, a touch moves the cursor to the
	// finger; in gameplay it is ignored for look. Also forwarded to the controller layer's context.
	void SetUiContext(bool ui);

	// Radians of view rotation one mouse count makes right now (the actor's `scale`, with the live FOV and
	// sensitivity). The controller's look is converted to counts with it, so its degrees per second hold at any
	// sensitivity. 0 (default) derives it from psMouseSens.
	void SetLookRadiansPerCount(float radians);

	// What the engine side does with controller events that are not keys. Set from xrGame once.
	struct PadHooks
	{
		std::function<void(float ui_x, float ui_y)> cursor_jump; // GetUICursor().SetUICursorPosition(...)
		std::function<void(int command)> ui_action;              // 0 previous page, 1 next page (tabs)
		std::function<void(bool connected)> connection;          // pause + "reconnect the controller"
	};
	PadHooks pad_hooks;

	// The controller layer (DualSense): bindings, tuning, feedback, key capture. Created with CInput.
	gameinput::GameInput* Pad();
	gameinput::SdlPadSource* PadSource();
#endif
};

extern ENGINE_API CInput* pInput;

#endif //__XR_INPUT__
