# xrXMLParser: TinyXML wrapper (CXml). Source list: src/xrXMLParser/xrXMLParser.vcxproj.
# Owner: leaf-libs session. Opt-in: -DANOMALY_BUILD_XRXMLPARSER=ON (or ANOMALY_BUILD_ALL_MODULES).
anomaly_module_begin(xrXMLParser "TinyXML wrapper")

anomaly_vcxproj_sources(xrxmlparser_sources "${ANOMALY_SRC_DIR}/xrXMLParser/xrXMLParser.vcxproj")

anomaly_add_module(xrXMLParser
    SOURCES ${xrxmlparser_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrXMLParser"
    DEFINES XRXMLPARSER_EXPORTS
    LINK xrCDB
)
