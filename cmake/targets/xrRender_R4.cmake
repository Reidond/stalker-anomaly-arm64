# xrRender_R4: the DX11 renderer. One project compiles the shared layer (Layers/xrRender), the DX10 layer
# (Layers/xrRenderDX10), the R4 layer (Layers/xrRenderPC_R4) and two xrEngine Lua files (227 .cpp), so one target
# covers the renderer layers. Source list: src/Layers/xrRenderPC_R4/xrRender_R4.vcxproj. Owner: renderer-r4 session.
# The D3D11/DXGI/D3DCompiler/D3DX11 headers come from the Metal shim (metal_d3d11_compat via Anomaly::EngineConfig).
# Opt-in: -DANOMALY_BUILD_XRRENDER_R4=ON (or ANOMALY_BUILD_ALL_MODULES).
anomaly_module_begin(xrRender_R4 "R4 (DX11) renderer layers")

anomaly_vcxproj_sources(xrrender_r4_sources "${ANOMALY_SRC_DIR}/Layers/xrRenderPC_R4/xrRender_R4.vcxproj")

anomaly_add_module(xrRender_R4
    SOURCES ${xrrender_r4_sources}
    INCLUDE_DIRS
        "${ANOMALY_SRC_DIR}/Layers/xrRenderPC_R4"
        "${ANOMALY_SRC_DIR}/3rd party/imgui"
    DEFINES USE_DX11 XRRENDER_R4_EXPORTS
    LINK xrAPI xrCPU_Pipe luabind LuaJIT::luajit
)
