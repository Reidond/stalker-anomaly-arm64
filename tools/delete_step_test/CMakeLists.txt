# Host test of the script runner's `delete` step (src/Platform/Apple/DeleteStep.h is header only).
if(NOT ANOMALY_IOS)
    add_executable(delete_step_test main.cpp)
    target_include_directories(delete_step_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(delete_step_test PRIVATE cxx_std_17)
    add_test(NAME delete_step_test COMMAND delete_step_test)
endif()
