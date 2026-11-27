#include "settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace gsa::settings {

namespace fs = std::filesystem;

std::string default_path() {
#ifdef _WIN32
  if (const char* a = std::getenv("APPDATA")) return (fs::path(a) / "GridOfSA" / "settings.json").string();
#endif
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return (fs::path(x) / "GridOfSA" / "settings.json").string();
  if (const char* h = std::getenv("HOME")) return (fs::path(h) / ".config" / "GridOfSA" / "settings.json").string();
  return "GridOfSA-settings.json";
}

Store::Store(std::string path) : path_(std::move(path)) {
  std::ifstream in(path_);
  if (!in) return;
  try {
    in >> data_;
    if (!data_.is_object()) data_ = nlohmann::json::object();
  } catch (...) {
    data_ = nlohmann::json::object();  // a damaged file starts afresh rather than stopping the app
  }
}

bool Store::save() const {
  std::error_code ec;
  const fs::path p(path_);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << data_.dump(1);
    if (!out) return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
  }
  return !ec;
}

}  // namespace gsa::settings
