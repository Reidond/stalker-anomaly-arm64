# Direct3D 11 / DXGI subset on top of Metal, used by the Anomaly R4 renderer (see docs/ios-port/METAL-SHIM.md).
add_library(metal_d3d11 STATIC
    src/mtl_context.mm
    src/mtl_d3dx11.mm
    src/mtl_device.mm
    src/mtl_dxgi.mm
    src/mtl_formats.mm
    src/mtl_shader.mm
    src/mtl_swapchain.mm
)
target_include_directories(metal_d3d11
    PUBLIC include
    PRIVATE src)
target_compile_features(metal_d3d11 PUBLIC cxx_std_17)
target_compile_options(metal_d3d11 PRIVATE -fobjc-arc -fobjc-weak -Wall -Wextra -Wno-unused-parameter
    -Wno-deprecated-declarations)
target_link_libraries(metal_d3d11 PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")

# Drop-in replacements for <d3d11.h>, <dxgi1_4.h>, <D3DCompiler.h>, <d3dx11.h>, ... Add this include path to the
# renderer targets on Apple platforms instead of the Windows SDK.
add_library(metal_d3d11_compat INTERFACE)
target_include_directories(metal_d3d11_compat INTERFACE compat)
target_link_libraries(metal_d3d11_compat INTERFACE metal_d3d11)
