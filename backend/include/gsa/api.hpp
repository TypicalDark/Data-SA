// The /v1 HTTP API (contract: api/openapi.yaml).
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "gsa/packs.hpp"

namespace httplib {
class Server;
}

namespace gsa {

inline constexpr const char* kApiVersion = "1";
inline constexpr const char* kBackendVersion = GSA_VERSION;

struct ApiConfig {
  std::shared_ptr<PackStore> store;
  std::string token;            // empty = no auth (tests and a local dev run only)
  std::string allowed_origin;   // CORS origin for a browser client, empty = none
};

// Adds every /v1 route and the auth check to the server.
void mount_api(httplib::Server& srv, ApiConfig cfg);

// Turns free text into a safe FTS5 query: each word becomes a quoted prefix term.
// Returns empty when nothing searchable is left.
std::string fts_query(const std::string& q);

// Type tag shown in the UI for a search ref ("area:WC053" + level, "node:power:..." + detail).
std::string kind_for(const std::string& ref, std::optional<int> level, const std::string& detail);
std::string kind_for_level(int level);

// Constant-time comparison for the bearer token.
bool token_equal(const std::string& a, const std::string& b);

}  // namespace gsa
