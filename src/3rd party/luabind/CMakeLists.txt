# luabind as used by Anomaly (heavily modified 0.7-era luabind), built against LuaJIT 2.1.
#
# luabind has no dependency on xrCore. Its allocator is already injectable through
# luabind::set_allocator(); the only external headers it needs are lua.hpp (LuaJIT) and the
# sm_boost shims in sdk/include.

# The engine builds luabind with LUABIND_NO_EXCEPTIONS (errors go to luabind::set_error_callback) and,
# through config.hpp, without argument/overload error checking. The exceptions mode is kept compilable
# for platforms where C++ exceptions must not cross Lua frames (LuaJIT is built without unwind tables
# on iOS); luabind's error-checking mode (LUA_DEBUG) is bit-rotted upstream and is not supported here.
option(ANOMALY_LUABIND_EXCEPTIONS "Build luabind with C++ exceptions (luabind::error) instead of error callbacks" OFF)

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
target_include_directories(luabind SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${luabind_sm_boost_dir}"
)
target_compile_definitions(luabind PRIVATE LUABIND_BUILDING)
target_link_libraries(luabind PUBLIC LuaJIT::luajit)

# This changes the behaviour of header-only templates, so every consumer must see it.
if(NOT ANOMALY_LUABIND_EXCEPTIONS)
    target_compile_definitions(luabind PUBLIC LUABIND_NO_EXCEPTIONS)
endif()

# Third-party code: keep the build log readable.
target_compile_options(luabind PRIVATE -Wno-everything)
