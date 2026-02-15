#include "vulkan_compute_processor.hpp"
#include "mirage_log.hpp"
#include <chrono>

namespace mirage::vk {

bool VulkanComputeProcessor::initialize(VulkanContext& ctx, const std::string& shader_dir) {
    ctx_ = &ctx;
    VkDevice dev = ctx.device();

    // Create compute command pool
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = ctx.queueFamilies().compute;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(dev, &cpci, nullptr, &cmd_pool_) != VK_SUCCESS) {
        MLOG_ERROR("VkProc", "Failed to create compute command pool");
        return false;
    }

    // Create fence
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(dev, &fci, nullptr, &fence_) != VK_SUCCESS) {
        MLOG_ERROR("VkProc", "Failed to create fence");
        return false;
    }

    // Load RGBA→Gray shader
    std::string spv_path = shader_dir + "/rgba_to_gray.spv";
    auto spirv = loadSPIRV(spv_path);
    if (spirv.empty()) {
        MLOG_ERROR("VkProc", "Failed to load shader: %s", spv_path.c_str());
        return false;
    }

    // Descriptor set layout: binding 0 = input image, binding 1 = output image
    std::vector<VkDescriptorSetLayoutBinding> bindings(2);

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    gray_pipeline_ = std::make_unique<VulkanComputePipeline>();
    if (!gray_pipeline_->create(ctx, spirv, bindings)) {
        MLOG_ERROR("VkProc", "Failed to create RGBA→Gray pipeline");
        return false;
    }

    initialized_ = true;
    MLOG_INFO("VkProc", "Vulkan Compute Processor initialized");
    return true;
}

void VulkanComputeProcessor::shutdown() {
    if (!ctx_) return;
    VkDevice dev = ctx_->device();
    vkDeviceWaitIdle(dev);

    gray_ds_ = VK_NULL_HANDLE;  // Freed with pipeline's pool
    gray_pipeline_.reset();
    input_rgba_.reset();
    output_gray_.reset();

    if (fence_)    { vkDestroyFence(dev, fence_, nullptr); fence_ = VK_NULL_HANDLE; }
    if (cmd_pool_) { vkDestroyCommandPool(dev, cmd_pool_, nullptr); cmd_pool_ = VK_NULL_HANDLE; }

    initialized_ = false;
    ctx_ = nullptr;
}

bool VulkanComputeProcessor::ensureImages(int width, int height) {
    if (input_rgba_ && current_width_ == width && current_height_ == height) {
        return true;  // Already correct size
    }

    // Recreate images
    input_rgba_.reset();
    output_gray_.reset();
    gray_ds_ = VK_NULL_HANDLE;

    input_rgba_ = std::make_unique<VulkanImage>();
    if (!input_rgba_->create(*ctx_, width, height, VK_FORMAT_R8G8B8A8_UNORM)) {
        MLOG_ERROR("VkProc", "Failed to create input RGBA image %dx%d", width, height);
        return false;
    }

    output_gray_ = std::make_unique<VulkanImage>();
    if (!output_gray_->create(*ctx_, width, height, VK_FORMAT_R8_UNORM)) {
        MLOG_ERROR("VkProc", "Failed to create output gray image %dx%d", width, height);
        return false;
    }

    // Allocate descriptor set
    gray_ds_ = gray_pipeline_->allocateDescriptorSet();
    if (gray_ds_ == VK_NULL_HANDLE) {
        MLOG_ERROR("VkProc", "Failed to allocate descriptor set");
        return false;
    }

    // Update descriptor set with image views
    VkDescriptorImageInfo input_info{};
    input_info.imageView = input_rgba_->imageView();
    input_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo output_info{};
    output_info.imageView = output_gray_->imageView();
    output_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = gray_ds_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &input_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = gray_ds_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &output_info;

    vkUpdateDescriptorSets(ctx_->device(), 2, writes, 0, nullptr);

    current_width_ = width;
    current_height_ = height;

    MLOG_INFO("VkProc", "Images allocated %dx%d (RGBA input + R8 output)", width, height);
    return true;
}

bool VulkanComputeProcessor::rgbaToGray(const uint8_t* rgba, int width, int height,
                                         uint8_t* out_gray) {
    auto* gray_img = rgbaToGrayGpu(rgba, width, height);
    if (!gray_img) return false;

    // Download result
    size_t gray_size = (size_t)width * height;
    return gray_img->download(cmd_pool_, ctx_->computeQueue(), out_gray, gray_size);
}

VulkanImage* VulkanComputeProcessor::rgbaToGrayGpu(const uint8_t* rgba, int width, int height) {
    if (!initialized_) return nullptr;

    auto t0 = std::chrono::high_resolution_clock::now();

    // Ensure images are correct size
    if (!ensureImages(width, height)) return nullptr;

    // Upload RGBA to GPU
    size_t rgba_size = (size_t)width * height * 4;
    if (!input_rgba_->upload(cmd_pool_, ctx_->computeQueue(), rgba, rgba_size)) {
        MLOG_ERROR("VkProc", "Failed to upload RGBA data");
        return nullptr;
    }

    // Transition output to GENERAL before compute
    {
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;

        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(ctx_->device(), &cai, &cmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        // Ensure output image is in GENERAL layout
        output_gray_->transitionLayout(cmd,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // Bind compute pipeline
        gray_pipeline_->bind(cmd);
        gray_pipeline_->bindDescriptorSet(cmd, gray_ds_);

        // Dispatch: 16x16 work groups
        uint32_t gx = (width + 15) / 16;
        uint32_t gy = (height + 15) / 16;
        gray_pipeline_->dispatch(cmd, gx, gy, 1);

        // Barrier: compute write → transfer read (for download) or next compute
        VkMemoryBarrier mem_bar{};
        mem_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mem_bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mem_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &mem_bar, 0, nullptr, 0, nullptr);

        vkEndCommandBuffer(cmd);

        // Submit and wait
        vkResetFences(ctx_->device(), 1, &fence_);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(ctx_->computeQueue(), 1, &si, fence_);
        vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        vkFreeCommandBuffers(ctx_->device(), cmd_pool_, 1, &cmd);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    stats_.conversions++;
    stats_.last_time_ms = ms;
    stats_.avg_time_ms = stats_.avg_time_ms * 0.9 + ms * 0.1;  // EMA

    if (stats_.conversions % 100 == 0) {
        MLOG_INFO("VkProc", "RGBA→Gray: %.1f ms (avg %.1f ms, %llu frames)",
                  ms, stats_.avg_time_ms, stats_.conversions);
    }

    return output_gray_.get();
}

} // namespace mirage::vk
