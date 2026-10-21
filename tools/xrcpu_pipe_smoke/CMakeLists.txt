# Headless xrCPU_Pipe check: the GCD worker pool (ttapi) and the threaded skinning built on it.
# Registered from cmake/targets/xrCPU_Pipe.cmake when ANOMALY_BUILD_XRCPU_PIPE is on.
add_executable(xrcpu_pipe_smoke main.cpp)
target_link_libraries(xrcpu_pipe_smoke PRIVATE Anomaly::xrCPU_Pipe)

add_test(NAME xrcpu_pipe_smoke COMMAND xrcpu_pipe_smoke)
