add_subdirectory(xrAPI)

if (WIN32)
    add_subdirectory(xrRenderPC_R4)
endif()

add_subdirectory(xrRenderPC_GL)

if (XRAY_BUILD_VK)
    add_subdirectory(xrRenderPC_VK)
endif()
