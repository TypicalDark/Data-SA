// Installed data packs. One storage interface in front of local files now,
// object storage and Postgres later (see docs/ARCHITECTURE.md).
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gsa/pmtiles.hpp"
#include "gsa/sqlite.hpp"

namespace gsa {

// SQLite pack schema versions this back-end can read ("gsa-pack/<n>").
inline const std::vector<int> kPackSchemas = {1};

struct PackInfo {
  std::string id;
  std::string kind;  // "tiles", "sqlite" or "json"
  std::string version;
  std::filesystem::path file;
  uint64_t bytes = 0;
};

// An immutable view of what is installed. Requests take a snapshot and keep it for
// their lifetime, so a pack swap never pulls a file out from under a request.
struct PackSet {
  std::map<std::string, std::shared_ptr<const pmtiles::Archive>> tiles;
  std::shared_ptr<sql::Pool> db;  // the attribute pack, may be null
  // Small JSON data packs (the Find data, ...), checked to parse when loaded, served as they are.
  std::map<std::string, std::shared_ptr<const std::string>> data;
  std::vector<PackInfo> info;
};

class PackStore {
 public:
  // data_dir holds packs/ (and later ui/, cache/). Throws on unreadable packs.
  explicit PackStore(std::filesystem::path data_dir, size_t db_connections = 4);

  std::shared_ptr<const PackSet> current() const {
    std::lock_guard<std::mutex> lk(mu_);
    return set_;
  }
  const std::filesystem::path& data_dir() const { return data_dir_; }

  // Re-reads packs/installed.json (or scans packs/) and swaps atomically.
  void reload();

 private:
  std::shared_ptr<const PackSet> load() const;
  std::filesystem::path data_dir_;
  size_t db_connections_;
  mutable std::mutex mu_;
  std::shared_ptr<const PackSet> set_;
};

// Valid pack id: lower case letters, digits, '-' and '_', 1 to 64 characters.
bool valid_pack_id(const std::string& id);

}  // namespace gsa
