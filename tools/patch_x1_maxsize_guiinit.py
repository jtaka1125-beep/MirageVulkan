from pathlib import Path
import re
p=Path(r"C:/MirageWork/MirageVulkan/src/gui/gui_init.cpp")
t=p.read_text(encoding='utf-8', errors='ignore')

# Add helper lambda to send maxsize broadcast after service start check in StartMirroringCallback block.
# We'll inject right after "Verify service started" success log.
needle = 'MLOG_INFO("gui", "ScreenCaptureService started successfully on %s",\n                          dev.display_name.c_str());'
if needle not in t:
    raise SystemExit('needle not found')

ins = needle + "\n\n                // X1: force max_size=2000 to enable rotated 2000x1200 encode (then GUI rotates to portrait)\n" \
      + "                std::string wm = g_adb_manager->adbCommand(adb_id, \"shell wm size\");\n" \
      + "                if (wm.find(\"1200x2000\") != std::string::npos) {\n" \
      + "                    g_adb_manager->adbCommand(adb_id, \"shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE -p com.mirage.capture --ei max_size 2000\");\n" \
      + "                    g_adb_manager->adbCommand(adb_id, \"shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR -p com.mirage.capture\");\n" \
      + "                    MLOG_INFO(\"gui\", \"MaxSize forced on X1 (%s)\", dev.display_name.c_str());\n" \
      + "                }\n"

t = t.replace(needle, ins, 1)

# Also on device select: when TCP-only mode and ADB broadcast fps, add maxsize for selected main device if X1
anchor = 'MLOG_INFO("gui", "FPS update (ADB): %s -> %d fps (%s)",'
if anchor in t and 'ACTION_VIDEO_MAXSIZE' not in t.split(anchor)[0][-500:]:
    pass

# Insert after FPS update loop for TCP-only branch
sel_block_pat = re.compile(r"std::thread\(\[devices, sel_id\]\(\) \{[\s\S]*?for \(const auto& dev : devices\) \{[\s\S]*?MLOG_INFO\(\"gui\", \"FPS update \(ADB\): %s -> %d fps \(%s\)\",[\s\S]*?\}\n            \}\)\.detach\(\);", re.MULTILINE)
m=sel_block_pat.search(t)
if m and 'ACTION_VIDEO_MAXSIZE' not in m.group(0):
    blk=m.group(0)
    # add after fps broadcast per device when it's MAIN and X1
    add = "\n                    if (dev.hardware_id == sel_id) {\n" \
          "                        std::string wm = g_adb_manager->adbCommand(dev.preferred_adb_id, \"shell wm size\");\n" \
          "                        if (wm.find(\"1200x2000\") != std::string::npos) {\n" \
          "                            g_adb_manager->adbCommand(dev.preferred_adb_id, \"shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE -p com.mirage.capture --ei max_size 2000\");\n" \
          "                            g_adb_manager->adbCommand(dev.preferred_adb_id, \"shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR -p com.mirage.capture\");\n" \
          "                            MLOG_INFO(\"gui\", \"MaxSize forced on MAIN X1 (%s)\", dev.display_name.c_str());\n" \
          "                        }\n" \
          "                    }\n"
    # inject right after fps adbCommand call
    blk2 = blk.replace('if (g_adb_manager) g_adb_manager->adbCommand(dev.preferred_adb_id, cmd);',
                       'if (g_adb_manager) g_adb_manager->adbCommand(dev.preferred_adb_id, cmd);'+add)
    t = t[:m.start()] + blk2 + t[m.end():]

p.write_text(t, encoding='utf-8')
print('patched gui_init.cpp for X1 maxsize')
