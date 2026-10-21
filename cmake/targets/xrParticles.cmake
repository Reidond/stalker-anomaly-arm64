# xrParticles: particle system API. Source list: src/xrParticles/xrParticles.vcxproj.
# Owner: leaf-libs session. SSE intrinsics (noise.cpp, particle_actions_collection.cpp) resolve to sse2neon through
# the forwarding <xmmintrin.h> in src/Common/compat. Opt-in: -DANOMALY_BUILD_XRPARTICLES=ON.
anomaly_module_begin(xrParticles "particle system")

anomaly_vcxproj_sources(xrparticles_sources "${ANOMALY_SRC_DIR}/xrParticles/xrParticles.vcxproj")

# SSE2NEON_PRECISE_MINMAX: x86 min/max NaN semantics for anything built on these intrinsics (see xrCDB.cmake)
anomaly_add_module(xrParticles
    SOURCES ${xrparticles_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrParticles"
    DEFINES XR_PARTICLES_EXPORTS SSE2NEON_PRECISE_MINMAX=1
    LINK xrCPU_Pipe
)

# Headless check of the SSE code against a scalar reference (macOS only), see tools/xrparticles_smoke/main.cpp
if(NOT ANOMALY_IOS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrparticles_smoke" "${PROJECT_BINARY_DIR}/tools/xrparticles_smoke")
endif()
