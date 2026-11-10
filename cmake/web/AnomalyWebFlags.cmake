# Compiler and linker settings of the WebAssembly build (wasm32, Emscripten, pthreads, SIMD). Included from
# cmake/AnomalyPlatform.cmake when ANOMALY_WEB is on, so they reach every target defined afterwards. docs/web-port/WASM-BUILD.md
# explains each choice.

# --- code generation ------------------------------------------------------------------------------------------------------
#  -pthread          threads (shared memory); every object must be built with it
#  -msimd128         wasm SIMD
#  -msse4.2          Emscripten's <xmmintrin.h> ... <nmmintrin.h> emulate SSE up to 4.2 on wasm SIMD
#  -fwasm-exceptions native wasm exception handling (no JS glue); the engine and luabind throw
#  -frtti            on (it is the clang default; spelled out because the engine uses dynamic_cast and typeid)
set(ANOMALY_WEB_ARCH_FLAGS -pthread -msimd128 -msse4.2 -fwasm-exceptions -frtti)
add_compile_options(${ANOMALY_WEB_ARCH_FLAGS})

# --- memory and threads ---------------------------------------------------------------------------------------------------
set(ANOMALY_WEB_MAIN_STACK_MB 8 CACHE STRING "Main thread stack, MB (-sSTACK_SIZE)")
set(ANOMALY_WEB_THREAD_STACK_MB 2 CACHE STRING "Default pthread stack, MB; the engine puts string4096 buffers on the stack, 2 MB is the Windows and Apple floor")
set(ANOMALY_WEB_POOL_THREADS 12 CACHE STRING "Worker threads created before main() (-sPTHREAD_POOL_SIZE): the engine's own threads plus the xr_task_group pool")
set(ANOMALY_WEB_INITIAL_MEMORY_MB 256 CACHE STRING "Initial heap, MB (-sINITIAL_MEMORY); it grows on demand")
set(ANOMALY_WEB_MAX_MEMORY_MB 4096 CACHE STRING "Heap ceiling, MB (-sMAXIMUM_MEMORY); 4096 is the most wasm32 allows")
# mimalloc: 16-byte aligned blocks, which xrMemory and the SSE code assume (dlmalloc and emmalloc give 8 on wasm32: xrcore_smoke
# fails its alignment check), and no global lock. It has no mallinfo(); PlatformWeb.cpp copes.
set(ANOMALY_WEB_MALLOC "mimalloc" CACHE STRING "Emscripten allocator (-sMALLOC): mimalloc, dlmalloc, emmalloc")
set_property(CACHE ANOMALY_WEB_MALLOC PROPERTY STRINGS mimalloc dlmalloc emmalloc)

math(EXPR anomaly_web_main_stack "${ANOMALY_WEB_MAIN_STACK_MB} * 1024 * 1024")
math(EXPR anomaly_web_thread_stack "${ANOMALY_WEB_THREAD_STACK_MB} * 1024 * 1024")
math(EXPR anomaly_web_initial_memory "${ANOMALY_WEB_INITIAL_MEMORY_MB} * 1024 * 1024")
math(EXPR anomaly_web_max_memory "${ANOMALY_WEB_MAX_MEMORY_MB} * 1024 * 1024")

set(ANOMALY_WEB_LINK_FLAGS
    ${ANOMALY_WEB_ARCH_FLAGS}
    -sSTACK_SIZE=${anomaly_web_main_stack}
    -sDEFAULT_PTHREAD_STACK_SIZE=${anomaly_web_thread_stack}
    -sPTHREAD_POOL_SIZE=${ANOMALY_WEB_POOL_THREADS}
    -sINITIAL_MEMORY=${anomaly_web_initial_memory}
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=${anomaly_web_max_memory}
    -sMALLOC=${ANOMALY_WEB_MALLOC}
    -sEXIT_RUNTIME=1
)

# --- the Node test runner -------------------------------------------------------------------------------------------------
# The smoke tools run under Node (CMAKE_CROSSCOMPILING_EMULATOR, found by the Emscripten toolchain file: the `node` on PATH).
# NODERAWFS maps the C library's file calls straight to Node's `fs`, so the tools see the real disk (the game data, their run
# directory) without a preload step. The browser build replaces this with OPFS and chunked data (docs/web-port/PLAN.md).
option(ANOMALY_WEB_NODE "Link the executables for Node (NODERAWFS) instead of the browser" ON)
if(ANOMALY_WEB_NODE)
    list(APPEND ANOMALY_WEB_LINK_FLAGS -sENVIRONMENT=node -sNODERAWFS=1
         "SHELL:--pre-js ${CMAKE_CURRENT_LIST_DIR}/node_argv.js")  # process.argv -> GetCommandLine()
else()
    list(APPEND ANOMALY_WEB_LINK_FLAGS -sENVIRONMENT=web,worker)
endif()

option(ANOMALY_WEB_LTO "Link-time optimisation (-flto) for the web build" OFF)
if(ANOMALY_WEB_LTO)
    add_compile_options(-flto)
    list(APPEND ANOMALY_WEB_LINK_FLAGS -flto)
endif()

add_link_options(${ANOMALY_WEB_LINK_FLAGS})
