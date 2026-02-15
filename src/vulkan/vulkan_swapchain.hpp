#pragma once

#include "vulkan_context.hpp"
#include <vector>

namespace mirage::vk {

class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain() = default;

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    bool create(VulkanContext& ctx, VkSurfaceKHR surface, int width, int height);
    bool recreate(int width, int height);
    void destroy();

    VkSwapchainKHR   swapchain()        const { return swapchain_; }
    VkRenderPass     renderPass()       const { return render_pass_; }
    VkFramebuffer    framebuffer(uint32_t i) const { return framebuffers_[i]; }
    VkExtent2D       extent()           const { return extent_; }
    uint32_t         imageCount()       const { return (uint32_t)image_views_.size(); }
    VkFormat         imageFormat()      const { return image_format_; }

private:
    bool createSwapchain(int width, int height);
    bool createImageViews();
    bool createRenderPass();
    bool createFramebuffers();
    void cleanupSwapchain();

    VulkanContext*               ctx_          = nullptr;
    VkSurfaceKHR                 surface_      = VK_NULL_HANDLE;
    VkSwapchainKHR               swapchain_    = VK_NULL_HANDLE;
    VkRenderPass                 render_pass_  = VK_NULL_HANDLE;
    VkFormat                     image_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D                   extent_       = {0, 0};
    std::vector<VkImage>         images_;
    std::vector<VkImageView>     image_views_;
    std::vector<VkFramebuffer>   framebuffers_;
};

} // namespace mirage::vk
