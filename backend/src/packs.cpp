#include "gsa/packs.hpp"

#include <algorithm>
#include <fstream>

namespace gsa {

namespace fs = std::filesystem;

bool valid_pack_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (i > 0 && (c == '-' || c == '_'));
    if (!ok) return false;
  }
  return true;
}

PackStore::PackStore(fs::path data_dir, size_t db_connections)
    : data_dir_(std::move(data_dir)), db_connections_(db_connections) {
  reload();
}

void PackStore::reload() {
  auto next = load();  // may throw; the current set stays in place then
  std::lock_guard<std::mutex> lk(mu_);
  set_ = std::move(next);
}

namespace {

int pack_schema(sql::Pool& pool) {
  auto db = pool.lease();
  std::string fmt;
  db->query("SELECT v FROM pack_info WHERE k='format'", {}, [&](const sql::Row& r) { fmt = r.text(0); });
  const std::string prefix = "gsa-pack/";
  if (fmt.rfind(prefix, 0) != 0) return -1;
  try {
    return std::stoi(fmt.substr(prefix.size()));
  } catch (...) {
    return -1;
  }
}

}  // namespace

std::shared_ptr<const PackSet> PackStore::load() const {
  auto set = std::make_shared<PackSet>();
  const fs::path packs = data_dir_ / "packs";
  std::vector<PackInfo> found;

  // installed.json is written by the pack updater; it is the source of truth when present.
  //   {"format":"gsa-installed/1","packs":{"admin":{"kind":"tiles","version":"admin-2026-11","file":"admin/admin-2026-11.pmtiles"}}}
  const fs::path manifest = packs / "installed.json";
  if (fs::exists(manifest)) {
    std::ifstream in(manifest);
    const auto j = nlohmann::json::parse(in);
    for (const auto& [id, p] : j.at("packs").items()) {
      if (!valid_pack_id(id)) throw std::runtime_error("bad pack id in installed.json: " + id);
      const fs::path file = (packs / p.at("file").get<std::string>()).lexically_normal();
      // the file must stay inside packs/
      const auto rel = file.lexically_relative(packs);
      if (rel.empty() || *rel.begin() == "..") throw std::runtime_error("pack file outside packs/: " + id);
      found.push_back({id, p.at("kind").get<std::string>(), p.value("version", "unknown"), file, 0});
    }
  } else if (fs::exists(packs)) {
    // Development: every *.pmtiles is a tile pack named after its file; pack.sqlite is the attribute pack.
    for (const auto& e : fs::directory_iterator(packs)) {
      if (!e.is_regular_file()) continue;
      const auto ext = e.path().extension().string();
      const auto stem = e.path().stem().string();
      if (ext == ".pmtiles" && valid_pack_id(stem)) found.push_back({stem, "tiles", "dev", e.path(), 0});
      if (ext == ".sqlite" && stem == "pack") found.push_back({"pack", "sqlite", "dev", e.path(), 0});
    }
  }

  for (auto& p : found) {
    p.bytes = fs::file_size(p.file);
    if (p.kind == "tiles") {
      set->tiles[p.id] = std::make_shared<const pmtiles::Archive>(p.file);
    } else if (p.kind == "sqlite") {
      auto pool = std::make_shared<sql::Pool>(p.file, db_connections_);
      const int schema = pack_schema(*pool);
      if (std::find(kPackSchemas.begin(), kPackSchemas.end(), schema) == kPackSchemas.end()) {
        throw std::runtime_error("pack " + p.id + " has schema " + std::to_string(schema) +
                                 ", which this back-end cannot read; update the app first");
      }
      set->db = pool;
    } else {
      throw std::runtime_error("unknown pack kind " + p.kind);
    }
    set->info.push_back(p);
  }
  std::sort(set->info.begin(), set->info.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  return set;
}

}  // namespace gsa
