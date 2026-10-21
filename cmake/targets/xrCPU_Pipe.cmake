# xrCPU_Pipe: CPU skinning / light-calc dispatch (mostly dead code, see M1-AUDIT 3.4).
# Source list: src/xrCPU_Pipe/xrCPU_Pipe.vcxproj. Owner: leaf-libs session. Opt-in: -DANOMALY_BUILD_XRCPU_PIPE=ON.
anomaly_module_begin(xrCPU_Pipe "CPU skinning pipe")

anomaly_vcxproj_sources(xrcpu_pipe_sources "${ANOMALY_SRC_DIR}/xrCPU_Pipe/xrCPU_Pipe.vcxproj")

anomaly_add_module(xrCPU_Pipe
    SOURCES ${xrcpu_pipe_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrCPU_Pipe"
    DEFINES XRCPU_PIPE_EXPORTS
)
