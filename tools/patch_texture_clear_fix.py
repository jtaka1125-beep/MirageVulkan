from pathlib import Path
import re
cpp = Path(r"C:/MirageWork/MirageVulkan/src/vulkan/vulkan_texture.cpp")
app = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")

cpp_t = cpp.read_text(encoding='utf-8', errors='ignore')
app_t = app.read_text(encoding='utf-8', errors='ignore')

# Fix clear() implementation: proper clear color + fence wait, no queueWaitIdle
if 'void VulkanTexture::clear' in cpp_t:
    # replace clear color block
    cpp_t = re.sub(r"VkClearColorValue cv\{\}[\s\S]*?vkCmdClearColorImage\(cb, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range\);",
                   "VkClearColorValue cv{};\n    cv.float32[0] = 0.0f;\n    cv.float32[1] = 0.0f;\n    cv.float32[2] = 0.0f;\n    cv.float32[3] = 1.0f;\n    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };\n    vkCmdClearColorImage(cb, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);",
                   cpp_t, count=1)

    # replace submit+wait section
    cpp_t = re.sub(r"vkQueueSubmit\(queue, 1, &si, upload_fence_\);\s*vkQueueWaitIdle\(queue\);",
                   "vkQueueSubmit(queue, 1, &si, upload_fence_);\n    // Wait briefly for clear to complete so the first frame never shows stale VRAM\n    vkWaitForFences(dev, 1, &upload_fence_, VK_TRUE, 200'000'000ULL);",
                   cpp_t, count=1)

# Add a log marker after clear call in gui_application.cpp
if 'device.vk_texture->clear' in app_t and 'Cleared texture to black' not in app_t:
    app_t = app_t.replace(
        'device.vk_texture->clear(vk_command_pool_, vk_context_->graphicsQueue(), 0x00000000u);',
        'device.vk_texture->clear(vk_command_pool_, vk_context_->graphicsQueue(), 0x00000000u);\n        MLOG_INFO("VkTex", "Cleared texture to black (device=%s)", id.c_str());'
    )

cpp.write_text(cpp_t, encoding='utf-8')
app.write_text(app_t, encoding='utf-8')
print('patched clear() and added log marker')
