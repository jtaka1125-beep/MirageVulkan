path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

old = '''        long long total = 0;
        auto start = std::chrono::steady_clock::now();

        while (bridge_running_) {
            int n = recv(tcp_sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                MLOG_WARN("adb", "Bridge: TCP recv returned %d", n);
                break;
            }
            // Forward raw H.264 to UDP (MirrorReceiver parses NAL units)
            // Send in chunks ≤ 1400 bytes for UDP MTU
            for (int offset = 0; offset < n; offset += 1400) {
                int chunk = std::min(n - offset, 1400);
                sendto(udp_sock, buf + offset, chunk, 0,
                       (sockaddr*)&udp_dest, sizeof(udp_dest));
            }
            total += n;

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start).count();
            if (elapsed > 10.0 && total > 0) {
                MLOG_INFO("adb", "Bridge: %.1fs %.2f Mbps",
                         elapsed, total * 8.0 / elapsed / 1e6);
            }
        }'''

new = '''        long long total = 0;
        auto start = std::chrono::steady_clock::now();
        auto last_log = start;

        while (bridge_running_) {
            int n = recv(tcp_sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                MLOG_WARN("adb", "Bridge: TCP recv returned %d", n);
                break;
            }
            // Forward raw H.264 to UDP (MirrorReceiver parses NAL units)
            // Send in chunks <= 1400 bytes for UDP MTU
            for (int offset = 0; offset < n; offset += 1400) {
                int chunk = std::min(n - offset, 1400);
                sendto(udp_sock, buf + offset, chunk, 0,
                       (sockaddr*)&udp_dest, sizeof(udp_dest));
            }
            total += n;

            auto now = std::chrono::steady_clock::now();
            double since_log = std::chrono::duration<double>(now - last_log).count();
            if (since_log >= 30.0) {
                double elapsed = std::chrono::duration<double>(now - start).count();
                MLOG_INFO("adb", "Bridge[%d]: %.0fs total=%lldKB %.2f Mbps",
                         udp_port_, elapsed, total/1024, total * 8.0 / elapsed / 1e6);
                last_log = now;
            }
        }'''

c = c.replace(old, new)

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)
print("FIXED bridge logging interval")
