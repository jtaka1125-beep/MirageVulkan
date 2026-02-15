#pragma once
#include <string>
#include <optional>

namespace gui {

struct IpcResponse {
  std::string raw_line;
};

class MirageIpcClient {
public:
  explicit MirageIpcClient(std::wstring pipe_name = L"\\\\.\\pipe\\miraged_ctl");
  ~MirageIpcClient();

  bool connect(int timeout_ms = 2000);
  void close();

  std::optional<IpcResponse> request_once(const std::string& json_line, int timeout_ms = 2000);

private:
  std::wstring pipe_name_;
  void* hPipe_ = nullptr;
  bool connect_failed_logged_ = false;  // suppress repeated log spam
};

} // namespace gui
