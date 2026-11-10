# The WebAssembly configuration of the project: xrCore and the headless engine libraries that build for wasm32, with their
# smoke tools run under Node. Included from the root CMakeLists.txt when ANOMALY_WEB is on (preset web-wasm32); the rest of the
# root file (Apple platform layer, SDL, OpenAL, Lua, Metal D3D11 shim, the app) is Apple-only and is not reached.
# Status and what the full platform split needs: docs/web-port/WASM-BUILD.md.

include(AnomalyModules)

# Third-party code the engine core needs. ODE has no dependency on xrCore.
add_subdirectory("src/3rd party/ode")
add_subdirectory(src/xrCore)

# The modules that build for wasm so far, included by name (not globbed like the root file does: launcher.cmake and some others
# enable Swift or look for SDL regardless of their option). Each is switched on here with a normal variable, which wins over
# the option() the module file declares.
foreach(module xrAPI xrCDB xrCPU_Pipe xrParticles xrPhysics)
    string(TOUPPER "${module}" module_upper)
    set(ANOMALY_BUILD_${module_upper} ON)
    include("${PROJECT_SOURCE_DIR}/cmake/targets/${module}.cmake")
endforeach()
anomaly_finalize_modules()

# Host-side smoke tests (ctest runs each under Node).
add_subdirectory(tools/ode_smoke)
add_subdirectory(tools/xrcore_smoke)
