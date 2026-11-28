// Starts the local back-end as a child process (the "shell" role on desktop):
// random port, one-time token, waits for its ready line. The child exits when
// this process exits, because it watches the stdin pipe we hold.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace gsa {

class BackendProcess {
 public:
  // Throws std::runtime_error when the back-end does not start within timeout_s.
  BackendProcess(const std::filesystem::path& exe, const std::filesystem::path& data_dir, double timeout_s = 20);
  ~BackendProcess();
  BackendProcess(const BackendProcess&) = delete;
  BackendProcess& operator=(const BackendProcess&) = delete;

  int port() const { return port_; }
  const std::string& token() const { return token_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  int port_ = 0;
  std::string token_;
};

std::string random_token();  // 32 random bytes, hex

}  // namespace gsa
