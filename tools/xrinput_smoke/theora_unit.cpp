// CTheoraStream (xrTheora_Stream.cpp) on the Theora/Ogg targets, reading an .ogm out of the game archives.
#include "test_env.h"

#include <cmath>

// The decoder state (frame size, rate, total time) is private to CTheoraStream; the test reads it.
#define class struct
#include "xrEngine/xrTheora_Stream.h"
#undef class
#include "xrEngine/xrTheora_Stream.cpp"

static bool find_video(std::string& name, u32& size)
{
	xr_vector<LPSTR>* list = FS.file_list_open("$game_textures$", FS_ListFiles);
	if (!list)
		return false;
	bool found = false;
	for (LPSTR n : *list)
	{
		const char* e = strext(n);
		if (e && (!strcmp(e, ".ogm") || !strcmp(e, ".ogv")) && !strstr(n, "#alpha"))
		{
			const CLocatorAPI::file* f = FS.exist("$game_textures$", n);
			if (f && (!found || f->size_real < size)) // the smallest one decodes fastest
			{
				name = n;
				size = f->size_real;
				found = true;
			}
		}
	}
	FS.file_list_close(list);
	return found;
}

int run_theora_tests()
{
	printf("\n[media] xrTheora_Stream: decode an .ogm from the game archives\n");

	// what the archives hold
	int ogm = 0, ogv = 0, avi = 0, gif = 0;
	{
		xr_vector<LPSTR>* list = FS.file_list_open("$game_textures$", FS_ListFiles);
		if (list)
		{
			for (LPSTR n : *list)
			{
				const char* e = strext(n);
				if (!e)
					continue;
				ogm += !strcmp(e, ".ogm");
				ogv += !strcmp(e, ".ogv");
				avi += !strcmp(e, ".avi");
				gif += !strcmp(e, ".gif");
			}
			FS.file_list_close(list);
		}
	}
	printf("  textures\\ holds %d .ogm, %d .ogv, %d .avi, %d .gif files\n", ogm, ogv, avi, gif);
	check(ogm + ogv > 0, "the archives contain Theora videos");

	// every video's header: size, rate and length (the stream reads the whole file to count frames)
	{
		xr_vector<LPSTR>* list = FS.file_list_open("$game_textures$", FS_ListFiles);
		if (list)
		{
			int parsed = 0, total = 0;
			for (LPSTR n : *list)
			{
				const char* e = strext(n);
				if (!e || strcmp(e, ".ogm") != 0)
					continue;
				++total;
				string_path full;
				FS.update_path(full, "$game_textures$", n);
				CTheoraStream probe;
				const CLocatorAPI::file* f = FS.exist("$game_textures$", n);
				if (probe.Load(full))
				{
					++parsed;
					printf("    %-40s %8u bytes  %4ux%-4u %6.2f fps  %7u ms  ~%u frames\n", n, f ? f->size_real : 0,
					       probe.t_info.frame_width, probe.t_info.frame_height,
					       double(probe.t_info.fps_numerator) / double(probe.t_info.fps_denominator), probe.tm_total,
					       u32(std::lround(probe.tm_total * probe.fpms)));
				}
				else
					printf("    %-40s did not parse\n", n);
			}
			FS.file_list_close(list);
			check(total > 0 && parsed == total, "every .ogm in the archives parses");
		}
	}

	std::string name;
	u32 size = 0;
	if (!find_video(name, size))
		return g_failures;
	printf("  decoding %s (%u bytes)\n", name.c_str(), size);

	string_path path;
	xr_strcpy(path, name.c_str());
	if (!strstr(path, "textures\\"))
	{
		// the stream opens files by registry name, relative to nothing: give it the full virtual path
		FS.update_path(path, "$game_textures$", name.c_str());
	}

	CTheoraStream stream;
	const BOOL loaded = stream.Load(path);
	check(loaded != FALSE, "CTheoraStream::Load parses the headers");
	if (!loaded)
		return g_failures;

	const u32 w = stream.t_info.frame_width;
	const u32 h = stream.t_info.frame_height;
	const double fps = double(stream.t_info.fps_numerator) / double(stream.t_info.fps_denominator);
	const u32 frames_est = u32(std::lround(stream.tm_total * stream.fpms));
	printf("  resolution %ux%u, %.3f fps, %u ms, about %u frames, key rate %u\n", w, h, fps, stream.tm_total, frames_est,
	       stream.key_rate);
	check(w > 0 && h > 0 && fps > 1.0 && stream.tm_total > 0, "sane size, rate and length");

	// decode every frame in order, as CTheoraSurface::Update does at the video's own rate
	u32 decoded = 0;
	u32 first_luma_min = 255, first_luma_max = 0;
	u64 first_hash = 0;
	bool planes_ok = true;
	const double ms_per_frame = 1000.0 / fps;
	for (u32 i = 0; i < frames_est + 2; ++i)
	{
		const u32 t = u32(std::ceil((i + 0.001) * ms_per_frame));
		if (t >= stream.tm_total)
			break;
		if (!stream.Decode(t))
			continue;
		++decoded;
		const yuv_buffer* yuv = stream.CurrentFrame();
		if (yuv->y_width < int(w) || yuv->y_height < int(h) || !yuv->y || !yuv->u || !yuv->v)
			planes_ok = false;
		if (decoded == 1)
		{
			for (u32 y = 0; y < h; ++y)
				for (u32 x = 0; x < w; ++x)
				{
					const u8 v = yuv->y[y * yuv->y_stride + x];
					first_luma_min = _min<u32>(first_luma_min, v);
					first_luma_max = _max<u32>(first_luma_max, v);
					first_hash = (first_hash ^ v) * 1099511628211ull;
				}
		}
	}
	printf("  decoded %u frames\n", decoded);
	check(planes_ok, "every frame has Y, U and V planes at least the frame size");
	check(decoded > 1 && abs(int(decoded) - int(frames_est)) <= 2, "the decoded frame count matches the stream length");
	check(first_luma_max > first_luma_min + 8, "the first frame is not a flat colour");

	// rewind and decode again: Reset() must replay from the start
	stream.Reset();
	u64 again = 0;
	if (stream.Decode(u32(std::ceil(0.001 * ms_per_frame))))
	{
		const yuv_buffer* yuv = stream.CurrentFrame();
		for (u32 y = 0; y < h; ++y)
			for (u32 x = 0; x < w; ++x)
				again = (again ^ yuv->y[y * yuv->y_stride + x]) * 1099511628211ull;
	}
	check(again == first_hash && again != 0, "Reset() rewinds: the first frame decodes to the same pixels again");
	return g_failures;
}
