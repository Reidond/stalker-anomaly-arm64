#include "stdafx.h"
#include "r4_rendertarget.h"

#if defined(XR_PLATFORM_APPLE)
#include <mtl_d3d11/mtl_upscale.h>
#endif

// Render scale. The scene was rendered at HW.SceneW() x HW.SceneH() and phase_pp() left it in rt_SceneOut. This is the end of the
// 3D part of the frame: bring it to the back buffer's size (MetalFX, or a filtered quad) so that the UI, which draws next, is
// at the output's resolution.

// The back buffer as the target, with the viewport of its size (the scene had a smaller one)
void CRenderTarget::rebind_output()
{
	u_setrt(Device.dwWidth, Device.dwHeight, HW.pBaseRT, NULL, NULL, HW_UIZB);
	RImplementation.rmNormal();
	RCache.set_CullMode(CULL_NONE);
	RCache.set_Stencil(FALSE);
}

void CRenderTarget::phase_upscale()
{
	if (!m_bSceneOutPending)
		return;
	m_bSceneOutPending = false;

	// A second-viewport frame (the scope picture) is not shown; its scene stays in rt_SceneOut for CRender::RenderToTarget
	if (Device.m_SecondViewport.IsSVPFrame())
	{
		rebind_output();
		return;
	}

	PIX_EVENT(phase_upscale);

	bool upscaled = false;
#if defined(XR_PLATFORM_APPLE)
	if (ps_r__upscaler == UPSCALER_METALFX && rt_Upscaled && mtl_d3d11::SpatialUpscalerAvailable(HW.pDevice))
		upscaled = mtl_d3d11::EncodeSpatialUpscale(HW.pContext, rt_SceneOut->pSurface, rt_Upscaled->pSurface);
#endif

	rebind_output();

	// The quad covers every pixel: a clear instead of a load of the drawable's old contents (a load action on a tile GPU, not a pass)
	{
		const FLOAT black[4] = {0.f, 0.f, 0.f, 1.f};
		HW.pContext->ClearRenderTargetView(HW.pBaseRT, black);
	}

	// Draw the image into the back buffer, one pixel to one pixel when MetalFX has done the work, filtered otherwise
	const float w = float(Device.dwWidth);
	const float h = float(Device.dwHeight);
	u32 Offset = 0;
	// the vertex shader adds half a pixel to what it is given
	const float x0 = -0.5f, y0 = -0.5f, x1 = w - 0.5f, y1 = h - 0.5f;
	const u32 C = color_rgba(255, 255, 255, 255);
	FVF::TL* pv = (FVF::TL*)RCache.Vertex.Lock(4, g_combine->vb_stride, Offset);
	pv->set(x0, y1, EPS_S, 1.f, C, 0.f, 1.f);
	pv++;
	pv->set(x0, y0, EPS_S, 1.f, C, 0.f, 0.f);
	pv++;
	pv->set(x1, y1, EPS_S, 1.f, C, 1.f, 1.f);
	pv++;
	pv->set(x1, y0, EPS_S, 1.f, C, 1.f, 0.f);
	pv++;
	RCache.Vertex.Unlock(4, g_combine->vb_stride);

	RCache.set_Element((upscaled ? s_upscale_mfx : s_upscale)->E[0]);
	// x: sharpening 0..1 (0: none), yz: texel size of the image that is read
	const float tw = upscaled ? w : float(HW.SceneW());
	const float th = upscaled ? h : float(HW.SceneH());
	RCache.set_c("upscale_params", ps_r__upscale_sharpness, 1.f / tw, 1.f / th, 0.f);
	RCache.set_Geometry(g_combine);
	RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}
