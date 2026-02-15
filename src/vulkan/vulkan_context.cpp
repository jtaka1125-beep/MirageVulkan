// =============================================================================
// MirageSystem - Vulkan Context
// =============================================================================
#include "vulkan_context.hpp"
#include "mirage_log.hpp"

#include <vector>
#include <cstring>
#include <set>

namespace mirage::vk {

#ifdef _DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        MLOG_ERROR("vulkan", "[VkValidation] %s", data->pMessage);
    else
        MLOG_INFO("vulkan", "[VkValidation] %s", data->pMessage);
    return VK_FALSE;
}

static VkResult createDebugMessenger(VkInstance inst,
    const VkDebugUtilsMessengerCreateInfoEXT* info,
    const VkAllocationCallbacks* alloc, VkDebugUtilsMessengerEXT* out)
{
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
    return fn ? fn(inst, info, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void destroyDebugMessenger(VkInstance inst,
    VkDebugUtilsMessengerEXT m, const VkAllocationCallbacks* alloc)
{
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) fn(inst, m, alloc);
}
#endif

bool VulkanContext::initialize(const char* app_name) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = app_name;
    appInfo.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
    appInfo.pEngineName = "MirageEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> exts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> layers;
#ifdef _DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    MLOG_INFO("vulkan", "[VulkanContext] Validation layers enabled");
#endif

    VkInstanceCreateInfo instInfo{};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = (uint32_t)exts.size();
    instInfo.ppEnabledExtensionNames = exts.data();
    instInfo.enabledLayerCount = (uint32_t)layers.size();
    instInfo.ppEnabledLayerNames = layers.data();

    VkResult r = vkCreateInstance(&instInfo, nullptr, &instance_);
    if (r != VK_SUCCESS) { MLOG_ERROR("vulkan", "[VulkanContext] vkCreateInstance: %d", (int)r); return false; }
    MLOG_INFO("vulkan", "[VulkanContext] VkInstance created");

#ifdef _DEBUG
    VkDebugUtilsMessengerCreateInfoEXT dbgInfo{};
    dbgInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbgInfo.pfnUserCallback = debugCallback;
    createDebugMessenger(instance_, &dbgInfo, nullptr, &debug_messenger_);
#endif

    // Physical device
    uint32_t cnt = 0;
    vkEnumeratePhysicalDevices(instance_, &cnt, nullptr);
    if (cnt == 0) { MLOG_ERROR("vulkan", "[VulkanContext] No Vulkan GPU"); return false; }
    std::vector<VkPhysicalDevice> devs(cnt);
    vkEnumeratePhysicalDevices(instance_, &cnt, devs.data());

    for (auto& dev : devs) {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qps(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qc, qps.data());

        QueueFamilyIndices idx;
        for (uint32_t i = 0; i < qc; i++) {
            if ((qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && idx.graphics == UINT32_MAX)
                idx.graphics = i;
            if ((qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                !(qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && idx.compute == UINT32_MAX)
                idx.compute = i;
            if ((qps[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                !(qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                !(qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && idx.transfer == UINT32_MAX)
                idx.transfer = i;
#ifdef VK_KHR_video_decode_queue
            if ((qps[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && idx.video_decode == UINT32_MAX)
                idx.video_decode = i;
#endif
        }
        if (idx.compute == UINT32_MAX) idx.compute = idx.graphics;
        if (idx.transfer == UINT32_MAX) idx.transfer = idx.graphics;

        if (idx.graphics != UINT32_MAX) {
            physical_device_ = dev;
            queue_families_ = idx;
            break;
        }
    }
    if (!physical_device_) { MLOG_ERROR("vulkan", "[VulkanContext] No suitable GPU"); return false; }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    MLOG_INFO("vulkan", "[VulkanContext] GPU: %s (Vulkan %d.%d.%d)", props.deviceName,
        VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));
    MLOG_INFO("vulkan", "[VulkanContext] Queues: gfx=%u compute=%u transfer=%u video_decode=%u",
        queue_families_.graphics, queue_families_.compute, queue_families_.transfer,
        queue_families_.video_decode);

    // Logical device
    std::set<uint32_t> families = { queue_families_.graphics, queue_families_.compute };
    if (queue_families_.video_decode != UINT32_MAX) {
        families.insert(queue_families_.video_decode);
    }
    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qInfos;
    for (uint32_t f : families) {
        VkDeviceQueueCreateInfo qi{}; qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = f; qi.queueCount = 1; qi.pQueuePriorities = &prio;
        qInfos.push_back(qi);
    }

    VkPhysicalDeviceFeatures feat{};

    // Build device extensions list
    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    // Add Vulkan Video extensions if video decode queue is available
#ifdef VK_KHR_video_decode_queue
    if (queue_families_.video_decode != UINT32_MAX) {
        deviceExtensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
        MLOG_INFO("vulkan", "[VulkanContext] Vulkan Video H.264 decode extensions enabled");
    }
#endif

    VkDeviceCreateInfo di{}; di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = (uint32_t)qInfos.size();
    di.pQueueCreateInfos = qInfos.data();
    di.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    di.ppEnabledExtensionNames = deviceExtensions.data();
    di.pEnabledFeatures = &feat;

    r = vkCreateDevice(physical_device_, &di, nullptr, &device_);
    if (r != VK_SUCCESS) { MLOG_ERROR("vulkan", "[VulkanContext] vkCreateDevice: %d", (int)r); return false; }

    vkGetDeviceQueue(device_, queue_families_.graphics, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, queue_families_.compute, 0, &compute_queue_);
    if (queue_families_.video_decode != UINT32_MAX) {
        vkGetDeviceQueue(device_, queue_families_.video_decode, 0, &video_decode_queue_);
        MLOG_INFO("vulkan", "[VulkanContext] Video decode queue acquired");
    }
    MLOG_INFO("vulkan", "[VulkanContext] Device created");
    return true;
}

void VulkanContext::shutdown() {
    if (device_) { vkDeviceWaitIdle(device_); vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
#ifdef _DEBUG
    if (debug_messenger_) { destroyDebugMessenger(instance_, debug_messenger_, nullptr); debug_messenger_ = VK_NULL_HANDLE; }
#endif
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = compute_queue_ = video_decode_queue_ = VK_NULL_HANDLE;
    queue_families_ = {};
    MLOG_INFO("vulkan", "[VulkanContext] Shutdown");
}

VkSurfaceKHR VulkanContext::createSurface(HWND hwnd) {
    VkWin32SurfaceCreateInfoKHR si{};
    si.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    si.hwnd = hwnd; si.hinstance = GetModuleHandle(nullptr);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult r = vkCreateWin32SurfaceKHR(instance_, &si, nullptr, &surface);
    if (r != VK_SUCCESS) { MLOG_ERROR("vulkan", "[VulkanContext] createSurface: %d", (int)r); return VK_NULL_HANDLE; }
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_families_.graphics, surface, &ok);
    if (!ok) { MLOG_ERROR("vulkan", "[VulkanContext] Surface not supported"); vkDestroySurfaceKHR(instance_, surface, nullptr); return VK_NULL_HANDLE; }
    MLOG_INFO("vulkan", "[VulkanContext] Surface created");
    return surface;
}

uint32_t VulkanContext::findMemoryType(uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    MLOG_ERROR("vulkan", "[VulkanContext] No suitable memory type");
    return UINT32_MAX;
}

} // namespace mirage::vk
