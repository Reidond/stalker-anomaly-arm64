# xrCPU_Pipe: CPU skinning / light-calc dispatch (mostly dead code, see M1-AUDIT 3.4).
# Source list: src/xrCPU_Pipe/xrCPU_Pipe.vcxproj. Owner: leaf-libs session. Opt-in: -DANOMALY_BUILD_XRCPU_PIPE=ON.
# On Apple the ttapi worker pool is Grand Central Dispatch (ttapi.cpp, XR_PLATFORM_APPLE branch); the engine binds this
# library through xrBind_PSGP() (extern "C"), which Engine.cpp has to call directly instead of loading xrCPU_Pipe.dll.
anomaly_module_begin(xrCPU_Pipe "CPU skinning pipe")

anomaly_vcxproj_sources(xrcpu_pipe_sources "${ANOMALY_SRC_DIR}/xrCPU_Pipe/xrCPU_Pipe.vcxproj")

anomaly_add_module(xrCPU_Pipe
    SOURCES ${xrcpu_pipe_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrCPU_Pipe"
    DEFINES XRCPU_PIPE_EXPORTS
)

# Headless check of the worker pool and the threaded skinning (macOS only), see tools/xrcpu_pipe_smoke/main.cpp
if(NOT ANOMALY_IOS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrcpu_pipe_smoke" "${PROJECT_BINARY_DIR}/tools/xrcpu_pipe_smoke")
endif()
