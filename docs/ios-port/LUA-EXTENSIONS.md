# lua_extensions on LuaJIT 2.1

`src/3rd party/lua-extensions` is what the script engine adds to Lua: `CScriptStorage::reinit` calls `luaopen_lua_extensions(L, isDebug)`
right after the standard libraries. The Apple port builds it as the `lua_extensions` module
(`-DANOMALY_BUILD_LUA_EXTENSIONS=ON`, `cmake/targets/lua_extensions.cmake`; the xrGame build links it) against **LuaJIT 2.1** from
`external/LuaJIT`. The Windows build compiles it against the bundled **LuaJIT 2.0.4** (`src/3rd party/luajit-2`). Both compile; the
differences below are the ones that matter.

`lua_ext_smoke` (`tools/lua_ext_smoke`, a ctest on macOS) loads the module the way `CScriptStorage` does and checks everything in
the table. The iOS build only checks that the library compiles.

## What Anomaly 1.5.3 uses

Found by unpacking `db/configs/scripts.db0` (414 scripts) and grepping:

| Library | Used by the stock scripts | On LuaJIT 2.1 |
|---|---|---|
| `marshal.encode/decode` | `alife_storage_manager` (the `.scoc` custom save data), `gamemode_ironman`, `ui_load_dialog` | works; format unchanged, see below |
| `lfs.mkdir/dir/attributes` | `alife_storage_manager` (`lfs.mkdir` of the saves folder), `lua_ext.script` (recursive directory walk) | works; needs the virtual-path hook |
| `require("lua_extensions")`, `require("lfs")` | `lua_ext.script` | works (`luaL_register` fills `package.loaded`) |
| `table.size` | `utils_data` | works |
| `string.trim/trim_l/trim_r/trim_w`, `table.keys/values/random` | addons | works |
| `math.random`, `math.randomseed` | 660 call sites | LuaJIT's own (see below) |
| `bit`, `socket`, `mime`, `lpack` | none | `socket.core` only with `-lua_debug` (LuaPanda); `lpack` (`LibLpack.c`), `keyboard.cpp`, `Logger.cpp`, `LibStr/LibMath/LibTab` are not in the Visual Studio project, so not built on either platform |

## Behaviour that differs from the Windows build

1. **Paths.** The engine's paths are virtual: backslash separators and roots such as `getFS():update_path('$game_saves$', '')`
   (`\users\...`, see XRCORE.md "Path model"). The Windows CRT takes them as they are; POSIX does not. `lfs.*` and the wrappers
   around `io.open`, `io.lines`, `loadfile` and `dofile` (`os.remove/rename/execute` and `io.popen` are removed by the engine anyway) now
   pass every path argument through `xr_native_path` (`lua_ext_set_native_path_hook`, installed by `CScriptStorage::reinit`;
   `lfs.c` and `script_additional_libs.cpp`, Apple only). Without the hook only the engine's `.scoc` writer would fail: it uses
   `io.open(update_path(...))`. `lua_ext.script` builds `node .. "\\" .. file` itself, which `xr_native_path` also handles.
2. **`marshal` data.** The encoder writes little-endian `uint32` lengths and `double` numbers on both platforms, so a `.scoc` file is
   readable on either. A Lua *function* inside the data is dumped as bytecode, and LuaJIT 2.0.4 and 2.1 (and the GC64 arm64 variant) use
   different bytecode formats: such a value does not load across platforms. The stock data contains none (tables of numbers,
   strings and booleans; the round trip with a function inside works within one build).
3. **`luaL_reg`.** The 2.0.4 `lauxlib.h` keeps the Lua 5.0 aliases (`luaL_reg`, `luaL_getn`, `lua_ref`); 2.1 dropped them. `lfs.c`,
   `lmarshal.c` and `base.cpp` use `luaL_reg`: the module defines `luaL_reg=luaL_Reg` (CMake). `luaL_getn/lua_ref` are not used.
4. **`luaL_pushmodule`.** luasocket carries its own for LuaJIT 2.0, which 2.1 exports itself: a duplicate symbol at link time. It is
   `static` now (`luasocket.c`).
5. **luasocket** uses `usocket.c` (BSD sockets, already in the tree) instead of `wsocket.c`.
6. **`bit` and `math` additions** (`bit_tobit` that returns a binary string, a Mersenne Twister `math.random`) are compiled only under
   `USE_LUAJIT_ONE`, which the vcxproj undefines: both platforms use LuaJIT's own `bit` and `math.random`. LuaJIT 2.1's PRNG is not
   guaranteed to give the 2.0.4 sequence for a seed; nothing in the stock scripts relies on that. `_itoa` (used by that dead code) has a
   local definition on Apple.
7. **`std::uniform_real/uniform_int`** (removed from C++17) became `uniform_real_distribution/uniform_int_distribution` (same behaviour).
8. **GC64.** LuaJIT 2.1 on arm64 has 64-bit GC references, so the 2 GB Lua heap limit of the Windows x64 build does not apply.
   Custom allocators (`USE_GSC_MEM_ALLOC`, off) would no longer need to return low addresses.

## Existing issues left alone

- `string.trim_l("")`/`trim_r` build `end = &front[size - 1]` before checking `size` (one byte before the string for an empty
  string); harmless in practice, the result is correct.
- `lmarshal.c` reads numbers and lengths through unaligned pointer casts; arm64 handles them.
- `luapanda.cpp` (the LuaPanda debugger glue) compiles unchanged; its Windows-only `GetProcAddress` branch is behind `_WIN32`.
- The Lua Studio and mailslot script debuggers in `xrServerEntities` need `USE_DEBUGGER`, which no configuration defines;
  `lua_studio.cpp` is empty on Apple (the SDK and its DLL are Windows only) and `mslotutils.h` has no-op mailslot helpers.
