"""Fix 2b: Properly place public setter for ai_processing_enabled_"""
from pathlib import Path

CPP_PATH = Path(r"C:\MirageWork\MirageVulkan\src\ai_engine.cpp")
cpp = CPP_PATH.read_text(encoding='utf-8')

# The previous fix added the setter in private scope. Replace it with proper public: accessor
old = '''    std::atomic<bool>                                   ai_processing_enabled_{true};  // Bug fix: propagated from AIEngine::enabled_
    void setAiProcessingEnabled(bool v) { ai_processing_enabled_.store(v, std::memory_order_relaxed); }'''

new = '''    std::atomic<bool>                                   ai_processing_enabled_{true};  // Bug fix: propagated from AIEngine::enabled_
public:
    void setAiProcessingEnabled(bool v) { ai_processing_enabled_.store(v, std::memory_order_relaxed); }
private:'''

cpp = cpp.replace(old, new, 1)
CPP_PATH.write_text(cpp, encoding='utf-8')
print("[OK] Added public: scope for setAiProcessingEnabled()")
