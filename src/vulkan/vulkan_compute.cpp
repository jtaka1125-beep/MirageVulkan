#include "vulkan_compute.hpp"
#include "../mirage_log.hpp"
#include <fstream>
#include <cassert>

namespace mirage::vk {

// =============================================================================
// Load SPIR-V
// =============================================================================

std::vector<uint8_t> loadSPIRV(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        MLOG_ERROR("vulkan", "Failed to open SPIR-V file: %s", path.c_str());
        return {};
    }
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<uint8_t> buffer(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

// =============================================================================
// VulkanComputePipeline
// =============================================================================

bool VulkanComputePipeline::create(
    VulkanContext& ctx,
    const std::vector<uint8_t>& spirv,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    uint32_t push_const_size)
{
    ctx_ = &ctx;
    push_const_size_ = push_const_size;
    VkDevice dev = ctx.device();

    // 1. Create shader module
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spirv.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());

    if (vkCreateShaderModule(dev, &smci, nullptr, &shader_module_) != VK_SUCCESS) {
        MLOG_ERROR("vulkan", "[Compute] Failed to create shader module");
        return false;
    }

    // 2. Create descriptor set layout
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = static_cast<uint32_t>(bindings.size());
    dslci.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &ds_layout_) != VK_SUCCESS) {
        MLOG_ERROR("vulkan", "[Compute] Failed to create descriptor set layout");
        return false;
    }

    // 3. Create pipeline layout
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &ds_layout_;

    VkPushConstantRange pcr{};
    if (push_const_size > 0) {
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = push_const_size;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
    }

    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        MLOG_ERROR("vulkan", "[Compute] Failed to create pipeline layout");
        return false;
    }

    // 4. Create compute pipeline
    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader_module_;
    cpci.stage.pName = "main";
    cpci.layout = pipeline_layout_;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline_) != VK_SUCCESS) {
        MLOG_ERROR("vulkan", "[Compute] Failed to create compute pipeline");
        return false;
    }

    // 5. Create descriptor pool (enough for 16 descriptor sets)
    // Count descriptor types from bindings
    std::vector<VkDescriptorPoolSize> pool_sizes;
    for (const auto& b : bindings) {
        pool_sizes.push_back({b.descriptorType, 16});
    }

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 16;
    dpci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    dpci.pPoolSizes = pool_sizes.data();
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &ds_pool_) != VK_SUCCESS) {
        MLOG_ERROR("vulkan", "[Compute] Failed to create descriptor pool");
        return false;
    }

    MLOG_INFO("vulkan", "[Compute] Pipeline created (%zu bindings, push=%u)",
              bindings.size(), push_const_size);
    return true;
}

void VulkanComputePipeline::destroy() {
    if (!ctx_) return;
    VkDevice dev = ctx_->device();

    if (pipeline_)       { vkDestroyPipeline(dev, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_){ vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    if (ds_layout_)      { vkDestroyDescriptorSetLayout(dev, ds_layout_, nullptr); ds_layout_ = VK_NULL_HANDLE; }
    if (ds_pool_)        { vkDestroyDescriptorPool(dev, ds_pool_, nullptr); ds_pool_ = VK_NULL_HANDLE; }
    if (shader_module_)  { vkDestroyShaderModule(dev, shader_module_, nullptr); shader_module_ = VK_NULL_HANDLE; }

    ctx_ = nullptr;
}

void VulkanComputePipeline::bind(VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
}

void VulkanComputePipeline::bindDescriptorSet(VkCommandBuffer cmd, VkDescriptorSet ds) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &ds, 0, nullptr);
}

void VulkanComputePipeline::pushConstants(VkCommandBuffer cmd, const void* data, uint32_t size) {
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
}

void VulkanComputePipeline::dispatch(VkCommandBuffer cmd,
                                      uint32_t group_x, uint32_t group_y, uint32_t group_z) {
    vkCmdDispatch(cmd, group_x, group_y, group_z);
}

VkDescriptorSet VulkanComputePipeline::allocateDescriptorSet() {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = ds_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &ds_layout_;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(ctx_->device(), &ai, &ds) != VK_SUCCESS) {
        MLOG_ERROR("vulkan", "[Compute] Failed to allocate descriptor set");
        return VK_NULL_HANDLE;
    }
    return ds;
}

} // namespace mirage::vk
