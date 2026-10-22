// Smoke test for lua_extensions against LuaJIT 2.1: the libraries Anomaly's scripts load through the script engine
// (string/table additions, marshal for the .scoc save data, lfs, the io wrappers that take the engine's virtual
// paths, luasocket's core). Mirrors how CScriptStorage opens them: luaL_openlibs, luaopen_lua_extensions, then the
// native-path hook.
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
#include <luajit.h>
}

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

-- LuaJIT's own bit and math stay in place (the extension versions are USE_LUAJIT_ONE only)
check(bit.band(0xff, 0x0f) == 15 and bit.tohex(255) == "000000ff", "bit")
check(math.random() < 1 and math.random(5) <= 5 and math.random(3, 4) >= 3, "math.random")

-- marshal: the .scoc save data
local t = {1, 2, 3, x = "y", nested = {a = true, b = 1.5, c = "str"}, [10] = false, [2.5] = "frac"}
t.self = t
local r = marshal.decode(marshal.encode(t))
check(r[1] == 1 and r.x == "y" and r.nested.b == 1.5 and r.nested.c == "str" and r[10] == false and r[2.5] == "frac", "marshal values")
check(r.self == r and r.nested.a == true, "marshal cycle and booleans")
check(marshal.decode(marshal.encode({f = function() return 42 end})).f() == 42, "marshal function (LuaJIT 2.1 bytecode)")
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
        std::printf("lua_ext_smoke: %s (%s)\n", failures ? "FAILED" : "all checks passed", LUAJIT_VERSION);
        rc = failures ? 1 : 0;
    }
    lua_close(L);

    unlink((g_root + "/sub/a.txt").c_str());
    rmdir((g_root + "/sub").c_str());
    rmdir(g_root.c_str());
    return rc;
}
