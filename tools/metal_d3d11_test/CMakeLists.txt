add_executable(metal_d3d11_test main.mm)
target_link_libraries(metal_d3d11_test PRIVATE SDL3::SDL3-static metal_d3d11_compat
    "-framework ImageIO" "-framework CoreGraphics" "-framework CoreFoundation")
target_compile_options(metal_d3d11_test PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)

# Reuses the platform probe's Info.plist templates.
anomaly_configure_app(metal_d3d11_test
    BUNDLE_SUFFIX ".metallab"
    DISPLAY_NAME "Metal D3D11 Lab"
    INFO_PLIST "${PROJECT_SOURCE_DIR}/tools/platform_probe/Info-${ANOMALY_PLATFORM_NAME}.plist.in"
    ENTITLEMENTS "${ANOMALY_IOS_ENTITLEMENTS}"
)
