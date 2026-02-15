#include "ipc_client.hpp"
#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif
#include <vector>
#include "mirage_log.hpp"

namespace gui {

MirageIpcClient::MirageIpcClient(std::wstring pipe_name) : pipe_name_(std::move(pipe_name)) {}
MirageIpcClient::~MirageIpcClient() { close(); }

bool MirageIpcClient::connect(int timeout_ms) {
#ifdef _WIN32
  close();

  HANDLE h = CreateFileW(
    pipe_name_.c_str(),
    GENERIC_READ | GENERIC_WRITE,
    0, nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL, nullptr
  );

  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    // Only log the first failure, suppress subsequent ones
    if (!connect_failed_logged_) {
      MLOG_WARN("IPC", "Pipe not available (error=%lu), will retry silently", err);
      connect_failed_logged_ = true;
    }
    return false;
  }

  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

  hPipe_ = h;
  connect_failed_logged_ = false;  // reset on success
  MLOG_INFO("IPC", "Connected to miraged pipe");
  return true;
#else
  (void)timeout_ms;
  return false;
#endif
}

void MirageIpcClient::close() {
#ifdef _WIN32
  if (hPipe_) {
    CloseHandle((HANDLE)hPipe_);
    hPipe_ = nullptr;
  }
#endif
}

std::optional<IpcResponse> MirageIpcClient::request_once(const std::string& json_line, int timeout_ms) {
#ifdef _WIN32
  if (!connect(timeout_ms)) return std::nullopt;

  DWORD written = 0;
  std::string line = json_line;
  if (line.empty() || line.back() != '\n') line.push_back('\n');

  if (!WriteFile((HANDLE)hPipe_, line.data(), (DWORD)line.size(), &written, nullptr)) {
    close();
    return std::nullopt;
  }

  std::string out;
  out.reserve(4096);
  char buf[256];
  DWORD read = 0;

  for (;;) {
    if (!ReadFile((HANDLE)hPipe_, buf, sizeof(buf), &read, nullptr) || read == 0) break;
    out.append(buf, buf + read);
    auto pos = out.find('\n');
    if (pos != std::string::npos) { out.resize(pos); break; }
    if (out.size() > 1024 * 1024) break;
  }

  close();
  if (out.empty()) return std::nullopt;
  return IpcResponse{out};
#else
  (void)json_line; (void)timeout_ms;
  return std::nullopt;
#endif
}

} // namespace gui
