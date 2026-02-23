from pathlib import Path
import re

hpp = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.hpp")
cpp = Path(r"C:/MirageWork/MirageVulkan/src/gui_application.cpp")

hpp_text = hpp.read_text(encoding='utf-8', errors='ignore')
cpp_text = cpp.read_text(encoding='utf-8', errors='ignore')

# 1) Add pending recreate members near resizing_
marker = "std::atomic<bool> resizing_{false};"
if marker in hpp_text and "swapchain_recreate_pending_" not in hpp_text:
    insert = marker + "  // Prevent render during resize\n" + \
             "    std::atomic<bool> swapchain_recreate_pending_{false};  // defer swapchain recreate to safe point\n" + \
             "    std::atomic<int> pending_swapchain_w_{0};\n" + \
             "    std::atomic<int> pending_swapchain_h_{0};\n"
    hpp_text = hpp_text.replace(marker + "  // Prevent render during resize", insert)

# 2) Patch onResize to NOT recreate immediately
# Replace the block that sets resizing_ and recreates swapchain with deferred flags.
pattern = re.compile(r"// Set resizing flag[\s\S]*?resizing_\.store\(false\);", re.MULTILINE)
if pattern.search(cpp_text):
    repl = """// Defer swapchain recreation to the render thread to avoid ImGui frame-scope asserts.
    // Doing vkDeviceWaitIdle + recreate inside WM_SIZE (WndProc) can interrupt ImGui frame lifecycle.
    pending_swapchain_w_.store(width, std::memory_order_relaxed);
    pending_swapchain_h_.store(height, std::memory_order_relaxed);
    swapchain_recreate_pending_.store(true, std::memory_order_release);

    // Update font scale based on window height (base: 1080p)
    if (imgui_initialized_) {
        float scale = static_cast<float>(height) / 1080.0f;
        current_font_scale_ = scale;
        ImGuiIO& io = ImGui::GetIO();
        io.FontGlobalScale = scale;
    }
"""
    cpp_text = pattern.sub(repl, cpp_text, count=1)

# 3) Patch vulkanBeginFrame: if pending recreate, do it here and return (safe point)
if "swapchain_recreate_pending_" in cpp_text:
    pass
else:
    # ensure member used compiles even if include mismatch
    pass

# Insert check after resizing_ guard
needle = "    // Skip frames during window resize to prevent Vulkan errors\n    if (resizing_.load()) return;\n"
if needle in cpp_text and "swapchain_recreate_pending_.load" not in cpp_text:
    add = needle + "\n    // Safe-point swapchain recreation (outside ImGui frame scope)\n" \
          "    if (swapchain_recreate_pending_.load(std::memory_order_acquire)) {\n" \
          "        int w = pending_swapchain_w_.load(std::memory_order_relaxed);\n" \
          "        int h = pending_swapchain_h_.load(std::memory_order_relaxed);\n" \
          "        if (w > 0 && h > 0 && vk_swapchain_ && vk_context_) {\n" \
          "            VkDevice dev2 = vk_context_->device();\n" \
          "            vkDeviceWaitIdle(dev2);\n" \
          "            vk_swapchain_->recreate(w, h);\n" \
          "        }\n" \
          "        swapchain_recreate_pending_.store(false, std::memory_order_release);\n" \
          "        return;  // skip this frame; next frame will acquire with new swapchain\n" \
          "    }\n"
    cpp_text = cpp_text.replace(needle, add)

# 4) Patch acquire/present OUT_OF_DATE paths to set pending instead of immediate recreate
cpp_text = cpp_text.replace(
    '        MLOG_INFO("vkframe", "acquire OUT_OF_DATE/SUBOPTIMAL (%d), recreating", (int)r);\n        vkDeviceWaitIdle(dev);\n        vk_swapchain_->recreate(window_width_, window_height_);\n        return;\n',
    '        MLOG_INFO("vkframe", "acquire OUT_OF_DATE/SUBOPTIMAL (%d), deferring recreate", (int)r);\n        pending_swapchain_w_.store(window_width_, std::memory_order_relaxed);\n        pending_swapchain_h_.store(window_height_, std::memory_order_relaxed);\n        swapchain_recreate_pending_.store(true, std::memory_order_release);\n        return;\n'
)

cpp_text = cpp_text.replace(
    '        MLOG_INFO("vkframe", "present OUT_OF_DATE/SUBOPTIMAL (%d), recreating", (int)r);\n        vkDeviceWaitIdle(dev);\n        vk_swapchain_->recreate(window_width_, window_height_);\n    }\n',
    '        MLOG_INFO("vkframe", "present OUT_OF_DATE/SUBOPTIMAL (%d), deferring recreate", (int)r);\n        pending_swapchain_w_.store(window_width_, std::memory_order_relaxed);\n        pending_swapchain_h_.store(window_height_, std::memory_order_relaxed);\n        swapchain_recreate_pending_.store(true, std::memory_order_release);\n    }\n'
)

hpp.write_text(hpp_text, encoding='utf-8')
cpp.write_text(cpp_text, encoding='utf-8')
print('patched gui_application.{hpp,cpp}')
