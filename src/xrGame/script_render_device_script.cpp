////////////////////////////////////////////////////////////////////////////
//	Module 		: script_render_device_script.cpp
//	Created 	: 28.06.2004
//  Modified 	: 28.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Script render device script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_render_device.h"

using namespace luabind;

bool is_device_paused(CRenderDevice* d)
{
	return !!Device.Paused();
}

void set_device_paused(CRenderDevice* d, bool b)
{
	Device.Pause(b, TRUE, FALSE, "set_device_paused_script");
}

extern ENGINE_API BOOL bShowPauseString;

void set_device_paused_ex(CRenderDevice* d, bool b)
{
	Device.Pause(b, TRUE, TRUE, "set_device_paused_ex_script");
	bShowPauseString = FALSE;
}

extern ENGINE_API BOOL g_appLoaded;

bool is_app_ready()
{
	return !!g_appLoaded;
}

u32 time_global(const CRenderDevice* self)
{
	THROW(self);
	return (self->dwTimeGlobal);
}

u32 time_continual(const CRenderDevice* self)
{
	THROW(self);
	return (self->dwTimeContinual);
}

// luabind's def_readonly(D T::*) does not deduce through a base class (the members live in CRenderDevice's bases); MSVC did.
template <class D, class B>
static D CRenderDevice::* dev_member(D B::* member) { return member; }

#pragma optimize("s",on)
template<> void CScriptRenderDevice::script_register(lua_State* L)
{
	module(L)
	[
		class_<CRenderDevice>("render_device")
		.def_readonly("width", dev_member(&CRenderDevice::dwWidth))
		.def_readonly("height", dev_member(&CRenderDevice::dwHeight))
		.def_readonly("time_delta", dev_member(&CRenderDevice::dwTimeDelta))
		.def_readonly("f_time_delta", dev_member(&CRenderDevice::fTimeDelta))
		.def_readonly("cam_pos", dev_member(&CRenderDevice::vCameraPosition))
		.def_readonly("cam_dir", dev_member(&CRenderDevice::vCameraDirection))
		.def_readonly("cam_top", dev_member(&CRenderDevice::vCameraTop))
		.def_readonly("cam_right", dev_member(&CRenderDevice::vCameraRight))
		//			.def_readonly("view",					&CRenderDevice::mView)
		//			.def_readonly("projection",				&CRenderDevice::mProject)
		//			.def_readonly("full_transform",			&CRenderDevice::mFullTransform)
		.def_readonly("fov", dev_member(&CRenderDevice::fFOV))
		.def_readonly("aspect_ratio", dev_member(&CRenderDevice::fASPECT))
		.def("time_global", &time_global)
		.def("time_continual", &time_continual)
		.def_readonly("precache_frame", dev_member(&CRenderDevice::dwPrecacheFrame))
		.def_readonly("frame", dev_member(&CRenderDevice::dwFrame))
		.def("is_paused", &is_device_paused)
		.def("pause", &set_device_paused)
		.def("pause_ex", &set_device_paused_ex),
		def("app_ready", &is_app_ready)
	];
}
