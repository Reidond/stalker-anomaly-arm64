# xrAPI: the engine-facing render interface globals (Render, RenderFactory, DU, UIRender, DRender).
# Source list: src/Layers/xrAPI/xrAPI.vcxproj. Owner: renderer-r4 session. Opt-in: -DANOMALY_BUILD_XRAPI=ON.
anomaly_module_begin(xrAPI "render interface globals")

anomaly_vcxproj_sources(xrapi_sources "${ANOMALY_SRC_DIR}/Layers/xrAPI/xrAPI.vcxproj")

anomaly_add_module(xrAPI
    SOURCES ${xrapi_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/Layers/xrAPI"
    DEFINES XRAPI_EXPORTS
)
