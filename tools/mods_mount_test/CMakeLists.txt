# Host test of the mod list and of the engine's mounting of mods (docs/ios-port/MOD-MANAGER.md, "Mounting"): generated mods, a stock-like
# archive and a real xrCore file system. One process per scenario, because xrCore can only be initialized once.
#   mods_mount_test <scenario>           one scenario (names below); exit status 0 when every check passes
#   mods_mount_test perf <files> <mb>    mounts one generated mod of that many files and megabytes and prints what it cost
if(NOT ANOMALY_IOS)
    set(apple_dir "${PROJECT_SOURCE_DIR}/src/Platform/Apple")
    add_executable(mods_mount_test main.cpp "${apple_dir}/Mods.mm" "${apple_dir}/Overlay.cpp")
    set_source_files_properties("${apple_dir}/Mods.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(mods_mount_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(mods_mount_test PRIVATE cxx_std_17)
    target_link_libraries(mods_mount_test PRIVATE Anomaly::xrCore "-framework Foundation")
    foreach(scenario priority priority_reversed manual_wins odd_paths disable_a disable_b case write_isolation mods_off bad_lists bad_list_fs)
        add_test(NAME mods_mount_${scenario} COMMAND mods_mount_test ${scenario})
    endforeach()
endif()
