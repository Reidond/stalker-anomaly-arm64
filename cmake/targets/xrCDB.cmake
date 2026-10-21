# xrCDB: the collision database (OPCODE) and the spatial DB. Source list: src/xrCDB/xrCDB.vcxproj.
# Owner: leaf-libs session. Opt-in: -DANOMALY_BUILD_XRCDB=ON (or ANOMALY_BUILD_ALL_MODULES).
anomaly_module_begin(xrCDB "collision database (OPCODE)")

anomaly_vcxproj_sources(xrcdb_sources "${ANOMALY_SRC_DIR}/xrCDB/xrCDB.vcxproj")

# SSE2NEON_PRECISE_MINMAX: the SSE ray/box slab test (xrCDB_ray.cpp, ISpatial_q_ray.cpp) relies on x86 min/max returning
# the second operand when one is NaN to filter 0 * inf; NEON fmin/fmax would propagate the NaN and drop the node, and
# every ray lying in a node's bounding plane would miss. PUBLIC, so the other users of these headers (the renderer's
# particle and detail code) get x86 semantics as well.
anomaly_add_module(xrCDB
    SOURCES ${xrcdb_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrCDB"
    DEFINES XRCDB_EXPORTS SSE2NEON_PRECISE_MINMAX=1
    LINK xrAPI
)

# Headless check of the queries (macOS only), see tools/xrcdb_smoke/main.cpp. ctest: xrcdb_smoke, xrcdb_smoke_mt_cdb.
if(NOT ANOMALY_IOS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrcdb_smoke" "${PROJECT_BINARY_DIR}/tools/xrcdb_smoke")
endif()
