# X-Ray's modified ODE (the physics core of xrPhysics), static library.
#
# ODE has no dependency on xrCore: it allocates through its own dSetAllocHandler() hooks and
# reports errors through dSetErrorHandler(). The Windows project is
# contrib/msvc7/ode_default/default.vcxproj; the source list below mirrors it.

set(ode_src "${CMAKE_CURRENT_SOURCE_DIR}/ode/src")

add_library(ode STATIC
    "${ode_src}/array.cpp"
    "${ode_src}/Bounder33.cpp"
    "${ode_src}/collision_kernel.cpp"
    "${ode_src}/collision_quadtreespace.cpp"
    "${ode_src}/collision_space.cpp"
    "${ode_src}/collision_std.cpp"
    "${ode_src}/collision_transform.cpp"
    "${ode_src}/collision_util.cpp"
    "${ode_src}/error.cpp"
    "${ode_src}/export-dif.cpp"
    "${ode_src}/fastdot.c"
    "${ode_src}/fastldlt.c"
    "${ode_src}/fastlsolve.c"
    "${ode_src}/fastltsolve.c"
    "${ode_src}/joint.cpp"
    "${ode_src}/lcp.cpp"
    "${ode_src}/mass.cpp"
    "${ode_src}/mat.cpp"
    "${ode_src}/matrix.cpp"
    "${ode_src}/memory.cpp"
    "${ode_src}/misc.cpp"
    "${ode_src}/obstack.cpp"
    "${ode_src}/ode.cpp"
    "${ode_src}/odemath.cpp"
    "${ode_src}/quickstep.cpp"
    "${ode_src}/rotation.cpp"
    "${ode_src}/step.cpp"
    "${ode_src}/stepfast.cpp"
    "${ode_src}/StepJointInternal.cpp"
    "${ode_src}/testing.cpp"
    "${ode_src}/timer.cpp"
    "${ode_src}/util.cpp"
    contrib/msvc7/ode_default/de_padf_integration.cpp
)

# xrPhysics includes the headers as "ode/..." from this directory and by relative path.
target_include_directories(ode SYSTEM PUBLIC include)
# Single precision, no ODE-internal debug checks (as in the Windows project).
target_compile_definitions(ode PUBLIC dSINGLE dNODEBUG)

# Clang on arm64 fuses a*b+c into FMA by default. That changes the single-precision rounding of the
# LCP solver enough that dWorldStep()/dWorldStepFast1() report "LCP internal error" and diverge on a
# plain box-on-plane case (tools/ode_smoke); x86-64 and arm64 with contraction off both pass.
# The same applies to any code that integrates or solves in float, e.g. xrPhysics.
target_compile_options(ode PRIVATE -ffp-contract=off)

# Third-party code: keep the build log readable.
target_compile_options(ode PRIVATE -Wno-everything)
