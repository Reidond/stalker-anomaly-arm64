// The real CInput (Xr_input.cpp + xr_input_sdl.cpp), IInputReceiver and line_edit_control, compiled against the
// stub Device of test_env.h and driven with synthetic SDL events and a virtual gamepad.
#include "test_env.h"

#include "xrEngine/Xr_input.cpp"
#include "xrEngine/xr_input_sdl.cpp"
#include "xrEngine/IInputReceiver.cpp"
#include "xrEngine/line_edit_control.cpp"
#include "xrEngine/edit_actions.cpp"

#include "GameInput/PadKeys.h"
#include "xrEngine/xr_input_keymap.h"

#include <cstdarg>
#include <cstring>

namespace
{
// ---- what a receiver sees ------------------------------------------------------------------------------------------
struct Recorder : public IInputReceiver
{
	std::vector<std::string> log;
	std::map<int, int> key_holds;

	static std::string fmt(const char* f, ...)
	{
		char buf[256];
		va_list args;
		va_start(args, f);
		vsnprintf(buf, sizeof(buf), f, args);
		va_end(args);
		return buf;
	}

	void IR_OnKeyboardPress(int k) override { log.push_back(fmt("kp:%d", k)); }
	void IR_OnKeyboardRelease(int k) override { log.push_back(fmt("kr:%d", k)); }
	void IR_OnKeyboardHold(int k) override { ++key_holds[k]; }
	void IR_OnMousePress(int b) override { log.push_back(fmt("bp:%d", b)); }
	void IR_OnMouseRelease(int b) override { log.push_back(fmt("br:%d", b)); }
	void IR_OnMouseHold(int b) override { log.push_back(fmt("bh:%d", b)); }
	void IR_OnMouseWheel(int d) override { log.push_back(fmt("mw:%d", d)); }
	void IR_OnMouseMove(int x, int y) override
	{
		log.push_back(fmt("mm:%d,%d", x, y));
		move_x += x;
		move_y += y;
	}
	void IR_OnTextInput(const char* t) override { log.push_back(std::string("tx:") + t); }

	int move_x = 0, move_y = 0;

	bool has(const std::string& s) const { return std::find(log.begin(), log.end(), s) != log.end(); }
	int count_prefix(const char* p) const
	{
		int n = 0;
		for (const auto& e : log)
			if (e.rfind(p, 0) == 0)
				++n;
		return n;
	}
	std::string dump() const
	{
		std::string s;
		for (const auto& e : log)
			s += e + " ";
		return s;
	}
	void clear()
	{
		log.clear();
		key_holds.clear();
		move_x = move_y = 0;
	}
};

void frame()
{
	++Device.dwFrame;
	pInput->OnFrame();
}

void step(int frames, int delay_ms = 0)
{
	for (int i = 0; i < frames; ++i)
	{
		if (delay_ms)
			SDL_Delay(delay_ms);
		frame();
	}
}

// ---- synthetic SDL events ------------------------------------------------------------------------------------------
void push_key(SDL_Scancode sc, bool down, bool repeat = false)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	e.key.scancode = sc;
	e.key.down = down;
	e.key.repeat = repeat;
	SDL_PushEvent(&e);
}

void push_motion(float x, float y, float xrel, float yrel, SDL_MouseID which = 0)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = SDL_EVENT_MOUSE_MOTION;
	e.motion.which = which;
	e.motion.x = x;
	e.motion.y = y;
	e.motion.xrel = xrel;
	e.motion.yrel = yrel;
	SDL_PushEvent(&e);
}

void push_button(Uint8 button, bool down, SDL_MouseID which = 0)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	e.button.which = which;
	e.button.button = button;
	e.button.down = down;
	e.button.clicks = 1;
	SDL_PushEvent(&e);
}

void push_wheel(float y)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = SDL_EVENT_MOUSE_WHEEL;
	e.wheel.y = y;
	SDL_PushEvent(&e);
}

void push_text(const char* utf8)
{
	// SDL frees the string of a pushed TEXT_INPUT event itself, so it must be an SDL allocation.
	SDL_Event e;
	SDL_zero(e);
	e.type = SDL_EVENT_TEXT_INPUT;
	e.text.text = SDL_strdup(utf8);
	SDL_PushEvent(&e);
}

// ---- tests ---------------------------------------------------------------------------------------------------------
void test_keyboard(Recorder& rec)
{
	printf("\n[input 1] keyboard: SDL scancodes arrive as DIK_* codes\n");

	// The table, through the whole path: event -> KBState -> receiver.
	struct Case
	{
		SDL_Scancode sc;
		int dik;
		const char* name;
	};
	const Case cases[] = {
		{SDL_SCANCODE_ESCAPE, DIK_ESCAPE, "Esc"},      {SDL_SCANCODE_A, DIK_A, "A"},
		{SDL_SCANCODE_W, DIK_W, "W"},                  {SDL_SCANCODE_1, DIK_1, "1"},
		{SDL_SCANCODE_0, DIK_0, "0"},                  {SDL_SCANCODE_RETURN, DIK_RETURN, "Return"},
		{SDL_SCANCODE_SPACE, DIK_SPACE, "Space"},      {SDL_SCANCODE_TAB, DIK_TAB, "Tab"},
		{SDL_SCANCODE_BACKSPACE, DIK_BACK, "Backspace"}, {SDL_SCANCODE_GRAVE, DIK_GRAVE, "Grave (console)"},
		{SDL_SCANCODE_LSHIFT, DIK_LSHIFT, "LShift"},   {SDL_SCANCODE_RCTRL, DIK_RCONTROL, "RCtrl"},
		{SDL_SCANCODE_LALT, DIK_LALT, "LAlt"},         {SDL_SCANCODE_RALT, DIK_RALT, "RAlt"},
		{SDL_SCANCODE_LGUI, DIK_LWIN, "LCmd"},         {SDL_SCANCODE_F1, DIK_F1, "F1"},
		{SDL_SCANCODE_F10, DIK_F10, "F10"},            {SDL_SCANCODE_F11, DIK_F11, "F11"},
		{SDL_SCANCODE_F12, DIK_F12, "F12"},            {SDL_SCANCODE_UP, DIK_UP, "Up"},
		{SDL_SCANCODE_DOWN, DIK_DOWN, "Down"},         {SDL_SCANCODE_LEFT, DIK_LEFT, "Left"},
		{SDL_SCANCODE_RIGHT, DIK_RIGHT, "Right"},      {SDL_SCANCODE_HOME, DIK_HOME, "Home"},
		{SDL_SCANCODE_END, DIK_END, "End"},            {SDL_SCANCODE_PAGEUP, DIK_PGUP, "PgUp"},
		{SDL_SCANCODE_PAGEDOWN, DIK_PGDN, "PgDn"},     {SDL_SCANCODE_INSERT, DIK_INSERT, "Insert"},
		{SDL_SCANCODE_DELETE, DIK_DELETE, "Delete"},   {SDL_SCANCODE_KP_0, DIK_NUMPAD0, "Num0"},
		{SDL_SCANCODE_KP_7, DIK_NUMPAD7, "Num7"},      {SDL_SCANCODE_KP_ENTER, DIK_NUMPADENTER, "NumEnter"},
		{SDL_SCANCODE_KP_PLUS, DIK_NUMPADPLUS, "Num+"}, {SDL_SCANCODE_KP_DIVIDE, DIK_NUMPADSLASH, "Num/"},
		{SDL_SCANCODE_CAPSLOCK, DIK_CAPSLOCK, "CapsLock"}, {SDL_SCANCODE_MINUS, DIK_MINUS, "-"},
		{SDL_SCANCODE_LEFTBRACKET, DIK_LBRACKET, "["}, {SDL_SCANCODE_SEMICOLON, DIK_SEMICOLON, ";"},
		{SDL_SCANCODE_PRINTSCREEN, DIK_SYSRQ, "PrintScreen"},
	};
	bool all_ok = true;
	for (const Case& c : cases)
	{
		rec.clear();
		push_key(c.sc, true);
		frame();
		const std::string press = Recorder::fmt("kp:%d", c.dik);
		const bool pressed = rec.has(press) && pInput->iGetAsyncKeyState(c.dik) && rec.key_holds[c.dik] >= 1;
		push_key(c.sc, false);
		frame();
		const bool released = rec.has(Recorder::fmt("kr:%d", c.dik)) && !pInput->iGetAsyncKeyState(c.dik);
		if (!pressed || !released)
		{
			all_ok = false;
			printf("    %s: scancode %d -> expected DIK 0x%02X, press=%d release=%d (log: %s)\n", c.name, int(c.sc),
			       c.dik, int(pressed), int(released), rec.dump().c_str());
		}
	}
	check(all_ok, "42 keys arrive as press, hold and release with the DirectInput numbering");

	check(xr_input::ScancodeToDik(SDL_SCANCODE_ESCAPE) == 0x01 && xr_input::ScancodeToDik(SDL_SCANCODE_F4) == 0x3E &&
	      xr_input::ScancodeToDik(SDL_SCANCODE_RIGHT) == 0xCD,
	      "DIK values are DirectInput's (Esc 0x01, F4 0x3E, Right 0xCD)");
	check(xr_input::ScancodeToDik(SDL_SCANCODE_UNKNOWN) == 0 && xr_input::ScancodeToDik(SDL_SCANCODE_COUNT) == 0,
	      "unknown scancodes map to 0 and are dropped");

	// round trip over the whole table
	int mapped = 0, roundtrip_bad = 0;
	for (int sc = 1; sc < SDL_SCANCODE_COUNT; ++sc)
	{
		const int dik = xr_input::ScancodeToDik(SDL_Scancode(sc));
		if (!dik)
			continue;
		++mapped;
		const SDL_Scancode back = xr_input::DikToScancode(dik);
		if (back == SDL_SCANCODE_UNKNOWN || xr_input::ScancodeToDik(back) != dik)
			++roundtrip_bad;
	}
	printf("  %d scancodes are mapped\n", mapped);
	check(mapped > 100 && roundtrip_bad == 0, "every mapped scancode round-trips through its DIK");

	// auto-repeat and duplicate edges are the engine's job (Hold), not SDL's
	rec.clear();
	push_key(SDL_SCANCODE_D, true);
	push_key(SDL_SCANCODE_D, true, true);
	push_key(SDL_SCANCODE_D, true, true);
	frame();
	check(rec.count_prefix("kp:") == 1, "SDL key repeats don't produce extra presses");
	frame();
	check(rec.key_holds[DIK_D] == 2, "the key is held once per frame while down");
	push_key(SDL_SCANCODE_D, false);
	frame();

	// deactivating (focus lost) releases what is down
	rec.clear();
	push_key(SDL_SCANCODE_S, true);
	frame();
	pInput->OnAppDeactivate();
	check(!pInput->iGetAsyncKeyState(DIK_S), "deactivation clears the key state");
	pInput->OnAppActivate();
	push_key(SDL_SCANCODE_S, false); // the release of a key already forgotten must not be delivered
	rec.clear();
	frame();
	check(rec.count_prefix("kr:") == 0, "a release without a press is dropped");
}

void test_mouse(Recorder& rec, SDL_Window* window)
{
	printf("\n[input 2] mouse: relative deltas, buttons, wheel, absolute position\n");
	rec.clear();

	check(pInput->IsPointerCaptured(), "pointer captured (relative mode) while the app is active");
	check(SDL_GetWindowRelativeMouseMode(window), "SDL relative mouse mode is on for the window");

	push_motion(320, 240, 5, -3);
	frame();
	check(rec.has("mm:5,-3"), "relative motion 5,-3 reaches IR_OnMouseMove(5,-3)");
	int ix = 0, iy = 0;
	{
		Ivector2 d;
		pInput->iGetLastMouseDelta(d);
		ix = d.x;
		iy = d.y;
	}
	check(ix == 5 && iy == -3, "iGetLastMouseDelta returns the same counts");

	// slow motion is carried, not rounded away
	rec.clear();
	for (int i = 0; i < 4; ++i)
		push_motion(0, 0, 0.5f, 0.25f);
	frame();
	check(rec.has("mm:2,1"), "four 0.5/0.25 steps add up to 2,1 counts in one frame");
	rec.clear();
	push_motion(0, 0, 0.4f, 0.f);
	frame();
	check(rec.count_prefix("mm:") == 0, "0.4 counts is held back this frame");
	push_motion(0, 0, 0.7f, 0.f);
	frame();
	check(rec.has("mm:1,0"), "and delivered with the next motion (0.4 + 0.7 = 1)");

	// buttons: SDL left/middle/right/X1/X2 -> engine 0/2/1/3/4
	const struct
	{
		Uint8 sdl;
		int idx;
		const char* name;
	} buttons[] = {
		{SDL_BUTTON_LEFT, 0, "left"}, {SDL_BUTTON_RIGHT, 1, "right"}, {SDL_BUTTON_MIDDLE, 2, "middle"},
		{SDL_BUTTON_X1, 3, "X1"},     {SDL_BUTTON_X2, 4, "X2"},
	};
	bool buttons_ok = true;
	for (const auto& b : buttons)
	{
		rec.clear();
		push_button(b.sdl, true);
		frame();
		const bool down = rec.has(Recorder::fmt("bp:%d", b.idx)) && pInput->iGetAsyncBtnState(b.idx) &&
			pInput->iGetAsyncKeyState(0xED + 100 + b.idx);
		frame();
		const bool held = rec.has(Recorder::fmt("bh:%d", b.idx));
		push_button(b.sdl, false);
		frame();
		const bool up = rec.has(Recorder::fmt("br:%d", b.idx)) && !pInput->iGetAsyncBtnState(b.idx);
		if (!down || !held || !up)
		{
			buttons_ok = false;
			printf("    %s: down=%d held=%d up=%d (%s)\n", b.name, int(down), int(held), int(up), rec.dump().c_str());
		}
	}
	check(buttons_ok, "5 buttons: press, hold, release; MOUSE_n keys follow (left 0, right 1, middle 2)");

	// wheel: 120 per notch, fractional (trackpad) scrolls accumulate
	rec.clear();
	push_wheel(1.f);
	frame();
	check(rec.has("mw:120"), "one notch up is +120");
	rec.clear();
	push_wheel(-2.f);
	frame();
	check(rec.has("mw:-240"), "two notches down is -240");
	rec.clear();
	push_wheel(0.5f);
	push_wheel(0.5f);
	frame();
	check(rec.has("mw:120"), "two half notches make one");

	// absolute position: window 640x480, client 1280x960 -> pixels double
	pInput->SetPointerCaptured(false);
	check(!pInput->IsPointerCaptured(), "SetPointerCaptured(false) gives the pointer back");
	push_motion(100, 50, 7, 7);
	frame();
	Ivector2 p;
	check(pInput->iGetAsyncMousePos(p) && p.x == 200 && p.y == 100, "absolute position (100,50) points is (200,100) client pixels");
	IInputReceiver::IR_GetMousePosReal(p);
	check(p.x == 200 && p.y == 100, "IR_GetMousePosReal returns it");
	Fvector2 f;
	IInputReceiver::IR_GetMousePosIndependent(f);
	check(fabsf(f.x - (2.f * 200 / 1280 - 1.f)) < 1e-5f && fabsf(f.y - (2.f * 100 / 960 - 1.f)) < 1e-5f,
	      "IR_GetMousePosIndependent normalizes to -1..1");
	pInput->SetPointerCaptured(true);
	push_motion(0, 0, 10, 20); // the virtual pointer keeps moving by the deltas while captured
	frame();
	pInput->iGetAsyncMousePos(p);
	check(p.x == 220 && p.y == 140, "captured: the pointer is the old position plus the deltas (110,70 points)");
	pInput->iSetMousePos(Ivector2().set(640, 480));
	pInput->iGetAsyncMousePos(p);
	check(p.x == 640 && p.y == 480, "iSetMousePos moves the tracked pointer");
	rec.clear();
	step(3);
	check(rec.count_prefix("mm:") == 0, "no motion events, no motion calls");
}

void test_ui_and_touch(Recorder& rec)
{
	printf("\n[input 3] UI context: cursor units and touch\n");
	rec.clear();
	pInput->SetUiContext(true);
	// 640x480 window, UI 1024x768: 5 points are 8 UI units on both axes
	push_motion(0, 0, 5, 5);
	frame();
	check(rec.has("mm:8,8"), "in a dialog, motion is in UI units (5 points = 8 units)");

	rec.clear();
	pInput->iSetMousePos(Ivector2().set(0, 0));
	push_motion(100, 50, 0, 0, SDL_TOUCH_MOUSEID);
	frame();
	check(rec.has("mm:160,80"), "a touch in a dialog moves the cursor to the finger (100,50 points = 160,80 units)");
	push_button(SDL_BUTTON_LEFT, true, SDL_TOUCH_MOUSEID);
	frame();
	check(rec.has("bp:0"), "a tap in a dialog is a left click");
	push_button(SDL_BUTTON_LEFT, false, SDL_TOUCH_MOUSEID);
	frame();

	pInput->SetUiContext(false);
	rec.clear();
	push_motion(300, 200, 0, 0, SDL_TOUCH_MOUSEID);
	push_button(SDL_BUTTON_LEFT, true, SDL_TOUCH_MOUSEID);
	push_button(SDL_BUTTON_LEFT, false, SDL_TOUCH_MOUSEID);
	frame();
	check(rec.count_prefix("mm:") == 0 && rec.count_prefix("bp:") == 0, "in the world a touch neither turns the view nor fires");
}

void test_text(Recorder& rec)
{
	printf("\n[input 4] text input, Windows-1251 and line_edit_control\n");
	SDL_Window* window = pInput->Window();

	// keymap helpers
	char out[8] = {};
	check(xr_input::DikToText(DIK_A, false, false, false, false, false, out, sizeof(out)) && !strcmp(out, "a"), "A types a");
	check(xr_input::DikToText(DIK_A, true, false, false, false, false, out, sizeof(out)) && !strcmp(out, "A"), "Shift+A types A");
	check(xr_input::DikToText(DIK_A, false, true, false, false, false, out, sizeof(out)) && !strcmp(out, "A"), "CapsLock+A types A");
	check(xr_input::DikToText(DIK_1, true, false, false, false, false, out, sizeof(out)) && !strcmp(out, "!"), "Shift+1 types !");
	check(!xr_input::DikToText(DIK_A, false, false, true, false, false, out, sizeof(out)), "Ctrl+A types nothing");
	check(!xr_input::DikToText(DIK_LSHIFT, false, false, false, false, false, out, sizeof(out)), "a modifier types nothing");
	check(!xr_input::DikToText(DIK_F1, false, false, false, false, false, out, sizeof(out)), "F1 types nothing");
	char name[64] = {};
	check(xr_input::DikName(DIK_A, name, sizeof(name)) && !strcmp(name, "A"), "key name of A is \"A\"");
	check(xr_input::DikName(DIK_LSHIFT, name, sizeof(name)) && strlen(name) > 0, "key name of Left Shift exists");
	char cp[32] = {};
	xr_input::Utf8ToCp1251(u8"Привет, Зона! € 中", cp, sizeof(cp));
	check(!strcmp(cp, "\xCF\xF0\xE8\xE2\xE5\xF2, \xC7\xEE\xED\xE0! \x88 ?"), "UTF-8 to Windows-1251: Cyrillic, euro, unmappable -> ?");

	// through CInput::dik_to_text: key events type while text input is off ...
	check(!pInput->IsTextInputEnabled() && !SDL_TextInputActive(window), "text input starts off");
	char buf[8] = {};
	check(pInput->dik_to_text(DIK_Q, false, false, false, false, false, buf, sizeof(buf)) && !strcmp(buf, "q"),
	      "dik_to_text(Q) = q while text input is off");
	text_editor::line_edit_control ec(128);
	ec.on_key_press(DIK_H);
	ec.on_key_press(DIK_I);
	check(!strcmp(ec.str_edit(), "hi"), "line_edit_control: hardware key presses type through dik_to_text");
	push_key(SDL_SCANCODE_LSHIFT, true); // the engine passes the modifier on after CInput has it
	frame();
	ec.on_key_press(DIK_LSHIFT);
	ec.on_key_press(DIK_A);
	check(!strcmp(ec.str_edit(), "hiA"), "... Shift gives capitals");
	ec.on_key_release(DIK_LSHIFT);
	push_key(SDL_SCANCODE_LSHIFT, false);
	frame();

	// ... and the text events take over once a field owns text input
	int owner = 0;
	Irect area;
	area.set(100, 600, 900, 660);
	pInput->EnableTextInput(&owner, &area);
	check(pInput->IsTextInputEnabled() && SDL_TextInputActive(window), "EnableTextInput starts SDL text input (the on-screen keyboard on iPad)");
	check(!pInput->dik_to_text(DIK_Q, false, false, false, false, false, buf, sizeof(buf)), "dik_to_text yields to text events (no double typing)");
	ec.clear_states();
	ec.on_key_press(DIK_H);
	check(!strcmp(ec.str_edit(), ""), "a key press types nothing while text events are on");

	rec.clear();
	push_text(u8"Привет a");
	frame();
	check(rec.has("tx:\xCF\xF0\xE8\xE2\xE5\xF2 a"), "a text event reaches IR_OnTextInput as Windows-1251");
	ec.on_text_input("\xCF\xF0\xE8\xE2\xE5\xF2 a");
	check(!strcmp(ec.str_edit(), "\xCF\xF0\xE8\xE2\xE5\xF2 a"), "line_edit_control::on_text_input inserts it at the cursor");
	ec.on_key_press(DIK_BACK);
	check(!strcmp(ec.str_edit(), "\xCF\xF0\xE8\xE2\xE5\xF2 "), "Backspace still works from key events");
	ec.on_key_press(DIK_HOME);
	ec.on_text_input(">");
	check(!strcmp(ec.str_edit(), ">\xCF\xF0\xE8\xE2\xE5\xF2 "), "text goes in at the cursor");
	push_key(SDL_SCANCODE_LCTRL, true);
	frame();
	ec.on_key_press(DIK_LCONTROL);
	ec.on_key_press(DIK_A); // select all
	ec.on_text_input("x");
	check(!strcmp(ec.str_edit(), "x"), "Ctrl+A, then typing replaces the selection");
	ec.on_key_release(DIK_LCONTROL);
	push_key(SDL_SCANCODE_LCTRL, false);
	frame();

	// modes
	ec.init(64, text_editor::im_number_only);
	ec.on_text_input("a1b2-+c");
	check(!strcmp(ec.str_edit(), "12-+"), "number-only mode keeps digits and signs");
	ec.init(64, text_editor::im_read_only);
	ec.on_text_input("x");
	check(!strcmp(ec.str_edit(), ""), "read-only mode takes no text");
	ec.init(64, text_editor::im_file_name_mode);
	ec.on_text_input("sa/ve:1*?");
	check(!strcmp(ec.str_edit(), "save1"), "file-name mode drops / : * ?");
	ec.init(8, text_editor::im_standart);
	ec.on_text_input("0123456789");
	check(strlen(ec.str_edit()) <= 7, "the buffer size is respected");

	// a second field keeps text input on until the last one lets go
	int owner2 = 0;
	pInput->EnableTextInput(&owner2);
	pInput->DisableTextInput(&owner);
	check(pInput->IsTextInputEnabled() && SDL_TextInputActive(window), "text input stays on while another field owns it");
	pInput->DisableTextInput(&owner2);
	check(!pInput->IsTextInputEnabled() && !SDL_TextInputActive(window), "and stops (keyboard hidden) when the last field lets go");
	pInput->DisableTextInput(&owner2); // unbalanced calls are harmless
	check(!pInput->IsTextInputEnabled(), "an extra DisableTextInput changes nothing");

	// the toggle keys line_edit_control asks about
	check(!ec.get_key_state(text_editor::ks_CapsLock), "caps lock state is read through iGetKeyToggleState");
}

void test_pad(Recorder& rec)
{
	printf("\n[input 5] DualSense layer: pad keys, look, cursor (virtual gamepad)\n");

	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.button_mask = 0xFFFFFFFFu;
	desc.axis_mask = 0xFFFFFFFFu;
	desc.name = "xrinput_smoke virtual pad";
	desc.vendor_id = 0x054C;
	desc.product_id = 0x0CE6; // DualSense
	const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
	check(id != 0, "virtual gamepad attached");
	if (!id)
	{
		printf("    %s\n", SDL_GetError());
		return;
	}
	SDL_Joystick* js = SDL_OpenJoystick(id);
	check(js != nullptr, "virtual joystick opened");

	int connection_events = 0;
	bool last_connected = false;
	pInput->pad_hooks.connection = [&](bool connected)
	{
		++connection_events;
		last_connected = connected;
	};

	step(3, 10);
	check(pInput->Pad() != nullptr && pInput->Pad()->devices().Active() != nullptr, "the controller layer found the pad");
	check(connection_events >= 1 && last_connected, "pad_hooks.connection(true) fired");

	using namespace gameinput;
	// Cross -> gp_cross (jump), R2 -> gp_r2, left stick right -> gp_lstick_right
	rec.clear();
	SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, true);
	step(2, 10);
	check(rec.has(Recorder::fmt("kp:%d", int(PK_Cross))), "Cross reaches the receiver as pad key gp_cross (1200)");
	check(rec.key_holds[PK_Cross] >= 1, "... with a Hold every frame");
	check(pInput->iGetAsyncKeyState(PK_Cross), "iGetAsyncKeyState(gp_cross) is true while held");
	SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, false);
	step(2, 10);
	check(rec.has(Recorder::fmt("kr:%d", int(PK_Cross))) && !pInput->iGetAsyncKeyState(PK_Cross), "release follows");

	rec.clear();
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 32767);
	step(3, 10);
	check(rec.has(Recorder::fmt("kp:%d", int(PK_LStickRight))), "left stick right becomes gp_lstick_right (strafe)");
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 0);
	step(3, 10);
	check(rec.has(Recorder::fmt("kr:%d", int(PK_LStickRight))), "and releases at centre");

	// analog look from the right stick adds to the mouse deltas
	step(1, 15);
	rec.clear();
	const Uint64 t0 = SDL_GetTicksNS();
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
	step(12, 15);
	const double seconds = double(SDL_GetTicksNS() - t0) * 1e-9;
	const int counts_x = rec.move_x, counts_y = rec.move_y;
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, 0);
	step(2, 15);
	check(counts_x > 5 && counts_y == 0, "right stick right becomes positive IR_OnMouseMove counts, none vertical");
	// the conversion: counts * (degrees per count) is what the layer turned
	const double deg_per_count = (psMouseSens * psMouseSensScale / 50.0) * 180.0 / 3.14159265;
	const double rate = counts_x * deg_per_count / seconds;
	printf("  full right tilt for %.2f s: %d counts = %.1f degrees = %.0f deg/s at %.3f degrees per count\n", seconds, counts_x,
	       counts_x * deg_per_count, rate, deg_per_count);
	check(rate > 0.6 * 220.0 && rate < 1.5 * 220.0, "... about the layer's 220 degrees per second at full tilt (the frames' own timing varies)");

	// the same turn with the mouse sensitivity halved: the layer's degrees per second do not change
	const float saved_sens = psMouseSens;
	psMouseSens = saved_sens * 0.5f;
	step(1, 15);
	rec.clear();
	const Uint64 t1 = SDL_GetTicksNS();
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
	step(12, 15);
	const double seconds2 = double(SDL_GetTicksNS() - t1) * 1e-9;
	const int counts_half = rec.move_x;
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, 0);
	step(2, 15);
	const double rate2 = counts_half * (deg_per_count * 0.5) / seconds2;
	psMouseSens = saved_sens;
	const double count_ratio = (counts_half / seconds2) / (counts_x / seconds);
	printf("  at half the sensitivity: %d counts in %.2f s (%.2fx the counts per second), %.0f deg/s\n", counts_half, seconds2, count_ratio, rate2);
	check(count_ratio > 1.5 && count_ratio < 2.5 && rate2 > 0.6 * 220.0 && rate2 < 1.5 * 220.0,
	      "at half the mouse sensitivity the same turn takes twice the counts (and the same degrees per second)");

	rec.clear();
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTY, 32767); // stick down = look down = +y like a mouse
	step(12, 15);
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTY, 0);
	step(2, 15);
	check(rec.move_y > 3 && rec.move_x == 0, "right stick down is positive vertical counts (mouse convention)");

	// menu context: the same buttons are mouse clicks and the stick moves the cursor
	pInput->SetUiContext(true);
	step(2, 10);
	rec.clear();
	SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, true);
	step(2, 10);
	SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_SOUTH, false);
	step(2, 10);
	check(rec.has(Recorder::fmt("kp:%d", 0xED + 100)) && rec.has(Recorder::fmt("kr:%d", 0xED + 100)),
	      "in a dialog Cross is MOUSE_1 (left click), press and release");
	rec.clear();
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 32767);
	step(10, 15);
	SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, 0);
	step(2, 15);
	printf("  left stick held 0.15 s in a dialog: %d,%d UI units\n", rec.move_x, rec.move_y);
	check(rec.move_x > 20 && rec.move_y == 0, "the left stick moves the cursor (UI units through IR_OnMouseMove)");
	pInput->SetUiContext(false);
	step(2, 10);

	// feedback
	pInput->feedback(65535, 65535, 0.5f);
	check(pInput->Pad()->feedback().Rumble().Active(), "CInput::feedback (HUDManager::HitMarked) starts a rumble hit");

	// disconnect releases everything and tells the game
	rec.clear();
	SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_EAST, true);
	step(2, 10);
	check(pInput->iGetAsyncKeyState(PK_Circle), "Circle held");
	connection_events = 0;
	SDL_CloseJoystick(js);
	SDL_DetachVirtualJoystick(id);
	step(3, 10);
	check(!pInput->iGetAsyncKeyState(PK_Circle) && rec.has(Recorder::fmt("kr:%d", int(PK_Circle))), "unplugging releases every held key");
	check(connection_events >= 1 && !last_connected, "pad_hooks.connection(false) fired (the game pauses)");
	pInput->pad_hooks.connection = nullptr;
}

void test_altf4_and_teardown(Recorder& rec)
{
	printf("\n[input 6] Alt+F4 and teardown\n");
	Engine.Event.deferred.clear();
	push_key(SDL_SCANCODE_LALT, true);
	push_key(SDL_SCANCODE_F4, true);
	frame();
	check(Engine.Event.deferred.size() == 2 && Engine.Event.deferred[1] == "KERNEL:quit", "Alt+F4 defers KERNEL:disconnect and KERNEL:quit");
	b_altF4 = FALSE;
	push_key(SDL_SCANCODE_F4, false);
	push_key(SDL_SCANCODE_LALT, false);
	frame();
}
} // namespace

CRenderDevice Device;
CEngine Engine;
int g_failures = 0;

void check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
	if (!ok)
		++g_failures;
}

int run_input_tests()
{
	// the dummy video driver gives a real window (and relative mouse mode) without a display
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		printf("SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow("xrinput_smoke", 640, 480, 0);
	check(window != nullptr, "SDL window (dummy video driver)");
	if (!window)
		return 1;

	StubStats stats;
	Device.Statistic = &stats;
	Device.m_hWnd = window;
	Device.clientWidth = 1280;
	Device.clientHeight = 960;

	pInput = new CInput(FALSE);
	check(pInput->Window() == window, "CInput takes its window from RDEVICE.m_hWnd");
	check(pInput->CurrentIR() == &dummyController, "the dummy receiver is on the stack");

	Recorder rec;
	rec.IR_Capture();
	check(pInput->CurrentIR() == &rec, "IR_Capture puts the test receiver on top");

	test_keyboard(rec);
	test_mouse(rec, window);
	test_ui_and_touch(rec);
	test_text(rec);
	test_pad(rec);
	test_altf4_and_teardown(rec);

	rec.IR_Release();
	check(pInput->CurrentIR() == &dummyController, "IR_Release restores the previous receiver");
	delete pInput;
	pInput = nullptr;
	check(!SDL_GetWindowRelativeMouseMode(window), "teardown gives the pointer back");
	SDL_DestroyWindow(window);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return g_failures;
}
