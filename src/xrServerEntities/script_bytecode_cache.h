////////////////////////////////////////////////////////////////////////////
//	Module 		: script_bytecode_cache.h
//	Description : On-disk cache of LuaJIT bytecode for script files (Apple port, docs/ios-port/LUA-PERF.md)
////////////////////////////////////////////////////////////////////////////
//
// The script engine wraps every script file in a namespace header and compiles the text with luaL_loadbuffer. Parsing the
// ~7 MB of stock scripts, the Monolith overlay and the addons is a share of every level load; the bytecode of an unchanged
// file is the same each time. `load` is luaL_loadbuffer with a cache in front of it:
//
//   - one file per chunk name (the script's path) in the cache directory; the entry carries the size and a 128-bit hash of the
//     exact text it was compiled from, the VM's bytecode header (version, FR2/GC64, endianness, FFI) and a hash of the bytecode;
//   - a hit needs every one of those to match, so an edited script, an addon that overrides a stock file, a cache written by a
//     LuaJIT with another bytecode format, a truncated or damaged file are all misses: the text is compiled and the entry
//     rewritten (atomically: a temporary file, then rename);
//   - any failure (no directory, unwritable, bytecode that does not load) falls back to the text: the result of `load` is always
//     what luaL_loadbuffer(L, text, ...) would have given.
//
// Bytecode is kept with its debug information (line numbers, local and upvalue names), so error messages and stack traces are the
// same as from the text.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct lua_State;

namespace script_cache
{
struct Stats
{
	uint32_t hits = 0;           // loaded from bytecode
	uint32_t prefetched = 0;     //   of which the entry came from the prefetch
	uint32_t misses = 0;         // no entry for the chunk
	uint32_t stale = 0;          // entry of other text (edited script, override): compiled again
	uint32_t incompatible = 0;   // entry of another VM / format version
	uint32_t corrupt = 0;        // truncated, damaged or unloadable entry
	uint32_t stored = 0;         // entries written
	uint32_t store_failed = 0;   // entries that could not be written (the load itself still worked)
	uint64_t source_bytes = 0;   // text that went through `load`
	uint64_t bytecode_bytes = 0; // bytecode read (hits)
	double hash_ms = 0;          // hashing the text
	double read_ms = 0;          // reading and checking an entry, and loading its bytecode (hits): io_ms + decode_ms + the checks
	double io_ms = 0;            //   of which reading the file
	double decode_ms = 0;        //   of which the VM loading the bytecode
	double compile_ms = 0;       // luaL_loadbuffer of the text (everything that was not a hit)
	double store_ms = 0;         // dumping and writing entries
};

// The cache directory, native path. Empty (the default) turns the cache off: `load` is luaL_loadbuffer then.
// `prefetch`: a thread reads the entries of the directory into memory in the background, so that the loads find them there (opening and reading
// hundreds of small files is most of the time of a hit: 53 of 60 ms for 474 entries on the iPad). Calling it again with the same directory
// refills the prefetch (the engine does this at every new Lua state).
void set_directory(const char* dir, bool prefetch = true);
// Waits until the prefetch thread has read the directory (tests).
void wait_prefetch();
const char* directory();

// luaL_loadbuffer(L, text, size, chunk_name) through the cache: 0 and the chunk on the stack, or an error code and message.
int load(lua_State* L, const char* text, size_t size, const char* chunk_name);

// Compiles and stores `text` without leaving a chunk on the stack (pre-warming). Returns whether the text compiled.
bool warm(lua_State* L, const char* text, size_t size, const char* chunk_name);

// The file that holds the entry of a chunk, in the current directory (empty when the cache is off).
std::string entry_path(const char* chunk_name);

// Deletes every entry (*.luac and leftover temporary files) of the current directory. Returns the number of files removed.
size_t purge();

// The 128-bit hash the entries use for the text and for the bytecode (exposed for the tests).
void hash128(const void* data, size_t size, uint64_t& h1, uint64_t& h2);

Stats stats();
void reset_stats();
} // namespace script_cache
