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

namespace live {
class Store;
class PlanePoller;
}  // namespace live

inline constexpr const char* kApiVersion = "1";
inline constexpr const char* kBackendVersion = GSA_VERSION;

struct ApiConfig {
  std::shared_ptr<PackStore> store;
  std::string token;            // empty = no auth (tests and a local dev run only)
  std::string allowed_origin;   // CORS origin for a browser client, empty = none
  std::shared_ptr<live::Store> live;           // live planes and ships; null = /v1/live answers 503
  std::shared_ptr<live::PlanePoller> poller;   // for /v1/live/status, may be null
  size_t cache_bytes = size_t(64) << 20;       // in-memory response cache for pack-backed GETs, 0 = off
};

// Adds every /v1 route and the auth check to the server.
void mount_api(httplib::Server& srv, ApiConfig cfg);

// Routes over the SQLite pack's plot, time series and grid graph tables (api_data.cpp).
// Called by mount_api; auth and CORS come from mount_api's pre-routing handler.
void mount_data_api(httplib::Server& srv, std::shared_ptr<PackStore> store);

// /v1/live/* (api_live.cpp). Called by mount_api.
void mount_live_api(httplib::Server& srv, std::shared_ptr<live::Store> store, std::shared_ptr<live::PlanePoller> poller);

// Turns free text into a safe FTS5 query: each word becomes a quoted prefix term.
// Returns empty when nothing searchable is left.
std::string fts_query(const std::string& q);

// Type tag shown in the UI for a search ref ("area:WC053" + level, "node:power:..." + detail).
std::string kind_for(const std::string& ref, std::optional<int> level, const std::string& detail);
std::string kind_for_level(int level);

// Constant-time comparison for the bearer token.
bool token_equal(const std::string& a, const std::string& b);

}  // namespace gsa
