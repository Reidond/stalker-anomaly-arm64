# Lua 5.1.5 and LuaBitOp in the Anomaly port

PUC Lua 5.1.5 (<https://www.lua.org/ftp/lua-5.1.5.tar.gz>, sha256 `2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333`)
and LuaBitOp 1.0.3 (<https://bitop.luajit.org/>), both MIT (`COPYRIGHT`, the header of `bitop/bit.c`). The script VM of
`ANOMALY_LUA=lua51` (`cmake/deps/Lua51.cmake`) and of the web build, where LuaJIT has no target. Why and what it costs:
`docs/web-port/LUA51.md`.

Layout: `include/` holds the public headers (`lua.h`, `luaconf.h`, `lualib.h`, `lauxlib.h` and `etc/lua.hpp`), `src/` the
library sources and private headers. `lua.c`, `luac.c` and `print.c` (the stand-alone programs) are left out. The unmodified
upstream files are the first commit that added this folder.

The library is compiled as C++ (`-x c++`): `lua_error` throws a C++ exception (`luaconf.h` `LUAI_THROW`) instead of `longjmp`,
so it unwinds the engine's C++ frames with their destructors, and the wasm build needs no `setjmp`.

## Changes to the upstream sources

Every change is marked `ANOMALY` in the source.

| File | Change | Why |
|---|---|---|
| `include/luaconf.h` | `LUA_API` is `extern "C"` when compiled as C++ | The library is C++, its API keeps C linkage: C files (`lfs.c`, `lmarshal.c`, luasocket) and C++ files that include `lua.h` inside `extern "C"` link to the same symbols |
| `include/luaconf.h` | `LUA_COMPAT_LSTR` off | As LuaJIT: `[[` inside a long string or comment is text, not the error "nesting of [[...]] is deprecated" |
| `src/llex.c` | `\xXX` and `\z` escapes | LuaJIT's (and 5.2's). The Monolith overlay writes 1,355 UTF-8 icon glyphs as `\xXX` (`imgui_helper.script`); stock 5.1 turns `\xef` into the text `xef` without an error |
| `src/lvm.c` | A numeric `for` with a step of 0 counts as a positive step | As LuaJIT: `for i=1,0,0` runs no iteration. Stock 5.1 takes 0 as negative and never ends; `item_radio.script:408` runs `for i=1,size,(size/5)` with `size = 0` in every new game, which froze the game on its first update |
| `src/lstrlib.c` | `string.format` `%s` and `%q` take any value | As LuaJIT: a `__tostring` metamethod is called once, otherwise `nil`, booleans and objects are written as `tostring` writes them. Stock 5.1 raises "string expected, got boolean"; the scripts pass booleans and nil to `%s` in log and UI text |
| `src/lmathlib.c` | `math.random` and `math.randomseed` are LuaJIT 2.1's | Same generator (Tausworthe, period 2^223, from LuaJIT's `lib_math.c` / `lj_prng.c`, MIT), same initial state, so a seed gives LuaJIT's numbers. Arguments are taken as LuaJIT takes them: numbers, no "interval is empty" check (`math.random(5, 1)` returns a number, stock 5.1 raises an error). Stock 5.1 uses the C library's `rand()` |
| `include/lualib.h`, `src/linit.c` | `luaopen_bit` declared and opened by `luaL_openlibs` | As LuaJIT's `luaL_openlibs` opens its built-in `bit` (LuaBitOp is the same API) |
| `src/lanomaly.{h,c}` (new), `src/ldo.c`, `src/lgc.c` | A global VM state (`'I'` bytecode, `'C'` a C function, `'G'` the collector, `'N'` outside), read by `lua51_vmstate()` | The sampling profiler of `script_perf.cpp` splits its samples by VM state as LuaJIT's does. One store per C function call, per Lua call from C and per collector step; restored by `luaD_rawrunprotected` after an error |
| `src/ldo.c` | A C++ exception that is not a Lua error and crosses `lua_pcall` becomes the runtime error "C++ exception" | Stock 5.1 compiled as C++ catches it with `catch (...)` and returns status -1 with no error object; LuaJIT reports such an exception the same way |

Compile definitions (`cmake/deps/Lua51.cmake`): `LUA_USE_POSIX`, `LUA_USE_DLOPEN`, and `LUA_USELONGLONG` so that `string.format`'s
`%d` and `%x` take 64-bit values on wasm32 too (`long` is 32-bit there). Consumers of the target see `ANOMALY_LUA51`.

Not changed (the scripts do not need it, see LUA51.md): `goto`, `\u{...}`, `table.move`, `math.log(x, base)`, `xpcall` with
arguments, `load` of a string, `coroutine.running()` in the main thread (LuaJIT returns the main coroutine and `true`, 5.1 `nil`),
yielding across `pcall`, metamethods and C calls.
