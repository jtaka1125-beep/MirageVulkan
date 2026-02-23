from pathlib import Path
import re
p = Path(r"C:/MirageWork/MirageVulkan/src/vulkan/vulkan_swapchain.cpp")
text = p.read_text(encoding="utf-8")
# add includes if missing
if "#include <thread>" not in text:
    text = text.replace("#include <limits>\n", "#include <limits>\n#include <thread>\n#include <chrono>\n")
# replace caps block
old = """    VkSurfaceCapabilitiesKHR caps;\n    VkResult capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface_, &caps);\n    if (capsResult != VK_SUCCESS) {\n        MLOG_ERROR(\"VkSwap\", \"getSurfaceCaps failed: %d\", (int)capsResult);\n        return false;\n    }\n"""
new = """    if (w <= 0 || h <= 0) {\n        MLOG_ERROR(\"VkSwap\", \"invalid swapchain extent: %d x %d\", w, h);\n        return false;\n    }\n\n    VkSurfaceCapabilitiesKHR caps{};\n    VkResult capsResult = VK_ERROR_UNKNOWN;\n    // Some drivers may return VK_ERROR_UNKNOWN if queried too early right after surface creation.\n    // Retry briefly to avoid transient startup freezes / init failures.\n    for (int i = 0; i < 20; i++) {\n        capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface_, &caps);\n        if (capsResult == VK_SUCCESS) break;\n        std::this_thread::sleep_for(std::chrono::milliseconds(50));\n    }\n    if (capsResult != VK_SUCCESS) {\n        MLOG_ERROR(\"VkSwap\", \"getSurfaceCaps failed after retry: %d\", (int)capsResult);\n        return false;\n    }\n"""
if old not in text:
    raise SystemExit("pattern not found; abort")
text = text.replace(old, new)
p.write_text(text, encoding="utf-8")
print("patched", p)
