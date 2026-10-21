# xrCore: the engine's foundation library (file system, memory, math, logging, ini files, threads).
# Static library for macOS/iOS. Windows keeps building xrCore.vcxproj from engine-vs2022.sln.

set(XR_SRC_DIR "${PROJECT_SOURCE_DIR}/src")

set(XRCORE_SOURCES
    _compressed_normal.cpp
    _math.cpp
    _sphere.cpp
    _std_extensions.cpp
    clsid.cpp
    cpuid.cpp
    crc32.cpp
    dump_string.cpp
    file_stream_reader.cpp
    FileSystem.cpp
    FS.cpp
    FTimer.cpp
    LocatorAPI.cpp
    LocatorAPI_auth.cpp
    LocatorAPI_defs.cpp
    Lock.cpp
    log.cpp
    LzHuf.cpp
    lzo_compressor.cpp
    memory_allocation_stats.cpp
    memory_monitor.cpp
    memory_usage.cpp
    mezz_stringbuffer.cpp
    Model.cpp
    NET_utils.cpp
    os_clipboard.cpp
    ppmd_compressor.cpp
    rt_compressor.cpp
    rt_compressor9.cpp
    rt_lzo1x_1.cpp
    rt_lzo1x_9x.cpp
    rt_lzo1x_d1.cpp
    rt_lzo1x_d2.cpp
    rt_lzo1x_d3.cpp
    rt_lzo_init.cpp
    ScopeLock.cpp
    ShaderSourceCRC.cpp
    stdafx.cpp
    stream_reader.cpp
    string_concatenations.cpp
    xr_shared.cpp
    xr_trims.cpp
    Xr_ini.cpp
    xrCore.cpp
    xrMemory.cpp
    xrMemory_align.cpp
    xrMemory_debug.cpp
    xrMemory_POOL.cpp
    xrMemory_pso_Copy.cpp
    xrMemory_pso_Fill.cpp
    xrMemory_pso_Fill32.cpp
    xrsharedmem.cpp
    xrstring.cpp
    xrSyncronize.cpp

    # Apple replacements for the Win32-only parts (see src/Common/Platform.h)
    Platform/PlatformApple.cpp
    Platform/xrDebug_posix.cpp
    Platform/xrMemory_apple.cpp
    Platform/xrCore_defaults_apple.cpp
)

add_library(xrCore STATIC ${XRCORE_SOURCES})
add_library(Anomaly::xrCore ALIAS xrCore)

# 'src' makes "Common/Platform.h" resolve; "3rd party" has fastdelegate and robin_hood. Do NOT add
# src/xrCore as an include directory: it has _types.h, _math.h, ... which would shadow Apple SDK headers
# such as <_types.h>. Dependents include xrCore headers as "../xrCore/xxx.h".
target_include_directories(xrCore PUBLIC
    "${XR_SRC_DIR}"
    "${XR_SRC_DIR}/3rd party"
    "${XR_SRC_DIR}/Include"
)

target_compile_definitions(xrCore PUBLIC
    PURE_ALLOC          # same as the Windows build: x-ray's own allocator front end
    XRCORE_EXPORTS      # XRCORE_API stays empty; this is a static library
)

# The shipped Release configuration leaves DEBUG undefined (MASTER_GOLD); match it by default.
option(ANOMALY_ENGINE_DEBUG "Define DEBUG for the engine (VERIFY, memory debugging, extra logging)" OFF)
if(ANOMALY_ENGINE_DEBUG)
    target_compile_definitions(xrCore PUBLIC DEBUG _DEBUG)
endif()

target_compile_options(xrCore PUBLIC
    -fno-strict-aliasing            # the engine type-puns through pointers all over the place
    -fno-delete-null-pointer-checks
    -fdelayed-template-parsing      # MSVC parses template bodies at instantiation; the headers rely on that
    -fno-operator-names             # `or`, `and`, `not` are used as identifiers (Flags::or)
    -ffp-contract=off               # keep float results identical to the MSVC build where possible
    -Wno-unknown-pragmas            # #pragma warning / hdrstop / comment
    -Wno-register                   # LZO and PPMd use the register keyword
    -Wno-ignored-attributes
    -Wno-microsoft-include
)
target_compile_options(xrCore PRIVATE
    -Wno-deprecated-declarations
    -Wno-deprecated-register
    -Wno-writable-strings
    -Wno-c++11-narrowing
    -Wno-invalid-offsetof
    -Wno-switch
    -Wno-return-type
    -Wno-pragma-pack
    -Wno-unused-value
    -Wno-address-of-temporary
    -Wno-dangling-else
)

find_package(Threads REQUIRED)
target_link_libraries(xrCore PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(xrCore PUBLIC "-framework Foundation" "-framework CoreFoundation")
endif()
