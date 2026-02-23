from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

if 'FORCE_X1_FPS_MAXSIZE' in t:
    print('already patched')
    raise SystemExit(0)

# Find line that sets cmd for ACTION_VIDEO_FPS
pat=re.compile(r"std::string cmd = \"shell am broadcast -a com\.mirage\.capture\.ACTION_VIDEO_FPS --ei fps \" \+ std::to_string\(fps\);")

m=pat.search(t)
if not m:
    raise SystemExit('fps cmd line not found')

replacement = (
    'const bool isX1 = (dev.display_name.find("Npad X1") != std::string::npos) ||\n'
    '                                   (dev.display_name.find("N-one Npad X1") != std::string::npos) ||\n'
    '                                   (dev.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) ||\n'
    '                                   (dev.preferred_adb_id.find("93020523431940") != std::string::npos);\n'
    '                     const int send_fps = isX1 ? 60 : fps;\n'
    '                     std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS --ei fps " + std::to_string(send_fps);\n'
    '                     // FORCE_X1_FPS_MAXSIZE: keep X1 high-res path stable (avoids 640x1072 downscale)\n'
    '                     const std::string cmd_max = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE -p com.mirage.capture --ei max_size 2000";\n'
    '                     const std::string cmd_idr = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR -p com.mirage.capture";'
)

t = t[:m.start()] + replacement + t[m.end():]

# In the thread body, after adbCommand(cmd), add extra broadcasts when isX1
# Locate the lambda thread body line 'if (g_adb_manager) g_adb_manager->adbCommand(adb_id, cmd);'
anchor = 'if (g_adb_manager) g_adb_manager->adbCommand(adb_id, cmd);'
if anchor not in t:
    raise SystemExit('thread anchor not found')
if 'cmd_max' not in t:
    t = t.replace(anchor, anchor + '\n                        if (isX1 && g_adb_manager) {\n                            g_adb_manager->adbCommand(adb_id, cmd_max);\n                            g_adb_manager->adbCommand(adb_id, cmd_idr);\n                        }', 1)

# Update log line to show send_fps
log_anchor = 'MLOG_INFO("RouteCtrl", "Sent FPS=%d to %s via ADB broadcast (%s)",' 
if log_anchor in t:
    t = t.replace('fps, device_id.c_str()', 'send_fps, device_id.c_str()', 1)

p.write_text(t, encoding='utf-8')
print('patched: force X1 fps=60 + max_size=2000')
