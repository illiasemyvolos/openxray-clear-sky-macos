// Platform spike for the Vulkan renderer, phase 1 of docs/METAL_ROADMAP.md.
//
// Deliberately standalone: it links SDL2 and Vulkan and nothing from the
// engine. IRender has 112 pure virtual methods, and stubbing them to find out
// whether a swapchain can be presented would put the risky part behind a wall
// of boilerplate. This proves the platform first; the RendererModule wiring
// comes after, on top of code that is known to work.
//
// It answers: does the loader find a driver, does SDL hand us a surface that
// the driver accepts, does a swapchain present, and do the validation layers
// load and stay quiet.
//
// The target is Vulkan 1.3, using dynamic rendering rather than VkRenderPass
// and VkFramebuffer objects. macOS has no old-driver floor to support - both
// drivers shipped in the SDK are conformant at 1.4 - and dynamic rendering is
// the closer match to Metal, where a render pass is described when the encoder
// is created rather than baked into an object ahead of time. See
// docs/BACKEND_OPTIONS.md for the measured baseline.

#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace
{
constexpr int kWidth = 1280;
constexpr int kHeight = 800;
constexpr int kFramesInFlight = 2;

// MoltenVK's guide recommends three swapchain images; two costs throughput in
// full-screen, more than three only adds latency.
constexpr uint32_t kDesiredImages = 3;

bool g_validation =
#ifdef NDEBUG
    false;
#else
    true;
#endif

int g_forced_device = -1; // --device N, to compare drivers without env vars

#define VK_CHECK(expr)                                                            \
    do                                                                            \
    {                                                                             \
        const VkResult vk_check_result = (expr);                                  \
        if (vk_check_result != VK_SUCCESS)                                        \
        {                                                                         \
            std::fprintf(stderr, "%s:%d: %s failed with VkResult %d\n",           \
                         __FILE__, __LINE__, #expr, int(vk_check_result));        \
            return false;                                                         \
        }                                                                         \
    } while (false)

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/)
{
    const char* level = "info";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        level = "ERROR";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        level = "warning";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
        return VK_FALSE; // too chatty to be useful here

    std::fprintf(stderr, "[validation %s] %s\n", level,
                 data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool has_layer(const char* name)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& layer : layers)
        if (std::strcmp(layer.layerName, name) == 0)
            return true;
    return false;
}

bool has_instance_extension(const char* name)
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    return false;
}

bool device_has_extension(VkPhysicalDevice device, const char* name)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    return false;
}

uint32_t instance_version()
{
    auto enumerate = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
        nullptr, "vkEnumerateInstanceVersion");
    if (!enumerate) // a 1.0 loader has no way to report anything else
        return VK_API_VERSION_1_0;
    uint32_t version = VK_API_VERSION_1_0;
    if (enumerate(&version) != VK_SUCCESS)
        return VK_API_VERSION_1_0;
    return version;
}

const char* device_type_name(VkPhysicalDeviceType type)
{
    switch (type)
    {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
    default:                                     return "other";
    }
}

// Query through the 1.1 properties2 chain so the driver identifies itself the
// way vulkaninfo reports it, rather than as an opaque driverVersion integer.
void describe_device(VkPhysicalDevice device, uint32_t index)
{
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    vkGetPhysicalDeviceProperties2(device, &props);

    const VkPhysicalDeviceProperties& base = props.properties;
    std::printf("  [%u] %s (%s)\n", index, base.deviceName,
                device_type_name(base.deviceType));
    std::printf("        api %u.%u.%u, driver %s %s\n",
                VK_API_VERSION_MAJOR(base.apiVersion),
                VK_API_VERSION_MINOR(base.apiVersion),
                VK_API_VERSION_PATCH(base.apiVersion),
                driver.driverName[0] ? driver.driverName : "(unnamed)",
                driver.driverInfo);
    std::printf("        conformance %u.%u.%u.%u\n",
                driver.conformanceVersion.major, driver.conformanceVersion.minor,
                driver.conformanceVersion.subminor, driver.conformanceVersion.patch);
}

struct Probe
{
    SDL_Window* window{};
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    uint32_t queue_family{};
    VkQueue queue{};

    VkSwapchainKHR swapchain{};
    VkFormat swapchain_format{};
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;

    VkCommandPool command_pool{};
    std::vector<VkCommandBuffer> command_buffers;
    VkSemaphore acquired[kFramesInFlight]{};
    std::vector<VkSemaphore> rendered; // one per swapchain image, not per frame
    VkFence in_flight[kFramesInFlight]{};

    bool create_instance();
    bool pick_device();
    bool create_device();
    bool create_swapchain();
    void destroy_swapchain();
    bool create_frame_resources();
    bool draw(uint32_t frame, float t, bool& needs_resize);
    void shutdown();
};

bool Probe::create_instance()
{
    unsigned int sdl_count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &sdl_count, nullptr))
    {
        std::fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return false;
    }
    std::vector<const char*> extensions(sdl_count);
    SDL_Vulkan_GetInstanceExtensions(window, &sdl_count, extensions.data());

    VkInstanceCreateFlags flags = 0;
    // MoltenVK is a portability driver: without this it is not enumerated at all.
    if (has_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    std::vector<const char*> layers;
    if (g_validation)
    {
        if (has_layer("VK_LAYER_KHRONOS_validation") &&
            has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        else
        {
            std::printf("note: validation layers unavailable, continuing without them\n");
            g_validation = false;
        }
    }

    // Ask for as much as the loader offers, capped at the newest version these
    // headers know. A driver reports its own apiVersion capped at what the
    // instance asked for, so requesting too little hides what it can actually
    // do - MoltenVK reports 1.2 to a 1.2 instance and 1.4 to a 1.4 one.
    const uint32_t available = instance_version();
    const uint32_t requested = std::min<uint32_t>(available, VK_API_VERSION_1_4);
    std::printf("instance: loader offers %u.%u.%u, requesting %u.%u\n",
                VK_API_VERSION_MAJOR(available), VK_API_VERSION_MINOR(available),
                VK_API_VERSION_PATCH(available), VK_API_VERSION_MAJOR(requested),
                VK_API_VERSION_MINOR(requested));

    if (requested < VK_API_VERSION_1_3)
    {
        std::fprintf(stderr, "need a Vulkan 1.3 loader for dynamic rendering\n");
        return false;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "OpenXRay Vulkan probe";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "X-Ray";
    app.apiVersion = requested;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = uint32_t(layers.size());
    info.ppEnabledLayerNames = layers.data();

    VK_CHECK(vkCreateInstance(&info, nullptr, &instance));

    if (g_validation)
    {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkCreateDebugUtilsMessengerEXT");
        if (create)
        {
            VkDebugUtilsMessengerCreateInfoEXT dbg{};
            dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg.pfnUserCallback = debug_callback;
            create(instance, &dbg, nullptr, &messenger);
        }
    }
    return true;
}

bool Probe::pick_device()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0)
    {
        std::fprintf(stderr, "no Vulkan physical devices; is an ICD installed?\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    std::printf("Vulkan devices (%u):\n", count);
    std::optional<uint32_t> chosen;
    std::optional<uint32_t> chosen_queue;

    for (uint32_t i = 0; i < count; ++i)
    {
        describe_device(devices[i], i);

        VkPhysicalDeviceProperties base{};
        vkGetPhysicalDeviceProperties(devices[i], &base);
        if (base.apiVersion < VK_API_VERSION_1_3)
        {
            std::printf("        skipped: below Vulkan 1.3\n");
            continue;
        }

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families.data());

        for (uint32_t f = 0; f < family_count; ++f)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], f, surface, &present);
            // One family that can do both keeps the spike free of ownership
            // transfers. Every Apple GPU satisfies this.
            if (present && (families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                const bool wanted = g_forced_device < 0 || uint32_t(g_forced_device) == i;
                if (wanted && !chosen)
                {
                    chosen = i;
                    chosen_queue = f;
                }
                break;
            }
        }
    }

    if (!chosen)
    {
        std::fprintf(stderr, "no suitable device with a graphics queue that can present\n");
        return false;
    }
    physical = devices[*chosen];
    queue_family = *chosen_queue;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    std::printf("using [%u] %s, queue family %u\n", *chosen, props.deviceName, queue_family);
    return true;
}

bool Probe::create_device()
{
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported.pNext = &supported13;
    vkGetPhysicalDeviceFeatures2(physical, &supported);

    if (!supported13.dynamicRendering)
    {
        std::fprintf(stderr, "device does not support dynamicRendering\n");
        return false;
    }
    std::printf("dynamic rendering: available, enabling\n");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    std::vector<const char*> extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    // Required by the spec whenever the driver advertises it, which MoltenVK does.
    if (device_has_extension(physical, "VK_KHR_portability_subset"))
        extensions.push_back("VK_KHR_portability_subset");

    VkPhysicalDeviceVulkan13Features enable13{};
    enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enable13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceFeatures2 enable{};
    enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enable.pNext = &enable13;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enable; // pEnabledFeatures must stay null when this chain is used
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    VK_CHECK(vkCreateDevice(physical, &info, nullptr, &device));
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    return true;
}

bool Probe::create_swapchain()
{
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());

    VkSurfaceFormatKHR picked = formats.front();
    for (const auto& format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            picked = format;
            break;
        }
    }

    swapchain_extent = caps.currentExtent;
    if (swapchain_extent.width == UINT32_MAX) // surface leaves the size to us
    {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window, &w, &h);
        swapchain_extent.width = std::clamp(uint32_t(w), caps.minImageExtent.width,
                                            caps.maxImageExtent.width);
        swapchain_extent.height = std::clamp(uint32_t(h), caps.minImageExtent.height,
                                             caps.maxImageExtent.height);
    }
    if (swapchain_extent.width == 0 || swapchain_extent.height == 0)
        return false; // minimised; caller retries

    uint32_t image_count = std::max(kDesiredImages, caps.minImageCount);
    if (caps.maxImageCount != 0)
        image_count = std::min(image_count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = image_count;
    info.imageFormat = picked.format;
    info.imageColorSpace = picked.colorSpace;
    info.imageExtent = swapchain_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always supported; vsync
    info.clipped = VK_TRUE;

    VK_CHECK(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain));
    swapchain_format = picked.format;

    uint32_t actual = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &actual, nullptr);
    images.resize(actual);
    vkGetSwapchainImagesKHR(device, swapchain, &actual, images.data());

    views.resize(actual);
    for (uint32_t i = 0; i < actual; ++i)
    {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = swapchain_format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(device, &view, nullptr, &views[i]));
    }

    std::printf("swapchain: %ux%u, %u images, format %d\n", swapchain_extent.width,
                swapchain_extent.height, actual, int(swapchain_format));
    return true;
}

bool Probe::create_frame_resources()
{
    if (command_pool == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family;
        VK_CHECK(vkCreateCommandPool(device, &pool, nullptr, &command_pool));

        command_buffers.resize(kFramesInFlight);
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = command_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kFramesInFlight;
        VK_CHECK(vkAllocateCommandBuffers(device, &alloc, command_buffers.data()));

        for (int i = 0; i < kFramesInFlight; ++i)
        {
            VkSemaphoreCreateInfo sem{};
            sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &acquired[i]));
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateFence(device, &fence, nullptr, &in_flight[i]));
        }
    }

    // The "render finished" semaphore is signalled by work on a specific image
    // and waited on by the present of that image, so it belongs to the image,
    // not to the frame slot. Sharing one per frame is a real, if intermittent,
    // synchronisation bug that validation flags.
    rendered.resize(images.size());
    for (auto& semaphore : rendered)
    {
        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &semaphore));
    }
    return true;
}

void Probe::destroy_swapchain()
{
    for (auto semaphore : rendered)
        vkDestroySemaphore(device, semaphore, nullptr);
    rendered.clear();
    for (auto view : views)
        vkDestroyImageView(device, view, nullptr);
    views.clear();
    images.clear();
    if (swapchain)
    {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

bool Probe::draw(uint32_t frame, float t, bool& needs_resize)
{
    const uint32_t slot = frame % kFramesInFlight;
    VK_CHECK(vkWaitForFences(device, 1, &in_flight[slot], VK_TRUE, UINT64_MAX));

    uint32_t index = 0;
    const VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                                   acquired[slot], VK_NULL_HANDLE, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
    {
        needs_resize = true;
        return true;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
    {
        std::fprintf(stderr, "vkAcquireNextImageKHR: %d\n", int(acquire));
        return false;
    }
    VK_CHECK(vkResetFences(device, 1, &in_flight[slot]));

    VkCommandBuffer cmd = command_buffers[slot];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    const VkImageSubresourceRange whole_image{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Without a render pass there is nothing to perform the implicit layout
    // transitions, so they become ours. Acquire hands the image over in an
    // undefined layout; present needs it in PRESENT_SRC_KHR.
    VkImageMemoryBarrier to_attachment{};
    to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.image = images[index];
    to_attachment.subresourceRange = whole_image;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_attachment);

    // Animated so that a frozen window is obvious at a glance.
    VkClearValue clear{};
    clear.color = { { 0.10f + 0.10f * SDL_sinf(t),
                      0.12f + 0.10f * SDL_sinf(t * 0.7f + 2.0f),
                      0.18f + 0.12f * SDL_sinf(t * 0.4f + 4.0f), 1.0f } };

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = views[index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = clear;

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = swapchain_extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = images[index];
    to_present.subresourceRange = whole_image;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_present);

    VK_CHECK(vkEndCommandBuffer(cmd));

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired[slot];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered[index];
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, in_flight[slot]));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    const VkResult presented = vkQueuePresentKHR(queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        needs_resize = true;
    else if (presented != VK_SUCCESS)
    {
        std::fprintf(stderr, "vkQueuePresentKHR: %d\n", int(presented));
        return false;
    }
    return true;
}

void Probe::shutdown()
{
    if (device)
    {
        vkDeviceWaitIdle(device);
        destroy_swapchain();
        for (int i = 0; i < kFramesInFlight; ++i)
        {
            vkDestroySemaphore(device, acquired[i], nullptr);
            vkDestroyFence(device, in_flight[i], nullptr);
        }
        vkDestroyCommandPool(device, command_pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (messenger)
    {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(instance, messenger, nullptr);
    }
    if (surface)
        vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}
} // namespace

int main(int argc, char** argv)
{
    // The launcher pipes us through tee, so stdout is a pipe rather than a
    // terminal and libc switches to full buffering. Without this the device
    // list and the swapchain description only appear when the window closes,
    // which is exactly when they stop being useful.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--no-validation") == 0)
            g_validation = false;
        else if (std::strcmp(argv[i], "--validation") == 0)
            g_validation = true;
        else if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc)
            g_forced_device = std::atoi(argv[++i]);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    Probe probe;
    probe.window = SDL_CreateWindow("OpenXRay Vulkan probe", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                        SDL_WINDOW_ALLOW_HIGHDPI);
    if (!probe.window)
    {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    std::printf("validation layers: %s\n", g_validation ? "requested" : "off");

    int status = 1;
    if (probe.create_instance() &&
        SDL_Vulkan_CreateSurface(probe.window, probe.instance, &probe.surface) &&
        probe.pick_device() && probe.create_device() && probe.create_swapchain() &&
        probe.create_frame_resources())
    {
        std::printf("presenting; close the window to finish\n");
        const uint64_t start = SDL_GetTicks64();
        uint32_t frame = 0;
        bool running = true;
        status = 0;

        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT)
                    running = false;
                else if (event.type == SDL_WINDOWEVENT &&
                         event.window.event == SDL_WINDOWEVENT_CLOSE)
                    running = false;
            }
            if (!running)
                break;

            bool needs_resize = false;
            const float t = float(SDL_GetTicks64() - start) / 1000.0f;
            if (!probe.draw(frame, t, needs_resize))
            {
                status = 1;
                break;
            }
            if (needs_resize)
            {
                vkDeviceWaitIdle(probe.device);
                probe.destroy_swapchain();
                if (!probe.create_swapchain() || !probe.create_frame_resources())
                {
                    SDL_Delay(50); // most likely minimised; try again next tick
                    continue;
                }
            }
            ++frame;
        }
        const float seconds = float(SDL_GetTicks64() - start) / 1000.0f;
        std::printf("presented %u frames over %.1f s (%.1f fps)\n", frame, seconds,
                    seconds > 0.0f ? float(frame) / seconds : 0.0f);
    }
    else if (!probe.surface && probe.instance)
    {
        std::fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    }

    probe.shutdown();
    return status;
}
