#pragma once

#include "vulkan/vulkan_context.hpp"
#include "vulkan/vulkan_image.hpp"
#include "vulkan/vulkan_compute.hpp"
#include "result.hpp"

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <chrono>

namespace mirage::vk {

struct VkMatchResult {
    int x = 0;
    int y = 0;
    float score = 0.0f;
    int template_id = 0;
    // テンプレートサイズ情報（addTemplate時に設定）
    int template_width = 0;
    int template_height = 0;
    // マッチ中心座標（左上 + サイズ/2）
    int center_x = 0;
    int center_y = 0;
};

struct VkMatcherConfig {
    float default_threshold = 0.80f;
    bool enable_multi_scale = true;
    int max_results = 1024;
    int pyramid_levels = 3;
    float coarse_threshold = 0.50f;
    int refine_radius = 4;
    bool enable_sat = true;           // Enable SAT-based NCC (Opt E)
    int sat_max_tpl_size = 48;        // Max template dimension for SAT path
};

struct GpuTemplate {
    std::string name;
    std::string group;
    int width = 0;
    int height = 0;
    std::unique_ptr<VulkanImage> image;
    std::vector<std::unique_ptr<VulkanImage>> pyramid;

    VkDescriptorSet ncc_desc_set = VK_NULL_HANDLE;
    VkDescriptorSet sat_desc_set = VK_NULL_HANDLE;  // SAT NCC descriptor

    // Precomputed template statistics for SAT path
    float sum_t = 0.0f;
    float sum_tt = 0.0f;
    float denom_t = 0.0f;

    // 改善E: 検索ROI ピクセル座標（roi_w=-1 で全画面）
    int roi_x = 0, roi_y = 0, roi_w = -1, roi_h = -1;
    // 改善E: 正規化ROI（0.0-1.0）- マニフェスト設定用。matchGpu時にフレームサイズで変換
    float roi_norm_x = 0.0f;
    float roi_norm_y = 0.0f;
    float roi_norm_w = 0.0f;  // 0 = 正規化ROI無効（ピクセルROIまたは全画面）
    float roi_norm_h = 0.0f;
};

class VulkanTemplateMatcher {
public:
    VulkanTemplateMatcher() = default;
    ~VulkanTemplateMatcher();

    mirage::Result<void> initialize(VulkanContext& ctx, const VkMatcherConfig& config,
                                    const std::string& shader_dir);

    mirage::Result<int> addTemplate(const std::string& name, const uint8_t* gray_data,
                                    int width, int height, const std::string& group);

    mirage::Result<std::vector<VkMatchResult>> matchGpu(VulkanImage* gray_image,
                                                         int width, int height);

    mirage::Result<std::vector<VkMatchResult>> match(const uint8_t* gray_data,
                                                      int width, int height);

    int getTemplateCount() const { return static_cast<int>(templates_.size()); }
    void clearAll();
    bool isInitialized() const { return initialized_; }

    // 改善E: テンプレートROI設定
    // 正規化座標版（0.0-1.0）- フレームサイズに依存しない
    bool setTemplateRoiNorm(const std::string& name,
                            float norm_x, float norm_y,
                            float norm_w, float norm_h);
    // ピクセル直接指定版
    bool setTemplateRoi(const std::string& name,
                        int px_x, int px_y, int px_w, int px_h);

    struct Stats {
        uint64_t match_calls = 0;
        double last_time_ms = 0.0;
        double avg_time_ms = 0.0;
    };
    Stats getStats() const { return stats_; }

private:
    mirage::Result<void> buildPyramid(GpuTemplate& tpl);
    bool buildSourcePyramid(VulkanImage* src, int width, int height);
    bool buildSAT(VulkanImage* gray_image, int width, int height);
    bool dispatchNcc(VkDescriptorSet desc_set,
                     VulkanImage* src, VulkanImage* tpl,
                     int src_w, int src_h, int tpl_w, int tpl_h,
                     int template_id, float threshold,
                     int search_x = 0, int search_y = 0,
                     int search_w_override = -1, int search_h_override = -1);
    bool dispatchSatNcc(GpuTemplate& tpl, VulkanImage* src,
                        int src_w, int src_h, int template_id,
                        int search_x = 0, int search_y = 0,
                        int search_w_override = -1, int search_h_override = -1);
    bool readResults(std::vector<VkMatchResult>& results);
    void resetCounter();
    VkDescriptorSet allocateNccDescSet();
    VkDescriptorSet allocateSatDescSet();
    void updateNccDescSetImages(VkDescriptorSet ds, VulkanImage* src, VulkanImage* tpl);

    VulkanContext* ctx_ = nullptr;
    VkMatcherConfig config_;
    bool initialized_ = false;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buf_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    // NCC pipeline (tile-based)
    std::unique_ptr<VulkanComputePipeline> ncc_pipeline_;

    // SAT pipelines (Opt E)
    std::unique_ptr<VulkanComputePipeline> prefix_h_pipeline_;
    std::unique_ptr<VulkanComputePipeline> prefix_v_pipeline_;
    std::unique_ptr<VulkanComputePipeline> sat_ncc_pipeline_;
    VkDescriptorSet prefix_h_desc_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSet prefix_v_desc_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // SAT images (reused per frame)
    std::unique_ptr<VulkanImage> sat_s_;
    std::unique_ptr<VulkanImage> sat_ss_;
    int sat_w_ = 0;
    int sat_h_ = 0;
    bool sat_built_ = false;

    // Pyramid pipeline
    std::unique_ptr<VulkanComputePipeline> pyramid_pipeline_;
    VkDescriptorSet pyr_desc_set_ = VK_NULL_HANDLE;

    // Result & counter buffers
    VkBuffer result_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory result_mem_ = VK_NULL_HANDLE;
    VkBuffer counter_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory counter_mem_ = VK_NULL_HANDLE;

    // Templates
    std::unordered_map<int, std::unique_ptr<GpuTemplate>> templates_;
    int next_id_ = 0;

    // Source pyramid for coarse-to-fine
    std::vector<std::unique_ptr<VulkanImage>> src_pyramid_;
    int src_pyr_w_ = 0;
    int src_pyr_h_ = 0;

    // Temp source image for CPU->GPU path
    std::unique_ptr<VulkanImage> temp_src_;
    int temp_src_w_ = 0;
    int temp_src_h_ = 0;

    Stats stats_;
};

} // namespace mirage::vk
