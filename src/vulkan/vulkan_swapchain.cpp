// =============================================================================
// MirageSystem - Vulkan Swapchain
// =============================================================================
#include "vulkan_swapchain.hpp"
#include "mirage_log.hpp"
#include <algorithm>
#include <limits>

namespace mirage::vk {

bool VulkanSwapchain::create(VulkanContext& ctx, VkSurfaceKHR surface, int w, int h) {
    ctx_ = &ctx; surface_ = surface;
    if (!createSwapchain(w, h) || !createImageViews() || !createRenderPass() || !createFramebuffers())
        return false;
    MLOG_INFO("vulkan", "[Swapchain] Created %ux%u, %u images", extent_.width, extent_.height, (uint32_t)images_.size());
    return true;
}

bool VulkanSwapchain::recreate(int w, int h) {
    if (!ctx_ || w == 0 || h == 0) return false;
    vkDeviceWaitIdle(ctx_->device());
    cleanupSwapchain();
    if (!createSwapchain(w, h) || !createImageViews() || !createFramebuffers()) return false;
    MLOG_INFO("vulkan", "[Swapchain] Recreated %ux%u", extent_.width, extent_.height);
    return true;
}

void VulkanSwapchain::destroy() {
    if (!ctx_) return;
    vkDeviceWaitIdle(ctx_->device());
    cleanupSwapchain();
    if (render_pass_) { vkDestroyRenderPass(ctx_->device(), render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
    if (surface_) { vkDestroySurfaceKHR(ctx_->instance(), surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    ctx_ = nullptr;
    MLOG_INFO("vulkan", "[Swapchain] Destroyed");
}

bool VulkanSwapchain::createSwapchain(int w, int h) {
    MLOG_INFO("VkSwap", "createSwapchain(%d, %d) begin", w, h);
    auto dev = ctx_->physicalDevice();
    VkSurfaceCapabilitiesKHR caps;
    VkResult capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface_, &caps);
    if (capsResult != VK_SUCCESS) {
        MLOG_ERROR("VkSwap", "getSurfaceCaps failed: %d", (int)capsResult);
        return false;
    }
    MLOG_INFO("VkSwap", "caps: min=%ux%u max=%ux%u cur=%ux%u minImg=%u maxImg=%u",
        caps.minImageExtent.width, caps.minImageExtent.height,
        caps.maxImageExtent.width, caps.maxImageExtent.height,
        caps.currentExtent.width, caps.currentExtent.height,
        caps.minImageCount, caps.maxImageCount);

    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface_, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface_, &fc, fmts.data());
    image_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            { image_format_ = f.format; break; }

    uint32_t pc = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface_, &pc, nullptr);
    std::vector<VkPresentModeKHR> pms(pc);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface_, &pc, pms.data());
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : pms) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pm = m; break; }

    if (caps.currentExtent.width != (std::numeric_limits<uint32_t>::max)())
        extent_ = caps.currentExtent;
    else {
        extent_.width = (std::clamp)((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = (std::clamp)((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t ic = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && ic > caps.maxImageCount) ic = caps.maxImageCount;

    VkSwapchainCreateInfoKHR si{}; si.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    si.surface = surface_; si.minImageCount = ic; si.imageFormat = image_format_;
    si.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    si.imageExtent = extent_; si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = caps.currentTransform;
    si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    si.presentMode = pm; si.clipped = VK_TRUE; si.oldSwapchain = swapchain_;

    MLOG_INFO("VkSwap", "vkCreateSwapchainKHR: %ux%u, %u images, fmt=%d, pm=%d",
        si.imageExtent.width, si.imageExtent.height, si.minImageCount,
        (int)si.imageFormat, (int)si.presentMode);
    VkSwapchainKHR old = swapchain_;
    VkResult r = vkCreateSwapchainKHR(ctx_->device(), &si, nullptr, &swapchain_);
    if (old) vkDestroySwapchainKHR(ctx_->device(), old, nullptr);
    if (r != VK_SUCCESS) { MLOG_ERROR("vulkan", "[Swapchain] create: %d", (int)r); return false; }

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(ctx_->device(), swapchain_, &n, images_.data());
    return true;
}

bool VulkanSwapchain::createImageViews() {
    image_views_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); i++) {
        VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = images_[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = image_format_;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkResult r = vkCreateImageView(ctx_->device(), &vi, nullptr, &image_views_[i]);
        if (r != VK_SUCCESS) { MLOG_ERROR("vulkan", "[Swapchain] imageView[%zu]: %d", i, (int)r); return false; }
    }
    return true;
}

bool VulkanSwapchain::createRenderPass() {
    VkAttachmentDescription att{}; att.format = image_format_; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{}; ref.attachment = 0; ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;

    VkSubpassDependency dep{}; dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1; rpi.pAttachments = &att; rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1; rpi.pDependencies = &dep;

    VkResult r = vkCreateRenderPass(ctx_->device(), &rpi, nullptr, &render_pass_);
    if (r != VK_SUCCESS) { MLOG_ERROR("vulkan", "[Swapchain] renderPass: %d", (int)r); return false; }
    return true;
}

bool VulkanSwapchain::createFramebuffers() {
    framebuffers_.resize(image_views_.size());
    for (size_t i = 0; i < image_views_.size(); i++) {
        VkFramebufferCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = render_pass_; fi.attachmentCount = 1; fi.pAttachments = &image_views_[i];
        fi.width = extent_.width; fi.height = extent_.height; fi.layers = 1;
        VkResult r = vkCreateFramebuffer(ctx_->device(), &fi, nullptr, &framebuffers_[i]);
        if (r != VK_SUCCESS) { MLOG_ERROR("vulkan", "[Swapchain] framebuffer[%zu]: %d", i, (int)r); return false; }
    }
    return true;
}

void VulkanSwapchain::cleanupSwapchain() {
    for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(ctx_->device(), fb, nullptr);
    framebuffers_.clear();
    for (auto v : image_views_) if (v) vkDestroyImageView(ctx_->device(), v, nullptr);
    image_views_.clear();
    if (swapchain_) { vkDestroySwapchainKHR(ctx_->device(), swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
    images_.clear();
}

} // namespace mirage::vk
