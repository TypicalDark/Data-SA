#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gsa {

// Inflate gzip or zlib data. Throws std::runtime_error on bad input or when the
// output would exceed max_out bytes (guards against decompression bombs).
std::vector<uint8_t> gunzip(const uint8_t* data, size_t n, size_t max_out = size_t(512) << 20);

// Compress to gzip (RFC 1952), for HTTP Content-Encoding: gzip. level 1 (fast) to 9 (small).
std::string gzip(const char* data, size_t n, int level = 6);

}  // namespace gsa
