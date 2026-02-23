from pathlib import Path
import re
p = Path(r"C:/MirageWork/MirageVulkan/src/frame_dispatcher.hpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# add includes
if '#include <vector>' not in t:
    t = t.replace('#include <set>\n', '#include <set>\n#include <vector>\n#include <unordered_map>\n#include <cstring>\n')

# add buffer members
if 'frame_buffers_' not in t:
    t = t.replace('mutable std::mutex devices_mutex_;\n    std::set<std::string> known_devices_;',
                  'mutable std::mutex devices_mutex_;\n    std::set<std::string> known_devices_;\n\n    // Persistent per-device RGBA buffers (FrameReadyEvent uses raw pointer; lifetime must outlive publish)\n    mutable std::mutex frames_mutex_;\n    std::unordered_map<std::string, std::vector<uint8_t>> frame_buffers_;')

# patch dispatchFrame to copy
if 'std::memcpy' not in t:
    # locate creation of FrameReadyEvent evt;
    marker = '        FrameReadyEvent evt;'
    idx = t.find(marker)
    if idx == -1:
        raise SystemExit('marker not found')
    # insert copy block right before evt assignment of rgba_data
    insert = """
        // Copy frame into persistent buffer so GUI/event consumers never see freed stack memory.
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        const uint8_t* stable_ptr = rgba_data;
        if (rgba_data && bytes > 0) {
            std::lock_guard<std::mutex> fl(frames_mutex_);
            auto& buf = frame_buffers_[device_id];
            if (buf.size() != bytes) buf.resize(bytes);
            std::memcpy(buf.data(), rgba_data, bytes);
            stable_ptr = buf.data();
        }

"""
    t = t.replace(marker, insert + marker)

    # replace evt.rgba_data assignment
    t = t.replace('        evt.rgba_data = rgba_data;', '        evt.rgba_data = stable_ptr;')

p.write_text(t, encoding='utf-8')
print('patched frame_dispatcher to copy frames')
