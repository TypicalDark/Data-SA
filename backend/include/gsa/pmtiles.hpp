// Minimal PMTiles v3 reader: header, directories (root + leaves), tile lookup.
// Spec: https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md
//
// Reads by byte range from a local file, so a multi-GB pack is never loaded whole.
// Tile bytes are returned exactly as stored; the HTTP layer sets Content-Encoding
// from the header's tile compression.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gsa::pmtiles {

enum class Compression : uint8_t { Unknown = 0, None = 1, Gzip = 2, Brotli = 3, Zstd = 4 };
enum class TileType : uint8_t { Unknown = 0, Mvt = 1, Png = 2, Jpeg = 3, Webp = 4, Avif = 5 };

const char* content_encoding(Compression c);  // nullptr when none
const char* content_type(TileType t);
const char* type_name(TileType t);

struct Header {
  uint64_t root_offset = 0, root_length = 0;
  uint64_t metadata_offset = 0, metadata_length = 0;
  uint64_t leaf_offset = 0, leaf_length = 0;
  uint64_t data_offset = 0, data_length = 0;
  uint64_t addressed_tiles = 0, tile_entries = 0, tile_contents = 0;
  bool clustered = false;
  Compression internal_compression = Compression::Unknown;
  Compression tile_compression = Compression::Unknown;
  TileType tile_type = TileType::Unknown;
  uint8_t min_zoom = 0, max_zoom = 0;
  double bounds[4] = {0, 0, 0, 0};  // min lon, min lat, max lon, max lat
  uint8_t center_zoom = 0;
  double center[2] = {0, 0};

  static constexpr size_t kLength = 127;
  static Header parse(const uint8_t* b, size_t n);  // throws Error
};

struct Entry {
  uint64_t tile_id = 0;
  uint64_t offset = 0;
  uint32_t length = 0;
  uint32_t run_length = 0;
  bool operator==(const Entry&) const = default;
};

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Hilbert tile id for z/x/y. Empty when x or y are outside the zoom's grid.
std::optional<uint64_t> zxy_to_tile_id(uint8_t z, uint32_t x, uint32_t y);
std::vector<Entry> decode_directory(const std::vector<uint8_t>& b);  // throws Error
// Entry with the largest tile_id <= id when it covers id, or a leaf pointer.
std::optional<Entry> find_entry(const std::vector<Entry>& dir, uint64_t id);

struct Tile {
  std::vector<uint8_t> data;
  Compression compression;
  TileType tile_type;
};

class File;  // positioned reads, platform specific

// An open archive. Thread safe; share it through shared_ptr.
class Archive {
 public:
  explicit Archive(const std::filesystem::path& path);  // throws Error
  ~Archive();
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  const Header& header() const { return header_; }
  const nlohmann::json& metadata() const { return metadata_; }
  const std::filesystem::path& path() const { return path_; }
  uint64_t bytes() const { return bytes_; }

  std::optional<Tile> tile(uint8_t z, uint32_t x, uint32_t y) const;  // throws Error on corrupt data

 private:
  std::shared_ptr<const std::vector<Entry>> leaf(uint64_t offset, uint64_t length) const;
  std::vector<uint8_t> read_internal(uint64_t offset, uint64_t length) const;

  std::filesystem::path path_;
  std::unique_ptr<File> file_;
  uint64_t bytes_ = 0;
  Header header_;
  std::vector<Entry> root_;
  nlohmann::json metadata_;
  mutable std::mutex mu_;
  mutable std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<const std::vector<Entry>>> leaves_;
};

}  // namespace gsa::pmtiles
