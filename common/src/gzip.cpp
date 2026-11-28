#include "gsa/gzip.hpp"

#include <algorithm>
#include <stdexcept>

#include <zlib.h>

namespace gsa {

std::vector<uint8_t> gunzip(const uint8_t* data, size_t n, size_t max_out) {
  z_stream s{};
  // 32 + MAX_WBITS: detect gzip or zlib header automatically
  if (inflateInit2(&s, 32 + MAX_WBITS) != Z_OK) throw std::runtime_error("inflateInit2 failed");
  std::vector<uint8_t> out;
  out.resize(n * 4 + 1024);
  s.next_in = const_cast<Bytef*>(data);
  s.avail_in = static_cast<uInt>(n);
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    if (s.total_out >= out.size()) {
      if (out.size() >= max_out) {
        inflateEnd(&s);
        throw std::runtime_error("inflated data too large");
      }
      out.resize(std::min(out.size() * 2, max_out));
    }
    s.next_out = out.data() + s.total_out;
    s.avail_out = static_cast<uInt>(out.size() - s.total_out);
    rc = inflate(&s, Z_NO_FLUSH);
    if (rc == Z_BUF_ERROR && s.avail_in == 0) break;  // truncated input
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
      inflateEnd(&s);
      throw std::runtime_error("corrupt compressed data");
    }
  }
  const bool done = rc == Z_STREAM_END;
  out.resize(s.total_out);
  inflateEnd(&s);
  if (!done) throw std::runtime_error("truncated compressed data");
  return out;
}

std::string gzip(const char* data, size_t n, int level) {
  z_stream s{};
  // 16 + MAX_WBITS: write a gzip header and trailer
  if (deflateInit2(&s, level, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) throw std::runtime_error("deflateInit2 failed");
  std::string out;
  out.resize(deflateBound(&s, static_cast<uLong>(n)));
  s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
  s.avail_in = static_cast<uInt>(n);
  s.next_out = reinterpret_cast<Bytef*>(out.data());
  s.avail_out = static_cast<uInt>(out.size());
  const int rc = deflate(&s, Z_FINISH);
  out.resize(s.total_out);
  deflateEnd(&s);
  if (rc != Z_STREAM_END) throw std::runtime_error("deflate failed");
  return out;
}

}  // namespace gsa
