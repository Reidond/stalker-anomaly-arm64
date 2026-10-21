# xrEngine smoke test (macOS): links xrCore + xrEngine, runs engine init through the Apple shell and shows the SDL3 window.
# Registered from cmake/targets/xrEngine.cmake, only when ANOMALY_BUILD_XRENGINE (or ALL_MODULES) is on.
#
# xrEngine links whichever other modules are enabled (-DANOMALY_BUILD_XRCDB=ON, ...). The symbols of a module that is not
# enabled come from stubs/stubs_<target>.cpp (compiled only while that target does not exist), so the tool links and runs
# while the other modules are still being ported. Turn a module on and its stub file drops out of the build. The symbols
# the stubs stand in for are listed per module in docs/ios-port/XRENGINE.md.
if(ANOMALY_IOS OR NOT TARGET xrEngine)
    return()
endif()

add_library(xrengine_smoke_layer STATIC layer_check.cpp layer_check.h)
target_link_libraries(xrengine_smoke_layer PUBLIC metal_d3d11 metal_d3d11_compat)

set(smoke_sources main.cpp)
file(GLOB smoke_stub_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs_*.cpp")
list(REMOVE_ITEM smoke_stub_sources "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs_engine_input.cpp")
foreach(stub IN LISTS smoke_stub_sources)
    get_filename_component(stub_name "${stub}" NAME_WE)
    string(REGEX REPLACE "^stubs_" "" stub_target "${stub_name}")
    if(NOT TARGET ${stub_target})
        list(APPEND smoke_sources "${stub}")
    endif()
endforeach()

# the files of the engine-input session, until its port (src/xrEngine/xr_input_sdl.cpp) is merged
if(NOT XRENGINE_INPUT_PORTED)
    list(APPEND smoke_sources "${CMAKE_CURRENT_SOURCE_DIR}/stubs/stubs_engine_input.cpp")
endif()

add_executable(xrengine_smoke ${smoke_sources})
target_include_directories(xrengine_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xrengine_smoke PRIVATE xrEngine anomaly_apple_shell xrengine_smoke_layer)
