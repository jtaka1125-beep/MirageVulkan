// =============================================================================
// MirageSystem - Vulkan YUV to RGBA Converter Implementation
// =============================================================================

#include "yuv_converter.hpp"
#include "../mirage_log.hpp"

#include <cstring>
#include <cstdio>
#include <vector>

namespace mirage::video {

// Embedded SPIR-V shader (compiled from yuv_to_rgba.comp)
// This is a placeholder - in production, load from file or embed compiled SPIR-V
static const uint32_t YUV_TO_RGBA_SPIRV[] = {
    // SPIR-V magic number and version
    0x07230203, 0x00010500, 0x00080001, 0x00000050,
    // ... (full SPIR-V would be here)
    // For now, we'll load from file in createPipeline()
};

VulkanYuvConverter::VulkanYuvConverter() = default;

VulkanYuvConverter::~VulkanYuvConverter() {
    destroy();
}

bool VulkanYuvConverter::initialize(VkDevice device,
                                    VkPhysicalDevice physical_device,
                                    uint32_t compute_queue_family,
                                    VkQueue compute_queue,
                                    const YuvConverterConfig& config) {
    std::lock_guard<std::mutex> lock(convert_mutex_);

    if (initialized_) {
        MLOG_WARN("YuvConv", "Already initialized");
        return true;
    }

    device_ = device;
    physical_device_ = physical_device;
    compute_queue_ = compute_queue;
    compute_queue_family_ = compute_queue_family;
    config_ = config;

    // Create command pool
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = compute_queue_family_;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(device_, &pool_info, nullptr, &cmd_pool_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create command pool");
        return false;
    }

    // Allocate command buffer
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cmd_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device_, &alloc_info, &cmd_buffer_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to allocate command buffer");
        destroy();
        return false;
    }

    // Create sampler
    if (!createSampler()) {
        MLOG_ERROR("YuvConv", "Failed to create sampler");
        destroy();
        return false;
    }

    // Create descriptor pool
    if (!createDescriptorPool()) {
        MLOG_ERROR("YuvConv", "Failed to create descriptor pool");
        destroy();
        return false;
    }

    // Create pipeline
    if (!createPipeline()) {
        MLOG_ERROR("YuvConv", "Failed to create compute pipeline");
        destroy();
        return false;
    }

    initialized_ = true;
    MLOG_INFO("YuvConv", "YUV converter initialized (max %dx%d, %s)",
              config_.max_width, config_.max_height,
              config_.color_space == ColorSpace::BT709 ? "BT.709" : "BT.601");

    return true;
}

void VulkanYuvConverter::destroy() {
    std::lock_guard<std::mutex> lock(convert_mutex_);

    if (device_ == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device_);

    // Destroy managed output image
    if (output_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, output_view_, nullptr);
        output_view_ = VK_NULL_HANDLE;
    }
    if (output_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, output_image_, nullptr);
        output_image_ = VK_NULL_HANDLE;
    }
    if (output_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, output_memory_, nullptr);
        output_memory_ = VK_NULL_HANDLE;
    }

    // Destroy pipeline
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (shader_module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, shader_module_, nullptr);
        shader_module_ = VK_NULL_HANDLE;
    }

    // Destroy descriptors
    if (desc_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        desc_pool_ = VK_NULL_HANDLE;
    }
    if (desc_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, desc_layout_, nullptr);
        desc_layout_ = VK_NULL_HANDLE;
    }

    // Destroy sampler
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }

    // Destroy command pool
    if (cmd_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
    MLOG_INFO("YuvConv", "YUV converter destroyed");
}

bool VulkanYuvConverter::createSampler() {
    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.mipLodBias = 0.0f;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;

    return vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) == VK_SUCCESS;
}

bool VulkanYuvConverter::createDescriptorPool() {
    // Descriptor set layout
    VkDescriptorSetLayoutBinding bindings[3] = {};

    // Binding 0: Y plane sampler
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: UV plane sampler
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 2: RGBA output storage image
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &desc_layout_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create descriptor set layout");
        return false;
    }

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 10;  // Allow multiple frames
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = 10;

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 5;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create descriptor pool");
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = desc_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &desc_layout_;

    if (vkAllocateDescriptorSets(device_, &alloc_info, &desc_set_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to allocate descriptor set");
        return false;
    }

    return true;
}

bool VulkanYuvConverter::createPipeline() {
    // Load pre-compiled SPIR-V shader
    std::vector<uint32_t> spirv_code;

    // Try multiple paths for the shader file
    const char* shader_paths[] = {
        "shaders/yuv_to_rgba.spv",
        "../shaders/yuv_to_rgba.spv",
        "yuv_to_rgba.spv",
        "C:/MirageWork/MirageVulkan/shaders/yuv_to_rgba.spv"
    };

    FILE* file = nullptr;
    for (const char* path : shader_paths) {
        file = fopen(path, "rb");
        if (file) {
            MLOG_INFO("YuvConv", "Loading shader from: %s", path);
            break;
        }
    }

    if (!file) {
        MLOG_ERROR("YuvConv", "Failed to open yuv_to_rgba.spv - shader not found");
        return false;
    }

    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size % 4 != 0) {
        MLOG_ERROR("YuvConv", "Invalid SPIR-V file size: %zu", file_size);
        fclose(file);
        return false;
    }

    spirv_code.resize(file_size / 4);
    size_t read_size = fread(spirv_code.data(), 1, file_size, file);
    fclose(file);

    if (read_size != file_size) {
        MLOG_ERROR("YuvConv", "Failed to read shader file");
        return false;
    }

    // Create shader module
    VkShaderModuleCreateInfo shader_info = {};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = spirv_code.size() * sizeof(uint32_t);
    shader_info.pCode = spirv_code.data();

    if (vkCreateShaderModule(device_, &shader_info, nullptr, &shader_module_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create shader module");
        return false;
    }
    MLOG_INFO("YuvConv", "Shader module created successfully");

    // Pipeline layout with push constants
    VkPushConstantRange push_range = {};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = 16;  // 4x uint32

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &desc_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create pipeline layout");
        return false;
    }

    // Create compute pipeline
    VkPipelineShaderStageCreateInfo stage_info = {};
    stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_info.module = shader_module_;
    stage_info.pName = "main";

    VkComputePipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage_info;
    pipeline_info.layout = pipeline_layout_;

    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create compute pipeline");
        return false;
    }

    MLOG_INFO("YuvConv", "Compute pipeline created successfully");
    return true;
}

bool VulkanYuvConverter::createOutputImage(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(convert_mutex_);

    if (current_width_ == width && current_height_ == height && output_image_ != VK_NULL_HANDLE) {
        return true;  // Already have correct size
    }

    // Destroy old image
    if (output_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, output_view_, nullptr);
        output_view_ = VK_NULL_HANDLE;
    }
    if (output_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, output_image_, nullptr);
        output_image_ = VK_NULL_HANDLE;
    }
    if (output_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, output_memory_, nullptr);
        output_memory_ = VK_NULL_HANDLE;
    }

    // Create RGBA image
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &image_info, nullptr, &output_image_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create output image");
        return false;
    }

    // Allocate memory
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, output_image_, &mem_req);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);

    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }

    if (mem_type == UINT32_MAX) {
        MLOG_ERROR("YuvConv", "No suitable memory type for output image");
        vkDestroyImage(device_, output_image_, nullptr);
        output_image_ = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &output_memory_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to allocate output image memory");
        vkDestroyImage(device_, output_image_, nullptr);
        output_image_ = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(device_, output_image_, output_memory_, 0) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to bind output image memory");
        vkFreeMemory(device_, output_memory_, nullptr);
        vkDestroyImage(device_, output_image_, nullptr);
        output_memory_ = VK_NULL_HANDLE;
        output_image_ = VK_NULL_HANDLE;
        return false;
    }

    // Create image view
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = output_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &view_info, nullptr, &output_view_) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to create output image view");
        vkFreeMemory(device_, output_memory_, nullptr);
        vkDestroyImage(device_, output_image_, nullptr);
        output_memory_ = VK_NULL_HANDLE;
        output_image_ = VK_NULL_HANDLE;
        return false;
    }

    current_width_ = width;
    current_height_ = height;

    MLOG_INFO("YuvConv", "Created output image %dx%d RGBA", width, height);
    return true;
}

bool VulkanYuvConverter::convert(VkImage nv12_input,
                                  VkImageView y_view,
                                  VkImageView uv_view,
                                  uint32_t width,
                                  uint32_t height,
                                  VkImage rgba_output,
                                  VkImageView rgba_view) {
    std::lock_guard<std::mutex> lock(convert_mutex_);

    if (!initialized_ || pipeline_ == VK_NULL_HANDLE) {
        MLOG_ERROR("YuvConv", "Converter not initialized");
        return false;
    }

    // Update descriptor set
    VkDescriptorImageInfo y_info = {};
    y_info.sampler = sampler_;
    y_info.imageView = y_view;
    y_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo uv_info = {};
    uv_info.sampler = sampler_;
    uv_info.imageView = uv_view;
    uv_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo output_info = {};
    output_info.imageView = rgba_view;
    output_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[3] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = desc_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &y_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = desc_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &uv_info;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = desc_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &output_info;

    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);

    // Record command buffer
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkResetCommandBuffer(cmd_buffer_, 0);
    vkBeginCommandBuffer(cmd_buffer_, &begin_info);

    // Transition output image to general layout
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = rgba_output;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd_buffer_,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Bind pipeline and descriptors
    vkCmdBindPipeline(cmd_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

    // Push constants
    struct {
        uint32_t width;
        uint32_t height;
        uint32_t color_space;
        uint32_t reserved;
    } push_data = {width, height, static_cast<uint32_t>(config_.color_space), 0};

    vkCmdPushConstants(cmd_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push_data), &push_data);

    // Dispatch compute
    uint32_t groups_x = (width + 15) / 16;
    uint32_t groups_y = (height + 15) / 16;
    vkCmdDispatch(cmd_buffer_, groups_x, groups_y, 1);

    // Transition output to shader read optimal for display
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkCmdPipelineBarrier(cmd_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd_buffer_);

    // Submit
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer_;

    if (vkQueueSubmit(compute_queue_, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to submit conversion command");
        return false;
    }

    vkQueueWaitIdle(compute_queue_);

    return true;
}

bool VulkanYuvConverter::convertAsync(VkImage nv12_input,
                                       VkImageView y_view,
                                       VkImageView uv_view,
                                       uint32_t width,
                                       uint32_t height,
                                       VkImage rgba_output,
                                       VkImageView rgba_view,
                                       VkSemaphore wait_semaphore,
                                       VkSemaphore signal_semaphore) {
    (void)nv12_input;  // Not used directly, we use plane views

    std::lock_guard<std::mutex> lock(convert_mutex_);

    if (!initialized_ || pipeline_ == VK_NULL_HANDLE) {
        MLOG_ERROR("YuvConv", "Converter not initialized for async conversion");
        return false;
    }

    // Update descriptor set
    VkDescriptorImageInfo y_info = {};
    y_info.sampler = sampler_;
    y_info.imageView = y_view;
    y_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo uv_info = {};
    uv_info.sampler = sampler_;
    uv_info.imageView = uv_view;
    uv_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo output_info = {};
    output_info.imageView = rgba_view;
    output_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[3] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = desc_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &y_info;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = desc_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &uv_info;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = desc_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &output_info;

    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);

    // Record command buffer
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkResetCommandBuffer(cmd_buffer_, 0);
    vkBeginCommandBuffer(cmd_buffer_, &begin_info);

    // Transition output image to general layout
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = rgba_output;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd_buffer_,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Bind pipeline and descriptors
    vkCmdBindPipeline(cmd_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

    // Push constants
    struct {
        uint32_t width;
        uint32_t height;
        uint32_t color_space;
        uint32_t reserved;
    } push_data = {width, height, static_cast<uint32_t>(config_.color_space), 0};

    vkCmdPushConstants(cmd_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push_data), &push_data);

    // Dispatch compute
    uint32_t groups_x = (width + 15) / 16;
    uint32_t groups_y = (height + 15) / 16;
    vkCmdDispatch(cmd_buffer_, groups_x, groups_y, 1);

    // Transition output to shader read optimal for display
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkCmdPipelineBarrier(cmd_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd_buffer_);

    // Submit with semaphores for pipeline synchronization
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = wait_semaphore != VK_NULL_HANDLE ? 1 : 0;
    submit_info.pWaitSemaphores = wait_semaphore != VK_NULL_HANDLE ? &wait_semaphore : nullptr;
    submit_info.pWaitDstStageMask = wait_semaphore != VK_NULL_HANDLE ? &wait_stage : nullptr;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buffer_;
    submit_info.signalSemaphoreCount = signal_semaphore != VK_NULL_HANDLE ? 1 : 0;
    submit_info.pSignalSemaphores = signal_semaphore != VK_NULL_HANDLE ? &signal_semaphore : nullptr;

    if (vkQueueSubmit(compute_queue_, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        MLOG_ERROR("YuvConv", "Failed to submit async conversion command");
        return false;
    }

    return true;
}

} // namespace mirage::video
