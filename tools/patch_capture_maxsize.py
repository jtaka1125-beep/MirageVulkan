from pathlib import Path
import re

p = Path(r"C:/MirageWork/MirageVulkan/src/tcp_video_receiver.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# Replace launchCaptureTcpMirror with version that sets max_size based on wm size.
pat = re.compile(r"void TcpVideoReceiver::launchCaptureTcpMirror\(const std::string& adb_serial\) \{[\s\S]*?\n\}", re.MULTILINE)

m = pat.search(t)
if not m:
    raise SystemExit('launchCaptureTcpMirror not found')

new = r'''void TcpVideoReceiver::launchCaptureTcpMirror(const std::string& adb_serial) {
    // Determine device native size and pass max_size to MirageCapture to avoid implicit downscale.
    // Many capture stacks default to max_size=1440, which turns 1200x2000 -> 864x1440.
    int max_side = 0;
    {
        std::string cmd = "adb -s " + adb_serial + " shell wm size";
        std::string out = execCommandHidden(cmd);
        // Parse "Physical size: WxH" or "Override size: WxH"
        auto parseWH = [&](const std::string& s) {
            int w=0,h=0;
            if (sscanf(s.c_str(), "%*[^0-9]%dx%d", &w, &h) == 2) {
                max_side = std::max(w, h);
            }
        };
        // Prefer Physical size if present
        size_t pos = out.find("Physical size");
        if (pos != std::string::npos) {
            parseWH(out.substr(pos));
        } else {
            parseWH(out);
        }
        if (max_side <= 0) max_side = 2000; // safe default for Npad X1
    }

    // Launch MirageCapture in TCP mirror mode.
    // We pass --ei max_size to request native-ish resolution (long side).
    std::string cmd = "adb -s " + adb_serial +
        " shell am start -n com.mirage.capture/.ui.CaptureActivity"
        " --ez auto_mirror true"
        " --es mirror_mode tcp"
        " --ei max_size " + std::to_string(max_side);
    execCommandHidden(cmd);
    MLOG_INFO("tcpvideo", "Launched MirageCapture TCP mirror (serial=%s, max_size=%d)", adb_serial.c_str(), max_side);
}'''

t = pat.sub(new, t, count=1)

p.write_text(t, encoding='utf-8')
print('patched launchCaptureTcpMirror with max_size')
