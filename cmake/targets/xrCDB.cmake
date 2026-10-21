# xrCDB: collision database (OPCODE) and the spatial DB. Source list: src/xrCDB/xrCDB.vcxproj.
# Owner: leaf-libs session. Opt-in: -DANOMALY_BUILD_XRCDB=ON (or ANOMALY_BUILD_ALL_MODULES).
anomaly_module_begin(xrCDB "collision database (OPCODE)")

anomaly_vcxproj_sources(xrcdb_sources "${ANOMALY_SRC_DIR}/xrCDB/xrCDB.vcxproj")

anomaly_add_module(xrCDB
    SOURCES ${xrcdb_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrCDB"
    DEFINES XRCDB_EXPORTS
    LINK xrAPI
)
