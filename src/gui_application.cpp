// =============================================================================
// MirageSystem v2 - GUI Implementation Part 1
// =============================================================================
// Vulkan initialization, ImGui setup, resource management
// =============================================================================

#include "gui_application.hpp"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <windows.h>
#include "mirage_log.hpp"
#include "mirage_config.hpp"

// ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mirage::gui {

// =============================================================================
// Constructor / Destructor
// =============================================================================

GuiApplication::GuiApplication() = default;

GuiApplication::~GuiApplication() {
    shutdown();
}

// =============================================================================
// Initialization
// =============================================================================

bool GuiApplication::initialize(HWND hwnd, const GuiConfig& config) {
    config_ = config;
    hwnd_ = hwnd;
    
    // Get window size
    RECT rect;
    GetClientRect(hwnd, &rect);
    window_width_ = rect.right - rect.left;
    window_height_ = rect.bottom - rect.top;
    
    // Initialize Vulkan backend
    MLOG_INFO("app", "Initializing Vulkan backend...");
    if (!createVulkanResources(hwnd) || !setupImGuiVulkan(hwnd)) {
        MLOG_ERROR("app", "Vulkan initialization failed");
        cleanupVulkanResources();
        return false;
    }
    vulkan_initialized_ = true;
    imgui_initialized_ = true;
    MLOG_INFO("app", "Vulkan backend initialized");

    logInfo("GUI initialized: " + std::to_string(window_width_) + "x" +
            std::to_string(window_height_));

    return true;
}

void GuiApplication::shutdown() {
    if (imgui_initialized_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
    }
    cleanupVulkanResources();
}

// =============================================================================
// Resize Handling
// =============================================================================

void GuiApplication::onResize(int width, int height) {
    MLOG_INFO("resize", "onResize(%d, %d) called", width, height);
    if (width <= 0 || height <= 0) return;
    if (width == window_width_ && height == window_height_) {
        MLOG_INFO("resize", "same size %dx%d, skipping", width, height);
        return;
    }

    // Set resizing flag to prevent render during resize
    resizing_.store(true);

    window_width_ = width;
    window_height_ = height;

    // Recreate Vulkan swapchain
    if (vk_swapchain_ && vk_context_) {
        vkDeviceWaitIdle(vk_context_->device());
        vk_swapchain_->recreate(width, height);
    }

    // Update font scale based on window height (base: 1080p)
    if (imgui_initialized_) {
        float scale = static_cast<float>(height) / 1080.0f;
        current_font_scale_ = scale;
        ImGuiIO& io = ImGui::GetIO();
        io.FontGlobalScale = scale;
    }

    resizing_.store(false);
}

// =============================================================================
// Device Management
// =============================================================================

void GuiApplication::addDevice(const std::string& id, const std::string& name) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    if (devices_.find(id) == devices_.end()) {
        DeviceInfo info;
        info.id = id;
        info.name = name;
        info.status = DeviceStatus::Idle;
        devices_[id] = std::move(info);
        device_order_.push_back(id);
        
        // Set as main if first device
        if (main_device_id_.empty()) {
            main_device_id_ = id;
            {
                std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
                main_view_rect_.valid = false;  // Reset view rect when main device changes
            }
        }
        
        logInfo("Device added: " + name + " (" + id + ")");
    }
}

void GuiApplication::removeDevice(const std::string& id) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    devices_.erase(id);
    device_order_.erase(
        std::remove(device_order_.begin(), device_order_.end(), id),
        device_order_.end()
    );
    
    if (main_device_id_ == id) {
        main_device_id_ = device_order_.empty() ? "" : device_order_[0];
        {
            std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
            main_view_rect_.valid = false;  // Reset view rect when main device changes
        }
    }
    
    logInfo("Device removed: " + id);
}

void GuiApplication::setMainDevice(const std::string& id) {
    bool should_callback = false;
    std::string callback_id;
    
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        
        if (devices_.find(id) != devices_.end()) {
            main_device_id_ = id;
            {
                std::lock_guard<std::mutex> rect_lock(view_rect_mutex_);
                main_view_rect_.valid = false;  // Reset view rect when main device changes
            }
            callback_id = id;
            should_callback = true;
        }
    }
    
    // Call callback OUTSIDE mutex to avoid deadlock
    if (should_callback && device_select_callback_) {
        device_select_callback_(callback_id);
    }
}

void GuiApplication::updateDeviceStatus(const std::string& id, DeviceStatus status) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    auto it = devices_.find(id);
    if (it != devices_.end()) {
        if (it->second.status != status) {
            it->second.status = status;
            it->second.status_changed_at = getCurrentTimeMs();
        }
    }
}

void GuiApplication::updateDeviceFrame(const std::string& id,
                                        const uint8_t* rgba_data,
                                        int width, int height) {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    auto it = devices_.find(id);
    if (it == devices_.end()) return;

    DeviceInfo& device = it->second;

    // Create or resize Vulkan texture if needed
    if (!device.vk_texture ||
        device.texture_width != width ||
        device.texture_height != height) {
        device.vk_texture = std::make_shared<mirage::vk::VulkanTexture>();
        if (!device.vk_texture->create(*vk_context_, vk_descriptor_pool_, width, height)) {
            MLOG_ERROR("app", "Failed to create Vulkan texture for %s", device.id.c_str());
            device.vk_texture.reset();

            return;
        }
        device.texture_width = width;
        device.texture_height = height;
        device.vk_texture_ds = device.vk_texture->imguiDescriptorSet();

    }

    // Update texture data
    device.vk_texture->update(vk_command_pool_, vk_context_->graphicsQueue(), rgba_data, width, height);

    // Update stats
    device.frame_count++;
    device.last_frame_time = getCurrentTimeMs();
}

// Thread-safe frame queue - can be called from any thread
void GuiApplication::queueFrame(const std::string& id,
                                 const uint8_t* rgba_data,
                                 int width, int height) {
    if (!rgba_data || width <= 0 || height <= 0) return;

    size_t data_size = static_cast<size_t>(width) * height * 4;

    {
        std::lock_guard<std::mutex> lock(pending_frames_mutex_);

        // Overwrite existing entry for this device (auto-discard old frame)
        auto& frame = pending_frames_[id];
        frame.width = width;
        frame.height = height;
        frame.rgba_data.resize(data_size);
        std::memcpy(frame.rgba_data.data(), rgba_data, data_size);
    }

    static std::atomic<int> dbg_count{0};
    if (dbg_count.fetch_add(1) < 10) {
        MLOG_INFO("app", "[queueFrame] device=%s w=%d h=%d (latest only)", id.c_str(), width, height);
    }
}

// Process pending frames - MUST be called from main thread only
void GuiApplication::processPendingFrames() {
    std::map<std::string, PendingFrame> frames_to_process;
    {
        std::lock_guard<std::mutex> lock(pending_frames_mutex_);
        frames_to_process.swap(pending_frames_);
    }

    if (frames_to_process.empty()) return;

    static std::atomic<int> dbg_count{0};
    if (dbg_count.load() < 10) {
        MLOG_INFO("app", "[processPendingFrames] devices=%zu", frames_to_process.size());
    }

    for (auto& [device_id, frame] : frames_to_process) {
        if (dbg_count.fetch_add(1) < 10) {
            MLOG_INFO("app", "[processPendingFrames] -> updateDeviceFrame device=%s w=%d h=%d",
                      device_id.c_str(), frame.width, frame.height);
        }
        updateDeviceFrame(device_id, frame.rgba_data.data(), frame.width, frame.height);
    }
}

void GuiApplication::updateDeviceOverlays(const std::string& id,
                                           const std::vector<DeviceInfo::MatchOverlay>& overlays) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    auto it = devices_.find(id);
    if (it != devices_.end()) {
        it->second.overlays = overlays;
    }
}

void GuiApplication::updateDeviceStats(const std::string& id, 
                                        float fps, float latency_ms, float bandwidth_mbps) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    
    auto it = devices_.find(id);
    if (it != devices_.end()) {
        it->second.fps = fps;
        it->second.latency_ms = latency_ms;
        it->second.bandwidth_mbps = bandwidth_mbps;
    }
}

// =============================================================================
// Logging
// =============================================================================

void GuiApplication::log(LogEntry::Level level, const std::string& message, 
                          const std::string& source) {
    std::lock_guard<std::mutex> lock(logs_mutex_);
    
    LogEntry entry;
    entry.level = level;
    entry.message = message;
    entry.source = source;
    entry.timestamp = getCurrentTimeMs();
    
    logs_.push_back(std::move(entry));
    
    // Trim old entries
    while (logs_.size() > static_cast<size_t>(config_.max_log_entries)) {
        logs_.pop_front();
    }
}

// =============================================================================
// Learning Mode
// =============================================================================

void GuiApplication::startLearningSession(const std::string& name) {
    learning_session_.active = true;
    learning_session_.session_name = name;
    learning_session_.started_at = getCurrentTimeMs();
    learning_session_.collected_clicks.clear();
    
    logInfo("Learning session started: " + name);
}

void GuiApplication::stopLearningSession() {
    if (learning_session_.active) {
        logInfo("Learning session stopped: " + 
                std::to_string(learning_session_.collected_clicks.size()) + " clicks collected");
    }
    learning_session_.active = false;
}

void GuiApplication::exportLearningData() {
    if (learning_session_.collected_clicks.empty()) {
        logWarning(u8"\u30a8\u30af\u30b9\u30dd\u30fc\u30c8\u3059\u308b\u30c7\u30fc\u30bf\u304c\u3042\u308a\u307e\u305b\u3093");
        return;
    }

    // Generate filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_s(&tm_buf, &time_t);
    char filename[256];
    snprintf(filename, sizeof(filename), "learning_%04d%02d%02d_%02d%02d%02d.json",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    // Export to exe directory (portable, no hardcoded path)
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string export_dir(exe_path);
    auto slash_pos = export_dir.find_last_of("\\/");
    if (slash_pos != std::string::npos) export_dir = export_dir.substr(0, slash_pos + 1);
    else export_dir = "./";
    std::string path = export_dir + filename;

    // Write JSON
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        logError(u8"\u30d5\u30a1\u30a4\u30eb\u4f5c\u6210\u5931\u6557: " + path);
        return;
    }

    ofs << "{\n";
    ofs << "  \"session\": \"" << learning_session_.session_name << "\",\n";
    ofs << "  \"started_at\": " << learning_session_.started_at << ",\n";
    ofs << "  \"clicks\": [\n";

    for (size_t i = 0; i < learning_session_.collected_clicks.size(); i++) {
        const auto& click = learning_session_.collected_clicks[i];
        ofs << "    {";
        ofs << "\"x\": " << click.click_x << ", ";
        ofs << "\"y\": " << click.click_y << ", ";
        ofs << "\"timestamp\": " << click.timestamp << ", ";
        ofs << "\"scene\": \"" << click.scene_name << "\"";

        if (!click.relative_positions.empty()) {
            ofs << ", \"relatives\": [";
            for (size_t j = 0; j < click.relative_positions.size(); j++) {
                const auto& rp = click.relative_positions[j];
                ofs << "{\"id\": \"" << rp.element_id << "\", ";
                ofs << "\"dx\": " << rp.dx << ", \"dy\": " << rp.dy << ", ";
                ofs << "\"dist\": " << rp.distance << "}";
                if (j + 1 < click.relative_positions.size()) ofs << ", ";
            }
            ofs << "]";
        }

        ofs << "}";
        if (i + 1 < learning_session_.collected_clicks.size()) ofs << ",";
        ofs << "\n";
    }

    ofs << "  ]\n}\n";
    ofs.close();

    logInfo(u8"\u30c7\u30fc\u30bf\u30a8\u30af\u30b9\u30dd\u30fc\u30c8\u5b8c\u4e86: " + std::string(filename) +
            " (" + std::to_string(learning_session_.collected_clicks.size()) + " clicks)");
}

LearningClickData GuiApplication::collectLearningData(const DeviceInfo& device, int x, int y) {
    LearningClickData data;
    data.click_x = x;
    data.click_y = y;
    data.timestamp = getCurrentTimeMs();
    data.visible_elements = device.overlays;
    
    // Calculate relative positions to nearby elements
    for (const auto& overlay : device.overlays) {
        int cx = overlay.x + overlay.w / 2;
        int cy = overlay.y + overlay.h / 2;
        int dx = x - cx;
        int dy = y - cy;
        float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
        
        // Include elements within reasonable distance
        if (distance < 500) {
            LearningClickData::RelativePosition rel;
            rel.element_id = overlay.template_id;
            rel.dx = dx;
            rel.dy = dy;
            rel.distance = distance;
            data.relative_positions.push_back(rel);
        }
    }
    
    // Sort by distance
    std::sort(data.relative_positions.begin(), data.relative_positions.end(),
              [](const auto& a, const auto& b) { return a.distance < b.distance; });
    
    return data;
}



// =============================================================================
// Vulkan Resource Management
// =============================================================================

bool GuiApplication::createVulkanResources(HWND hwnd) {
    vk_context_ = std::make_unique<mirage::vk::VulkanContext>();
    if (!vk_context_->initialize("MirageSystem")) {
        MLOG_ERROR("app", "Vulkan context init failed");
        return false;
    }

    VkSurfaceKHR surface = vk_context_->createSurface(hwnd);
    if (surface == VK_NULL_HANDLE) return false;

    MLOG_INFO("app", "Creating Vulkan swapchain (%dx%d)...", window_width_, window_height_);
    vk_swapchain_ = std::make_unique<mirage::vk::VulkanSwapchain>();
    if (!vk_swapchain_->create(*vk_context_, surface, window_width_, window_height_)) {
        MLOG_ERROR("app", "Vulkan swapchain creation failed");
        return false;
    }
    MLOG_INFO("app", "Vulkan swapchain created (%u images)", vk_swapchain_->imageCount());

    MLOG_INFO("app", "Vulkan swapchain OK, creating resources...");
    VkDevice dev = vk_context_->device();

    // Descriptor pool (ImGui needs this)
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
    };
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 100;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &vk_descriptor_pool_) != VK_SUCCESS) return false;

    // Command pool
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vk_context_->queueFamilies().graphics;
    if (vkCreateCommandPool(dev, &cpi, nullptr, &vk_command_pool_) != VK_SUCCESS) return false;

    // Command buffers
    vk_command_buffers_.resize(VK_MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = vk_command_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = VK_MAX_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(dev, &cai, vk_command_buffers_.data()) != VK_SUCCESS) return false;

    // Sync objects
    vk_image_available_.resize(VK_MAX_FRAMES_IN_FLIGHT);
    vk_render_finished_.resize(VK_MAX_FRAMES_IN_FLIGHT);
    vk_in_flight_.resize(VK_MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(dev, &sci, nullptr, &vk_image_available_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(dev, &sci, nullptr, &vk_render_finished_[i]) != VK_SUCCESS ||
            vkCreateFence(dev, &fci, nullptr, &vk_in_flight_[i]) != VK_SUCCESS)
            return false;
    }

    MLOG_INFO("app", "Vulkan resources created");
    return true;
}

void GuiApplication::cleanupVulkanResources() {
    if (vk_context_ && vk_context_->device()) {
        VkDevice dev = vk_context_->device();
        vkDeviceWaitIdle(dev);

        for (auto& s : vk_image_available_) if (s) vkDestroySemaphore(dev, s, nullptr);
        for (auto& s : vk_render_finished_) if (s) vkDestroySemaphore(dev, s, nullptr);
        for (auto& f : vk_in_flight_) if (f) vkDestroyFence(dev, f, nullptr);
        vk_image_available_.clear();
        vk_render_finished_.clear();
        vk_in_flight_.clear();

        if (vk_command_pool_) { vkDestroyCommandPool(dev, vk_command_pool_, nullptr); vk_command_pool_ = VK_NULL_HANDLE; }
        if (vk_descriptor_pool_) { vkDestroyDescriptorPool(dev, vk_descriptor_pool_, nullptr); vk_descriptor_pool_ = VK_NULL_HANDLE; }
    }

    if (vk_swapchain_) { vk_swapchain_->destroy(); vk_swapchain_.reset(); }
    if (vk_context_) { vk_context_->shutdown(); vk_context_.reset(); }

    vulkan_initialized_ = false;
    MLOG_INFO("app", "Vulkan resources cleaned up");
}

bool GuiApplication::setupImGuiVulkan(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Font setup (Vulkan path)
    base_font_size_ = 18.0f;
    float scale = static_cast<float>(window_height_) / 1080.0f;
    current_font_scale_ = scale;

    ImFontConfig fontConfig;
    fontConfig.MergeMode = false;

    // Load fonts from configuration
    const auto& sys_config = config::getSystemConfig();
    ImFont* font = nullptr;
    for (const auto& path : sys_config.font_paths) {
        font = io.Fonts->AddFontFromFileTTF(path.c_str(), base_font_size_, &fontConfig, io.Fonts->GetGlyphRangesJapanese());
        if (font) { MLOG_INFO("app", "Font: %s", path.c_str()); break; }
    }
    if (!font) io.Fonts->AddFontDefault();
    io.FontGlobalScale = scale;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f; style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f; style.WindowBorderSize = 0.0f; style.FrameBorderSize = 0.0f;
    style.ScaleAllSizes(scale);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.40f, 0.60f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.50f, 0.75f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.35f, 0.55f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.40f, 0.60f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.50f, 0.75f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.35f, 0.55f, 1.00f);

    // ImGui Vulkan init
    MLOG_INFO("app", "Initializing ImGui Vulkan backend...");
    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplVulkan_InitInfo vkInfo{};
    vkInfo.Instance = vk_context_->instance();
    vkInfo.PhysicalDevice = vk_context_->physicalDevice();
    vkInfo.Device = vk_context_->device();
    vkInfo.QueueFamily = vk_context_->queueFamilies().graphics;
    vkInfo.Queue = vk_context_->graphicsQueue();
    vkInfo.DescriptorPool = vk_descriptor_pool_;
    vkInfo.MinImageCount = 2;
    vkInfo.ImageCount = vk_swapchain_->imageCount();
    vkInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    vkInfo.PipelineInfoMain.RenderPass = vk_swapchain_->renderPass();
    vkInfo.UseDynamicRendering = false;

    ImGui_ImplVulkan_Init(&vkInfo);

    // Upload fonts

    MLOG_INFO("app", "ImGui Vulkan initialized");
    return true;
}

void GuiApplication::vulkanBeginFrame() {
    frame_valid_ = false;

    VkDevice dev = vk_context_->device();
    uint32_t fi = vk_current_frame_;

    vkWaitForFences(dev, 1, &vk_in_flight_[fi], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &vk_in_flight_[fi]);

    uint32_t imageIndex;
    VkResult r = vkAcquireNextImageKHR(dev, vk_swapchain_->swapchain(),
        UINT64_MAX, vk_image_available_[fi], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        MLOG_INFO("vkframe", "acquire OUT_OF_DATE, recreating");
        vk_swapchain_->recreate(window_width_, window_height_);
        return;
    }

    // Store imageIndex for endFrame
    vk_current_image_index_ = imageIndex;

    VkCommandBuffer cmd = vk_command_buffers_[fi];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear = {{{0.10f, 0.10f, 0.12f, 1.0f}}};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = vk_swapchain_->renderPass();
    rbi.framebuffer = vk_swapchain_->framebuffer(imageIndex);
    rbi.renderArea.extent = vk_swapchain_->extent();
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    frame_valid_ = true;

    static std::atomic<int> begin_frame_count{0};
    int bfc = begin_frame_count.fetch_add(1);
    if (bfc < 20 || (bfc % 300 == 0)) {
        MLOG_INFO("vkframe", "beginFrame #%d fi=%u img=%u extent=%ux%u",
                  bfc, fi, imageIndex,
                  vk_swapchain_->extent().width, vk_swapchain_->extent().height);
    }
}

void GuiApplication::vulkanEndFrame() {
    static std::atomic<int> end_frame_count{0};
    int efc = end_frame_count.fetch_add(1);

    VkDevice dev = vk_context_->device();
    uint32_t fi = vk_current_frame_;
    VkCommandBuffer cmd = vk_command_buffers_[fi];

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vk_image_available_[fi];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk_render_finished_[fi];
    VkResult sr = vkQueueSubmit(vk_context_->graphicsQueue(), 1, &si, vk_in_flight_[fi]);

    VkSwapchainKHR sc = vk_swapchain_->swapchain();
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk_render_finished_[fi];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &vk_current_image_index_;
    VkResult r = vkQueuePresentKHR(vk_context_->graphicsQueue(), &pi);

    if (efc < 20 || (efc % 300 == 0)) {
        MLOG_INFO("vkframe", "endFrame #%d fi=%u img=%u submit=%d present=%d extent=%ux%u",
                  efc, fi, vk_current_image_index_, (int)sr, (int)r,
                  vk_swapchain_->extent().width, vk_swapchain_->extent().height);
    }

    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        MLOG_INFO("vkframe", "present OUT_OF_DATE, recreating");
        vk_swapchain_->recreate(window_width_, window_height_);
    }

    vk_current_frame_ = (vk_current_frame_ + 1) % VK_MAX_FRAMES_IN_FLIGHT;
}

} // namespace mirage::gui
