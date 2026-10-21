# Headless xrParticles check: the SSE noise and turbulence code (sse2neon) against a scalar reference.
# Registered from cmake/targets/xrParticles.cmake when ANOMALY_BUILD_XRPARTICLES is on.
add_executable(xrparticles_smoke main.cpp)
target_link_libraries(xrparticles_smoke PRIVATE Anomaly::xrParticles)

add_test(NAME xrparticles_smoke COMMAND xrparticles_smoke)
