# Shared plumbing for the engine module targets in cmake/targets/*.cmake (xrCDB, xrSound, xrGame, ...).
#
# Every module is a static library whose source list is read from the module's Visual Studio project, so it
# follows upstream merges. Each one is opt-in (ANOMALY_BUILD_<MODULE>, or ANOMALY_BUILD_ALL_MODULES) until the session
# that owns the module makes it compile. The root CMakeLists.txt includes every cmake/targets/*.cmake; a module
# session only edits its own file. docs/ios-port/XRCORE.md ("For module sessions") describes the workflow.

set(ANOMALY_SRC_DIR "${PROJECT_SOURCE_DIR}/src")

option(ANOMALY_BUILD_ALL_MODULES "Build every engine module target (the per-module ANOMALY_BUILD_* options are ignored)" OFF)

# MSVC-compatible dialect. The default is the audit's measured baseline (docs/ios-port/M1-AUDIT.md 7.4):
# -fms-extensions only. -fms-compatibility makes clang accept dependent-base lookup, template-parameter shadowing and
# forward-declared enums. Measured at P0 on the same 60 sampled TUs per module (XRCORE.md, "Dialect flags"): xrGame
# unique module error sites 541 -> 350 and total errors 2,680 -> 706, xrPhysics 28 -> 25, xrEngine and the renderer
# unchanged, but 34 luabind `type_info` sites appear in every script TU (so no xrGame TU is clean until luabind is
# patched), and the Microsoft-mode diagnostics it hides stay hidden. Apple's SDK headers break under it unless
# __GNUC__ is defined again (TargetConditionals.h, va_list), so turning it on defines __GNUC__ too. It does not
# define _MSC_VER. Default OFF; the xrgame-triage session decides about xrGame.
option(ANOMALY_MS_COMPAT "Compile engine modules with -fms-compatibility -fms-compatibility-version=19.30" OFF)

# ---------------------------------------------------------------------------------------------------------------
# Anomaly::EngineConfig: dialect flags, include paths, shim headers and warning policy shared by all engine modules.
# Linking it PUBLIC from a module hands the same configuration to everything that consumes the module.
# ---------------------------------------------------------------------------------------------------------------
add_library(anomaly_engine_config INTERFACE)
add_library(Anomaly::EngineConfig ALIAS anomaly_engine_config)

target_link_libraries(anomaly_engine_config INTERFACE
    Anomaly::xrCore         # -fdelayed-template-parsing, -fno-operator-names, -fno-strict-aliasing, src/ include roots
    sse2neon::sse2neon      # the forwarding <xmmintrin.h> & co. in src/Common/compat land here
    metal_d3d11_compat      # <d3d11.h>, <dxgi*.h>, <d3dcompiler.h>, <d3dx11.h> ... on top of Metal
)

# src/Common/compat first: it holds stand-ins for Windows and compiler headers the engine includes by name
# (<windows.h>, <malloc.h>, <dinput.h>, <d3d9types.h>, <DPlay/dplay8.h>, <xmmintrin.h>, ...), and it must win
# over both the Apple SDK and the real Windows SDK headers in sdk/include (which no Apple target puts on a path).
target_include_directories(anomaly_engine_config BEFORE INTERFACE "${ANOMALY_SRC_DIR}/Common/compat")
target_include_directories(anomaly_engine_config INTERFACE
    "${ANOMALY_SRC_DIR}"
    "${ANOMALY_SRC_DIR}/3rd party"
    "${ANOMALY_SRC_DIR}/Include"
)

target_compile_definitions(anomaly_engine_config INTERFACE
    # the dxsdk-style types are provided by Common/Platform.h, not by the Metal shim's own fallbacks
    MTL_D3D11_NO_WINTYPES
    dSINGLE                 # ODE single precision (xrPhysics, xrGame)
    $<$<NOT:$<BOOL:${ANOMALY_ENGINE_DEBUG}>>:NDEBUG>
)

set(anomaly_engine_dialect_flags
    -fms-extensions                 # `\` in #include paths, __declspec, __super, `i64` literal suffix
    # "-iquote src/xrCore": xrCore/_types.h and cpuid.h shadow Apple SDK headers if xrCore is on -I (M1-AUDIT 7.4)
    "SHELL:-iquote \"${ANOMALY_SRC_DIR}/xrCore\""
)
if(ANOMALY_MS_COMPAT)
    list(APPEND anomaly_engine_dialect_flags -fms-compatibility -fms-compatibility-version=19.30
        -D__GNUC__=4 -D__GNUC_MINOR__=2)
endif()
target_compile_options(anomaly_engine_config INTERFACE ${anomaly_engine_dialect_flags})

# Warning policy. The sources were written against MSVC /W3 with exceptions off; these are the diagnostics that
# carry no information for the port or flood the log. Real bugs MSVC accepted (uninitialized use, bad format
# strings, narrowing) stay visible.
target_compile_options(anomaly_engine_config INTERFACE
    -Wno-microsoft
    -Wno-unknown-pragmas -Wno-ignored-pragmas -Wno-pragma-pack -Wno-ignored-attributes -Wno-attributes
    -Wno-nonportable-include-path -Wno-deprecated-declarations -Wno-deprecated-register -Wno-register
    -Wno-writable-strings -Wno-c++11-narrowing -Wno-invalid-offsetof -Wno-switch -Wno-unused-value
    -Wno-address-of-temporary -Wno-dangling-else -Wno-inconsistent-missing-override -Wno-parentheses
    -Wno-logical-op-parentheses -Wno-bitwise-op-parentheses -Wno-tautological-compare -Wno-unused-function
    -Wno-unused-variable -Wno-unused-private-field -Wno-reorder-ctor -Wno-delete-non-abstract-non-virtual-dtor
    -Wno-undefined-var-template -Wno-extern-c-compat -Wno-sizeof-pointer-memaccess -Wno-dynamic-class-memaccess
    -Wno-class-memaccess -Wno-nontrivial-memaccess -Wno-unused-local-typedef -Wno-enum-compare
    -Wno-deprecated-non-prototype -Wno-vla-extension
)

# ---------------------------------------------------------------------------------------------------------------
# Module registration
# ---------------------------------------------------------------------------------------------------------------
define_property(GLOBAL PROPERTY ANOMALY_MODULE_TARGETS BRIEF_DOCS "engine module targets" FULL_DOCS "engine module targets")

# anomaly_module_begin(<MODULE_NAME> <description>)
#
# Declares the option ANOMALY_BUILD_<MODULE_NAME> (upper case) and returns from the including cmake/targets file
# when the module is not selected. Use it as the first line of a module file (a macro, so return() leaves the file).
macro(anomaly_module_begin module_name description)
    string(TOUPPER "${module_name}" _anomaly_module_upper)
    option(ANOMALY_BUILD_${_anomaly_module_upper} "Build ${module_name}: ${description}" OFF)
    if(NOT (ANOMALY_BUILD_${_anomaly_module_upper} OR ANOMALY_BUILD_ALL_MODULES))
        return()
    endif()
endmacro()

# anomaly_vcxproj_sources(<out_var> <vcxproj> [EXCLUDE <regex>...] [INCLUDE <regex>...])
#
# Collects the <ClCompile Include="..."> entries of a Visual Studio project as absolute paths. Entries marked
# <ExcludedFromBuild>true</ExcludedFromBuild> are skipped. EXCLUDE drops sources whose project-relative path (with `/`,
# lower-cased, so write the regexes in lower case) matches any of them; INCLUDE keeps only the matches. A listed file that is missing on
# disk is reported once per file (the project files are not always in sync with the tree) and skipped.
function(anomaly_vcxproj_sources out_var vcxproj)
    cmake_parse_arguments(ARG "" "" "EXCLUDE;INCLUDE" ${ARGN})
    get_filename_component(proj_dir "${vcxproj}" DIRECTORY)
    file(READ "${vcxproj}" content)
    string(REGEX MATCHALL "<ClCompile[ \t\r\n]+Include=\"[^\"]*\"[^>]*>" tags "${content}")
    set(result "")
    set(missing "")
    foreach(tag IN LISTS tags)
        string(REGEX REPLACE "^<ClCompile[ \t\r\n]+Include=\"([^\"]*)\".*$" "\\1" rel "${tag}")
        string(REGEX MATCH "/>$" self_closing "${tag}")
        if(NOT self_closing)
            # element with children: look for ExcludedFromBuild=true inside this element only
            string(FIND "${content}" "${tag}" start)
            string(SUBSTRING "${content}" ${start} -1 tail)
            string(FIND "${tail}" "</ClCompile>" end)
            string(SUBSTRING "${tail}" 0 ${end} body)
            if(body MATCHES "<ExcludedFromBuild[^>]*>[ \t\r\n]*true[ \t\r\n]*</ExcludedFromBuild>")
                continue()
            endif()
        endif()
        string(REPLACE "\\" "/" rel "${rel}")
        string(TOLOWER "${rel}" rel_lower)
        set(keep TRUE)
        foreach(re IN LISTS ARG_EXCLUDE)
            if(rel_lower MATCHES "${re}")
                set(keep FALSE)
            endif()
        endforeach()
        if(ARG_INCLUDE)
            set(keep FALSE)
            foreach(re IN LISTS ARG_INCLUDE)
                if(rel_lower MATCHES "${re}")
                    set(keep TRUE)
                endif()
            endforeach()
        endif()
        if(NOT keep)
            continue()
        endif()
        file(REAL_PATH "${rel}" abs BASE_DIRECTORY "${proj_dir}")
        if(NOT EXISTS "${abs}")
            list(APPEND missing "${rel}")
            continue()
        endif()
        list(APPEND result "${abs}")
    endforeach()
    list(REMOVE_DUPLICATES result)
    if(missing)
        list(LENGTH missing missing_count)
        message(STATUS "${vcxproj}: ${missing_count} listed source(s) missing on disk: ${missing}")
    endif()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# anomaly_add_module(<target>
#     SOURCES <file>...                 # usually from anomaly_vcxproj_sources()
#     [INCLUDE_DIRS <dir>...]           # PRIVATE include roots from the vcxproj (the module's own dir, ...); not
#                                       # src/xrCore, which is -iquote only (see EngineConfig)
#     [PUBLIC_INCLUDE_DIRS <dir>...]    # include roots the module's consumers need as well
#     [DEFINES <def>...]                # PUBLIC compile definitions
#     [PRIVATE_DEFINES <def>...]
#     [COMPILE_OPTIONS <opt>...]        # PRIVATE
#     [LINK <target>...])               # PUBLIC link dependencies; absent targets are skipped
#
# Creates a static library <target> with an Anomaly::<target> alias, wired to Anomaly::EngineConfig, and records it
# for the `anomaly_modules` aggregate target. Link entries may name targets of modules that are switched off.
function(anomaly_add_module target)
    cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDE_DIRS;PUBLIC_INCLUDE_DIRS;DEFINES;PRIVATE_DEFINES;COMPILE_OPTIONS;LINK" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "anomaly_add_module(${target}): no sources")
    endif()
    list(LENGTH ARG_SOURCES source_count)
    message(STATUS "Module ${target}: ${source_count} sources")

    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(Anomaly::${target} ALIAS ${target})
    target_link_libraries(${target} PUBLIC Anomaly::EngineConfig)
    foreach(dep IN LISTS ARG_LINK)
        target_link_libraries(${target} PUBLIC "$<TARGET_NAME_IF_EXISTS:${dep}>")
    endforeach()
    if(ARG_INCLUDE_DIRS)
        target_include_directories(${target} PRIVATE ${ARG_INCLUDE_DIRS})
        # Quote includes ("stdafx.h" from pch_script.h, ...) must find the module's own headers before Anomaly::EngineConfig's
        # `-iquote src/xrCore`, which also holds a stdafx.h. -iquote dirs are searched ahead of every -I dir, so the
        # module's include roots are repeated as -iquote in front of it (target options precede interface options).
        foreach(dir IN LISTS ARG_INCLUDE_DIRS)
            target_compile_options(${target} PRIVATE "SHELL:-iquote \"${dir}\"")
        endforeach()
    endif()
    if(ARG_PUBLIC_INCLUDE_DIRS)
        target_include_directories(${target} PUBLIC ${ARG_PUBLIC_INCLUDE_DIRS})
    endif()
    if(ARG_DEFINES)
        target_compile_definitions(${target} PUBLIC ${ARG_DEFINES})
    endif()
    if(ARG_PRIVATE_DEFINES)
        target_compile_definitions(${target} PRIVATE ${ARG_PRIVATE_DEFINES})
    endif()
    if(ARG_COMPILE_OPTIONS)
        target_compile_options(${target} PRIVATE ${ARG_COMPILE_OPTIONS})
    endif()
    set_property(GLOBAL APPEND PROPERTY ANOMALY_MODULE_TARGETS ${target})
endfunction()

# Builds every enabled module: `cmake --build <dir> --target anomaly_modules -- -k 0`.
function(anomaly_finalize_modules)
    get_property(module_targets GLOBAL PROPERTY ANOMALY_MODULE_TARGETS)
    if(module_targets)
        add_custom_target(anomaly_modules DEPENDS ${module_targets})
        message(STATUS "Engine modules enabled: ${module_targets}")
    endif()
endfunction()
