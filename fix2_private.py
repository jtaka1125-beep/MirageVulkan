"""Fix 2: Make ai_processing_enabled_ accessible via public setter"""
from pathlib import Path

CPP_PATH = Path(r"C:\MirageWork\MirageVulkan\src\ai_engine.cpp")
cpp = CPP_PATH.read_text(encoding='utf-8')

# Fix: Add public setter method in Impl
# Find the Impl class public section and add a setter
old = '    std::atomic<bool>                                   ai_processing_enabled_{true};  // Bug fix: propagated from AIEngine::enabled_'
new = '    std::atomic<bool>                                   ai_processing_enabled_{true};  // Bug fix: propagated from AIEngine::enabled_\n    void setAiProcessingEnabled(bool v) { ai_processing_enabled_.store(v, std::memory_order_relaxed); }'

cpp = cpp.replace(old, new, 1)

# Fix the setEnabled implementation to use the setter
old_set = "        impl_->ai_processing_enabled_.store(enabled, std::memory_order_relaxed);"
new_set = "        impl_->setAiProcessingEnabled(enabled);"
cpp = cpp.replace(old_set, new_set, 1)

CPP_PATH.write_text(cpp, encoding='utf-8')
print("[OK] Added setAiProcessingEnabled() public setter and updated setEnabled()")
