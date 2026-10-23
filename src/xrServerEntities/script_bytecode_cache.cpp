////////////////////////////////////////////////////////////////////////////
//	Module 		: script_bytecode_cache.cpp
//	Description : On-disk cache of LuaJIT bytecode for script files (Apple port, docs/ios-port/LUA-PERF.md)
////////////////////////////////////////////////////////////////////////////
//
// Standalone on purpose (standard library and LuaJIT only): tools/lua_cache_test links this file without the engine.

#include "script_bytecode_cache.h"

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <luajit.h>
}

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace script_cache
{
namespace
{
constexpr uint32_t kFormat = 1;
constexpr char kMagic[8] = {'A', 'L', 'U', 'A', 'B', 'C', '1', 0};
constexpr size_t kVmHeaderSize = 5; // ESC 'L' 'J' <bytecode version> <flags>

// Entry layout: Header, the chunk name (name_len bytes), the bytecode (bc_size bytes). Host byte order: a cache is never
// moved between machines of another endianness (the bytecode's own header says so as well).
struct Header
{
	char magic[8];
	uint32_t format;
	uint32_t vm_id;     // hash of the VM's bytecode header and version string
	uint32_t name_len;
	uint32_t reserved;
	uint64_t src_size;  // the text the bytecode was compiled from
	uint64_t src_h1, src_h2;
	uint64_t bc_size;
	uint64_t bc_h1, bc_h2;
};
static_assert(sizeof(Header) == 72, "cache entry header layout");

struct State
{
	std::mutex lock;
	std::string dir;
	Stats stats;
	bool vm_probed = false;
	bool vm_ok = false;
	uint32_t vm_id = 0;
	unsigned char vm_header[kVmHeaderSize] = {};
	std::atomic<uint32_t> tmp_counter{0};
};

State& S()
{
	static State s;
	return s;
}

double now_ms()
{
	return double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-6;
}

// ---- hashing: two interleaved 64-bit lanes, 16 bytes a step; for change detection, not for security -------------------------------

constexpr uint64_t kK1 = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kK2 = 0xC2B2AE3D27D4EB4Full;

inline uint64_t rotl(uint64_t x, int r)
{
	return (x << r) | (x >> (64 - r));
}

inline uint64_t fmix(uint64_t h)
{
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return h;
}

inline uint64_t load64(const uint8_t* p)
{
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

void hash128_impl(const void* data, size_t n, uint64_t& h1, uint64_t& h2)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	const uint8_t* const end = p + n;
	uint64_t a = 0x243F6A8885A308D3ull ^ uint64_t(n);
	uint64_t b = 0x13198A2E03707344ull + uint64_t(n) * kK1;
	while (size_t(end - p) >= 16)
	{
		a = rotl(a ^ (load64(p) * kK2), 27) * kK1 + b;
		b = rotl(b ^ (load64(p + 8) * kK1), 31) * kK2 + a;
		p += 16;
	}
	uint8_t tail[16] = {};
	memcpy(tail, p, size_t(end - p));
	a = rotl(a ^ (load64(tail) * kK2), 27) * kK1 + b;
	b = rotl(b ^ (load64(tail + 8) * kK1), 31) * kK2 + a;
	a ^= b;
	b += a;
	h1 = fmix(a);
	h2 = fmix(b ^ kK1);
}

uint64_t hash64(const char* s)
{
	uint64_t h1, h2;
	hash128_impl(s, strlen(s), h1, h2);
	return h1 ^ rotl(h2, 21);
}

// ---- the VM's bytecode format ----------------------------------------------------------------------------------------------------

int dump_writer(lua_State*, const void* p, size_t sz, void* ud)
{
	static_cast<std::string*>(ud)->append(static_cast<const char*>(p), sz);
	return 0;
}

// The first bytes of a dump say the bytecode version and the flags (FR2 = 64-bit GC references, big endian, FFI): what a
// bytecode file must match to be loadable by this VM. A tiny chunk is dumped once to read them.
bool probe_vm(lua_State* L, State& s)
{
	if (s.vm_probed)
		return s.vm_ok;
	s.vm_probed = true;
	const int top = lua_gettop(L);
	std::string dump;
	if (luaL_loadbuffer(L, "return 1", 8, "=probe") == 0 && lua_dump(L, dump_writer, &dump) == 0 && dump.size() >= kVmHeaderSize)
	{
		memcpy(s.vm_header, dump.data(), kVmHeaderSize);
		std::string id = LUAJIT_VERSION;
		id.append(reinterpret_cast<const char*>(s.vm_header), kVmHeaderSize);
		const uint8_t ptr = sizeof(void*);
		id.append(reinterpret_cast<const char*>(&ptr), 1);
		uint64_t h1, h2;
		hash128_impl(id.data(), id.size(), h1, h2);
		s.vm_id = uint32_t(h1 ^ (h1 >> 32));
		s.vm_ok = s.vm_header[0] == 0x1b && s.vm_header[1] == 'L' && s.vm_header[2] == 'J';
	}
	lua_settop(L, top);
	return s.vm_ok;
}

std::string hex16(uint64_t v)
{
	char buf[17];
	snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
	return buf;
}

std::string path_in(const std::string& dir, const char* chunk_name)
{
	return dir + "/" + hex16(hash64(chunk_name)) + ".luac";
}

// open, fstat, read, close: four system calls for a file (an entry is read on every script load)
bool read_file(const std::string& path, std::vector<char>& out)
{
	const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct stat st;
	bool ok = false;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0)
	{
		out.resize(size_t(st.st_size));
		size_t done = 0;
		ok = true;
		while (done < out.size())
		{
			const ssize_t n = read(fd, out.data() + done, out.size() - done);
			if (n <= 0)
			{
				ok = false;
				break;
			}
			done += size_t(n);
		}
	}
	close(fd);
	return ok;
}

enum class Lookup { Hit, Miss, Stale, Incompatible, Corrupt };

// Tries the entry at `path`. On Hit the chunk is on the stack.
Lookup try_entry(lua_State* L, State& s, const std::string& path, const char* chunk_name, size_t src_size, uint64_t src_h1, uint64_t src_h2,
                 size_t& bc_bytes, double& io_ms, double& decode_ms)
{
	thread_local std::vector<char> buf;
	const double io0 = now_ms();
	const bool read_ok = read_file(path, buf);
	io_ms = now_ms() - io0;
	if (!read_ok)
		return Lookup::Miss;
	if (buf.size() < sizeof(Header))
		return Lookup::Corrupt;
	Header h;
	memcpy(&h, buf.data(), sizeof(h));
	if (memcmp(h.magic, kMagic, sizeof(kMagic)) != 0)
		return Lookup::Corrupt;
	if (h.format != kFormat || h.vm_id != s.vm_id)
		return Lookup::Incompatible;
	const size_t name_len = strlen(chunk_name);
	if (h.name_len > buf.size() || h.bc_size > buf.size() || sizeof(Header) + size_t(h.name_len) + size_t(h.bc_size) != buf.size())
		return Lookup::Corrupt;
	const char* name = buf.data() + sizeof(Header);
	const char* bc = name + h.name_len;
	if (h.name_len != name_len || memcmp(name, chunk_name, name_len) != 0)
		return Lookup::Stale; // another chunk name that hashes to the same file
	if (h.src_size != src_size || h.src_h1 != src_h1 || h.src_h2 != src_h2)
		return Lookup::Stale;
	uint64_t b1, b2;
	hash128_impl(bc, size_t(h.bc_size), b1, b2);
	if (b1 != h.bc_h1 || b2 != h.bc_h2)
		return Lookup::Corrupt;
	if (h.bc_size < kVmHeaderSize || memcmp(bc, s.vm_header, kVmHeaderSize) != 0)
		return Lookup::Incompatible;
	const double dec0 = now_ms();
	const int load_rc = luaL_loadbuffer(L, bc, size_t(h.bc_size), chunk_name);
	decode_ms = now_ms() - dec0;
	if (load_rc != 0)
	{
		lua_pop(L, 1); // the error message
		return Lookup::Corrupt;
	}
	bc_bytes = size_t(h.bc_size);
	return Lookup::Hit;
}

// Dumps the chunk on top of the stack and writes it as the entry at `path`. Atomic: a temporary file, then rename.
bool store_entry(lua_State* L, State& s, const std::string& dir, const std::string& path, const char* chunk_name, size_t src_size,
                 uint64_t src_h1, uint64_t src_h2)
{
	std::string bc;
	bc.reserve(src_size / 2 + 256);
	if (lua_dump(L, dump_writer, &bc) != 0 || bc.size() < kVmHeaderSize)
		return false;
	Header h = {};
	memcpy(h.magic, kMagic, sizeof(kMagic));
	h.format = kFormat;
	h.vm_id = s.vm_id;
	h.name_len = uint32_t(strlen(chunk_name));
	h.src_size = src_size;
	h.src_h1 = src_h1;
	h.src_h2 = src_h2;
	h.bc_size = bc.size();
	hash128_impl(bc.data(), bc.size(), h.bc_h1, h.bc_h2);

	std::error_code ec;
	std::filesystem::create_directories(dir, ec); // no-op when the shell made it already
	const std::string tmp = path + "." + std::to_string(getpid()) + "." + std::to_string(s.tmp_counter++) + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(chunk_name, 1, h.name_len, f) == h.name_len &&
		fwrite(bc.data(), 1, bc.size(), f) == bc.size();
	ok = (fclose(f) == 0) && ok;
	if (ok)
		ok = rename(tmp.c_str(), path.c_str()) == 0;
	if (!ok)
		remove(tmp.c_str());
	return ok;
}
} // namespace

void hash128(const void* data, size_t size, uint64_t& h1, uint64_t& h2)
{
	hash128_impl(data, size, h1, h2);
}

void set_directory(const char* dir)
{
	State& s = S();
	std::lock_guard<std::mutex> g(s.lock);
	s.dir = dir ? dir : "";
	while (s.dir.size() > 1 && (s.dir.back() == '/' || s.dir.back() == '\\'))
		s.dir.pop_back();
}

const char* directory()
{
	return S().dir.c_str();
}

std::string entry_path(const char* chunk_name)
{
	State& s = S();
	std::lock_guard<std::mutex> g(s.lock);
	return s.dir.empty() ? std::string() : path_in(s.dir, chunk_name);
}

int load(lua_State* L, const char* text, size_t size, const char* chunk_name)
{
	State& s = S();
	std::string dir;
	bool vm_ok;
	{
		std::lock_guard<std::mutex> g(s.lock);
		dir = s.dir;
		vm_ok = dir.empty() ? false : probe_vm(L, s);
	}
	if (dir.empty() || !vm_ok)
		return luaL_loadbuffer(L, text, size, chunk_name);

	const double t0 = now_ms();
	uint64_t h1, h2;
	hash128_impl(text, size, h1, h2);
	const std::string path = path_in(dir, chunk_name);
	const double t1 = now_ms();
	size_t bc_bytes = 0;
	double io_ms = 0, decode_ms = 0;
	const Lookup found = try_entry(L, s, path, chunk_name, size, h1, h2, bc_bytes, io_ms, decode_ms);
	const double t2 = now_ms();

	Stats d; // the deltas of this call
	d.source_bytes = size;
	d.hash_ms = t1 - t0;
	int rc = 0;
	if (found == Lookup::Hit)
	{
		d.hits = 1;
		d.read_ms = t2 - t1;
		d.io_ms = io_ms;
		d.decode_ms = decode_ms;
		d.bytecode_bytes = bc_bytes;
	}
	else
	{
		switch (found)
		{
		case Lookup::Miss: d.misses = 1; break;
		case Lookup::Stale: d.stale = 1; break;
		case Lookup::Incompatible: d.incompatible = 1; break;
		default: d.corrupt = 1; break;
		}
		const double c0 = now_ms();
		rc = luaL_loadbuffer(L, text, size, chunk_name);
		const double c1 = now_ms();
		d.compile_ms = c1 - c0;
		if (rc == 0)
		{
			if (store_entry(L, s, dir, path, chunk_name, size, h1, h2))
				d.stored = 1;
			else
				d.store_failed = 1;
			d.store_ms = now_ms() - c1;
		}
	}

	std::lock_guard<std::mutex> g(s.lock);
	Stats& t = s.stats;
	t.hits += d.hits;
	t.misses += d.misses;
	t.stale += d.stale;
	t.incompatible += d.incompatible;
	t.corrupt += d.corrupt;
	t.stored += d.stored;
	t.store_failed += d.store_failed;
	t.source_bytes += d.source_bytes;
	t.bytecode_bytes += d.bytecode_bytes;
	t.hash_ms += d.hash_ms;
	t.read_ms += d.read_ms;
	t.io_ms += d.io_ms;
	t.decode_ms += d.decode_ms;
	t.compile_ms += d.compile_ms;
	t.store_ms += d.store_ms;
	return rc;
}

bool warm(lua_State* L, const char* text, size_t size, const char* chunk_name)
{
	const int top = lua_gettop(L);
	const int rc = load(L, text, size, chunk_name);
	lua_settop(L, top);
	return rc == 0;
}

size_t purge()
{
	std::string dir;
	{
		State& s = S();
		std::lock_guard<std::mutex> g(s.lock);
		dir = s.dir;
	}
	size_t removed = 0;
	if (dir.empty())
		return 0;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
	{
		const std::string name = entry.path().filename().string();
		if (name.size() > 5 && (name.compare(name.size() - 5, 5, ".luac") == 0 || name.compare(name.size() - 4, 4, ".tmp") == 0))
		{
			std::error_code rm_ec;
			if (std::filesystem::remove(entry.path(), rm_ec))
				++removed;
		}
	}
	return removed;
}

Stats stats()
{
	State& s = S();
	std::lock_guard<std::mutex> g(s.lock);
	return s.stats;
}

void reset_stats()
{
	State& s = S();
	std::lock_guard<std::mutex> g(s.lock);
	s.stats = Stats();
}
} // namespace script_cache
