# Experimental Vulkan renderer, phase 1 of docs/METAL_ROADMAP.md.
#
# For now this directory builds only the platform probe: a standalone
# executable that opens a Vulkan surface and presents. It links nothing from
# the engine, so it cannot affect the OpenGL build, and it is only configured
# at all when XRAY_BUILD_VK is ON.
#
# The renderer module itself lands here later, next to the probe.

add_executable(xrVkProbe vk_probe.cpp)

target_link_libraries(xrVkProbe PRIVATE Vulkan::Vulkan SDL2::SDL2)

target_compile_features(xrVkProbe PRIVATE cxx_std_17)

set_target_properties(xrVkProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
