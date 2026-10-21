# xrPhysics: ODE-based physics shells (also compiles xrServerEntities/PHNetState.cpp).
# Source list: src/xrPhysics/xrPhysics.vcxproj. Owner: physics session. Opt-in: -DANOMALY_BUILD_XRPHYSICS=ON.
# Uses the Anomaly-patched ODE (src/3rd party/ode, target `ode`).
anomaly_module_begin(xrPhysics "ODE physics shells")

anomaly_vcxproj_sources(xrphysics_sources "${ANOMALY_SRC_DIR}/xrPhysics/xrPhysics.vcxproj")

anomaly_add_module(xrPhysics
    SOURCES ${xrphysics_sources}
    INCLUDE_DIRS
        "${ANOMALY_SRC_DIR}/xrPhysics"
        "${ANOMALY_SRC_DIR}/3rd party/ode"      # <ode/src/...> internals
    DEFINES XRPHYSICS_EXPORTS
    # no fused multiply-add: the arithmetic of the Windows build (no FMA) is what the contact and solver tolerances were
    # tuned on. xrCore hands the same flag to every module (PUBLIC); it is repeated here so xrPhysics does not depend on
    # that. Measured in docs/ios-port/XRPHYSICS.md: stable either way, but not bit-compatible with fusing.
    COMPILE_OPTIONS -ffp-contract=off
    LINK ode xrCDB xrAPI xrSound
)

# Headless smoke test: world, shells and characters on a static mesh through xrPhysics's own API, against the real
# xrCDB (enable it too: -DANOMALY_BUILD_XRCDB=ON). A console tool, so not for iOS.
if(TARGET xrCDB AND NOT ANOMALY_IOS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrphysics_smoke" "${CMAKE_BINARY_DIR}/tools/xrphysics_smoke")
endif()
