// Memory statistics for the browser-heap study (docs/web-port/MEMORY.md): what a 32-bit WebAssembly heap (4 GB cap) would have to hold.
// Plain C types and no engine includes, so the app shell can use it too. Counters only; nothing here changes behaviour.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The places that map a window of a file. On the web these become heap buffers, so their live total counts against the heap.
enum XrMapSite
{
	XR_MAP_ARCHIVE = 0,    // an archive entry read through LocatorAPI (CPackReader, or the temporary window of a compressed entry)
	XR_MAP_STREAM = 1,     // a CStreamReader window onto an archive or file (level geometry and the like)
	XR_MAP_LOOSE_READ = 2, // a whole loose file (CVirtualFileReader)
	XR_MAP_LOOSE_RW = 3,   // a whole loose file mapped read-write (CVirtualFileRW: save games)
	XR_MAP_SITE_COUNT = 4
};

// Call at every mmap (positive) and munmap (negative) of a window.
void xr_apple_map_note(int site, long long delta_bytes);

// One extra named value an application adds to every sample (the renderer's CPU-visible buffers and textures, for example).
struct XrMemExtra
{
	const char* name; // a stable string: the CSV header is written from the first sample
	uint64_t value;
};
enum { XR_MEM_EXTRA_MAX = 32 };
// Fills `out` (at most XR_MEM_EXTRA_MAX entries) and returns how many; called on the sampler thread.
typedef int (*XrMemProbe)(XrMemExtra* out);
typedef void (*XrMemLog)(const char* line);

struct XrMemValues
{
	uint64_t malloc_in_use = 0;    // malloc_zone_statistics, all zones: bytes in live blocks
	uint64_t malloc_allocated = 0; // bytes the zones hold from the OS (in use plus fragmentation and free lists)
	uint64_t malloc_blocks = 0;
	uint64_t malloc_max_in_use = 0; // the zone's own high-water mark of bytes in use (0 when a zone does not keep one)
	uint64_t footprint = 0;        // phys_footprint: what the OS charges the process (Metal, compressed and mapped memory included)
	uint64_t internal = 0;         // dirty anonymous memory (task_vm_info.internal)
	uint64_t compressed = 0;
	uint64_t resident = 0;
	uint64_t map_live[XR_MAP_SITE_COUNT] = {};
	uint64_t map_total = 0;        // sum of map_live
	uint64_t map_peak = 0;         // largest map_total seen at any mmap, exact (not sampled)
	uint64_t map_count = 0;        // windows mapped since start
	uint32_t threads = 0;
};

// Starts the sampler thread. Every `interval_ms` it reads malloc, the task's memory, the mapped windows and the probe's values,
// appends a row to the CSV at `csv_path` (may be null: no file), keeps the maximum of every column, and every 10 s writes a
// "mem:" line through `log`. Safe to call once; later calls are ignored.
void xr_apple_memstat_start(const char* csv_path, unsigned interval_ms, XrMemProbe probe, XrMemLog log);
// Stops the thread, writes a final "mem: peak" line and closes the CSV.
void xr_apple_memstat_stop();
bool xr_apple_memstat_running();
// The current sample, taken now (works without the sampler).
void xr_apple_memstat_now(XrMemValues* out);
// A line in the CSV and the log, for correlating a phase with the numbers (e.g. "level loaded").
void xr_apple_memstat_mark(const char* label);
