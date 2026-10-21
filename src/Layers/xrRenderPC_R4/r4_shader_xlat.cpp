// r4_shader_xlat.cpp: D3DCompile for the Metal build. R4 keeps calling D3DCompile with its macro list; on Apple the
// call is answered by ShaderXlat (HLSL -> SPIR-V -> MSL) through the hook of the Metal D3D11 shim, and the "bytecode"
// is the shim's shader container. Only built for Apple platforms.
#include "stdafx.h"
#pragma hdrstop

#if defined(XR_PLATFORM_APPLE)

#include <ShaderXlat/MetalBridge.h>

namespace
{
void log_line(const std::string& text)
{
	// the bridge may log several lines at once, and Msg() treats its argument as a format string
	Msg("%s", text.c_str());
}
} // namespace

void r4_install_shader_xlat()
{
	shaderxlat_metal::Config config;
	config.log = &log_line;

	// translations are rebuildable data: Library/Caches/Anomaly/shaders/xlat ($shader_cache$ comes from the app shell's
	// generated fsgame.ltx); without the shell they go next to the engine's other generated data
	string_path dir;
	if (FS.path_exist("$shader_cache$"))
		FS.update_path(dir, "$shader_cache$", "xlat");
	else
		FS.update_path(dir, "$app_data_root$", "shaders_cache\\xlat");
	config.cache_dir = xr_native_path(dir);

	shaderxlat_metal::install(config);
	Msg("* ShaderXlat: %s, translation cache %s", shaderxlat::version(), config.cache_dir.c_str());
}

void r4_report_shader_xlat()
{
	const shaderxlat_metal::Stats s = shaderxlat_metal::stats();
	Msg("* ShaderXlat: %llu shaders requested, %llu from the translation cache, %llu failed, %llu skipped (stage not on Metal), %.0f ms translating",
	    (unsigned long long)s.requests, (unsigned long long)s.cache_hits, (unsigned long long)s.failures,
	    (unsigned long long)s.unsupported_stage, s.translate_ms);
}

#endif // XR_PLATFORM_APPLE
