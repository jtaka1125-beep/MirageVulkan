from pathlib import Path
import re

hpp = Path(r"C:/MirageWork/MirageVulkan/src/vulkan/vulkan_texture.hpp")
cpp = Path(r"C:/MirageWork/MirageVulkan/src/vulkan/vulkan_texture.cpp")
app = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")

hpp_t = hpp.read_text(encoding='utf-8')
cpp_t = cpp.read_text(encoding='utf-8')
app_t = app.read_text(encoding='utf-8', errors='ignore')

# 1) Add clear() declaration
if 'void clear(' not in hpp_t:
    hpp_t = hpp_t.replace(
        'void update(VkCommandPool cmd_pool, VkQueue queue, const uint8_t* rgba, int width, int height);',
        'void update(VkCommandPool cmd_pool, VkQueue queue, const uint8_t* rgba, int width, int height);\n    // Initialize/clear texture to a known color (prevents showing uninitialized VRAM)\n    void clear(VkCommandPool cmd_pool, VkQueue queue, uint32_t rgba = 0xFF000000u);'
    )

# 2) Implement clear() in cpp (use vkCmdClearColorImage + transitions)
if 'VulkanTexture::clear' not in cpp_t:
    insert_point = cpp_t.find('void VulkanTexture::update')
    if insert_point < 0:
        raise SystemExit('update() not found')

    clear_impl = r'''
void VulkanTexture::clear(VkCommandPool cmd_pool, VkQueue queue, uint32_t rgba) {
    if (!ctx_ || !image_) return;
    VkDevice dev = ctx_->device();

    // Wait previous upload (short) to avoid fighting fences
    if (upload_fence_) {
        vkWaitForFences(dev, 1, &upload_fence_, VK_TRUE, 50'000'000ULL); // 50ms max
        vkResetFences(dev, 1, &upload_fence_);
    }

    VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmd_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    if (vkAllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) return;

    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier b1{}; b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = image_;
    b1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b1.srcAccessMask = 0;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);

    // Clear
    VkClearColorValue cv{};
    cv.uint32[0] = rgba; // ABGR in memory isn't important here; we use raw bytes
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);

    // Transition to SHADER_READ
    VkImageMemoryBarrier b2{}; b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = image_;
    b2.subresourceRange = range;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b2);

    vkEndCommandBuffer(cb);

    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &si, upload_fence_);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(dev, cmd_pool, 1, &cb);
    layout_initialized_ = True;
}
'''
    # fix Python True typo after insertion below
    cpp_t = cpp_t[:insert_point] + clear_impl + "\n" + cpp_t[insert_point:]
    cpp_t = cpp_t.replace('layout_initialized_ = True;', 'layout_initialized_ = true;')

# 3) Call clear() right after create() in updateDeviceFrame
# Find the block after create succeeds and before setting texture_width.
if '->clear(' not in app_t:
    app_t = app_t.replace(
        'if (!device.vk_texture->create(*vk_context_, vk_descriptor_pool_, width, height)) {',
        'if (!device.vk_texture->create(*vk_context_, vk_descriptor_pool_, width, height)) {'
    )
    # inject after successful create (after the if block ends) by inserting before setting texture_width
    marker = '        device.texture_width = width;'
    if marker in app_t:
        app_t = app_t.replace(
            marker,
            '        // Clear to black once so we never display uninitialized VRAM ("last closed frame" artifact)\n'
            '        device.vk_texture->clear(vk_command_pool_, vk_context_->graphicsQueue(), 0x00000000u);\n'
            + marker
        )

# 4) Auto-relax expected resolution if we skip too many frames
if 'auto_relax_cnt' not in app_t:
    # Insert into the non-native skip branch right before return;
    needle = '                return;'
    idx = app_t.find(needle)
    # Use a more specific region: inside the "Skipping non-native frame" branch
    # We'll replace the first occurrence after the warning block.
    # Safer: locate the warning string.
    wpos = app_t.find('Skipping non-native frame')
    if wpos != -1:
        # find the return after wpos
        rpos = app_t.find('return;', wpos)
        if rpos != -1:
            insert = (
                '                static std::map<std::string, int> auto_relax_cnt;\n'
                '                int& sc = auto_relax_cnt[id];\n'
                '                sc++;\n'
                '                if (sc >= 60) {\n'
                '                    MLOG_WARN("VkTex", "Auto-relax expected size after %d skips: device=%s expected=%dx%d -> accept %dx%d",\n'
                '                              sc, id.c_str(), exp_w, exp_h, width, height);\n'
                '                    device.expected_width = 0;\n'
                '                    device.expected_height = 0;\n'
                '                    device.video_width = width;\n'
                '                    device.video_height = height;\n'
                '                    // continue without returning;\n'
                '                } else {\n'
                '                    return;\n'
                '                }\n'
            )
            # replace the single 'return;' at rpos with insert (keeping indentation)
            app_t = app_t[:rpos] + insert + app_t[rpos+len('return;'):]

hpp.write_text(hpp_t, encoding='utf-8')
cpp.write_text(cpp_t, encoding='utf-8')
app.write_text(app_t, encoding='utf-8')
print('patched vulkan_texture.{hpp,cpp} and gui_application.cpp')
