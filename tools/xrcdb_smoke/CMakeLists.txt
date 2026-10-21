# Headless xrCDB check: collision model and spatial DB queries with known answers and a double precision reference,
# in both ray paths (SSE on sse2neon and scalar). Registered from cmake/targets/xrCDB.cmake when ANOMALY_BUILD_XRCDB is on.
add_executable(xrcdb_smoke main.cpp)
target_link_libraries(xrcdb_smoke PRIVATE Anomaly::xrCDB)

add_test(NAME xrcdb_smoke COMMAND xrcdb_smoke)
# the same checks with the models built on the engine's helper thread
add_test(NAME xrcdb_smoke_mt_cdb COMMAND xrcdb_smoke -mt_cdb)
