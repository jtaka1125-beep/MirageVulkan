#pragma once
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <cstdint>
#include <climits>

namespace mirage::vk {

struct QueueFamilyIndices {
    uint32_t graphics     = UINT32_MAX;
    uint32_t compute      = UINT32_MAX;
    uint32_t transfer     = UINT32_MAX;
    uint32_t video_decode = UINT32_MAX;  // For Vulkan Video H.264 decode
};

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() { shutdown(); }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    bool initialize(const char* app_name = "MirageSystem");
    void shutdown();

    VkSurfaceKHR createSurface(HWND hwnd);
    uint32_t findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags props);

    VkInstance       instance()       const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_device_; }
    VkDevice         device()         const { return device_; }
    VkQueue          graphicsQueue()  const { return graphics_queue_; }
    VkQueue          computeQueue()   const { return compute_queue_; }

    // Queue mutex: must be held during vkQueueSubmit / vkQueuePresentKHR.
    // AMD integrated GPU shares graphics+compute queue handle; concurrent
    // submissions from different threads violate the Vulkan spec and cause
    // driver-internal memcpy crashes (msvcrt.dll Access Violation).
    std::mutex& queueMutex() { return queue_mutex_; }

    // Thread-safe wrappers: these acquire queue_mutex_ before calling Vulkan.
    // Always use these instead of raw vkQueueSubmit/vkQueuePresentKHR.
    VkResult safeQueueSubmit(VkQueue queue, uint32_t submitCount,
                              const VkSubmitInfo* pSubmits, VkFence fence) {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        return vkQueueSubmit(queue, submitCount, pSubmits, fence);
    }
    VkResult safeQueuePresent(const VkPresentInfoKHR* pPresentInfo) {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        return vkQueuePresentKHR(graphics_queue_, pPresentInfo);
    }
    VkResult safeQueueWaitIdle(VkQueue queue) {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        return vkQueueWaitIdle(queue);
    }
    VkQueue          videoDecodeQueue() const { return video_decode_queue_; }
    const QueueFamilyIndices& queueFamilies() const { return queue_families_; }

    // Check if Vulkan Video decode is available
    bool hasVideoDecodeSupport() const { return queue_families_.video_decode != UINT32_MAX; }

private:
    VkInstance               instance_            = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_device_     = VK_NULL_HANDLE;
    VkDevice                 device_              = VK_NULL_HANDLE;
    VkQueue                  graphics_queue_      = VK_NULL_HANDLE;
    VkQueue                  compute_queue_       = VK_NULL_HANDLE;
    VkQueue                  video_decode_queue_  = VK_NULL_HANDLE;
    QueueFamilyIndices       queue_families_      = {};
    VkDebugUtilsMessengerEXT debug_messenger_     = VK_NULL_HANDLE;
    mutable std::mutex       queue_mutex_;          // Serializes vkQueueSubmit (shared gfx+compute queue on AMD iGPU)
};

} // namespace mirage::vk
