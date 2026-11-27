// Typed client for the /v1 API. The app's only way to reach data.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gsa::api {

struct Endpoint {
  std::string host = "127.0.0.1";
  int port = 0;
  std::string token;
};

struct Response {
  int status = 0;  // 0 = no connection
  std::string body;
  std::string error;
  bool ok() const { return status == 200; }
};

struct SearchHit {
  std::string ref, kind, label, detail;
  std::optional<double> lon, lat;
};

// Not thread safe; make one per thread (they are cheap).
class Client {
 public:
  explicit Client(Endpoint ep);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Response get(const std::string& path);
  std::optional<nlohmann::json> get_json(const std::string& path, std::string* err = nullptr);

  // Raw tile bytes, already decompressed. Empty optional with status 204 means "no tile".
  Response tile(const std::string& pack, int z, uint32_t x, uint32_t y);
  std::vector<SearchHit> search(const std::string& q, int limit, std::string* err = nullptr);

  const Endpoint& endpoint() const { return ep_; }

 private:
  struct Impl;
  Endpoint ep_;
  std::unique_ptr<Impl> impl_;
};

std::string url_encode(const std::string& s);

}  // namespace gsa::api
