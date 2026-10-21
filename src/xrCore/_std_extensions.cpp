#include "stdafx.h"
#pragma hdrstop

#include <time.h>

#ifdef BREAK_AT_STRCMP
int xr_strcmp(const char* S1, const char* S2)
{
#ifdef DEBUG_MEMORY_MANAGER
    Memory.stat_strcmp++;
#endif // DEBUG_MEMORY_MANAGER
    int res = (int)strcmp(S1, S2);
    return res;
}
#endif

char* timestamp(string64& dest)
{
	string64 temp;

	/* Set time zone from TZ environment variable. If TZ is not set,
	* the operating system is queried to obtain the default value
	* for the variable.
	*/
#ifdef XR_PLATFORM_APPLE
	{
		// _strdate -> "mm/dd/yy", _strtime -> "hh:mm:ss"
		time_t now = time(nullptr);
		tm local;
		localtime_r(&now, &local);
		strftime(temp, sizeof(temp), "%m/%d/%y", &local);
	}
#else
	_tzset();
#endif
	u32 it;

	// date
#ifndef XR_PLATFORM_APPLE
	_strdate(temp);
#endif
	for (it = 0; it < xr_strlen(temp); it++)
		if ('/' == temp[it]) temp[it] = '-';
	strconcat(sizeof(dest), dest, temp, "_");

	// time
#ifdef XR_PLATFORM_APPLE
	{
		time_t now = time(nullptr);
		tm local;
		localtime_r(&now, &local);
		strftime(temp, sizeof(temp), "%H:%M:%S", &local);
	}
#else
	_strtime(temp);
#endif
	for (it = 0; it < xr_strlen(temp); it++)
		if (':' == temp[it]) temp[it] = '-';
	xr_strcat(dest, sizeof(dest), temp);
	return dest;
}
