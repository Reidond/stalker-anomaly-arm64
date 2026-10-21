// Stand-in for the files of the engine-input session (Xr_input.cpp, IInputReceiver.cpp, line_edit_control.cpp,
// imgui_base_input.cpp, MonitorList.cpp). Compiled only while src/xrEngine/xr_input_sdl.cpp does not exist; when that
// session's port lands, cmake/targets/xrEngine.cmake builds the real files and this one drops out.
// Nothing is read from a keyboard or mouse; the console's line editor and ImGui get no input.
#include "../../../src/xrEngine/stdafx.h"
#include "../../../src/xrEngine/xr_input.h"
#include "../../../src/xrEngine/IInputReceiver.h"
#include "../../../src/xrEngine/line_edit_control.h"
#include "../../../src/xrEngine/MonitorList.h"

// ---- Xr_input.cpp ----------------------------------------------------------------------------------------------------
ENGINE_API CInput* pInput = nullptr;
ENGINE_API float psMouseSens = 1.f;
ENGINE_API float psMouseSensScale = 1.f;
ENGINE_API float psMouseSensVerticalK = 1.f;
ENGINE_API Flags32 psMouseInvert = {FALSE};

CInput::CInput(BOOL, int) {}
CInput::~CInput() {}
void CInput::OnFrame() {}
void CInput::OnAppActivate() {}
void CInput::OnAppDeactivate() {}
bool CInput::get_exclusive_mode() { return false; }

// ---- IInputReceiver.cpp ----------------------------------------------------------------------------------------------
void IInputReceiver::IR_Capture() {}
void IInputReceiver::IR_Release() {}
void IInputReceiver::IR_OnDeactivate() {}
void IInputReceiver::IR_OnActivate() {}

// ---- line_edit_control.cpp -------------------------------------------------------------------------------------------
namespace text_editor
{
void remove_spaces(PSTR) {}
void split_cmd(PSTR first, PSTR second, LPCSTR str)
{
	first[0] = 0;
	second[0] = 0;
	(void)str;
}

line_edit_control::line_edit_control(u32) {}
line_edit_control::~line_edit_control() {}
void line_edit_control::clear_states() {}
void line_edit_control::on_key_press(int) {}
void line_edit_control::on_key_hold(int) {}
void line_edit_control::on_key_release(int) {}
void line_edit_control::on_frame() {}
void line_edit_control::assign_callback(u32 const, key_state, Callback const&) {}
void line_edit_control::set_edit(LPCSTR) {}
} // namespace text_editor

// ---- imgui_base_input.cpp ---------------------------------------------------------------------------------------------
namespace xr_imgui
{
void ide::EnableInput(bool) {}
void ide::OnAppActivate() {}
void ide::OnAppDeactivate() {}
void ide::IR_Capture() {}
void ide::IR_Release() {}
void ide::IR_OnMousePress(int) {}
void ide::IR_OnMouseRelease(int) {}
void ide::IR_OnMouseWheel(int) {}
void ide::IR_OnMouseMove(int, int) {}
void ide::IR_OnKeyboardPress(int) {}
void ide::IR_OnKeyboardRelease(int) {}
} // namespace xr_imgui

// ---- MonitorList.cpp -------------------------------------------------------------------------------------------------
ENGINE_API xr_token* vid_monitor_token = nullptr;
ENGINE_API xr_string vid_monitor_name = "Auto";
ENGINE_API volatile long g_monitor_list_dirty = 0;

void fill_vid_monitor_list() {}
void free_vid_monitor_list() {}
void refresh_vid_monitor_list() {}
HMONITOR ResolveSelectedMonitor() { return nullptr; }
