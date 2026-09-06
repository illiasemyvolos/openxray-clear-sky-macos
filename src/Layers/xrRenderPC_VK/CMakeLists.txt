# Experimental Vulkan renderer, phases 1 and 2 of docs/METAL_ROADMAP.md.
#
# Two targets, configured only when XRAY_BUILD_VK is ON so the OpenGL build is
# untouched by default:
#
#   xrVkProbe    - the standalone platform probe. Links nothing from the engine
#                  and opens its own window; it proves the platform presents.
#   xrRender_VK  - the renderer module. Currently generated no-op stubs, so it
#                  links against the engine but draws nothing.

add_executable(xrVkProbe vk_probe.cpp)

target_link_libraries(xrVkProbe PRIVATE Vulkan::Vulkan SDL2::SDL2)

target_compile_features(xrVkProbe PRIVATE cxx_std_17)

set_target_properties(xrVkProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_library(xrRender_VK)

target_sources(xrRender_VK PRIVATE
    stdafx.h
    vk_stubs_generated.h
    xrRender_VK.cpp
)

target_include_directories(xrRender_VK
    PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(xrRender_VK
    PRIVATE
    xrAPI
    xrCore
    xrEngine
    xrImGui  # xrEngine/stdafx.h reaches editor_base.h, which needs imgui.h
)

target_compile_definitions(xrRender_VK
    PRIVATE
    XRRENDER_VK_EXPORTS
    RENDER_NAMESPACE=render_vk
)

set_target_properties(xrRender_VK PROPERTIES
    PREFIX ""
)

if (BUILD_SHARED_LIBS)
    install(TARGETS xrRender_VK LIBRARY
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()
