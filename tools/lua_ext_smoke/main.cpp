// Smoke test for lua_extensions against the configured VM (LuaJIT 2.1, or Lua 5.1.5 with ANOMALY_LUA=lua51): the libraries
// Anomaly's scripts load through the script engine (string/table additions, marshal for the .scoc save data, lfs, the io
// wrappers that take the engine's virtual paths, luasocket's core). Mirrors how CScriptStorage opens them: luaL_openlibs,
// luaopen_lua_extensions, then the native-path hook. Also checks the LuaJIT behaviour the scripts rely on and the vendored
// Lua 5.1 reproduces (src/3rd party/lua-5.1.5/README.anomaly.md): the same results on both VMs.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

extern "C"
{
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#ifndef ANOMALY_LUA51
#include <luajit.h>
#endif
}

#ifdef ANOMALY_LUA51
#define SMOKE_VM LUA_RELEASE
#else
#define SMOKE_VM LUAJIT_VERSION
#endif

extern int luaopen_lua_extensions(lua_State* L, bool IsDebug);
extern lua_CFunction luaopen_socket_core_init();
extern void lua_ext_set_native_path_hook(const char* (*hook)(const char*));

namespace
{
std::string g_root;

// Stand-in for xr_native_path: '\' becomes '/', and the virtual root "\virt\" lives in a temp directory.
const char* native_path(const char* path)
{
    static thread_local char buf[1024];
    std::string s = path;
    for (char& c : s)
        if (c == '\\')
            c = '/';
    if (s.rfind("/virt/", 0) == 0)
        s = g_root + "/" + s.substr(6);
    std::snprintf(buf, sizeof buf, "%s", s.c_str());
    return buf;
}

const char* script = R"LUA(
local failures = 0
local function check(cond, what)
    if not cond then failures = failures + 1; print("  FAIL: " .. what) end
end

-- string / table additions (script_additional_libs.cpp)
check(string.trim("  hi  ") == "hi", "string.trim")
check(string.trim_l("  hi  ") == "hi  ", "string.trim_l")
check(string.trim_r("  hi  ") == "  hi", "string.trim_r")
check(string.trim_w("  hi  there") == "hi", "string.trim_w")
check(table.size({a = 1, b = 2, 3}) == 3, "table.size")
check(#table.keys({a = 1, b = 2}) == 2 and #table.values({a = 1, b = 2}) == 2, "table.keys/values")
local k, v = table.random({x = 1})
check(k == "x" and v == 1, "table.random")

-- LuaJIT's own bit and math stay in place (the extension versions are USE_LUAJIT_ONE only); on Lua 5.1, LuaBitOp and LuaJIT's
-- generator (README.anomaly.md)
check(bit.band(0xff, 0x0f) == 15 and bit.tohex(255) == "000000ff", "bit")
check(bit.tohex(-1) == "ffffffff" and bit.bor(1, 2, 4) == 7 and bit.lshift(1, 31) == -2147483648, "bit edge cases")
check(math.random() < 1 and math.random(5) <= 5 and math.random(3, 4) >= 3, "math.random")

-- What LuaJIT 2.1 gives (values taken from it): the same on both VMs
math.randomseed(42)
local r1, r2, r3, r4 = math.random(1, 100), math.random(1, 100), math.random(1, 100), math.random(1000000)
check(r1 == 96 and r2 == 1 and r3 == 55 and r4 == 434114, "math.randomseed(42) sequence")
check(string.format("%.17g", math.random()) == "0.68847511065221734", "math.random() after the seed")
check(math.random(5, 1) == 2 and math.random(0) == 1 and math.random(2.5, 7.25) == 3.5, "math.random: no interval check, number bounds")
check(string.format("%s|%s|%s|%q", nil, true, false, nil) == 'nil|true|false|"nil"', "string.format %s/%q of nil and booleans")
check(string.format("%s", setmetatable({}, {__tostring = function() return "obj" end})) == "obj", "string.format %s: __tostring")
check(string.format("%s", {}):find("^table: ") ~= nil, "string.format %s of a table")
check(string.format("%d %x", 3.7, 2^40) == "3 10000000000", "string.format %d and 64-bit %x")
check("\x41\x42" == "AB" and "a\z
      b" == "ab", "\\x and \\z escapes")
check(loadstring("return [[a [[b]]")() == "a [[b", "[[ inside a long string")
check(math.mod and string.gfind, "math.mod and string.gfind (set by the engine on LuaJIT 2.1)")

-- marshal: the .scoc save data
local t = {1, 2, 3, x = "y", nested = {a = true, b = 1.5, c = "str"}, [10] = false, [2.5] = "frac"}
t.self = t
local r = marshal.decode(marshal.encode(t))
check(r[1] == 1 and r.x == "y" and r.nested.b == 1.5 and r.nested.c == "str" and r[10] == false and r[2.5] == "frac", "marshal values")
check(r.self == r and r.nested.a == true, "marshal cycle and booleans")
check(marshal.decode(marshal.encode({f = function() return 42 end})).f() == 42, "marshal function (the VM's bytecode)")
check(not pcall(marshal.decode, "garbage\1\2\3"), "marshal rejects garbage")
local big = {}
for i = 1, 5000 do big[i] = tostring(i) end
local rb = marshal.decode(marshal.encode(big))
check(#rb == 5000 and rb[5000] == "5000", "marshal large table")
check(marshal.decode(marshal.encode({s = ("x"):rep(100000)})).s == ("x"):rep(100000), "marshal long string")

-- lfs and io with the engine's virtual paths
check(lfs.currentdir() ~= nil, "lfs.currentdir")
check(lfs.mkdir("\\virt\\sub"), "lfs.mkdir virtual path")
check(lfs.attributes("\\virt\\sub", "mode") == "directory", "lfs.attributes virtual path")
local f = io.open("\\virt\\sub\\a.txt", "wb")
check(io.type(f) == "file", "io.open virtual path")
f:write("hello")
f:close()
local names = {}
for e in lfs.dir("\\virt\\sub") do names[e] = true end
check(names["a.txt"] and names["."] and names[".."], "lfs.dir virtual path")
check(lfs.attributes("\\virt\\sub\\a.txt", "size") == 5, "lfs.attributes size")
local lines = 0
for l in io.lines("\\virt\\sub\\a.txt") do lines = lines + 1; check(l == "hello", "io.lines content") end
check(lines == 1, "io.lines")
check(select("#", loadfile("\\virt\\missing.lua")) == 2, "loadfile reports an error for a missing file")
check(not lfs.rmdir("\\virt\\sub"), "lfs.rmdir refuses a non-empty directory")
check(lfs.rmdir("\\virt\\sub") == nil, "lfs.rmdir returns nil, err")

-- luasocket core
local core = open_socket_core()
check(type(core) == "table" and type(core.gettime) == "function" and type(core.tcp) == "function", "socket.core loads")
check(core.gettime() > 0, "socket.gettime")

return failures
)LUA";
} // namespace

int main()
{
    char tmpl[] = "/tmp/lua_ext_smoke.XXXXXX";
    if (!mkdtemp(tmpl))
    {
        std::printf("lua_ext_smoke: cannot create a temp directory\n");
        return 2;
    }
    g_root = tmpl;

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_dostring(L, "math.mod = math.fmod string.gfind = string.gmatch"); // as CScriptStorage::reinit
    luaopen_lua_extensions(L, false);
    lua_ext_set_native_path_hook(native_path);
    lua_pushcfunction(L, luaopen_socket_core_init());
    lua_setglobal(L, "open_socket_core");

    int rc = 0;
    if (luaL_dostring(L, script))
    {
        std::printf("lua_ext_smoke: script error: %s\n", lua_tostring(L, -1));
        rc = 1;
    }
    else
    {
        int failures = (int)lua_tointeger(L, -1);
        std::printf("lua_ext_smoke: %s (%s)\n", failures ? "FAILED" : "all checks passed", SMOKE_VM);
        rc = failures ? 1 : 0;
    }
    lua_close(L);

    unlink((g_root + "/sub/a.txt").c_str());
    rmdir((g_root + "/sub").c_str());
    rmdir(g_root.c_str());
    return rc;
}
