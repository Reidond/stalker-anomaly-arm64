// Stand-in for the R4 renderer (Layers/xrRender, xrRenderDX10, xrRenderPC_R4): its two entry points, the factory that
// xrAPI's RenderFactory points at (it creates nothing, except a do-nothing UI shader the console constructor touches) and
// the console variables the engine reads.
#include "../../../src/xrEngine/stdafx.h"
#include "../../../src/Include/xrRender/UIShader.h"
#include "../../../src/Layers/xrRender/dxRenderFactory.h"

BOOL DllMainXrRenderR4(HANDLE, DWORD, LPVOID) { return TRUE; }
extern "C" bool SupportsDX11Rendering() { return false; }

Flags32 psDeviceFlags2 = {0};
Fvector4 ps_ssfx_grass_interactive = {5.f, 5.f, 5.f, 5.f};
Fvector4 ps_ssfx_wind_trees = {1.f, 1.f, 1.f, 1.f};

namespace
{
class NullUIShader : public IUIShader
{
public:
	void Copy(IUIShader&) override {}
	void create(LPCSTR, LPCSTR, bool) override {}
	bool inited() override { return false; }
};
} // namespace

dxRenderFactory RenderFactoryImpl;

#define STUB_FACTORY(Class) \
	I##Class* dxRenderFactory::Create##Class() { return nullptr; } \
	void dxRenderFactory::Destroy##Class(I##Class*) {}

STUB_FACTORY(UISequenceVideoItem)
STUB_FACTORY(StatGraphRender)
STUB_FACTORY(ConsoleRender)
STUB_FACTORY(RenderDeviceRender)
STUB_FACTORY(ApplicationRender)
STUB_FACTORY(WallMarkArray)
STUB_FACTORY(StatsRender)
STUB_FACTORY(FlareRender)
STUB_FACTORY(ThunderboltRender)
STUB_FACTORY(ThunderboltDescRender)
STUB_FACTORY(RainRender)
STUB_FACTORY(LensFlareRender)
STUB_FACTORY(ImGuiRender)
STUB_FACTORY(EnvironmentRender)
STUB_FACTORY(EnvDescriptorMixerRender)
STUB_FACTORY(EnvDescriptorRender)
STUB_FACTORY(FontRender)

IUIShader* dxRenderFactory::CreateUIShader() { return new NullUIShader; }
void dxRenderFactory::DestroyUIShader(IUIShader* shader) { delete shader; }
