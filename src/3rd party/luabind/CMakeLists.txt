# luabind as used by Anomaly (heavily modified 0.7-era luabind), built against LuaJIT 2.1.
#
# luabind has no dependency on xrCore. Its allocator is already injectable through
# luabind::set_allocator(); the only external headers it needs are lua.hpp (LuaJIT) and the
# sm_boost shims in sdk/include.

# These mirror the two engine configurations (see xrEngine/stdafx.h): the release engine builds
# luabind without exceptions and without error checking, the debug engine with both.
option(ANOMALY_LUABIND_EXCEPTIONS "Build luabind with C++ exceptions (luabind::error) instead of error callbacks" OFF)
option(ANOMALY_LUABIND_ERROR_CHECKING "Build luabind with argument/overload error checking (engine DEBUG behaviour)" OFF)

# Headers include <sm_boost/...>, which lives in sdk/include next to the Windows-only SDK headers
# (ogg, vorbis, DirectX...). Expose just sm_boost so those do not shadow the pinned submodules.
set(luabind_sm_boost_dir "${CMAKE_BINARY_DIR}/generated/sm_boost_include")
file(MAKE_DIRECTORY "${luabind_sm_boost_dir}")
file(CREATE_LINK "${PROJECT_SOURCE_DIR}/sdk/include/sm_boost" "${luabind_sm_boost_dir}/sm_boost" SYMBOLIC)

add_library(luabind STATIC
    luabind/src/class.cpp
    luabind/src/class_info.cpp
    luabind/src/class_registry.cpp
    luabind/src/class_rep.cpp
    luabind/src/create_class.cpp
    luabind/src/error.cpp
    luabind/src/find_best_match.cpp
    luabind/src/function.cpp
    luabind/src/implicit_cast.cpp
    luabind/src/link_compatibility.cpp
    luabind/src/object.cpp
    luabind/src/object_rep.cpp
    luabind/src/open.cpp
    luabind/src/pcall.cpp
    luabind/src/ref.cpp
    luabind/src/scope.cpp
    luabind/src/stack_content_by_name.cpp
    luabind/src/weak_ref.cpp
    luabind/src/wrapper_base.cpp
    luabind_memory.cpp
)

# Headers are included as <luabind/...>, so the include root is the directory holding luabind/.
target_include_directories(luabind PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${luabind_sm_boost_dir}"
)
target_compile_definitions(luabind PRIVATE LUABIND_BUILDING)
target_link_libraries(luabind PUBLIC LuaJIT::luajit)

# These change the layout/behaviour of header-only templates, so every consumer must see them.
if(NOT ANOMALY_LUABIND_EXCEPTIONS)
    target_compile_definitions(luabind PUBLIC LUABIND_NO_EXCEPTIONS)
endif()
if(ANOMALY_LUABIND_ERROR_CHECKING)
    target_compile_definitions(luabind PUBLIC LUA_DEBUG)
endif()

# Third-party code: keep the build log readable.
target_compile_options(luabind PRIVATE -Wno-everything)
