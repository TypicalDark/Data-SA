#pragma once

#include <cstdint>
#include <vector>

namespace gsa {

// Inflate gzip or zlib data. Throws std::runtime_error on bad input or when the
// output would exceed max_out bytes (guards against decompression bombs).
std::vector<uint8_t> gunzip(const uint8_t* data, size_t n, size_t max_out = size_t(512) << 20);

}  // namespace gsa
