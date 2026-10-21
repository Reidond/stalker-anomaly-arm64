// Smoke test for the luabind port against LuaJIT 2.1: binds a class, a property and free functions,
// calls them from Lua and from C++, and checks how errors and C++ exceptions travel across the boundary.
//
// What "error" and "exception" mean depends on how luabind was built (see src/3rd party/luabind/CMakeLists.txt):
//  - LUABIND_NO_EXCEPTIONS (the release engine's mode, our default): luabind never throws or catches; a failed
//    call_function() goes to luabind::set_error_callback(). C++ exceptions thrown by bound functions are not
//    caught by luabind, so they cross the LuaJIT frames natively.
//  - otherwise (the debug engine's mode): luabind::error / cast_failed are thrown and bound functions' exceptions
//    are converted into Lua errors.
// Both modes must pass; the test reports which one it ran in.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include <luabind/luabind.hpp>

namespace
{
int g_failures = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);                                               \
            ++g_failures;                                                                                        \
        }                                                                                                        \
    } while (0)

// ---- the bound C++ side ---------------------------------------------------------------------------------

struct Counter
{
    explicit Counter(int v) : value(v) { ++live; }
    Counter(const Counter& o) : value(o.value), scale(o.scale) { ++live; }
    virtual ~Counter() { --live; }

    void add(int n) { value += n; }
    int get() const { return value; }
    void set_value(int v) { value = v; }
    [[noreturn]] void explode() const { throw std::runtime_error("counter exploded"); }

    int value;
    float scale = 1.5f;
    static int live;
};
int Counter::live = 0;

struct Derived : Counter
{
    Derived() : Counter(100) {}
    int twice() const { return value * 2; }
};

int free_add(int a, int b) { return a + b; }

const char* greet(const char* name)
{
    static std::string buffer;
    buffer = std::string("Hello, ") + name;
    return buffer.c_str();
}

[[noreturn]] void free_throw() { throw std::runtime_error("free function exploded"); }

void register_bindings(lua_State* L)
{
    using namespace luabind;
    open(L);
    module(L)
    [
        def("free_add", &free_add),
        def("greet", &greet),
        def("free_throw", &free_throw),

        class_<Counter>("Counter")
            .def(constructor<int>())
            .def("add", &Counter::add)
            .def("get", &Counter::get)
            .def("explode", &Counter::explode)
            .property("value", &Counter::get, &Counter::set_value)
            .def_readwrite("scale", &Counter::scale),

        class_<Derived, Counter>("Derived")
            .def(constructor<>())
            .def("twice", &Derived::twice)
    ];
}

// ---- helpers -------------------------------------------------------------------------------------------

// Runs a chunk under lua_pcall. Returns true on success; otherwise copies the error message into 'error'.
bool run(lua_State* L, const char* code, std::string* error = nullptr)
{
    int status = luaL_loadstring(L, code);
    if (status == 0)
        status = lua_pcall(L, 0, 0, 0);
    if (status != 0)
    {
        if (error)
            *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(non-string error)";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

lua_State* new_state()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    register_bindings(L);
    return L;
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// ---- tests ---------------------------------------------------------------------------------------------

void test_bindings()
{
    std::printf("bindings: class, method, property, inheritance, free functions\n");
    lua_State* L = new_state();

    std::string err;
    const bool ok = run(L, R"lua(
        local c = Counter(5)
        c:add(3)
        assert(c:get() == 8, "method")
        c.value = 20
        assert(c.value == 20 and c:get() == 20, "property")
        c.scale = 2.5
        assert(c.scale == 2.5, "readwrite member")
        assert(free_add(2, 3) == 5, "free function")
        assert(greet("Zone") == "Hello, Zone", "string argument and result")

        local d = Derived()
        d:add(1)
        assert(d:get() == 101 and d:twice() == 202, "inherited method")
        result = c:get() + free_add(40, 2)
    )lua", &err);
    if (!ok)
        std::printf("  script error: %s\n", err.c_str());
    CHECK(ok);

    lua_getglobal(L, "result");
    CHECK(lua_tointeger(L, -1) == 62);
    lua_pop(L, 1);

    // A Lua error must come back through lua_pcall with its message intact.
    CHECK(!run(L, "error('boom from lua')", &err) && contains(err, "boom from lua"));
    CHECK(!run(L, "local t = nil; return t.x", &err) && contains(err, "index"));
    CHECK(!run(L, "Counter(1):no_such_method()", &err));
    std::printf("  lua error message: %s\n", err.c_str());

    lua_close(L);
    CHECK(Counter::live == 0);
}

void test_call_from_cpp()
{
    std::printf("call_function: C++ -> Lua with values and a C++ object\n");
    lua_State* L = new_state();
    CHECK(run(L, "function lua_mul(a, b) return a * b end\n"
                 "function touch(c) c:add(10) return c:get() end"));

    CHECK(luabind::call_function<int>(L, "lua_mul", 6, 7) == 42);

    Counter native(1);
    CHECK(luabind::call_function<int>(L, "touch", &native) == 11);
    CHECK(native.get() == 11); // the Lua side mutated the C++ object, not a copy

    lua_close(L);
    CHECK(Counter::live == 1); // only 'native' is left
}

void test_gc()
{
    std::printf("garbage collection destroys bound C++ objects\n");
    CHECK(Counter::live == 0);
    lua_State* L = new_state();
    CHECK(run(L, "do local c = Counter(1); local d = Derived() end collectgarbage() collectgarbage()"));
    CHECK(Counter::live == 0);
    CHECK(run(L, "keep = Counter(7)"));
    CHECK(Counter::live == 1);
    lua_close(L); // lua_close runs the remaining finalizers
    CHECK(Counter::live == 0);
}

void test_cpp_exceptions_in_bound_functions()
{
    std::printf("a C++ exception thrown by a bound function\n");
    lua_State* L = new_state();
    std::string err;
    bool propagated = false;
    bool ok = true;
    try
    {
        ok = run(L, "free_throw()", &err);
    }
    catch (const std::exception& e)
    {
        propagated = true;
        err = e.what();
    }

#ifdef LUABIND_NO_EXCEPTIONS
    // luabind does not catch: either LuaJIT's unwinder hands it to C++ or lua_pcall reports it. Never a crash.
    std::printf("  (no-exceptions build) %s: \"%s\"\n",
                propagated ? "propagated through the Lua frames to the C++ catch" : "caught by lua_pcall", err.c_str());
    CHECK(propagated || !ok);
    // Do not close this state if the exception skipped lua_pcall's cleanup.
    if (!propagated)
        lua_close(L);
#else
    std::printf("  (exceptions build) lua_pcall reported: \"%s\"\n", err.c_str());
    CHECK(!propagated && !ok && contains(err, "free function exploded"));
    CHECK(!run(L, "Counter(3):explode()", &err) && contains(err, "counter exploded"));
    lua_close(L);
#endif
}

void test_argument_conversion()
{
    std::printf("argument conversion (luabind has no error checking in this build)\n");
    lua_State* L = new_state();
    // Arguments are converted by lua_tonumber/lua_tostring without checks: numeric strings work.
    CHECK(run(L, "assert(free_add('7', 1) == 8)"));
    lua_close(L);
}

#ifdef LUABIND_NO_EXCEPTIONS
int g_error_pipe = -1;

void on_lua_error(lua_State* L)
{
    const char* msg = lua_tostring(L, -1);
    if (!msg)
        msg = "(no message)";
    const ssize_t n = write(g_error_pipe, msg, std::strlen(msg));
    (void)n;
    _exit(42); // like the engine's callback (Debug.fatal) this never returns to luabind
}
#endif

void test_call_function_failure()
{
    std::printf("call_function on a Lua function that errors\n");
    lua_State* L = new_state();
    // A non-void call: the void variant reports from a noexcept destructor, which cannot throw (it would
    // terminate in exceptions mode), so errors in void calls are only observable through the callback.
    CHECK(run(L, "function kaboom() error('lua kaboom') return 1 end"));

#ifdef LUABIND_NO_EXCEPTIONS
    // luabind hands the failure to the error callback and then carries on regardless, so the callback has to
    // leave. Run it in a child process to observe that.
    int fds[2];
    CHECK(pipe(fds) == 0);
    const pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        g_error_pipe = fds[1];
        luabind::set_error_callback(&on_lua_error);
        (void)static_cast<int>(luabind::call_function<int>(L, "kaboom"));
        _exit(0); // not reached if the callback ran
    }
    close(fds[1]);
    char buf[256] = {};
    const ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    std::printf("  error callback received: \"%s\"\n", n > 0 ? buf : "");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    CHECK(contains(buf, "lua kaboom"));
#else
    bool caught = false;
    try
    {
        (void)static_cast<int>(luabind::call_function<int>(L, "kaboom"));
    }
    catch (const luabind::error& e)
    {
        caught = true;
        const char* msg = lua_tostring(e.state(), -1);
        std::printf("  luabind::error: \"%s\"\n", msg ? msg : "");
        CHECK(msg && contains(msg, "lua kaboom"));
    }
    CHECK(caught);
#endif
    lua_close(L);
}

void test_allocator_hook()
{
    std::printf("luabind::set_allocator is honoured\n");
    static size_t calls = 0;
    static auto counting = [](const void* ptr, size_t size) -> void*
    {
        ++calls;
        if (!size)
        {
            std::free(const_cast<void*>(ptr));
            return nullptr;
        }
        return ptr ? std::realloc(const_cast<void*>(ptr), size) : std::malloc(size);
    };
    luabind::set_allocator(+[](const void* ptr, size_t size) -> void* { return counting(ptr, size); });

    lua_State* L = new_state();
    CHECK(run(L, "local c = Counter(1); c:add(1)"));
    lua_close(L);
    std::printf("  allocator calls: %zu\n", calls);
    CHECK(calls > 0);

    luabind::set_allocator(nullptr); // back to malloc/realloc/free
}

void report_lua()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    run(L, "local ok, jit = pcall(require, 'jit');"
           "io.write('  ', _VERSION, ', ', ok and jit.version or 'no jit module', ', jit ',"
           "         (ok and jit.status()) and 'on' or 'off', ', ', ok and jit.arch or '?', '\\n')");
    lua_close(L);
}
} // namespace

int main()
{
    std::printf("luabind_smoke (%s)\n",
#ifdef LUABIND_NO_EXCEPTIONS
                "LUABIND_NO_EXCEPTIONS"
#else
                "exceptions enabled"
#endif
    );
    report_lua();

    test_bindings();
    test_call_from_cpp();
    test_gc();
    test_argument_conversion();
    test_call_function_failure();
    test_allocator_hook();
    test_cpp_exceptions_in_bound_functions(); // last: the no-exceptions variant may leave a state unusable

    std::printf(g_failures ? "luabind_smoke: FAILED (%d)\n" : "luabind_smoke: passed\n", g_failures);
    return g_failures ? 1 : 0;
}
