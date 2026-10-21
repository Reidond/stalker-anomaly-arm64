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
    LINK ode xrCDB xrAPI xrSound
)
