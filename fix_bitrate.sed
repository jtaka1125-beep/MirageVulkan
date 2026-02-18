s/int bit_rate = is_main ? 4000000 : 1000000;/int bit_rate = 2000000;  \/\/ 2Mbps uniform for all devices/
s/int max_fps_val = is_main ? 30 : 15;/int max_fps_val = 30;     \/\/ 30fps uniform for all devices/
s/MLOG_INFO("adb", "scrcpy params: is_main=%d bit_rate=%d max_fps=%d", is_main, bit_rate, max_fps_val);/MLOG_INFO("adb", "scrcpy params: bit_rate=%d max_fps=%d", bit_rate, max_fps_val);/
