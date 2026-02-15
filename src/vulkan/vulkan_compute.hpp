#pragma once

#include "vulkan_context.hpp"
#include <vector>
#include <string>

namespace mirage::vk {

/**
 * Vulkan Compute Pipeline
 * 
 * Manages a single compute shader pipeline with descriptor sets.
 * Used for GPU image processing (grayscale conversion, template matching, etc.)
 */
class VulkanComputePipeline {
public:
    VulkanComputePipeline() = default;
    ~VulkanComputePipeline() { destroy(); }

    VulkanComputePipeline(const VulkanComputePipeline&) = delete;
    VulkanComputePipeline& operator=(const VulkanComputePipeline&) = delete;

    /**
     * Create compute pipeline from SPIR-V bytecode.
     * @param ctx            Vulkan context (device, queues)
     * @param spirv          Compiled SPIR-V shader bytecode
     * @param bindings       Descriptor set layout bindings
     * @param push_const_size Size of push constant block (0 if unused)
     */
    bool create(VulkanContext& ctx,
                const std::vector<uint8_t>& spirv,
                const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                uint32_t push_const_size = 0);

    void destroy();

    // Bind pipeline to command buffer
    void bind(VkCommandBuffer cmd);

    // Bind descriptor set
    void bindDescriptorSet(VkCommandBuffer cmd, VkDescriptorSet ds);

    // Push constants
    void pushConstants(VkCommandBuffer cmd, const void* data, uint32_t size);

    // Dispatch compute work
    void dispatch(VkCommandBuffer cmd,
                  uint32_t group_x, uint32_t group_y, uint32_t group_z);

    // Allocate a descriptor set from the internal pool
    VkDescriptorSet allocateDescriptorSet();

    // Accessors
    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineLayout pipelineLayout() const { return pipeline_layout_; }
    VkDescriptorSetLayout descriptorSetLayout() const { return ds_layout_; }
    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    VulkanContext* ctx_ = nullptr;

    VkShaderModule          shader_module_  = VK_NULL_HANDLE;
    VkDescriptorSetLayout   ds_layout_      = VK_NULL_HANDLE;
    VkPipelineLayout        pipeline_layout_= VK_NULL_HANDLE;
    VkPipeline              pipeline_       = VK_NULL_HANDLE;
    VkDescriptorPool        ds_pool_        = VK_NULL_HANDLE;

    uint32_t push_const_size_ = 0;
};

/**
 * Load SPIR-V file from disk.
 */
std::vector<uint8_t> loadSPIRV(const std::string& path);

} // namespace mirage::vk
