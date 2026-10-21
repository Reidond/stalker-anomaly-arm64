#include "stdafx.h"
#include "file_stream_reader.h"

#ifdef XR_PLATFORM_APPLE
void CFileStreamReader::construct(LPCSTR file_name, const u32& window_size)
{
	int fd = ::open(xr_native_path(file_name), O_RDONLY);
	R_ASSERT3(fd >= 0, "cannot open file", file_name);
	m_file_handle = (HANDLE)(intptr_t)(fd + 1);

	struct stat st;
	R_ASSERT(fstat(fd, &st) == 0);
	inherited::construct(m_file_handle, 0, (u32)st.st_size, (u32)st.st_size, window_size);
}

void CFileStreamReader::destroy()
{
	inherited::destroy();
	::close(int(intptr_t(m_file_handle)) - 1);
}
#else // XR_PLATFORM_APPLE
void CFileStreamReader::construct(LPCSTR file_name, const u32& window_size)
{
	m_file_handle = CreateFile(file_name, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);

	VERIFY(m_file_handle != INVALID_HANDLE_VALUE);
	u32 file_size = (u32)GetFileSize(m_file_handle, NULL);

	HANDLE file_mapping_handle = CreateFileMapping(m_file_handle, 0, PAGE_READONLY, 0, 0, 0);
	VERIFY(file_mapping_handle != INVALID_HANDLE_VALUE);

	inherited::construct(file_mapping_handle, 0, file_size, file_size, window_size);
}

void CFileStreamReader::destroy()
{
	HANDLE file_mapping_handle = this->file_mapping_handle();
	inherited::destroy();
	CloseHandle(file_mapping_handle);
	CloseHandle(m_file_handle);
}
#endif // XR_PLATFORM_APPLE
