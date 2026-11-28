// Per-user settings kept between runs: saved views, watchlist, sidebar and last camera.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace gsa::settings {

// %APPDATA%\GridOfSA\settings.json on Windows, $XDG_CONFIG_HOME/GridOfSA or ~/.config/GridOfSA elsewhere.
std::string default_path();

class Store {
 public:
  explicit Store(std::string path = default_path());
  const nlohmann::json& data() const { return data_; }
  nlohmann::json& data() { return data_; }
  // Writes to a temporary file and renames it over the old one. False on failure.
  bool save() const;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  nlohmann::json data_ = nlohmann::json::object();
};

}  // namespace gsa::settings
