# xrNetServer: network layer (NET_Packet queues, multipacket framing/compression, IPureClient/IPureServer).
# Source list: src/xrNetServer/xrNetServer.vcxproj.
# Owner: netserver session. DirectPlay does not exist on Apple, so NET_Client.cpp / NET_Server.cpp swap the transport
# for an in-process stub behind XR_NET_STUB_TRANSPORT (set by NET_Shared.h on Apple); see docs/ios-port/NETSERVER.md.
# Opt-in: -DANOMALY_BUILD_XRNETSERVER=ON (or ANOMALY_BUILD_ALL_MODULES).
anomaly_module_begin(xrNetServer "network server/client")

anomaly_vcxproj_sources(xrnetserver_sources "${ANOMALY_SRC_DIR}/xrNetServer/xrNetServer.vcxproj")

anomaly_add_module(xrNetServer
    SOURCES ${xrnetserver_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrNetServer"
    DEFINES XR_NETSERVER_EXPORTS
)

# Host-side check of the single-player message path (tools/xrnet_smoke); the iOS build only checks that the library compiles.
if(NOT ANOMALY_IOS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tools/xrnet_smoke" "${CMAKE_BINARY_DIR}/tools/xrnet_smoke")
endif()
