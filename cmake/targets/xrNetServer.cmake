# xrNetServer: network layer (DirectPlay transport + NET_Packet). Source list: src/xrNetServer/xrNetServer.vcxproj.
# Owner: netserver session (swaps the DirectPlay transport for OpenXRay's `empty/` backend).
# Opt-in: -DANOMALY_BUILD_XRNETSERVER=ON (or ANOMALY_BUILD_ALL_MODULES).
anomaly_module_begin(xrNetServer "network server/client")

anomaly_vcxproj_sources(xrnetserver_sources "${ANOMALY_SRC_DIR}/xrNetServer/xrNetServer.vcxproj")

anomaly_add_module(xrNetServer
    SOURCES ${xrnetserver_sources}
    INCLUDE_DIRS "${ANOMALY_SRC_DIR}/xrNetServer"
    DEFINES XR_NETSERVER_EXPORTS
)
