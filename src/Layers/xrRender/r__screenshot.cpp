#include "stdafx.h"
//#include "../../xrEngine/xr_effgamma.h"
#include "xr_effgamma.h"
#include "dxRenderDeviceRender.h"
#include "../xrRender/tga.h"
#include "../../xrEngine/xrImage_Resampler.h"

#if defined(USE_DX10) || defined(USE_DX11)
#include "d3dx10tex.h"
#endif	//	USE_DX10

#define	GAMESAVE_SIZE	128

extern u32 ps_r_screenshot_token;
extern int ps_r4_hdr10_on;

#if defined(XR_PLATFORM_APPLE)
#include <mtl_d3d11/mtl_d3d11.h>

// The Metal back buffer holds nothing once it was presented, so a screenshot is taken from the frame being presented:
// the console command queues it (optionally some frames ahead), dxRenderDeviceRender::End asks the swap chain for a
// capture before Present and writes the file when it arrives, one or two frames later. It is what is on screen,
// post-processing and UI included.
namespace
{
struct AppleShot
{
	bool queued = false;
	bool armed = false;
	int delay = 0;
	int waited = 0;
	string_path name = "";
} g_apple_shot;
} // namespace

void AppleQueueScreenshot(LPCSTR name, int delay)
{
	if (g_apple_shot.queued || g_apple_shot.armed)
	{
		Msg("! screenshot: one is still pending");
		return;
	}
	g_apple_shot.queued = true;
	g_apple_shot.delay = delay < 0 ? 0 : delay;
	Msg("* screenshot: queued '%s', %d frames ahead", name ? name : "", g_apple_shot.delay);
	g_apple_shot.name[0] = 0;
	if (name && name[0])
		xr_strcpy(g_apple_shot.name, sizeof(g_apple_shot.name), name);
}

// ---- load_shots: the loading screen while a level loads ----------------------------------------------------------------
// load_shots <prefix>: a screenshot (<prefix>_<n>.png) of every loading-screen frame that shows a new load stage; "" switches it off.
// The load blocks the main loop for many seconds, so a script step cannot ask for these.
namespace
{
string64 g_load_shots = "";
int g_load_shots_last = -1;
int g_load_shots_count = 0;
} // namespace

void AppleSetLoadShots(LPCSTR prefix)
{
	xr_strcpy(g_load_shots, prefix ? prefix : "");
	g_load_shots_last = -1;
	g_load_shots_count = 0;
}

void AppleLoadScreenshot(int stage, int max_stage)
{
	if (!g_load_shots[0] || stage == g_load_shots_last)
		return;
	g_load_shots_last = stage;
	string_path name;
	xr_sprintf(name, "%s_%02d_stage%d_of_%d", g_load_shots, g_load_shots_count++, stage, max_stage);
	AppleQueueScreenshot(name, 0);
}

// ---- rt_dump: look at intermediate render targets -------------------------------------------------------------------
// rt_dump <target> <file> [stage] [mode] [lo hi]
//   target: a render target name ($user$accum, $user$position, ...), `depth` (the scene depth-stencil), `smap` (the shadow map)
//   stage:  where in the frame: gbuffer (after the G-buffer and wallmarks), rain_patch, rain_end (inside the wet-surface layer),
//           lit (before the post-process), frame_end (default)
//   mode:   c = RGB (default), s = signed RGB, r/g/b/a = one channel as grey, d = depth, st = stencil; lo hi = the range for grey
// The image goes to $screenshots$\<file>.png (8 bit); the log line has the format, size, min, max and mean.
namespace
{
struct RTDump
{
	bool pending = false;
	string64 target = "";
	string64 file = "";
	string32 stage = "frame_end";
	string32 mode = "c";
	float lo = 0, hi = 0;
	u32 crop[4] = {0, 0, 0, 0};
} g_rt_dump;
} // namespace

void AppleQueueRTDump(LPCSTR target, LPCSTR file, LPCSTR stage, LPCSTR mode, float lo, float hi, const u32* crop)
{
	for (int i = 0; i < 4; ++i)
		g_rt_dump.crop[i] = crop ? crop[i] : 0;
	xr_strcpy(g_rt_dump.target, target);
	xr_strcpy(g_rt_dump.file, file);
	xr_strcpy(g_rt_dump.stage, stage && *stage ? stage : "frame_end");
	xr_strcpy(g_rt_dump.mode, mode && *mode ? mode : "c");
	g_rt_dump.lo = lo;
	g_rt_dump.hi = hi;
	g_rt_dump.pending = true;
	Msg("* rt_dump: '%s' at %s (%s) queued", target, g_rt_dump.stage, g_rt_dump.mode);
}

void AppleRTDumpHook(const char* stage)
{
	if (!g_rt_dump.pending || xr_strcmp(stage, g_rt_dump.stage))
		return;
	g_rt_dump.pending = false;

	ID3DResource* res = nullptr;
	bool release = false;
	if (!xr_strcmp(g_rt_dump.target, "depth"))
	{
		if (HW.pBaseZB)
		{
			HW.pBaseZB->GetResource(&res);
			release = true;
		}
	}
	else if (!xr_strcmp(g_rt_dump.target, "smap"))
		res = RImplementation.Target->rt_smap_depth ? RImplementation.Target->rt_smap_depth->pSurface : nullptr;
	else if (!strncmp(g_rt_dump.target, "tex:", 4))
	{
		// a loaded texture by its engine name (tex:ui\\ui_icon_equipment)
		if (CTexture* t = dxRenderDeviceRender::Instance().Resources->DebugFindTexture(g_rt_dump.target + 4))
			res = t->surface_get();
	}
	else if (CRT* rt = dxRenderDeviceRender::Instance().Resources->DebugFindRT(g_rt_dump.target))
		res = rt->pSurface;
	if (!res)
	{
		// a texture that is not loaded yet (a loading-screen texture, asked for before the load): look again at the next frames
		static int s_retries = 0;
		if (!strncmp(g_rt_dump.target, "tex:", 4) && s_retries++ < 3000)
		{
			g_rt_dump.pending = true;
			return;
		}
		s_retries = 0;
		Msg("! rt_dump: no render target '%s'", g_rt_dump.target);
		return;
	}

	mtl_d3d11::DebugReadOptions opt;
	const char* m = g_rt_dump.mode;
	opt.lo = g_rt_dump.lo;
	opt.hi = g_rt_dump.hi;
	opt.cropX = g_rt_dump.crop[0];
	opt.cropY = g_rt_dump.crop[1];
	opt.cropW = g_rt_dump.crop[2];
	opt.cropH = g_rt_dump.crop[3];
	if (!xr_strcmp(m, "s"))
		opt.mode = mtl_d3d11::DebugReadOptions::Signed;
	else if (!xr_strcmp(m, "c"))
		opt.mode = mtl_d3d11::DebugReadOptions::Colour;
	else
	{
		opt.mode = mtl_d3d11::DebugReadOptions::Channel;
		if (!xr_strcmp(m, "g")) opt.channel = 1;
		else if (!xr_strcmp(m, "b")) opt.channel = 2;
		else if (!xr_strcmp(m, "a")) opt.channel = 3;
		else if (!xr_strcmp(m, "st")) opt.stencil = true;
	}
	mtl_d3d11::DebugImage img;
	if (!mtl_d3d11::DebugReadTexture(HW.pContext, res, opt, &img))
		Msg("! rt_dump: cannot read '%s'", g_rt_dump.target);
	else
	{
		mtl_d3d11::CaptureResult cap;
		cap.width = img.width;
		cap.height = img.height;
		cap.rgba = std::move(img.rgba);
		std::vector<uint8_t> png;
		string_path name;
		xr_sprintf(name, sizeof(name), "%s.png", g_rt_dump.file);
		if (mtl_d3d11::EncodeCapture(cap, 1, &png))
			if (IWriter* fs = FS.w_open("$screenshots$", name))
			{
				fs->w(png.data(), (u32)png.size());
				FS.w_close(fs);
			}
		Msg("* rt_dump: %s '%s' %ux%u %s min %.4f max %.4f mean %.4f", name, g_rt_dump.target, img.width, img.height,
		    img.format.c_str(), img.minValue, img.maxValue, img.mean);
	}
	FlushLog(); // scripted runs are sometimes killed on the way out, and the engine log is buffered
	if (release)
		_RELEASE(res);
}

void AppleScreenshotBeforePresent()
{
	if (!g_apple_shot.queued || ps_r4_hdr10_on)
		return;
	if (g_apple_shot.delay > 0)
	{
		--g_apple_shot.delay;
		return;
	}
	g_apple_shot.queued = false;
	g_apple_shot.armed = true;
	g_apple_shot.waited = 0;
	Msg("* screenshot: capturing '%s'", g_apple_shot.name);
	mtl_d3d11::RequestCapture(HW.m_pSwapChain);
}

void AppleScreenshotAfterPresent()
{
	if (!g_apple_shot.armed)
		return;
	mtl_d3d11::CaptureResult shot;
	if (!mtl_d3d11::PollCapture(HW.m_pSwapChain, &shot))
	{
		if (++g_apple_shot.waited > 300)
		{
			g_apple_shot.armed = false;
			Msg("! screenshot: no frame arrived");
		}
		return;
	}
	g_apple_shot.armed = false;
	const bool png = ps_r_screenshot_token != 0;
	std::vector<uint8_t> encoded;
	if (!mtl_d3d11::EncodeCapture(shot, png ? 1 : 0, &encoded))
	{
		Msg("! screenshot: encoding failed");
		return;
	}
	string_path buf;
	if (g_apple_shot.name[0])
		xr_sprintf(buf, sizeof(buf), "%s.%s", g_apple_shot.name, png ? "png" : "jpg");
	else
	{
		string64 t_stemp;
		xr_sprintf(buf, sizeof(buf), "ss_%s_%s_(%s).%s", Core.UserName, timestamp(t_stemp),
		           (g_pGameLevel) ? g_pGameLevel->name().c_str() : "mainmenu", png ? "png" : "jpg");
	}
	if (IWriter* fs = FS.w_open("$screenshots$", buf))
	{
		fs->w(encoded.data(), (u32)encoded.size());
		FS.w_close(fs);
		Msg("* screenshot: %s (%ux%u, %u KB)", buf, shot.width, shot.height, (u32)(encoded.size() / 1024));
		FlushLog();
	}
	else
		Msg("! screenshot: cannot write %s", buf);
}
#endif // XR_PLATFORM_APPLE

IC u32 convert(float c)
{
	u32 C = iFloor(c);
	if (C > 255) C = 255;
	return C;
}

IC void MouseRayFromPoint(Fvector& direction, int x, int y, Fmatrix& m_CamMat)
{
	int halfwidth = Device.dwWidth / 2;
	int halfheight = Device.dwHeight / 2;

	Ivector2 point2;
	point2.set(x - halfwidth, halfheight - y);

	float size_y = VIEWPORT_NEAR * tanf(deg2rad(60.f) * 0.5f);
	float size_x = size_y / (Device.fHeight_2 / Device.fWidth_2);

	float r_pt = float(point2.x) * size_x / (float)halfwidth;
	float u_pt = float(point2.y) * size_y / (float)halfheight;

	direction.mul(m_CamMat.k, VIEWPORT_NEAR);
	direction.mad(direction, m_CamMat.j, u_pt);
	direction.mad(direction, m_CamMat.i, r_pt);
	direction.normalize();
}

#define SM_FOR_SEND_WIDTH 640
#define SM_FOR_SEND_HEIGHT 480

#if defined(USE_DX10) || defined(USE_DX11)
void CRender::ScreenshotImpl(ScreenshotMode mode, LPCSTR name, CMemoryWriter* memory_writer)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}

	ID3DResource* pSrcTexture;
	HW.pBaseRT->GetResource(&pSrcTexture);

	VERIFY(pSrcTexture);

	// Save
	switch (mode)
	{
	case IRender_interface::SM_FOR_GAMESAVE:
		{
			ID3DTexture2D* pSrcSmallTexture;

			D3D_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = GAMESAVE_SIZE;
			desc.Height = GAMESAVE_SIZE;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_BC1_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D_USAGE_DEFAULT;
			desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
			CHK_DX(HW.pDevice->CreateTexture2D( &desc, NULL, &pSrcSmallTexture ));

			//	D3DX10_TEXTURE_LOAD_INFO *pLoadInfo

#ifdef USE_DX11
			CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
				NULL, pSrcSmallTexture ));
#else
			CHK_DX(D3DX10LoadTextureFromTexture( pSrcTexture,
				NULL, pSrcSmallTexture ));
#endif

			// save (logical & physical)
			ID3DBlob* saved = 0;
#ifdef USE_DX11
			HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
#else
			HRESULT hr = D3DX10SaveTextureToMemory(pSrcSmallTexture, D3DX10_IFF_DDS, &saved, 0);
			//HRESULT hr					= D3DXSaveTextureToFileInMemory (&saved,D3DXIFF_DDS,texture,0);
#endif
			if (hr == D3D_OK)
			{
				IWriter* fs = FS.w_open(name);
				if (fs)
				{
					fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
					FS.w_close(fs);
				}
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(pSrcSmallTexture);
		}
		break;
	case IRender_interface::SM_FOR_MPSENDING:
		{
			ID3DTexture2D* pSrcSmallTexture;

			D3D_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = SM_FOR_SEND_WIDTH;
			desc.Height = SM_FOR_SEND_HEIGHT;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_BC1_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D_USAGE_DEFAULT;
			desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
			CHK_DX(HW.pDevice->CreateTexture2D( &desc, NULL, &pSrcSmallTexture ));

			//	D3DX10_TEXTURE_LOAD_INFO *pLoadInfo

#ifdef USE_DX11
			CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
				NULL, pSrcSmallTexture ));
#else
			CHK_DX(D3DX10LoadTextureFromTexture( pSrcTexture,
				NULL, pSrcSmallTexture ));
#endif
			// save (logical & physical)
			ID3DBlob* saved = 0;
#ifdef USE_DX11
			HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
#else
			HRESULT hr = D3DX10SaveTextureToMemory(pSrcSmallTexture, D3DX10_IFF_DDS, &saved, 0);
			//HRESULT hr					= D3DXSaveTextureToFileInMemory (&saved,D3DXIFF_DDS,texture,0);
#endif
			if (hr == D3D_OK)
			{
				if (!memory_writer)
				{
					IWriter* fs = FS.w_open(name);
					if (fs)
					{
						fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
						FS.w_close(fs);
					}
				}
				else
				{
					memory_writer->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
				}
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(pSrcSmallTexture);
		}
		break;
	case IRender_interface::SM_NORMAL:
		{
#if defined(XR_PLATFORM_APPLE)
			AppleQueueScreenshot(name, 0);
			break;
#endif
			string64 t_stemp;
			string_path buf;
			xr_sprintf(buf, sizeof(buf), "ss_%s_%s_(%s).%s", Core.UserName, timestamp(t_stemp),
				(g_pGameLevel) ? g_pGameLevel->name().c_str() : "mainmenu", ps_r_screenshot_token == 0 ? "jpg" : (ps_r_screenshot_token == 1 ? "png" : "tga"));
			ID3DBlob* saved = 0;
#ifdef USE_DX11
			CHK_DX(D3DX11SaveTextureToMemory(HW.pContext, pSrcTexture, ps_r_screenshot_token == 0 ? D3DX11_IFF_JPG : (ps_r_screenshot_token == 1 ? D3DX11_IFF_PNG : D3DX11_IFF_BMP), &saved, 0));
#else
			CHK_DX(D3DX10SaveTextureToMemory( pSrcTexture, ps_r_screenshot_token == 0 ? D3DX10_IFF_JPG : (ps_r_screenshot_token == 1 ? D3DX10_IFF_PNG : D3DX10_IFF_BMP), &saved, 0));
#endif
			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);
			fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
			FS.w_close(fs);
			_RELEASE(saved);
		}
		break;
	case IRender_interface::SM_FOR_LEVELMAP:
	case IRender_interface::SM_FOR_CUBEMAP:
		{
		string_path buf;
		VERIFY(name);
		strconcat(sizeof(buf), buf, name, ".tga");

		// TODO: this won't work due to texture format incompat
		ID3DTexture2D* pTex = Target->t_ss_async;
		HW.pContext->CopyResource(pTex, pSrcTexture);

		D3D_MAPPED_TEXTURE2D MappedData;
#ifdef USE_DX11
		HW.pContext->Map(pTex, 0, D3D_MAP_READ, 0, &MappedData);
#else
		pTex->Map(0, D3D_MAP_READ, 0, &MappedData);
#endif
		// Swap r and b, but don't kill alpha
		{
			u32* pPixel = (u32*)MappedData.pData;
			u32* pEnd = pPixel + (Device.dwWidth * Device.dwHeight);

			for (; pPixel != pEnd; pPixel++)
			{
				u32 p = *pPixel;
				*pPixel = color_argb(color_get_A(p), color_get_B(p), color_get_G(p), color_get_R(p));
			}
		}

		u32* data = (u32*)xr_malloc(Device.dwHeight * Device.dwHeight * 4);
		imf_Process(data, Device.dwHeight, Device.dwHeight, (u32*)MappedData.pData, Device.dwWidth, Device.dwHeight, imf_lanczos3);
#ifdef USE_DX11
		HW.pContext->Unmap(pTex, 0);
#else
		pTex->Unmap(0);
#endif
		IWriter* fs = FS.w_open("$screenshots$", buf);
		R_ASSERT(fs);

		TGAdesc p;
		p.format = IMG_24B;
		p.scanlenght = Device.dwHeight * 4;
		p.width = Device.dwHeight;
		p.height = Device.dwHeight;
		p.data = data;
		p.maketga(*fs);
		xr_free(data);
		FS.w_close(fs);
		}
		break;
	}

	_RELEASE(pSrcTexture);
}

#else	//	USE_DX10

void CRender::ScreenshotImpl(ScreenshotMode mode, LPCSTR name, CMemoryWriter* memory_writer)
{
	if (!Device.b_is_Ready) return;

	// Create temp-surface
	IDirect3DSurface9* pFB;
	D3DLOCKED_RECT D;

	HRESULT hr = HW.pDevice->CreateOffscreenPlainSurface(Device.dwWidth, Device.dwHeight, HW.DevPP.BackBufferFormat,
	                                                     D3DPOOL_SYSTEMMEM, &pFB, nullptr);
	if (FAILED(hr)) return;

	hr = HW.pDevice->GetRenderTargetData(HW.pBaseRT, pFB);
	if (FAILED(hr)) goto _end_;

	hr = pFB->LockRect(&D, 0, D3DLOCK_NOSYSLOCK);
	if (FAILED(hr)) goto _end_;

	// Image processing (gamma-correct)
	u32* pPixel = (u32*)D.pBits;
	u32* pEnd = pPixel + (Device.dwWidth * Device.dwHeight);

	//	Kill alpha
	for (; pPixel != pEnd; pPixel++)
	{
		u32 p = *pPixel;
		*pPixel = color_xrgb(color_get_R(p), color_get_G(p), color_get_B(p));
	}

	hr = pFB->UnlockRect();
	if (hr != D3D_OK) goto _end_;


	// Save
	switch (mode)
	{
	case IRender_interface::SM_FOR_GAMESAVE:
		{
			// texture
			ID3DTexture2D* texture = NULL;
			hr = D3DXCreateTexture(HW.pDevice,GAMESAVE_SIZE,GAMESAVE_SIZE, 1, 0, D3DFMT_DXT1, D3DPOOL_SCRATCH,
			                       &texture);
			if (hr != D3D_OK) goto _end_;
			if (NULL == texture) goto _end_;

			// resize&convert to surface
			IDirect3DSurface9* surface = 0;
			hr = texture->GetSurfaceLevel(0, &surface);
			if (hr != D3D_OK) goto _end_;
			VERIFY(surface);
			hr = D3DXLoadSurfaceFromSurface(surface, 0, 0, pFB, 0, 0,D3DX_DEFAULT, 0);
			_RELEASE(surface);
			if (hr != D3D_OK) goto _end_;

			// save (logical & physical)
			ID3DBlob* saved = 0;
			hr = D3DXSaveTextureToFileInMemory(&saved, D3DXIFF_DDS, texture, 0);
			if (hr != D3D_OK) goto _end_;

			IWriter* fs = FS.w_open(name);
			if (fs)
			{
				fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
				FS.w_close(fs);
			}
			_RELEASE(saved);

			// cleanup
			_RELEASE(texture);
		}
		break;
	case IRender_interface::SM_FOR_MPSENDING:
		{
			// texture
			ID3DTexture2D* texture = NULL;
			hr = D3DXCreateTexture(HW.pDevice,SM_FOR_SEND_WIDTH,SM_FOR_SEND_HEIGHT, 1, 0, D3DFMT_R8G8B8,
			                       D3DPOOL_SCRATCH, &texture);
			if (hr != D3D_OK) goto _end_;
			if (NULL == texture) goto _end_;

			// resize&convert to surface
			IDirect3DSurface9* surface = 0;
			hr = texture->GetSurfaceLevel(0, &surface);
			if (hr != D3D_OK) goto _end_;
			VERIFY(surface);
			hr = D3DXLoadSurfaceFromSurface(surface, 0, 0, pFB, 0, 0,D3DX_DEFAULT, 0);
			_RELEASE(surface);
			if (hr != D3D_OK) goto _end_;

			// save (logical & physical)
			ID3DBlob* saved = 0;
			hr = D3DXSaveTextureToFileInMemory(&saved, D3DXIFF_DDS, texture, 0);
			if (hr != D3D_OK) goto _end_;

			if (!memory_writer)
			{
				IWriter* fs = FS.w_open(name);
				if (fs)
				{
					fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
					FS.w_close(fs);
				}
			}
			else
			{
				memory_writer->w(saved->GetBufferPointer(), saved->GetBufferSize());
			}

			_RELEASE(saved);

			// cleanup
			_RELEASE(texture);
		}
		break;
	case IRender_interface::SM_NORMAL:
		{
			string64 t_stemp;
			string_path buf;
			xr_sprintf(buf, sizeof(buf), "ss_%s_%s_(%s).%s", Core.UserName, timestamp(t_stemp),
			           (g_pGameLevel) ? g_pGameLevel->name().c_str() : "mainmenu", ps_r_screenshot_token == 0 ? "jpg" : (ps_r_screenshot_token == 1 ? "png" : "tga"));
			ID3DBlob* saved = 0;
			CHK_DX(D3DXSaveSurfaceToFileInMemory (&saved, ps_r_screenshot_token == 0 ? D3DXIFF_JPG : (ps_r_screenshot_token == 1 ? D3DXIFF_PNG : D3DXIFF_TGA) ,pFB,0,0));
			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);
			fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
			FS.w_close(fs);
			_RELEASE(saved);
		}
		break;
	case IRender_interface::SM_FOR_LEVELMAP:
	case IRender_interface::SM_FOR_CUBEMAP:
		{
			//				string64			t_stemp;
			string_path buf;
			VERIFY(name);
			strconcat(sizeof(buf), buf, name, ".tga");
			IWriter* fs = FS.w_open("$screenshots$", buf);
			R_ASSERT(fs);
			TGAdesc p;
			p.format = IMG_24B;

			//	TODO: DX10: This is totally incorrect but mimics 
			//	original behavior. Fix later.
			hr = pFB->LockRect(&D, 0,D3DLOCK_NOSYSLOCK);
			if (hr != D3D_OK) return;
			hr = pFB->UnlockRect();
			if (hr != D3D_OK) goto _end_;

			// save
			u32* data = (u32*)xr_malloc(Device.dwHeight * Device.dwHeight * 4);
			imf_Process(data, Device.dwHeight, Device.dwHeight, (u32*)D.pBits, Device.dwWidth, Device.dwHeight,
			            imf_lanczos3);
			p.scanlenght = Device.dwHeight * 4;
			p.width = Device.dwHeight;
			p.height = Device.dwHeight;
			p.data = data;
			p.maketga(*fs);
			xr_free(data);

			FS.w_close(fs);
		}
		break;
	}

_end_:
	_RELEASE(pFB);
}

#endif	//	USE_DX10

void CRender::Screenshot(ScreenshotMode mode, LPCSTR name)
{
	ScreenshotImpl(mode, name, NULL);
}

void CRender::Screenshot(ScreenshotMode mode, CMemoryWriter& memory_writer)
{
	if (mode != SM_FOR_MPSENDING)
	{
		Log("~ Not implemented screenshot mode...");
		return;
	}
	ScreenshotImpl(mode, NULL, &memory_writer);
}

void CRender::ScreenshotAsyncBegin()
{
	VERIFY(!m_bMakeAsyncSS);
	m_bMakeAsyncSS = true;
}

#if defined(USE_DX10) || defined(USE_DX11)

void CRender::ScreenshotAsyncEnd(CMemoryWriter& memory_writer)
{
	VERIFY(!m_bMakeAsyncSS);

	//	Don't own. No need to release.
	ID3DTexture2D* pTex = Target->t_ss_async;

	D3D_MAPPED_TEXTURE2D MappedData;

#ifdef USE_DX11
	HW.pContext->Map(pTex, 0, D3D_MAP_READ, 0, &MappedData);
#else
	pTex->Map(0, D3D_MAP_READ, 0, &MappedData);
#endif

	{
		u32* pPixel = (u32*)MappedData.pData;
		u32* pEnd = pPixel + (Device.dwWidth * Device.dwHeight);

		//	Kill alpha and swap r and b.
		for (; pPixel != pEnd; pPixel++)
		{
			u32 p = *pPixel;
			*pPixel = color_xrgb(
				color_get_B(p),
				color_get_G(p),
				color_get_R(p)
			);
		}

		memory_writer.w(&Device.dwWidth, sizeof(Device.dwWidth));
		memory_writer.w(&Device.dwHeight, sizeof(Device.dwHeight));
		memory_writer.w(MappedData.pData, (Device.dwWidth * Device.dwHeight) * 4);
	}

#ifdef USE_DX11
	HW.pContext->Unmap(pTex, 0);
#else
	pTex->Unmap(0);
#endif
}

#else	//	USE_DX10

void CRender::ScreenshotAsyncEnd(CMemoryWriter& memory_writer)
{
	if (!Device.b_is_Ready) return;

	VERIFY(!m_bMakeAsyncSS);

	D3DLOCKED_RECT D;
	HRESULT hr;
	IDirect3DSurface9* pFB;

	pFB = Target->pFB;

	hr = pFB->LockRect(&D, 0,D3DLOCK_NOSYSLOCK);
	if (hr != D3D_OK) return;

#if	RENDER == R_R1
	u32 rtWidth = Target->get_rtwidth();
	u32 rtHeight = Target->get_rtheight();
#else	//	RENDER != R_R1
	u32 rtWidth = Device.dwWidth;
	u32 rtHeight = Device.dwHeight;
#endif	//	RENDER != R_R1

	// Image processing (gamma-correct)
	u32* pPixel = (u32*)D.pBits;
	u32* pOrigin = pPixel;
	u32* pEnd = pPixel + (rtWidth * rtHeight);

	//	Kill alpha
#if	RENDER != R_R1
	if (Target->rt_Color->fmt == D3DFMT_A16B16G16R16F)
	{
		static const int iMaxPixelsInARow = 1024;
		D3DXFLOAT16* pPixelElement16 = (D3DXFLOAT16*)pPixel;

		FLOAT tmpArray[4 * iMaxPixelsInARow];
		while (pPixel != pEnd)
		{
			const int iProcessPixels = _min(iMaxPixelsInARow, (s32)(pEnd - pPixel));

			D3DXFloat16To32Array(tmpArray, pPixelElement16, iProcessPixels * 4);

			for (int i = 0; i < iProcessPixels; ++i)
			{
				*pPixel = color_argb_f(
					1.0f,
					tmpArray[i * 4],
					tmpArray[i * 4 + 1],
					tmpArray[i * 4 + 2]
				);

				++pPixel;
			}

			pPixelElement16 += iProcessPixels * 4;
		}
	}
	else
#endif	//	RENDER != R_R1
	{
		for (; pPixel != pEnd; pPixel++)
		{
			u32 p = *pPixel;
			*pPixel = color_xrgb(
				color_get_R(p),
				color_get_G(p),
				color_get_B(p)
			);
		}
	}

	{
		memory_writer.w(&rtWidth, sizeof(rtWidth));
		memory_writer.w(&rtHeight, sizeof(rtHeight));
		memory_writer.w(pOrigin, (rtWidth * rtHeight) * 4);
	}

	hr = pFB->UnlockRect();
}

#endif	//	USE_DX10

void DoAsyncScreenshot()
{
	RImplementation.Target->DoAsyncScreenshot();
}

// Antglobes: Export Screenshot Func + variable resolution & encoding
#if defined(USE_DX10) || defined(USE_DX11)
void CRender::TakeScreenshot(LPCSTR path, Fvector2 dimensions, DxEncoding encoding)
{
	// demonized: Disable screenshots if HDR is enabled
	if (ps_r4_hdr10_on) {
		return;
	}
	if (!Device.b_is_Ready) return;

	string_path fname;
	if (0 == strext(path)) strconcat(sizeof(fname), fname, path, ".dds");
	else xr_strcpy(fname, sizeof(fname), path);
	FS.update_path(fname, "$game_textures$", fname);

	// Clamp image dimensions
	int actual_width = Device.dwWidth;
	int actual_height = Device.dwHeight;
	int width = dimensions.x;
	int height = dimensions.y;
	clamp(width, 0, actual_width);
	clamp(height, 0, actual_height);

	ID3DResource* pSrcTexture;
	HW.pBaseRT->GetResource(&pSrcTexture);

	VERIFY(pSrcTexture);


	DXGI_FORMAT dx_encoding;
	clamp(encoding, eDXE_A8R8G8B8, eDXE_DXT5);
	switch (encoding)
	{
	case IRender_interface::eDXE_A8R8G8B8: 
	{
		dx_encoding = DXGI_FORMAT_R8G8B8A8_UNORM;
	}
	break;
	case IRender_interface::eDXE_DXT1: 
	{
		dx_encoding = DXGI_FORMAT_BC1_UNORM;
	}
	break;
	case IRender_interface::eDXE_DXT5: 
	{
		dx_encoding = DXGI_FORMAT_BC5_UNORM;
	}
	break;
	case IRender_interface::eDXE_BC7:
	{
		dx_encoding = DXGI_FORMAT_BC7_UNORM;
	}
	break;
	default:
		dx_encoding = DXGI_FORMAT_R8G8B8A8_UNORM;
		break;
	}

	ID3DTexture2D* pSrcSmallTexture;

	D3D_TEXTURE2D_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.Width = u32(width);
	desc.Height = u32(height);
	desc.MipLevels = encoding == eDXE_A8R8G8B8 ? 0: 1;
	desc.ArraySize = 1;
	desc.Format = dx_encoding;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D_USAGE_DEFAULT;
	desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
	CHK_DX(HW.pDevice->CreateTexture2D(&desc, NULL, &pSrcSmallTexture));

	//	D3DX10_TEXTURE_LOAD_INFO *pLoadInfo

#ifdef USE_DX11
	CHK_DX(D3DX11LoadTextureFromTexture(HW.pContext, pSrcTexture,
		NULL, pSrcSmallTexture));
#else
	CHK_DX(D3DX10LoadTextureFromTexture(pSrcTexture,
		NULL, pSrcSmallTexture));
#endif

	

	// save (logical & physical)
	ID3DBlob* saved = 0;
#ifdef USE_DX11
	HRESULT hr = D3DX11SaveTextureToMemory(HW.pContext, pSrcSmallTexture, D3DX11_IFF_DDS, &saved, 0);
#else
	HRESULT hr = D3DX10SaveTextureToMemory(pSrcSmallTexture, D3DX10_IFF_DDS, &saved, 0);
	//HRESULT hr					= D3DXSaveTextureToFileInMemory (&saved,D3DXIFF_DDS,texture,0);
#endif
	if (hr == D3D_OK)
	{
		IWriter* fs = FS.w_open(fname);
		if (fs)
		{
			fs->w(saved->GetBufferPointer(), (u32)saved->GetBufferSize());
			FS.w_close(fs);
		}
	}
	_RELEASE(saved);

	// cleanup
	_RELEASE(pSrcSmallTexture);
}
#else //DX
// Antglobes: Export Screenshot Func + variable resolution & encoding
void CRender::TakeScreenshot(LPCSTR path, Fvector2 dimensions, DxEncoding encoding)
{
	if (!Device.b_is_Ready) return;
	
	string_path fname;
	if (0 == strext(path)) strconcat(sizeof(fname), fname, path, ".dds");
	else xr_strcpy(fname, sizeof(fname), path);
	FS.update_path(fname, "$game_textures$", fname);

	// Clamp image dimensions
	int actual_width = Device.dwWidth;
	int actual_height = Device.dwHeight;
	int width = int(dimensions.x);
	int height = int(dimensions.y);
	clamp(width, 0, actual_width);
	clamp(height, 0, actual_height);

	// antglobes: dx encoding
	D3DFORMAT dx_encoding;
	switch (encoding)
	{
	case IRender_interface::eDXE_A8R8G8B8: {
		dx_encoding = D3DFMT_A8R8G8B8;
	}
	break;
	case IRender_interface::eDXE_DXT1: {
		dx_encoding = D3DFMT_DXT1;
	}
	break;
	case IRender_interface::eDXE_DXT5: {
		dx_encoding = D3DFMT_DXT5;
	}
	break;
	default:
		dx_encoding = D3DFMT_A8R8G8B8;
		break;
	}
	// Antglobes: DDS Image Prep from ScreenshotImpl

	// Create temp-surface
	IDirect3DSurface9* pFB;
	D3DLOCKED_RECT D;
	HRESULT hr = HW.pDevice->CreateOffscreenPlainSurface(Device.dwWidth, Device.dwHeight, HW.DevPP.BackBufferFormat,
		D3DPOOL_SYSTEMMEM, &pFB, nullptr);
	if (FAILED(hr)) return;

	hr = HW.pDevice->GetRenderTargetData(HW.pBaseRT, pFB);
	if (FAILED(hr)) goto _end_;

	hr = pFB->LockRect(&D, 0, D3DLOCK_NOSYSLOCK);
	if (FAILED(hr)) goto _end_;
	// Image processing (gamma-correct)
	u32* pPixel = (u32*)D.pBits;
	u32* pEnd = pPixel + (Device.dwWidth * Device.dwHeight);
	//	Kill alpha
	for (; pPixel != pEnd; pPixel++)
	{
		u32 p = *pPixel;
		*pPixel = color_xrgb(color_get_R(p), color_get_G(p), color_get_B(p));
	}

	hr = pFB->UnlockRect();
	if (hr != D3D_OK) goto _end_;
	// texture width/height = resolution
	ID3DTexture2D* texture = NULL;
	hr = D3DXCreateTexture(HW.pDevice, u32(width), u32(height), 1, 0, dx_encoding, D3DPOOL_SCRATCH,
		&texture);
	if (hr != D3D_OK) goto _end_;
	if (NULL == texture) goto _end_;
	// resize&convert to surface
	IDirect3DSurface9* surface = 0;
	hr = texture->GetSurfaceLevel(0, &surface);
	if (hr != D3D_OK) goto _end_;
	VERIFY(surface);

	hr = D3DXLoadSurfaceFromSurface(surface, 0, 0, pFB, 0, 0, D3DX_DEFAULT, 0);
	_RELEASE(surface);
	if (hr != D3D_OK) goto _end_;
	// save (logical & physical)
	ID3DBlob* saved = 0;
	hr = D3DXSaveTextureToFileInMemory(&saved, D3DXIFF_DDS, texture, 0);
	if (hr != D3D_OK) goto _end_;

	IWriter* fs = FS.w_open(fname);
	if (fs)
	{
		fs->w(saved->GetBufferPointer(), saved->GetBufferSize());
		FS.w_close(fs);
	}
	_RELEASE(saved);

	// cleanup
	_RELEASE(texture);

_end_:
	_RELEASE(pFB);
}
#endif