// Host test of the script bytecode cache (src/xrServerEntities/script_bytecode_cache.cpp): the cache has no engine dependency.
//
//   lua_cache_test                     the checks below; exit status 0 when every one passes
//   lua_cache_test --bench <dir> [n]   compiles every *.script under <dir> n times (default 3) from the text and then through a warm
//                                      cache, wrapped the way CScriptStorage wraps a script, and prints the times
//
// Every check runs in a fresh lua_State and a fresh cache directory, the way two launches of the game would.

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luajit.h>
}

#include <xrServerEntities/script_bytecode_cache.h>

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char* what, const std::string& detail = std::string())
{
	++g_checks;
	if (!ok)
	{
		++g_failed;
		printf("  FAIL  %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
	}
}

#define CHECK(cond) check((cond), #cond)
#define CHECK_EQ(a, b) do { const std::string _a = (a), _b = (b); check(_a == _b, #a " == " #b, "'" + _a + "' vs '" + _b + "'"); } while (0)

struct Lua
{
	lua_State* L;
	Lua() : L(luaL_newstate()) { luaL_openlibs(L); }
	~Lua() { lua_close(L); }
	Lua(const Lua&) = delete;
	Lua& operator=(const Lua&) = delete;
};

// Loads `text` as `name` through the cache and runs it. Returns what the chunk returned (tostring of each value, joined by '|'), or
// "LOADERR:<message>" / "RUNERR:<message>".
std::string run(const std::string& text, const char* name, bool* from_cache = nullptr)
{
	Lua lua;
	const script_cache::Stats before = script_cache::stats();
	const int rc = script_cache::load(lua.L, text.data(), text.size(), name);
	if (from_cache)
		*from_cache = script_cache::stats().hits > before.hits;
	if (rc != 0)
		return std::string("LOADERR:") + lua_tostring(lua.L, -1);
	if (lua_pcall(lua.L, 0, LUA_MULTRET, 0) != 0)
		return std::string("RUNERR:") + lua_tostring(lua.L, -1);
	std::string out;
	for (int i = 1; i <= lua_gettop(lua.L); ++i)
	{
		if (i > 1)
			out += "|";
		lua_getglobal(lua.L, "tostring");
		lua_pushvalue(lua.L, i);
		lua_call(lua.L, 1, 1);
		out += lua_tostring(lua.L, -1);
		lua_pop(lua.L, 1);
	}
	return out;
}

std::string read_all(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void write_all(const std::string& path, const std::string& data)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(data.data(), std::streamsize(data.size()));
}

struct TempDir
{
	std::string path;
	explicit TempDir(const char* tag)
	{
		static int counter = 0;
		path = (fs::temp_directory_path() / ("lua_cache_test_" + std::to_string(getpid()) + "_" + tag + "_" + std::to_string(counter++))).string();
		fs::remove_all(path);
		fs::create_directories(path);
	}
	~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// The cache header as the file keeps it (script_bytecode_cache.cpp), for tests that damage an entry in a chosen way.
struct Header
{
	char magic[8];
	uint32_t format, vm_id, name_len, reserved;
	uint64_t src_size, src_h1, src_h2, bc_size, bc_h1, bc_h2;
};
static_assert(sizeof(Header) == 72, "entry header");

const char* const kName = "@scripts/test_script.script";

// A script with several lines, a function, an upvalue and a runtime error to compare the debug information.
const std::string kScriptV1 =
	"local counter = 0\n"
	"local function bump(n)\n"
	"\tcounter = counter + n\n"
	"\treturn counter\n"
	"end\n"
	"local info = debug.getinfo(bump, 'S')\n"
	"local ok, err = pcall(function() error('boom') end)\n"
	"return 'v1', bump(2) + bump(3), info.short_src, info.linedefined, err\n";

std::string variant(const char* tag)
{
	return std::string("return '") + tag + "'\n";
}

// ---- the checks --------------------------------------------------------------------------------------------------------------------

void test_off_by_default()
{
	printf("off by default / empty directory\n");
	script_cache::set_directory("");
	script_cache::reset_stats();
	CHECK_EQ(run(variant("a"), kName), "a");
	const auto s = script_cache::stats();
	CHECK(s.hits + s.misses + s.stored == 0);
	CHECK(script_cache::entry_path(kName).empty());
}

void test_cold_then_warm()
{
	printf("miss, store, hit\n");
	TempDir dir("cold");
	script_cache::set_directory(dir.path.c_str());
	script_cache::reset_stats();
	bool cached = false;
	const std::string first = run(kScriptV1, kName, &cached);
	CHECK(!cached);
	CHECK(script_cache::stats().misses == 1 && script_cache::stats().stored == 1);
	CHECK(fs::exists(script_cache::entry_path(kName)));
	const std::string second = run(kScriptV1, kName, &cached);
	CHECK(cached);
	CHECK_EQ(first, second);
	CHECK(script_cache::stats().hits == 1);
	// the chunk name, and so error messages and tracebacks, survive: short_src and the line numbers come from the bytecode's debug info
	CHECK(first.find("test_script.script") != std::string::npos);
	CHECK(first.find("test_script.script:7: boom") != std::string::npos);
	// no temporary files are left behind
	for (const auto& e : fs::directory_iterator(dir.path))
		CHECK(e.path().extension() == ".luac");
}

void test_stale_source()
{
	printf("stale source: an edited script loads from the text\n");
	TempDir dir("stale");
	script_cache::set_directory(dir.path.c_str());
	script_cache::reset_stats();
	run(variant("one"), kName);
	bool cached = false;
	CHECK_EQ(run(variant("one"), kName, &cached), "one");
	CHECK(cached);
	// edited: another text under the same chunk name
	CHECK_EQ(run(variant("two"), kName, &cached), "two");
	CHECK(!cached);
	CHECK(script_cache::stats().stale == 1);
	// the entry now serves the new text, and the old text is stale in turn
	CHECK_EQ(run(variant("two"), kName, &cached), "two");
	CHECK(cached);
	CHECK_EQ(run(variant("one"), kName, &cached), "one");
	CHECK(!cached);
	// a change of one byte at the end of a long text, and a change that keeps the size
	std::string big = "local t = {}\n";
	for (int i = 0; i < 3000; ++i)
		big += "t[" + std::to_string(i) + "] = " + std::to_string(i * 7) + "\n";
	const std::string big_a = big + "return t[2999]\n", big_b = big + "return t[2998]\n";
	CHECK_EQ(run(big_a, "@scripts/big.script"), "20993");
	CHECK_EQ(run(big_b, "@scripts/big.script", &cached), "20986");
	CHECK(!cached);
	CHECK_EQ(run(big_b, "@scripts/big.script", &cached), "20986");
	CHECK(cached);
}

void test_addon_override()
{
	printf("addon override of a stock script\n");
	TempDir dir("addon");
	script_cache::set_directory(dir.path.c_str());
	script_cache::reset_stats();
	const char* name = "@gamedata/scripts/bind_stalker.script";
	bool cached = false;
	// launch 1: the stock script (from an archive); launch 2: same path, an addon's file; launch 3: the addon removed; launch 4: back
	CHECK_EQ(run(variant("stock"), name, &cached), "stock");
	CHECK_EQ(run(variant("stock"), name, &cached), "stock");
	CHECK(cached);
	CHECK_EQ(run(variant("addon"), name, &cached), "addon");
	CHECK(!cached);
	CHECK_EQ(run(variant("addon"), name, &cached), "addon");
	CHECK(cached);
	CHECK_EQ(run(variant("stock"), name, &cached), "stock");
	CHECK(!cached);
	CHECK_EQ(run(variant("stock"), name, &cached), "stock");
	CHECK(cached);
	// an addon script with no stock counterpart is cached on its own
	CHECK_EQ(run(variant("new"), "@gamedata/scripts/my_addon.script", &cached), "new");
	CHECK_EQ(run(variant("new"), "@gamedata/scripts/my_addon.script", &cached), "new");
	CHECK(cached);
	CHECK(script_cache::entry_path(name) != script_cache::entry_path("@gamedata/scripts/my_addon.script"));
}

void test_corrupt_entries()
{
	printf("corrupt entries fall back to the text and are rewritten\n");
	TempDir dir("corrupt");
	script_cache::set_directory(dir.path.c_str());
	const std::string path = [] { return script_cache::entry_path(kName); }();
	auto fresh_entry = [&]() { fs::remove(path); run(kScriptV1, kName); return read_all(path); };
	const std::string expect = run(kScriptV1, kName);
	auto expect_fallback = [&](const char* what, const std::string& damaged, uint32_t script_cache::Stats::*counter) {
		fresh_entry();
		write_all(path, damaged);
		script_cache::reset_stats();
		bool cached = true;
		const std::string result = run(kScriptV1, kName, &cached);
		check(result == expect, what, "result '" + result + "'");
		check(!cached, what, "was a hit");
		check(script_cache::stats().*counter == 1, what, "wrong counter");
		// the entry was rewritten: the next load is a hit
		run(kScriptV1, kName, &cached);
		check(cached, what, "entry not repaired");
	};

	const std::string good = fresh_entry();
	CHECK(good.size() > sizeof(Header));
	expect_fallback("empty file", "", &script_cache::Stats::corrupt);
	expect_fallback("garbage", std::string(500, 'x'), &script_cache::Stats::corrupt);
	expect_fallback("shorter than the header", good.substr(0, 40), &script_cache::Stats::corrupt);
	expect_fallback("truncated bytecode", good.substr(0, good.size() - 30), &script_cache::Stats::corrupt);
	expect_fallback("truncated to the header", good.substr(0, sizeof(Header)), &script_cache::Stats::corrupt);
	expect_fallback("extra bytes at the end", good + "tail", &script_cache::Stats::corrupt);
	{
		std::string d = good;
		d[d.size() - 10] ^= 0x55; // a flipped bit in the bytecode
		expect_fallback("flipped bytecode byte", d, &script_cache::Stats::corrupt);
	}
	{
		std::string d = good;
		d[0] = 'X'; // not our file
		expect_fallback("bad magic", d, &script_cache::Stats::corrupt);
	}
	{
		std::string d = good;
		Header h;
		memcpy(&h, d.data(), sizeof(h));
		h.bc_size += 100; // a size that does not add up
		memcpy(&d[0], &h, sizeof(h));
		expect_fallback("wrong bytecode size", d, &script_cache::Stats::corrupt);
	}
	{
		std::string d = good;
		Header h;
		memcpy(&h, d.data(), sizeof(h));
		h.name_len = 0xffffffffu;
		memcpy(&d[0], &h, sizeof(h));
		expect_fallback("absurd name length", d, &script_cache::Stats::corrupt);
	}
	{
		// the entry is internally consistent (the bytecode hash matches) but the bytecode itself is garbage: the VM refuses it
		std::string d = good.substr(0, sizeof(Header) + strlen(kName));
		const std::string junk = good.substr(sizeof(Header) + strlen(kName), 5) + std::string(40, '\x7f'); // the VM's own bytecode header, then junk
		Header h;
		memcpy(&h, d.data(), sizeof(h));
		h.bc_size = junk.size();
		script_cache::hash128(junk.data(), junk.size(), h.bc_h1, h.bc_h2);
		memcpy(&d[0], &h, sizeof(h));
		d += junk;
		expect_fallback("bytecode the VM refuses", d, &script_cache::Stats::corrupt);
	}
	// a directory where the entry should be
	fs::remove(path);
	fs::create_directory(path);
	script_cache::reset_stats();
	CHECK_EQ(run(kScriptV1, kName), expect);
	CHECK(script_cache::stats().store_failed == 1);
	fs::remove_all(path);
}

void test_other_vm()
{
	printf("an entry of another VM or format is not used\n");
	TempDir dir("vm");
	script_cache::set_directory(dir.path.c_str());
	run(kScriptV1, kName);
	const std::string path = script_cache::entry_path(kName);
	const std::string good = read_all(path);
	const std::string expect = run(kScriptV1, kName);
	auto damaged = [&](const char* what, auto&& edit, uint32_t script_cache::Stats::*counter) {
		std::string d = good;
		Header h;
		memcpy(&h, d.data(), sizeof(h));
		edit(h, d);
		memcpy(&d[0], &h, sizeof(h));
		write_all(path, d);
		script_cache::reset_stats();
		bool cached = true;
		check(run(kScriptV1, kName, &cached) == expect, what);
		check(!cached, what, "was a hit");
		check(script_cache::stats().*counter == 1, what, "wrong counter");
	};
	damaged("another VM id", [](Header& h, std::string&) { h.vm_id ^= 0x1234; }, &script_cache::Stats::incompatible);
	damaged("another cache format", [](Header& h, std::string&) { h.format = 99; }, &script_cache::Stats::incompatible);
	// bytecode of the same text with the flags of another build (32-bit GC references, big endian): the entry is consistent, its
	// bytecode header is not the running VM's
	for (unsigned char flip : {0x01, 0x08})
	{
		char what[64];
		snprintf(what, sizeof(what), "bytecode flags ^ 0x%02x", flip);
		damaged(what, [&](Header& h, std::string& d) {
			char* bc = &d[sizeof(Header) + h.name_len];
			bc[4] = char(bc[4] ^ flip);
			script_cache::hash128(bc, size_t(h.bc_size), h.bc_h1, h.bc_h2);
		}, &script_cache::Stats::incompatible);
	}
	// a rewritten entry is good again
	bool cached = false;
	run(kScriptV1, kName, &cached);
	run(kScriptV1, kName, &cached);
	CHECK(cached);
}

void test_errors_and_tracebacks()
{
	printf("error messages and tracebacks are the same from bytecode\n");
	const std::string text =
		"local function inner()\n"
		"\terror('deep')\n"
		"end\n"
		"local function outer()\n"
		"\tinner()\n"
		"end\n"
		"local ok, err = xpcall(outer, debug.traceback)\n"
		"return err, debug.getinfo(1, 'S').source\n";
	TempDir dir("trace");
	script_cache::set_directory(dir.path.c_str());
	bool cached = false;
	const std::string cold = run(text, "@/g/scripts/trace.script", &cached);
	CHECK(!cached);
	const std::string warm = run(text, "@/g/scripts/trace.script", &cached);
	CHECK(cached);
	CHECK_EQ(cold, warm);
	CHECK(cold.find("/g/scripts/trace.script:2: deep") != std::string::npos);
	CHECK(cold.find("/g/scripts/trace.script:5:") != std::string::npos);
	// a runtime error in the main chunk
	const std::string bad = "local x = 1\nlocal y = nil\nreturn y.field\n";
	const std::string e1 = run(bad, "@/g/scripts/bad.script", &cached), e2 = run(bad, "@/g/scripts/bad.script", &cached);
	CHECK(cached);
	CHECK_EQ(e1, e2);
	CHECK(e1.find("bad.script:3:") != std::string::npos);
	// a syntax error: the same message as luaL_loadbuffer, nothing stored
	const std::string syntax = "local x = = 1\n";
	Lua lua;
	CHECK(luaL_loadbuffer(lua.L, syntax.data(), syntax.size(), "@/g/scripts/syn.script") != 0);
	const std::string plain = lua_tostring(lua.L, -1);
	script_cache::reset_stats();
	CHECK_EQ(run(syntax, "@/g/scripts/syn.script"), "LOADERR:" + plain);
	CHECK(script_cache::stats().stored == 0);
	CHECK(!fs::exists(script_cache::entry_path("@/g/scripts/syn.script")));
}

void test_unwritable()
{
	printf("an unusable directory never breaks a load\n");
	TempDir dir("unwritable");
	const std::string file = dir.path + "/a_file";
	write_all(file, "x");
	script_cache::reset_stats();
	script_cache::set_directory((file + "/under_a_file").c_str());
	CHECK_EQ(run(variant("a"), kName), "a");
	CHECK(script_cache::stats().store_failed == 1);
	script_cache::set_directory((dir.path + "/missing/a/b/c").c_str()); // created on demand
	script_cache::reset_stats();
	bool cached = false;
	CHECK_EQ(run(variant("b"), kName), "b");
	CHECK_EQ(run(variant("b"), kName, &cached), "b");
	CHECK(cached);
	// read-only directory (not when running as root)
	const std::string ro = dir.path + "/ro";
	fs::create_directory(ro);
	fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec);
	script_cache::set_directory(ro.c_str());
	script_cache::reset_stats();
	CHECK_EQ(run(variant("c"), kName), "c");
	if (geteuid() != 0)
		CHECK(script_cache::stats().store_failed == 1);
	fs::permissions(ro, fs::perms::owner_all);
}

void test_warm_and_purge()
{
	printf("warm and purge\n");
	TempDir dir("warm");
	script_cache::set_directory(dir.path.c_str());
	script_cache::reset_stats();
	Lua lua;
	CHECK(script_cache::warm(lua.L, kScriptV1.data(), kScriptV1.size(), kName));
	CHECK(lua_gettop(lua.L) == 0);
	CHECK(fs::exists(script_cache::entry_path(kName)));
	bool cached = false;
	run(kScriptV1, kName, &cached);
	CHECK(cached);
	const std::string bad = "= =";
	CHECK(!script_cache::warm(lua.L, bad.data(), bad.size(), "@bad"));
	write_all(dir.path + "/leftover.1.2.tmp", "x");
	write_all(dir.path + "/other.txt", "keep");
	CHECK(script_cache::purge() == 2);
	CHECK(fs::exists(dir.path + "/other.txt"));
	run(kScriptV1, kName, &cached);
	CHECK(!cached);
}

void test_hash()
{
	printf("hash\n");
	std::string a(10000, 'a');
	for (size_t i = 0; i < a.size(); ++i)
		a[i] = char('a' + (i * 7) % 23);
	uint64_t h1, h2, g1, g2;
	script_cache::hash128(a.data(), a.size(), h1, h2);
	script_cache::hash128(a.data(), a.size(), g1, g2);
	CHECK(h1 == g1 && h2 == g2);
	int collisions = 0;
	// every length and a change of every byte of the first 300 bytes gives another hash
	for (size_t n = 0; n < 300; ++n)
	{
		script_cache::hash128(a.data(), n, g1, g2);
		if (n > 0 && g1 == h1 && g2 == h2)
			++collisions;
		std::string b = a.substr(0, 300);
		if (n < b.size())
		{
			uint64_t c1, c2, d1, d2;
			script_cache::hash128(b.data(), b.size(), c1, c2);
			b[n] ^= 1;
			script_cache::hash128(b.data(), b.size(), d1, d2);
			if (c1 == d1 && c2 == d2)
				++collisions;
		}
	}
	CHECK(collisions == 0);
	// order matters: two 16-byte blocks swapped
	std::string c = a;
	std::swap_ranges(c.begin(), c.begin() + 16, c.begin() + 16);
	script_cache::hash128(c.data(), c.size(), g1, g2);
	CHECK(!(g1 == h1 && g2 == h2));
}

void test_concurrent()
{
	printf("two threads storing and loading the same chunk\n");
	TempDir dir("threads");
	script_cache::set_directory(dir.path.c_str());
	std::atomic<int> wrong{0};
	auto worker = [&](int id) {
		for (int i = 0; i < 150; ++i)
		{
			Lua lua;
			// alternating texts: stores race with each other and with reads of the other thread
			const std::string text = variant((i + id) % 2 ? "odd" : "even");
			const std::string want = (i + id) % 2 ? "odd" : "even";
			if (script_cache::load(lua.L, text.data(), text.size(), kName) != 0 || lua_pcall(lua.L, 0, 1, 0) != 0 ||
			    want != lua_tostring(lua.L, -1))
				++wrong;
			if (i % 40 == 0)
				script_cache::purge();
		}
	};
	std::thread a(worker, 0), b(worker, 1);
	a.join();
	b.join();
	CHECK(wrong == 0);
	for (const auto& e : fs::directory_iterator(dir.path))
		CHECK(e.path().extension() == ".luac");
}

void test_large_script()
{
	printf("a large generated script\n");
	TempDir dir("large");
	script_cache::set_directory(dir.path.c_str());
	std::string text;
	for (int i = 0; i < 4000; ++i)
		text += "function f" + std::to_string(i) + "(a, b) local t = {a, b, 'x" + std::to_string(i) + "'} if a > b then return t[1] * " + std::to_string(i) +
			" else return #t[3] + b end end\n";
	text += "return f3999(1, 2) + f10(5, 1)\n";
	bool cached = false;
	const std::string cold = run(text, "@large.script", &cached);
	const std::string warm = run(text, "@large.script", &cached);
	CHECK(cached);
	CHECK_EQ(cold, warm);
	CHECK_EQ(warm, "57");
}

// ---- timing of the real scripts ---------------------------------------------------------------------------------------------------

double now_ms()
{
	return double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-6;
}

int bench(const char* dir, int rounds)
{
	// CScriptStorage's header (file_header_old), with the namespace the file name gives
	auto wrap = [](const std::string& ns, const std::string& text) {
		return "local function script_name() return \"" + ns + "\" end local this = {} " + ns + " = this  setmetatable(this, {__index = _G}) setfenv(1, this) " + text;
	};
	std::vector<std::pair<std::string, std::string>> files;
	size_t total = 0;
	for (const auto& e : fs::recursive_directory_iterator(dir))
	{
		if (e.path().extension() != ".script")
			continue;
		std::string text = read_all(e.path().string());
		files.emplace_back("@" + e.path().string(), wrap(e.path().stem().string(), text));
		total += text.size();
	}
	printf("%zu scripts, %.2f MB\n", files.size(), double(total) / 1e6);
	TempDir cache("bench");
	Lua lua;
	double text_ms = 1e30, cold_ms = 1e30, warm_ms = 1e30;
	for (int r = 0; r < rounds; ++r)
	{
		script_cache::set_directory("");
		double t = now_ms();
		int errors = 0;
		for (const auto& f : files)
		{
			if (luaL_loadbuffer(lua.L, f.second.data(), f.second.size(), f.first.c_str()) != 0)
				++errors;
			lua_settop(lua.L, 0);
		}
		text_ms = std::min(text_ms, now_ms() - t);
		script_cache::set_directory(cache.path.c_str());
		script_cache::purge();
		t = now_ms();
		for (const auto& f : files)
		{
			script_cache::load(lua.L, f.second.data(), f.second.size(), f.first.c_str());
			lua_settop(lua.L, 0);
		}
		cold_ms = std::min(cold_ms, now_ms() - t);
		t = now_ms();
		for (const auto& f : files)
		{
			script_cache::load(lua.L, f.second.data(), f.second.size(), f.first.c_str());
			lua_settop(lua.L, 0);
		}
		warm_ms = std::min(warm_ms, now_ms() - t);
		if (errors)
			printf("  (%d scripts do not compile on their own)\n", errors);
	}
	const auto s = script_cache::stats();
	printf("from the text %.1f ms, cold cache (compile + store) %.1f ms, warm cache %.1f ms (best of %d)\n", text_ms, cold_ms, warm_ms, rounds);
	size_t bytes = 0;
	for (const auto& e : fs::directory_iterator(cache.path))
		bytes += e.file_size();
	printf("cache %.2f MB for %.2f MB of text; last stats: %u hits, %u misses, hash %.1f ms, read %.1f ms\n", double(bytes) / 1e6, double(total) / 1e6,
	       s.hits, s.misses, s.hash_ms, s.read_ms);
	return 0;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc >= 3 && !strcmp(argv[1], "--bench"))
		return bench(argv[2], argc > 3 ? atoi(argv[3]) : 3);

	test_off_by_default();
	test_cold_then_warm();
	test_stale_source();
	test_addon_override();
	test_corrupt_entries();
	test_other_vm();
	test_errors_and_tracebacks();
	test_unwritable();
	test_warm_and_purge();
	test_hash();
	test_concurrent();
	test_large_script();
	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
