#ifndef tntQAVIH
#define tntQAVIH

#include <math.h>

//#include "tntTimer.h"
#ifdef XR_PLATFORM_APPLE
// Video for Windows has no counterpart here. The renderer's .avi texture path (SH_Texture.cpp,
// dx10SH_Texture.cpp) still names this class, so it keeps its interface and Load() fails; the shipped game has no
// .avi textures (its videos are Theora .ogm, see xrTheora_Surface).
class ENGINE_API CAviPlayerCustom
{
public:
	CAviPlayerCustom() : m_dwWidth(0), m_dwHeight(0) {}

	DWORD m_dwWidth, m_dwHeight;

	VOID GetSize(DWORD* dwWidth, DWORD* dwHeight)
	{
		*dwWidth = m_dwWidth;
		*dwHeight = m_dwHeight;
	}

	BOOL Load(char* fname);
	BOOL GetFrame(BYTE** pDest) { return FALSE; }

	BOOL NeedUpdate() { return FALSE; }
	INT SetSpeed(INT nPercent) { return 0; }
};
#else // XR_PLATFORM_APPLE
#include "vfw.h"
#include "mmsystem.h"

// replaced with standard AVIIF_KEYFRAME
//rr #define AVIINDEX_ISKEYFRAME 0x10 // ключевой кадр

// reverse enginered AVI index v.1 format
/*struct AviIndex {

 DWORD dwChunkType; // chunk type, i.e. '##dc' - DIB compressed
 DWORD dwFlags; // key-frame etc.
 DWORD dwOffset; // sub-chunk offset from the begining of the LIST chunk
 DWORD dwLenght; // chunk lenght

 };

 typedef struct {
 FOURCC fccType;
 FOURCC fccHandler;
 DWORD dwFlags;
 DWORD dwPriority;
 DWORD dwInitialFrames;
 DWORD dwScale;
 DWORD dwRate;
 DWORD dwStart;
 DWORD dwLength;
 DWORD dwSuggestedBufferSize;
 DWORD dwQuality;
 DWORD dwSampleSize;
 RECT rcFrame;
 } AVIStreamHeader;
 */
typedef struct
{
	FOURCC fccType;
	FOURCC fccHandler;
	DWORD dwFlags;
	DWORD dwPriority;
	DWORD dwInitialFrames;
	DWORD dwScale;
	DWORD dwRate;
	DWORD dwStart;
	DWORD dwLength;
	DWORD dwSuggestedBufferSize;
	DWORD dwQuality;
	DWORD dwSampleSize;

	struct
	{
		WORD left;
		WORD top;
		WORD right;
		WORD bottom;
	};

	// RECT rcFrame; - лажа в MSDN
} AVIStreamHeaderCustom;

class ENGINE_API CAviPlayerCustom
{
protected:
	CAviPlayerCustom* alpha;
protected:
	AVIINDEXENTRY* m_pMovieIndex;
	BYTE* m_pMovieData;
	HIC m_aviIC;
	BYTE* m_pDecompressedBuf;

	BITMAPINFOHEADER m_biOutFormat;
	BITMAPINFOHEADER m_biInFormat;

	float m_fRate; // стандартная скорость, fps
	float m_fCurrentRate; // текущая скорость, fps

	DWORD m_dwFrameTotal;
	DWORD m_dwFrameCurrent;
	u32 m_dwFirstFrameOffset;


	DWORD CalcFrame();

	BOOL DecompressFrame(DWORD dwFrameNum);
	VOID PreRoll(DWORD dwFrameNum);

public:
	CAviPlayerCustom();
	~CAviPlayerCustom();

	DWORD m_dwWidth, m_dwHeight;

	VOID GetSize(DWORD* dwWidth, DWORD* dwHeight);

	BOOL Load(char* fname);
	BOOL GetFrame(BYTE** pDest);

	BOOL NeedUpdate() { return CalcFrame() != m_dwFrameCurrent; }
	INT SetSpeed(INT nPercent);
};
#endif // XR_PLATFORM_APPLE
#endif
