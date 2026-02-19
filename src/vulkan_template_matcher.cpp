#include "vulkan_template_matcher.hpp"
#include "mirage_log.hpp"

#include <cstring>
#include <algorithm>
#include <cmath>

namespace mirage::vk {

// Push constants for tile-based NCC shader (must match shader layout exactly)
struct NccPushConstants {
    int32_t src_width;
    int32_t src_height;
    int32_t tpl_width;
    int32_t tpl_height;
    int32_t template_id;
    float   threshold;
    int32_t search_width;
    int32_t search_height;
};

// Push constants for SAT-based NCC shader
struct SatNccPushConstants {
    int32_t src_width;
    int32_t src_height;
    int32_t tpl_width;
    int32_t tpl_height;
    int32_t template_id;
    float   threshold;
    int32_t search_width;
    int32_t search_height;
    float   sum_t;
    float   sum_tt;
    float   inv_n;
    float   denom_t;
};

// Push constants for prefix sum shaders
struct SatPushConstants {
    int32_t width;
    int32_t height;
    int32_t mode;
    int32_t pad0;
};

struct GpuMatchResult {
    int32_t x;
    int32_t y;
    float   score;
    int32_t template_id;
};

static constexpr int MAX_RESULTS = 1024;

static bool createHostBuffer(VulkanContext& ctx, VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(ctx.device(), &ci, nullptr, &buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx.device(), buf, &req);

    uint32_t memType = ctx.findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memType == UINT32_MAX) {
        vkDestroyBuffer(ctx.device(), buf, nullptr);
        buf = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType;

    if (vkAllocateMemory(ctx.device(), &ai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(ctx.device(), buf, nullptr);
        buf = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(ctx.device(), buf, mem, 0);
    return true;
}

VulkanTemplateMatcher::~VulkanTemplateMatcher() {
    clearAll();

    if (ctx_) {
        VkDevice dev = ctx_->device();
        if (result_buf_)  { vkDestroyBuffer(dev, result_buf_, nullptr); vkFreeMemory(dev, result_mem_, nullptr); }
        if (counter_buf_) { vkDestroyBuffer(dev, counter_buf_, nullptr); vkFreeMemory(dev, counter_mem_, nullptr); }
        if (fence_)       vkDestroyFence(dev, fence_, nullptr);
        if (cmd_pool_)    vkDestroyCommandPool(dev, cmd_pool_, nullptr);
    }
}

mirage::Result<void> VulkanTemplateMatcher::initialize(VulkanContext& ctx,
                                                        const VkMatcherConfig& config,
                                                        const std::string& shader_dir) {
    ctx_ = &ctx;
    config_ = config;
    VkDevice dev = ctx.device();

    // Command pool
    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = ctx.queueFamilies().compute;

    if (vkCreateCommandPool(dev, &poolCI, nullptr, &cmd_pool_) != VK_SUCCESS) {
        return mirage::Err<void>("Failed to create compute command pool");
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = cmd_pool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &allocInfo, &cmd_buf_);

    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(dev, &fenceCI, nullptr, &fence_);

    // Result & counter buffers
    if (!createHostBuffer(ctx, sizeof(GpuMatchResult) * MAX_RESULTS,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          result_buf_, result_mem_)) {
        return mirage::Err<void>("Failed to create result buffer");
    }
    if (!createHostBuffer(ctx, sizeof(int32_t),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          counter_buf_, counter_mem_)) {
        return mirage::Err<void>("Failed to create counter buffer");
    }

    // === Tile-based NCC pipeline (Opt G+C+B) ===
    std::vector<VkDescriptorSetLayoutBinding> nccBindings(4);
    nccBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    nccBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    nccBindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    nccBindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    auto nccCode = loadSPIRV(shader_dir + "/template_match_ncc.spv");
    if (nccCode.empty()) {
        return mirage::Err<void>("Failed to load NCC shader");
    }

    ncc_pipeline_ = std::make_unique<VulkanComputePipeline>();
    if (!ncc_pipeline_->create(ctx, nccCode, nccBindings, sizeof(NccPushConstants))) {
        return mirage::Err<void>("Failed to create NCC compute pipeline");
    }

    // === SAT pipelines (Opt E) ===
    if (config_.enable_sat) {
        std::vector<VkDescriptorSetLayoutBinding> prefixBindings(2);
        prefixBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        prefixBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        auto prefixHCode = loadSPIRV(shader_dir + "/prefix_sum_horizontal.spv");
        if (!prefixHCode.empty()) {
            prefix_h_pipeline_ = std::make_unique<VulkanComputePipeline>();
            if (!prefix_h_pipeline_->create(ctx, prefixHCode, prefixBindings, sizeof(SatPushConstants))) {
                MLOG_WARN("matcher", "SAT horizontal pipeline failed, disabling SAT");
                config_.enable_sat = false;
            } else {
                prefix_h_desc_[0] = prefix_h_pipeline_->allocateDescriptorSet();
                prefix_h_desc_[1] = prefix_h_pipeline_->allocateDescriptorSet();
            }
        } else {
            MLOG_WARN("matcher", "SAT horizontal shader not found, disabling SAT");
            config_.enable_sat = false;
        }

        if (config_.enable_sat) {
            std::vector<VkDescriptorSetLayoutBinding> prefixVBindings(1);
            prefixVBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

            auto prefixVCode = loadSPIRV(shader_dir + "/prefix_sum_vertical.spv");
            if (!prefixVCode.empty()) {
                prefix_v_pipeline_ = std::make_unique<VulkanComputePipeline>();
                if (!prefix_v_pipeline_->create(ctx, prefixVCode, prefixVBindings, sizeof(SatPushConstants))) {
                    MLOG_WARN("matcher", "SAT vertical pipeline failed, disabling SAT");
                    config_.enable_sat = false;
                } else {
                    prefix_v_desc_[0] = prefix_v_pipeline_->allocateDescriptorSet();
                prefix_v_desc_[1] = prefix_v_pipeline_->allocateDescriptorSet();
                }
            } else {
                MLOG_WARN("matcher", "SAT vertical shader not found, disabling SAT");
                config_.enable_sat = false;
            }
        }

        if (config_.enable_sat) {
            std::vector<VkDescriptorSetLayoutBinding> satNccBindings(6);
            satNccBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            satNccBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            satNccBindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            satNccBindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            satNccBindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            satNccBindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

            auto satNccCode = loadSPIRV(shader_dir + "/template_match_sat.spv");
            if (!satNccCode.empty()) {
                sat_ncc_pipeline_ = std::make_unique<VulkanComputePipeline>();
                if (!sat_ncc_pipeline_->create(ctx, satNccCode, satNccBindings, sizeof(SatNccPushConstants))) {
                    MLOG_WARN("matcher", "SAT NCC pipeline failed, disabling SAT");
                    config_.enable_sat = false;
                }
            } else {
                MLOG_WARN("matcher", "SAT NCC shader not found, disabling SAT");
                config_.enable_sat = false;
            }
        }

        if (config_.enable_sat) {
            MLOG_INFO("matcher", "SAT-based NCC enabled (max tpl size: %d)", config_.sat_max_tpl_size);
        }
    }

    // === Pyramid pipeline ===
    std::vector<VkDescriptorSetLayoutBinding> pyrBindings(2);
    pyrBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    pyrBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    auto pyrCode = loadSPIRV(shader_dir + "/pyramid_downsample.spv");
    if (pyrCode.empty()) {
        return mirage::Err<void>("Failed to load pyramid shader");
    }

    pyramid_pipeline_ = std::make_unique<VulkanComputePipeline>();
    if (!pyramid_pipeline_->create(ctx, pyrCode, pyrBindings, 0)) {
        return mirage::Err<void>("Failed to create pyramid compute pipeline");
    }

    pyr_desc_set_ = pyramid_pipeline_->allocateDescriptorSet();
    if (pyr_desc_set_ == VK_NULL_HANDLE) {
        return mirage::Err<void>("Failed to allocate pyramid descriptor set");
    }

    initialized_ = true;
    MLOG_INFO("matcher", "VulkanTemplateMatcher initialized (threshold=%.2f, pyramid=%d levels)",
              config_.default_threshold, config_.pyramid_levels);
    return mirage::Ok();
}

VkDescriptorSet VulkanTemplateMatcher::allocateNccDescSet() {
    VkDescriptorSet ds = ncc_pipeline_->allocateDescriptorSet();
    if (ds == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    VkDescriptorBufferInfo resultBufInfo{result_buf_, 0, sizeof(GpuMatchResult) * MAX_RESULTS};
    VkDescriptorBufferInfo counterBufInfo{counter_buf_, 0, sizeof(int32_t)};

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 2;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &resultBufInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 3;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &counterBufInfo;

    vkUpdateDescriptorSets(ctx_->device(), 2, writes, 0, nullptr);
    return ds;
}

VkDescriptorSet VulkanTemplateMatcher::allocateSatDescSet() {
    if (!sat_ncc_pipeline_) return VK_NULL_HANDLE;
    VkDescriptorSet ds = sat_ncc_pipeline_->allocateDescriptorSet();
    if (ds == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    VkDescriptorBufferInfo resultBufInfo{result_buf_, 0, sizeof(GpuMatchResult) * MAX_RESULTS};
    VkDescriptorBufferInfo counterBufInfo{counter_buf_, 0, sizeof(int32_t)};

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 4;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &resultBufInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 5;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &counterBufInfo;

    vkUpdateDescriptorSets(ctx_->device(), 2, writes, 0, nullptr);
    return ds;
}

void VulkanTemplateMatcher::updateNccDescSetImages(VkDescriptorSet ds,
                                                     VulkanImage* src, VulkanImage* tpl) {
    VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, src->imageView(), VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tplInfo{VK_NULL_HANDLE, tpl->imageView(), VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet imgWrites[2] = {};
    imgWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    imgWrites[0].dstSet = ds;
    imgWrites[0].dstBinding = 0;
    imgWrites[0].descriptorCount = 1;
    imgWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    imgWrites[0].pImageInfo = &srcInfo;

    imgWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    imgWrites[1].dstSet = ds;
    imgWrites[1].dstBinding = 1;
    imgWrites[1].descriptorCount = 1;
    imgWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    imgWrites[1].pImageInfo = &tplInfo;

    vkUpdateDescriptorSets(ctx_->device(), 2, imgWrites, 0, nullptr);
}

mirage::Result<int> VulkanTemplateMatcher::addTemplate(const std::string& name,
                                                        const uint8_t* gray_data,
                                                        int width, int height,
                                                        const std::string& group) {
    if (!initialized_) {
        return mirage::Err<int>("Matcher not initialized");
    }

    auto tpl = std::make_unique<GpuTemplate>();
    tpl->name = name;
    tpl->group = group;
    tpl->width = width;
    tpl->height = height;

    tpl->image = std::make_unique<VulkanImage>();
    if (!tpl->image->create(*ctx_, width, height, VK_FORMAT_R8_UNORM,
                             VK_IMAGE_USAGE_STORAGE_BIT)) {
        return mirage::Err<int>("Failed to create template VulkanImage");
    }

    if (!tpl->image->upload(cmd_pool_, ctx_->computeQueue(), gray_data,
                             width * height)) {
        return mirage::Err<int>("Failed to upload template data");
    }

    tpl->ncc_desc_set = allocateNccDescSet();
    if (tpl->ncc_desc_set == VK_NULL_HANDLE) {
        return mirage::Err<int>("Failed to allocate per-template descriptor set");
    }

    // Precompute template statistics for SAT path (Opt E)
    if (config_.enable_sat && width <= config_.sat_max_tpl_size && height <= config_.sat_max_tpl_size) {
        float sum = 0.0f, sum_sq = 0.0f;
        for (int i = 0; i < width * height; i++) {
            float v = gray_data[i];
            sum += v;
            sum_sq += v * v;
        }
        float n = (float)(width * height);
        tpl->sum_t = sum;
        tpl->sum_tt = sum_sq;
        tpl->denom_t = n * sum_sq - sum * sum;
        tpl->sat_desc_set = allocateSatDescSet();
    }

    if (config_.enable_multi_scale) {
        auto pyrResult = buildPyramid(*tpl);
        if (pyrResult.is_err()) {
            MLOG_WARN("matcher", "Failed to build pyramid for '%s': %s",
                      name.c_str(), pyrResult.error().message.c_str());
        }
    }

    int id = next_id_++;
    templates_[id] = std::move(tpl);
    MLOG_INFO("matcher", "Template added: '%s' id=%d (%dx%d)", name.c_str(), id, width, height);
    return id;
}

mirage::Result<void> VulkanTemplateMatcher::buildPyramid(GpuTemplate& tpl) {
    int w = tpl.width;
    int h = tpl.height;

    for (int level = 1; level < config_.pyramid_levels; level++) {
        int nw = w / 2;
        int nh = h / 2;
        if (nw < 4 || nh < 4) break;

        auto downImg = std::make_unique<VulkanImage>();
        if (!downImg->create(*ctx_, nw, nh, VK_FORMAT_R8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT)) {
            return mirage::Err<void>("Failed to create pyramid level " + std::to_string(level));
        }

        VulkanImage* srcLevel = (level == 1) ? tpl.image.get()
                                              : tpl.pyramid.back().get();

        VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, srcLevel->imageView(), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, downImg->imageView(), VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet pyrWrites[2] = {};
        pyrWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        pyrWrites[0].dstSet = pyr_desc_set_;
        pyrWrites[0].dstBinding = 0;
        pyrWrites[0].descriptorCount = 1;
        pyrWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pyrWrites[0].pImageInfo = &srcInfo;

        pyrWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        pyrWrites[1].dstSet = pyr_desc_set_;
        pyrWrites[1].dstBinding = 1;
        pyrWrites[1].descriptorCount = 1;
        pyrWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pyrWrites[1].pImageInfo = &dstInfo;

        vkUpdateDescriptorSets(ctx_->device(), 2, pyrWrites, 0, nullptr);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd_buf_, 0);
        vkBeginCommandBuffer(cmd_buf_, &beginInfo);

        downImg->transitionLayout(cmd_buf_, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        pyramid_pipeline_->bind(cmd_buf_);
        pyramid_pipeline_->bindDescriptorSet(cmd_buf_, pyr_desc_set_);
        pyramid_pipeline_->dispatch(cmd_buf_, (nw + 15) / 16, (nh + 15) / 16, 1);

        vkEndCommandBuffer(cmd_buf_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd_buf_;

        vkResetFences(ctx_->device(), 1, &fence_);
        vkQueueSubmit(ctx_->computeQueue(), 1, &submitInfo, fence_);
        vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        tpl.pyramid.push_back(std::move(downImg));
        w = nw;
        h = nh;
    }
    return mirage::Ok();
}

bool VulkanTemplateMatcher::buildSourcePyramid(VulkanImage* src, int width, int height) {
    if (src_pyr_w_ == width && src_pyr_h_ == height && !src_pyramid_.empty())
        return true;

    src_pyramid_.clear();
    src_pyr_w_ = width;
    src_pyr_h_ = height;

    int w = width, h = height;
    VulkanImage* prev = src;

    for (int level = 1; level < config_.pyramid_levels; level++) {
        int nw = w / 2;
        int nh = h / 2;
        if (nw < 4 || nh < 4) break;

        auto downImg = std::make_unique<VulkanImage>();
        if (!downImg->create(*ctx_, nw, nh, VK_FORMAT_R8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT)) {
            src_pyramid_.clear();
            return false;
        }

        VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, prev->imageView(), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, downImg->imageView(), VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet pyrWrites[2] = {};
        pyrWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        pyrWrites[0].dstSet = pyr_desc_set_;
        pyrWrites[0].dstBinding = 0;
        pyrWrites[0].descriptorCount = 1;
        pyrWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pyrWrites[0].pImageInfo = &srcInfo;

        pyrWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        pyrWrites[1].dstSet = pyr_desc_set_;
        pyrWrites[1].dstBinding = 1;
        pyrWrites[1].descriptorCount = 1;
        pyrWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pyrWrites[1].pImageInfo = &dstInfo;

        vkUpdateDescriptorSets(ctx_->device(), 2, pyrWrites, 0, nullptr);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd_buf_, 0);
        vkBeginCommandBuffer(cmd_buf_, &beginInfo);

        downImg->transitionLayout(cmd_buf_, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        pyramid_pipeline_->bind(cmd_buf_);
        pyramid_pipeline_->bindDescriptorSet(cmd_buf_, pyr_desc_set_);
        pyramid_pipeline_->dispatch(cmd_buf_, (nw + 15) / 16, (nh + 15) / 16, 1);

        VkMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd_buf_,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &bar, 0, nullptr, 0, nullptr);

        vkEndCommandBuffer(cmd_buf_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd_buf_;

        vkResetFences(ctx_->device(), 1, &fence_);
        vkQueueSubmit(ctx_->computeQueue(), 1, &submitInfo, fence_);
        vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        prev = downImg.get();
        src_pyramid_.push_back(std::move(downImg));
        w = nw;
        h = nh;
    }
    return true;
}

bool VulkanTemplateMatcher::buildSAT(VulkanImage* gray_image, int width, int height) {
    if (!config_.enable_sat || !prefix_h_pipeline_ || !prefix_v_pipeline_)
        return false;

    if (sat_w_ != width || sat_h_ != height) {
        sat_s_.reset();
        sat_ss_.reset();

        sat_s_ = std::make_unique<VulkanImage>();
        if (!sat_s_->create(*ctx_, width, height, VK_FORMAT_R32_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT))
            return false;

        sat_ss_ = std::make_unique<VulkanImage>();
        if (!sat_ss_->create(*ctx_, width, height, VK_FORMAT_R32_SFLOAT,
                              VK_IMAGE_USAGE_STORAGE_BIT)) {
            sat_s_.reset();
            return false;
        }
        sat_w_ = width;
        sat_h_ = height;
    }

    // Pre-update all 4 descriptor sets before recording command buffer
    VulkanImage* satImages[2] = { sat_s_.get(), sat_ss_.get() };
    for (int i = 0; i < 2; i++) {
        VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, gray_image->imageView(), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, satImages[i]->imageView(), VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = prefix_h_desc_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &srcInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = prefix_h_desc_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dstInfo;
        vkUpdateDescriptorSets(ctx_->device(), 2, writes, 0, nullptr);

        VkDescriptorImageInfo satInfo{VK_NULL_HANDLE, satImages[i]->imageView(), VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet vWrite{};
        vWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vWrite.dstSet = prefix_v_desc_[i];
        vWrite.dstBinding = 0;
        vWrite.descriptorCount = 1;
        vWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        vWrite.pImageInfo = &satInfo;
        vkUpdateDescriptorSets(ctx_->device(), 1, &vWrite, 0, nullptr);
    }

    // Single command buffer with all 4 dispatches
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd_buf_, 0);
    vkBeginCommandBuffer(cmd_buf_, &beginInfo);

    // Transition both SAT images
    sat_s_->transitionLayout(cmd_buf_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    sat_ss_->transitionLayout(cmd_buf_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    VkMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    for (int i = 0; i < 2; i++) {
        SatPushConstants pc{width, height, i, 0};

        // Horizontal prefix sum
        prefix_h_pipeline_->bind(cmd_buf_);
        prefix_h_pipeline_->bindDescriptorSet(cmd_buf_, prefix_h_desc_[i]);
        prefix_h_pipeline_->pushConstants(cmd_buf_, &pc, sizeof(pc));
        prefix_h_pipeline_->dispatch(cmd_buf_, height, 1, 1);

        // Barrier: H-prefix write -> V-prefix read
        vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &bar, 0, nullptr, 0, nullptr);

        // Vertical prefix sum
        SatPushConstants vpc{width, height, 0, 0};
        prefix_v_pipeline_->bind(cmd_buf_);
        prefix_v_pipeline_->bindDescriptorSet(cmd_buf_, prefix_v_desc_[i]);
        prefix_v_pipeline_->pushConstants(cmd_buf_, &vpc, sizeof(vpc));
        prefix_v_pipeline_->dispatch(cmd_buf_, width, 1, 1);

        // Barrier: V-prefix write -> next iteration or NCC read
        vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &bar, 0, nullptr, 0, nullptr);
    }

    vkEndCommandBuffer(cmd_buf_);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd_buf_;
    vkResetFences(ctx_->device(), 1, &fence_);
    vkQueueSubmit(ctx_->computeQueue(), 1, &submitInfo, fence_);
    vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

    sat_built_ = true;
    return true;
}

void VulkanTemplateMatcher::resetCounter() {
    void* data;
    vkMapMemory(ctx_->device(), counter_mem_, 0, sizeof(int32_t), 0, &data);
    memset(data, 0, sizeof(int32_t));
    vkUnmapMemory(ctx_->device(), counter_mem_);
}

bool VulkanTemplateMatcher::readResults(std::vector<VkMatchResult>& results) {
    void* counterData;
    vkMapMemory(ctx_->device(), counter_mem_, 0, sizeof(int32_t), 0, &counterData);
    int32_t count = 0;
    memcpy(&count, counterData, sizeof(count));
    vkUnmapMemory(ctx_->device(), counter_mem_);

    count = std::min(count, (int32_t)MAX_RESULTS);
    if (count <= 0) return true;

    void* resultData;
    vkMapMemory(ctx_->device(), result_mem_, 0, sizeof(GpuMatchResult) * count, 0, &resultData);
    const GpuMatchResult* gpuResults = static_cast<const GpuMatchResult*>(resultData);

    results.reserve(count);
    for (int32_t i = 0; i < count; i++) {
        VkMatchResult r;
        r.x = gpuResults[i].x;
        r.y = gpuResults[i].y;
        r.score = gpuResults[i].score;
        r.template_id = gpuResults[i].template_id;

        // テンプレートサイズ情報をマップから補完
        auto it = templates_.find(r.template_id);
        if (it != templates_.end()) {
            r.template_width  = it->second->width;
            r.template_height = it->second->height;
        }
        // 中心座標を計算（左上 + サイズ/2）
        r.center_x = r.x + r.template_width / 2;
        r.center_y = r.y + r.template_height / 2;

        results.push_back(r);
    }
    vkUnmapMemory(ctx_->device(), result_mem_);
    return true;
}

bool VulkanTemplateMatcher::dispatchNcc(VkDescriptorSet desc_set,
                                         VulkanImage* src, VulkanImage* tpl,
                                         int src_w, int src_h,
                                         int tpl_w, int tpl_h,
                                         int template_id, float threshold) {
    int search_w = src_w - tpl_w + 1;
    int search_h = src_h - tpl_h + 1;
    if (search_w <= 0 || search_h <= 0) return true;

    updateNccDescSetImages(desc_set, src, tpl);

    NccPushConstants pc{};
    pc.src_width = src_w;  pc.src_height = src_h;
    pc.tpl_width = tpl_w;  pc.tpl_height = tpl_h;
    pc.template_id = template_id;  pc.threshold = threshold;
    pc.search_width = search_w;  pc.search_height = search_h;

    ncc_pipeline_->bind(cmd_buf_);
    ncc_pipeline_->bindDescriptorSet(cmd_buf_, desc_set);
    ncc_pipeline_->pushConstants(cmd_buf_, &pc, sizeof(pc));
    ncc_pipeline_->dispatch(cmd_buf_, (search_w + 15) / 16, (search_h + 15) / 16, 1);

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    return true;
}

bool VulkanTemplateMatcher::dispatchSatNcc(GpuTemplate& tpl, VulkanImage* src,
                                            int src_w, int src_h, int template_id) {
    if (!sat_ncc_pipeline_ || !sat_built_ || !sat_s_ || !sat_ss_) return false;
    if (tpl.sat_desc_set == VK_NULL_HANDLE) return false;

    int search_w = src_w - tpl.width + 1;
    int search_h = src_h - tpl.height + 1;
    if (search_w <= 0 || search_h <= 0) return true;

    VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, src->imageView(), VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tplInfo{VK_NULL_HANDLE, tpl.image->imageView(), VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo satSInfo{VK_NULL_HANDLE, sat_s_->imageView(), VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo satSSInfo{VK_NULL_HANDLE, sat_ss_->imageView(), VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet imgWrites[4] = {};
    for (int i = 0; i < 4; i++) {
        imgWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        imgWrites[i].dstSet = tpl.sat_desc_set;
        imgWrites[i].dstBinding = i;
        imgWrites[i].descriptorCount = 1;
        imgWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    imgWrites[0].pImageInfo = &srcInfo;
    imgWrites[1].pImageInfo = &tplInfo;
    imgWrites[2].pImageInfo = &satSInfo;
    imgWrites[3].pImageInfo = &satSSInfo;
    vkUpdateDescriptorSets(ctx_->device(), 4, imgWrites, 0, nullptr);

    float n = (float)(tpl.width * tpl.height);
    SatNccPushConstants pc{};
    pc.src_width = src_w;  pc.src_height = src_h;
    pc.tpl_width = tpl.width;  pc.tpl_height = tpl.height;
    pc.template_id = template_id;  pc.threshold = config_.default_threshold;
    pc.search_width = search_w;  pc.search_height = search_h;
    pc.sum_t = tpl.sum_t;  pc.sum_tt = tpl.sum_tt;
    pc.inv_n = 1.0f / n;  pc.denom_t = tpl.denom_t;

    sat_ncc_pipeline_->bind(cmd_buf_);
    sat_ncc_pipeline_->bindDescriptorSet(cmd_buf_, tpl.sat_desc_set);
    sat_ncc_pipeline_->pushConstants(cmd_buf_, &pc, sizeof(pc));
    sat_ncc_pipeline_->dispatch(cmd_buf_, (search_w + 15) / 16, (search_h + 15) / 16, 1);

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    return true;
}

mirage::Result<std::vector<VkMatchResult>> VulkanTemplateMatcher::matchGpu(
    VulkanImage* gray_image, int width, int height) {
    if (!initialized_) return mirage::Err<std::vector<VkMatchResult>>("Not initialized");
    if (templates_.empty()) return std::vector<VkMatchResult>{};

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<VkMatchResult> results;
    sat_built_ = false;

    bool any_sat = false;
    if (config_.enable_sat) {
        for (const auto& [id, tpl] : templates_) {
            if (tpl->sat_desc_set != VK_NULL_HANDLE) { any_sat = true; break; }
        }
        if (any_sat) {
            buildSAT(gray_image, width, height);
        }
    }

    if (config_.enable_multi_scale && config_.pyramid_levels >= 2) {
        if (!buildSourcePyramid(gray_image, width, height)) {
            goto direct_match;
        }

        int coarse_level = (int)src_pyramid_.size() - 1;
        if (coarse_level < 0) goto direct_match;

        {
        VulkanImage* coarse_src = src_pyramid_[coarse_level].get();
        int scale = 1 << (coarse_level + 1);
        int cw = width / scale;
        int ch = height / scale;

        resetCounter();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd_buf_, 0);
        vkBeginCommandBuffer(cmd_buf_, &beginInfo);

        for (const auto& [id, tpl] : templates_) {
            if (!tpl->image) continue;

            VulkanImage* coarse_tpl = tpl->image.get();
            int ctw = tpl->width, cth = tpl->height;

            if (!tpl->pyramid.empty()) {
                int tpl_level = std::min(coarse_level, (int)tpl->pyramid.size() - 1);
                coarse_tpl = tpl->pyramid[tpl_level].get();
                int tpl_scale = 1 << (tpl_level + 1);
                ctw = tpl->width / tpl_scale;
                cth = tpl->height / tpl_scale;
            }

            if (ctw < 4 || cth < 4) {
                dispatchNcc(tpl->ncc_desc_set, gray_image, tpl->image.get(),
                           width, height, tpl->width, tpl->height, id,
                           config_.default_threshold);
                continue;
            }

            dispatchNcc(tpl->ncc_desc_set, coarse_src, coarse_tpl,
                       cw, ch, ctw, cth, id, config_.coarse_threshold);
        }

        vkEndCommandBuffer(cmd_buf_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd_buf_;

        vkResetFences(ctx_->device(), 1, &fence_);
        vkQueueSubmit(ctx_->computeQueue(), 1, &submitInfo, fence_);
        vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        std::vector<VkMatchResult> coarse_results;
        readResults(coarse_results);

        if (coarse_results.empty()) goto finish;

        resetCounter();
        vkResetCommandBuffer(cmd_buf_, 0);
        vkBeginCommandBuffer(cmd_buf_, &beginInfo);

        std::unordered_map<int, bool> desc_updated;

        for (const auto& cr : coarse_results) {
            auto it = templates_.find(cr.template_id);
            if (it == templates_.end()) continue;
            auto& tpl = it->second;

            int fx = cr.x * scale;
            int fy = cr.y * scale;
            int radius = config_.refine_radius;
            int rx = std::max(0, fx - radius);
            int ry = std::max(0, fy - radius);
            int rw = std::min(width - tpl->width, fx + radius) - rx + 1;
            int rh = std::min(height - tpl->height, fy + radius) - ry + 1;

            if (rw <= 0 || rh <= 0) continue;

            if (!desc_updated[cr.template_id]) {
                updateNccDescSetImages(tpl->ncc_desc_set, gray_image, tpl->image.get());
                desc_updated[cr.template_id] = true;
            }

            NccPushConstants pc{};
            pc.src_width = width;  pc.src_height = height;
            pc.tpl_width = tpl->width;  pc.tpl_height = tpl->height;
            pc.template_id = cr.template_id;  pc.threshold = config_.default_threshold;
            pc.search_width = rw;  pc.search_height = rh;

            ncc_pipeline_->bind(cmd_buf_);
            ncc_pipeline_->bindDescriptorSet(cmd_buf_, tpl->ncc_desc_set);
            ncc_pipeline_->pushConstants(cmd_buf_, &pc, sizeof(pc));
            ncc_pipeline_->dispatch(cmd_buf_, (rw + 15) / 16, (rh + 15) / 16, 1);

            VkMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &bar, 0, nullptr, 0, nullptr);
        }

        vkEndCommandBuffer(cmd_buf_);
        vkResetFences(ctx_->device(), 1, &fence_);
        vkQueueSubmit(ctx_->computeQueue(), 1, &submitInfo, fence_);
        vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);

        readResults(results);
        goto finish;
        } // end multi-scale block
    }

direct_match:
    {
        resetCounter();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd_buf_, 0);
        vkBeginCommandBuffer(cmd_buf_, &beginInfo);

        for (const auto& [id, tpl] : templates_) {
            if (!tpl->image) continue;

            if (sat_built_ && tpl->sat_desc_set != VK_NULL_HANDLE) {
                dispatchSatNcc(*tpl, gray_image, width, height, id);
            } else {
                dispatchNcc(tpl->ncc_desc_set, gray_image, tpl->image.get(),
                           width, height, tpl->width, tpl->height, id,
                           config_.default_threshold);
            }
        }

        vkEndCommandBuffer(cmd_buf_);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd_buf_;

        vkResetFences(ctx_->device(), 1, &fence_);
        if (vkQueueSubmit(ctx_->computeQueue(), 1, &submitInfo, fence_) != VK_SUCCESS) {
            return mirage::Err<std::vector<VkMatchResult>>("Failed to submit NCC compute");
        }

        vkWaitForFences(ctx_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);
        readResults(results);
    }

finish:
    {
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats_.match_calls++;
        stats_.last_time_ms = ms;
        stats_.avg_time_ms = stats_.avg_time_ms * 0.9 + ms * 0.1;

        if (stats_.match_calls % 100 == 0) {
            MLOG_INFO("matcher", "%.1f ms (avg %.1f ms, %llu calls, %zu matches)",
                      ms, stats_.avg_time_ms, (unsigned long long)stats_.match_calls, results.size());
        }
    }

    return results;
}

mirage::Result<std::vector<VkMatchResult>> VulkanTemplateMatcher::match(
    const uint8_t* gray_data, int width, int height) {
    if (!temp_src_ || temp_src_w_ != width || temp_src_h_ != height) {
        temp_src_ = std::make_unique<VulkanImage>();
        if (!temp_src_->create(*ctx_, width, height, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_STORAGE_BIT)) {
            return mirage::Err<std::vector<VkMatchResult>>("Failed to create temp source image");
        }
        temp_src_w_ = width;
        temp_src_h_ = height;
    }

    if (!temp_src_->upload(cmd_pool_, ctx_->computeQueue(), gray_data,
                            width * height)) {
        return mirage::Err<std::vector<VkMatchResult>>("Failed to upload source frame");
    }

    return matchGpu(temp_src_.get(), width, height);
}

void VulkanTemplateMatcher::clearAll() {
    templates_.clear();
    src_pyramid_.clear();
    src_pyr_w_ = 0;
    src_pyr_h_ = 0;
    sat_s_.reset();
    sat_ss_.reset();
    sat_w_ = 0;
    sat_h_ = 0;
    sat_built_ = false;
    temp_src_.reset();
    temp_src_w_ = 0;
    temp_src_h_ = 0;
    next_id_ = 0;
}

} // namespace mirage::vk
