"""
Add TCP receive mode to MirrorReceiver.
Instead of UDP (lossy) -> raw H.264 parse, connect directly to scrcpy TCP port.
"""
hpp_path = r"C:\MirageWork\MirageVulkan\src\mirror_receiver.hpp"
cpp_path = r"C:\MirageWork\MirageVulkan\src\mirror_receiver.cpp"

# === HPP: Add start_tcp method and tcp_port member ===
with open(hpp_path, 'r', encoding='utf-8') as f:
    h = f.read()

if 'start_tcp' not in h:
    # Add start_tcp declaration
    h = h.replace(
        '  // Start decoder + decode thread only (no UDP socket) - for TCP receiver mode\n'
        '  bool start_decoder_only();',
        '  // Start decoder + decode thread only (no UDP socket) - for TCP receiver mode\n'
        '  bool start_decoder_only();\n\n'
        '  // Start TCP receive mode (connects to localhost:tcp_port for raw H.264)\n'
        '  bool start_tcp(uint16_t tcp_port);'
    )
    # Add tcp receive thread declaration
    h = h.replace(
        '  void receive_thread(uint16_t port);',
        '  void receive_thread(uint16_t port);\n'
        '  void tcp_receive_thread(uint16_t tcp_port);'
    )
    # Add tcp_port member
    h = h.replace(
        '  std::atomic<uint16_t> bound_port_{0};',
        '  std::atomic<uint16_t> bound_port_{0};\n'
        '  uint16_t tcp_port_{0};  // TCP port for scrcpy direct connection'
    )

with open(hpp_path, 'w', encoding='utf-8') as f:
    f.write(h)

# === CPP: Add start_tcp and tcp_receive_thread ===
with open(cpp_path, 'r', encoding='utf-8') as f:
    c = f.read()

if 'tcp_receive_thread' not in c:
    # Find a good place to insert - after receive_thread function or before process_rtp_packet
    # Insert before the raw H.264 processing section
    insert_point = '// ==============================================================================\n// Raw H.264 Annex B stream processing'
    
    tcp_code = '''// ==============================================================================
// TCP receive mode - connects directly to scrcpy-server via ADB forward
// Eliminates UDP packet loss that causes green screen / block artifacts
// ==============================================================================
bool MirrorReceiver::start_tcp(uint16_t tcp_port) {
  tcp_port_ = tcp_port;
  if (!init_decoder()) return false;
  running_.store(true);
  bound_port_.store(tcp_port);  // Report the TCP port as "bound"
  recv_thread_ = std::thread(&MirrorReceiver::tcp_receive_thread, this, tcp_port);
  decode_thread_ = std::thread(&MirrorReceiver::decode_thread_func, this);
  return true;
}

void MirrorReceiver::tcp_receive_thread(uint16_t tcp_port) {
  MLOG_INFO("mirror", "TCP receive thread: connecting to localhost:%d", tcp_port);

#ifdef _WIN32
  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) {
    MLOG_ERROR("mirror", "TCP socket creation failed");
    running_.store(false);
    return;
  }

  // TCP_NODELAY for low latency
  int nodelay = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

  // Large receive buffer
  int bufsize = 4 * 1024 * 1024;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(tcp_port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  // Retry connect (scrcpy server may still be starting)
  bool connected = false;
  for (int i = 0; i < 30 && running_.load(); i++) {
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (!connected) {
    MLOG_ERROR("mirror", "TCP connect to localhost:%d failed after retries", tcp_port);
    closesocket(sock);
    running_.store(false);
    return;
  }

  MLOG_INFO("mirror", "TCP connected to scrcpy on port %d", tcp_port);

  // Set recv timeout so we can check running_ flag
  DWORD tv = 100;  // 100ms
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

  char buf[65536];
  while (running_.load()) {
    int n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      bytes_received_.fetch_add(n);
      process_raw_h264(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
    } else if (n == 0) {
      MLOG_WARN("mirror", "TCP connection closed by server");
      break;
    } else {
      int err = WSAGetLastError();
      if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
        MLOG_ERROR("mirror", "TCP recv error: %d", err);
        break;
      }
    }
  }

  closesocket(sock);
#endif
  MLOG_INFO("mirror", "TCP receive thread ended");
}

'''
    c = c.replace(insert_point, tcp_code + insert_point)

with open(cpp_path, 'w', encoding='utf-8') as f:
    f.write(c)

print("DONE: Added TCP receive mode to MirrorReceiver")
