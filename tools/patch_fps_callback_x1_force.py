from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Find the FPS callback cmd line and enhance for X1
# We'll replace the line that builds cmd for fps with a block.
pat=re.compile(r"std::string cmd = \"shell am broadcast -a com\.mirage\.capture\.ACTION_VIDEO_FPS --ei fps \" \+ std::to_string\(fps\);")
if not pat.search(t):
    raise SystemExit('fps cmd line not found')

block = r'''// Force X1 to stay at high quality (main) even if adaptive logic mislabels fps.
                    const bool isX1 = (dev.display_name.find("Npad X1") != std::string::npos) ||
                                      (dev.display_name.find("N-one Npad X1") != std::string::npos) ||
                                      (dev.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos);
                    int send_fps = fps;
                    if (isX1 && send_fps < 60) send_fps = 60;
                    std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS -p com.mirage.capture --ei fps " + std::to_string(send_fps);
                    std::string cmd2;
                    if (isX1) {
                        // Keep max_size at 2000 and request IDR so SPS refresh happens.
                        cmd2 = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE -p com.mirage.capture --ei max_size 2000";
                    }
                    std::string cmd3;
                    if (isX1) {
                        cmd3 = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR -p com.mirage.capture";
                    }'''

t = pat.sub(block, t, count=1)

# Update thread lambda to send cmd2/cmd3 if non-empty
# Find inside thread body: adbCommand(adb_id, cmd);
pat2=re.compile(r"if \(g_adb_manager\) g_adb_manager->adbCommand\(adb_id, cmd\);")
if not pat2.search(t):
    raise SystemExit('adbCommand line not found')

t = pat2.sub("if (g_adb_manager) { g_adb_manager->adbCommand(adb_id, cmd); if(!cmd2.empty()) g_adb_manager->adbCommand(adb_id, cmd2); if(!cmd3.empty()) g_adb_manager->adbCommand(adb_id, cmd3); }", t, count=1)

p.write_text(t, encoding='utf-8')
print('patched fps callback to force X1 fps/max_size/idr')
