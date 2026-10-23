#ifndef log_linesH
#define log_linesH
#pragma once

// The log lines, for the console's log window and its clear_log command. LogFile (log.h) is changed by every thread that logs and
// is only touched under the log lock, in log.cpp: these copy or clear under it.
void XRCORE_API ClearLog();
u32 XRCORE_API LogLineCount();
// copies lines [last - count + 1, last] (clipped to the log) into `out`; returns the index of the first line copied
u32 XRCORE_API LogCopyLines(u32 last, u32 count, xr_vector<xr_string>& out);

#endif
