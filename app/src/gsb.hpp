// Grid of SA national buildings (GSB1), as the website stores them: one z12 tile's bytes from the
// `buildings` pack, decoded into a vector-tile layer "building" so the map draws them like any pack.
// A port of the website's gsb_decode.js.
//
// Tile bytes: varint nR, nP | rects: flags[nR], zigzag dx[nR], dy[nR] (tile/16384), L[nR], W[nR]
// (metres x 2), angle[nR] (delta, pi/128 steps), heights (0.5 m, only when flag & 8) | polygons: flags[nP],
// heights (flag & 8), vertex counts, zigzag vertex deltas (tile/16384; each first vertex is a delta
// from the previous polygon's first vertex).
// Flags: bits 0-1 kind (0 house, 1 flats, 2 work, 3 other), bit 2 machine-traced, bit 3 height known,
// bit 4 Microsoft.
#pragma once

#include <cstdint>
#include <string>

#include "mvt.hpp"

namespace gsa::gsb {

// Every building as a Polygon feature with one ring (tile units, y down), properties "h" (metres,
// measured or estimated as on the website) and "k" (kind 0-3). Throws on malformed bytes.
mvt::Tile decode(const std::string& bytes, uint32_t tx, uint32_t ty);

// The website's height estimate from footprint area (m2), kind and whether it was machine-traced.
double estimate_height(double area_m2, int kind, bool machine);

}  // namespace gsa::gsb
