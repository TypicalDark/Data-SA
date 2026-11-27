#include "gsa/pmtiles.hpp"

#include <algorithm>
#include <cstring>

#include "gsa/gzip.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gsa::pmtiles {

namespace {
constexpr size_t kLeafCacheMax = 512;

uint64_t u64le(const uint8_t* b) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
  return v;
}
int32_t i32le(const uint8_t* b) {
  uint32_t v = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
  return static_cast<int32_t>(v);
}
uint64_t read_varint(const std::vector<uint8_t>& b, size_t& pos) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (pos >= b.size()) throw Error("corrupt directory");
    uint8_t byte = b[pos++];
    v |= uint64_t(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return v;
  }
  throw Error("corrupt directory");
}
}  // namespace

const char* content_encoding(Compression c) {
  switch (c) {
    case Compression::Gzip: return "gzip";
    case Compression::Brotli: return "br";
    case Compression::Zstd: return "zstd";
    default: return nullptr;
  }
}
const char* content_type(TileType t) {
  switch (t) {
    case TileType::Mvt: return "application/vnd.mapbox-vector-tile";
    case TileType::Png: return "image/png";
    case TileType::Jpeg: return "image/jpeg";
    case TileType::Webp: return "image/webp";
    case TileType::Avif: return "image/avif";
    default: return "application/octet-stream";
  }
}
const char* type_name(TileType t) {
  switch (t) {
    case TileType::Mvt: return "mvt";
    case TileType::Png: return "png";
    case TileType::Jpeg: return "jpeg";
    case TileType::Webp: return "webp";
    case TileType::Avif: return "avif";
    default: return "unknown";
  }
}

Header Header::parse(const uint8_t* b, size_t n) {
  if (n < kLength || std::memcmp(b, "PMTiles", 7) != 0 || b[7] != 3) throw Error("not a PMTiles v3 file");
  Header h;
  h.root_offset = u64le(b + 8);
  h.root_length = u64le(b + 16);
  h.metadata_offset = u64le(b + 24);
  h.metadata_length = u64le(b + 32);
  h.leaf_offset = u64le(b + 40);
  h.leaf_length = u64le(b + 48);
  h.data_offset = u64le(b + 56);
  h.data_length = u64le(b + 64);
  h.addressed_tiles = u64le(b + 72);
  h.tile_entries = u64le(b + 80);
  h.tile_contents = u64le(b + 88);
  h.clustered = b[96] == 1;
  h.internal_compression = static_cast<Compression>(b[97] <= 4 ? b[97] : 0);
  h.tile_compression = static_cast<Compression>(b[98] <= 4 ? b[98] : 0);
  h.tile_type = static_cast<TileType>(b[99] <= 5 ? b[99] : 0);
  h.min_zoom = b[100];
  h.max_zoom = b[101];
  for (int i = 0; i < 4; ++i) h.bounds[i] = i32le(b + 102 + 4 * i) / 1e7;
  h.center_zoom = b[118];
  h.center[0] = i32le(b + 119) / 1e7;
  h.center[1] = i32le(b + 123) / 1e7;
  return h;
}

std::optional<uint64_t> zxy_to_tile_id(uint8_t z, uint32_t x, uint32_t y) {
  if (z > 31) return std::nullopt;
  const uint64_t n = uint64_t(1) << z;
  if (x >= n || y >= n) return std::nullopt;
  const uint64_t acc = ((uint64_t(1) << (2 * z)) - 1) / 3;  // tiles in all lower zooms
  uint64_t xx = x, yy = y, d = 0;
  for (uint64_t s = n / 2; s > 0; s /= 2) {
    const uint64_t rx = (xx & s) ? 1 : 0;
    const uint64_t ry = (yy & s) ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    if (ry == 0) {
      if (rx == 1) {
        xx = n - 1 - xx;
        yy = n - 1 - yy;
      }
      std::swap(xx, yy);
    }
  }
  return acc + d;
}

std::vector<Entry> decode_directory(const std::vector<uint8_t>& b) {
  size_t p = 0;
  const uint64_t n = read_varint(b, p);
  if (n > b.size()) throw Error("corrupt directory");  // every entry needs at least 4 bytes
  std::vector<Entry> e(n);
  uint64_t last = 0;
  for (auto& it : e) it.tile_id = last += read_varint(b, p);
  for (auto& it : e) it.run_length = static_cast<uint32_t>(read_varint(b, p));
  for (auto& it : e) it.length = static_cast<uint32_t>(read_varint(b, p));
  for (size_t i = 0; i < n; ++i) {
    const uint64_t v = read_varint(b, p);
    if (v == 0 && i > 0) {
      e[i].offset = e[i - 1].offset + e[i - 1].length;
    } else {
      if (v == 0) throw Error("corrupt directory");
      e[i].offset = v - 1;
    }
  }
  return e;
}

std::optional<Entry> find_entry(const std::vector<Entry>& dir, uint64_t id) {
  auto it = std::upper_bound(dir.begin(), dir.end(), id,
                             [](uint64_t v, const Entry& e) { return v < e.tile_id; });
  if (it == dir.begin()) return std::nullopt;
  const Entry& e = *(it - 1);
  if (e.run_length == 0) return e;  // leaf directory pointer
  if (id < e.tile_id + e.run_length) return e;
  return std::nullopt;
}

// ---------------------------------------------------------------- File

class File {
 public:
  explicit File(const std::filesystem::path& p) {
#ifdef _WIN32
    h_ = CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) throw Error("cannot open " + p.string());
    LARGE_INTEGER sz;
    GetFileSizeEx(h_, &sz);
    size_ = uint64_t(sz.QuadPart);
#else
    fd_ = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) throw Error("cannot open " + p.string());
    struct stat st{};
    ::fstat(fd_, &st);
    size_ = uint64_t(st.st_size);
#endif
  }
  ~File() {
#ifdef _WIN32
    CloseHandle(h_);
#else
    ::close(fd_);
#endif
  }
  uint64_t size() const { return size_; }
  std::vector<uint8_t> read(uint64_t off, uint64_t len) const {
    if (off > size_ || len > size_ - off) throw Error("read past end of archive");
    std::vector<uint8_t> buf(len);
    uint64_t done = 0;
    while (done < len) {
#ifdef _WIN32
      OVERLAPPED ov{};
      const uint64_t at = off + done;
      ov.Offset = DWORD(at & 0xffffffffu);
      ov.OffsetHigh = DWORD(at >> 32);
      DWORD got = 0;
      const DWORD want = DWORD(std::min<uint64_t>(len - done, 1u << 30));
      if (!ReadFile(h_, buf.data() + done, want, &got, &ov) || got == 0) throw Error("read failed");
#else
      const ssize_t got = ::pread(fd_, buf.data() + done, len - done, off_t(off + done));
      if (got <= 0) throw Error("read failed");
#endif
      done += uint64_t(got);
    }
    return buf;
  }

 private:
#ifdef _WIN32
  HANDLE h_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
  uint64_t size_ = 0;
};

// ---------------------------------------------------------------- Archive

Archive::Archive(const std::filesystem::path& path) : path_(path), file_(std::make_unique<File>(path)) {
  bytes_ = file_->size();
  auto hb = file_->read(0, Header::kLength);
  header_ = Header::parse(hb.data(), hb.size());
  root_ = decode_directory(read_internal(header_.root_offset, header_.root_length));
  if (header_.metadata_length > 0) {
    auto m = read_internal(header_.metadata_offset, header_.metadata_length);
    metadata_ = nlohmann::json::parse(m.begin(), m.end(), nullptr, /*allow_exceptions=*/false);
    if (metadata_.is_discarded()) metadata_ = nlohmann::json::object();
  } else {
    metadata_ = nlohmann::json::object();
  }
}

Archive::~Archive() = default;

std::vector<uint8_t> Archive::read_internal(uint64_t offset, uint64_t length) const {
  auto raw = file_->read(offset, length);
  switch (header_.internal_compression) {
    case Compression::None:
    case Compression::Unknown: return raw;
    case Compression::Gzip:
      try {
        return gunzip(raw.data(), raw.size(), size_t(64) << 20);
      } catch (const std::exception& e) {
        throw Error(e.what());
      }
    default: throw Error("unsupported internal compression");
  }
}

std::shared_ptr<const std::vector<Entry>> Archive::leaf(uint64_t offset, uint64_t length) const {
  const auto key = std::make_pair(offset, length);
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = leaves_.find(key); it != leaves_.end()) return it->second;
  }
  auto dir = std::make_shared<const std::vector<Entry>>(decode_directory(read_internal(header_.leaf_offset + offset, length)));
  std::lock_guard<std::mutex> lk(mu_);
  if (leaves_.size() >= kLeafCacheMax) leaves_.clear();
  leaves_.emplace(key, dir);
  return dir;
}

std::optional<Tile> Archive::tile(uint8_t z, uint32_t x, uint32_t y) const {
  if (z < header_.min_zoom || z > header_.max_zoom) return std::nullopt;
  const auto id = zxy_to_tile_id(z, x, y);
  if (!id) return std::nullopt;
  std::shared_ptr<const std::vector<Entry>> hold;
  const std::vector<Entry>* cur = &root_;
  for (int depth = 0; depth < 4; ++depth) {
    const auto e = find_entry(*cur, *id);
    if (!e) return std::nullopt;
    if (e->run_length > 0) {
      return Tile{file_->read(header_.data_offset + e->offset, e->length), header_.tile_compression, header_.tile_type};
    }
    hold = leaf(e->offset, e->length);
    cur = hold.get();
  }
  throw Error("directory nesting too deep");
}

}  // namespace gsa::pmtiles
