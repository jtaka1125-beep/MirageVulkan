path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

old = '''            // Forward raw H.264 to UDP (MirrorReceiver parses NAL units)
            // Send in chunks <= 1400 bytes for UDP MTU
            for (int offset = 0; offset < n; offset += 1400) {
                int chunk = std::min(n - offset, 1400);
                sendto(udp_sock, buf + offset, chunk, 0,
                       (sockaddr*)&udp_dest, sizeof(udp_dest));
            }'''

new = '''            // Forward raw H.264 to UDP (localhost - no MTU fragmentation needed)
            // Send entire TCP recv chunk as single UDP datagram
            // Localhost UDP supports up to 65535 bytes without fragmentation issues
            sendto(udp_sock, buf, n, 0,
                   (sockaddr*)&udp_dest, sizeof(udp_dest));'''

c = c.replace(old, new)

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)
print("FIXED: bridge sends full TCP chunks without fragmentation")
