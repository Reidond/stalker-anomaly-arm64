# xrXMLParser: TinyXML wrapper (CXml). Source list: src/xrXMLParser/xrXMLParser.vcxproj.
# Owner: leaf-libs session. Opt-in: -DANOMALY_BUILD_XRXMLPARSER=ON (or ANOMALY_BUILD_ALL_MODULES).
# CXml::Load calls XMLLuaCallback(CXml&, LPCSTR), defined in xrGame/ScriptXMLInit.cpp: the final link needs it.
anomaly_module_begin(xrXMLParser "TinyXML wrapper")

anomaly_vcxproj_sources(xrxmlparser_sources "${ANOMALY_SRC_DIR}/xrXMLParser/xrXMLParser.vcxproj")

anomaly_add_module(xrXMLParser
    SOURCES ${xrxmlparser_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrXMLParser"
    DEFINES XRXMLPARSER_EXPORTS
    LINK xrCDB
)

# Headless check against the real game archives (macOS only), see tools/xrxml_smoke/main.cpp
if(NOT ANOMALY_IOS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrxml_smoke" "${PROJECT_BINARY_DIR}/tools/xrxml_smoke")
endif()
