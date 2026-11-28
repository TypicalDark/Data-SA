#include "atlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace gsa::text {

struct Atlas::Font {
  std::vector<unsigned char> data;
  stbtt_fontinfo info{};
  float scale = 1;  // font units -> base pixels
  float ascent = 0, descent = 0;
};

Atlas::Atlas() : pixels_(size_t(kSize) * kSize, 0) {}

Atlas::~Atlas() {
  for (auto* f : fonts_) delete f;
}

int Atlas::add_font(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return -1;
  auto* f = new Font;
  f->data.assign(std::istreambuf_iterator<char>(in), {});
  if (f->data.empty() || !stbtt_InitFont(&f->info, f->data.data(), stbtt_GetFontOffsetForIndex(f->data.data(), 0))) {
    delete f;
    return -1;
  }
  f->scale = stbtt_ScaleForPixelHeight(&f->info, float(kBase));
  int a, d, g;
  stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
  f->ascent = float(a) * f->scale;
  f->descent = float(d) * f->scale;
  fonts_.push_back(f);
  return int(fonts_.size()) - 1;
}

float Atlas::ascent(int font) const { return font >= 0 && font < int(fonts_.size()) ? fonts_[size_t(font)]->ascent : 0; }
float Atlas::descent(int font) const { return font >= 0 && font < int(fonts_.size()) ? fonts_[size_t(font)]->descent : 0; }

float Atlas::kern(int font, uint32_t a, uint32_t b) const {
  if (font < 0 || font >= int(fonts_.size())) return 0;
  const Font* f = fonts_[size_t(font)];
  return float(stbtt_GetCodepointKernAdvance(&f->info, int(a), int(b))) * f->scale;
}

bool Atlas::place(int w, int h, int& x, int& y) {
  // shelf packing with a one-texel gutter
  if (shelf_x_ + w + 1 > kSize) {
    shelf_x_ = 1;
    shelf_y_ += shelf_h_ + 1;
    shelf_h_ = 0;
  }
  if (shelf_y_ + h + 1 > kSize || w + 2 > kSize) return false;
  x = shelf_x_;
  y = shelf_y_;
  shelf_x_ += w + 1;
  shelf_h_ = std::max(shelf_h_, h);
  return true;
}

void Atlas::reset() {
  glyphs_.clear();
  icons_.clear();
  std::fill(pixels_.begin(), pixels_.end(), uint8_t(0));
  shelf_x_ = shelf_y_ = 1;
  shelf_h_ = 0;
  dirty_ = true;
  ++generation_;
}

Glyph Atlas::store(const uint8_t* sdf, int w, int h, float xoff, float yoff, float advance) {
  Glyph g;
  g.advance = advance;
  if (!sdf || w <= 0 || h <= 0) {
    g.valid = true;  // a space: advance only
    return g;
  }
  int x, y;
  if (!place(w, h, x, y)) {
    reset();
    if (!place(w, h, x, y)) return g;
  }
  for (int r = 0; r < h; ++r) std::memcpy(&pixels_[size_t(y + r) * kSize + size_t(x)], sdf + size_t(r) * size_t(w), size_t(w));
  dirty_ = true;
  g.valid = true;
  g.u0 = float(x) / kSize;
  g.v0 = float(y) / kSize;
  g.u1 = float(x + w) / kSize;
  g.v1 = float(y + h) / kSize;
  g.x0 = xoff;
  g.y0 = yoff;
  g.x1 = xoff + float(w);
  g.y1 = yoff + float(h);
  return g;
}

const Glyph& Atlas::glyph(int font, uint32_t cp) {
  static const Glyph none;
  if (font < 0 || font >= int(fonts_.size())) return none;
  const auto key = std::make_pair(font, cp);
  if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
  Font* f = fonts_[size_t(font)];
  int index = stbtt_FindGlyphIndex(&f->info, int(cp));
  if (index == 0 && cp != '?' && cp > 32) {
    const Glyph q = glyph(font, '?');
    return glyphs_[key] = q;
  }
  int adv, lsb;
  stbtt_GetGlyphHMetrics(&f->info, index, &adv, &lsb);
  int w = 0, h = 0, xo = 0, yo = 0;
  unsigned char* sdf = stbtt_GetGlyphSDF(&f->info, f->scale, index, kPad, kOnEdge, kDistScale, &w, &h, &xo, &yo);
  Glyph g = store(sdf, w, h, float(xo), float(yo), float(adv) * f->scale);
  if (sdf) stbtt_FreeSDF(sdf, nullptr);
  return glyphs_[key] = g;
}

namespace {

// Signed distance to an icon outline in base pixels, negative inside; (x, y) relative to the centre.
float icon_distance(Icon kind, float x, float y, float r) {
  switch (kind) {
    case Icon::Circle:
      return std::sqrt(x * x + y * y) - r;
    case Icon::Square: {
      const float s = r * 0.86f;  // a square reads as large as a circle at a smaller half side
      const float dx = std::abs(x) - s, dy = std::abs(y) - s;
      const float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
      return std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f);
    }
    case Icon::Diamond:
      return (std::abs(x) + std::abs(y) - r) * 0.70710678f;
    case Icon::Triangle: {
      // equilateral, pointing up (Inigo Quilez's exact distance), half side h, y up
      const float k = 1.7320508f, h = r * 0.95f;
      float px = std::abs(x) - h, py = -y + h / k;
      if (px + k * py > 0) {
        const float nx = (px - k * py) / 2, ny = (-k * px - py) / 2;
        px = nx;
        py = ny;
      }
      px -= std::clamp(px, -2 * h, 0.0f);
      return -std::sqrt(px * px + py * py) * (py > 0 ? 1.0f : -1.0f);
    }
  }
  return 0;
}

}  // namespace

const Glyph& Atlas::icon(Icon kind) {
  const int key = int(kind);
  if (auto it = icons_.find(key); it != icons_.end()) return it->second;
  const int side = 2 * (kIconRadius + kPad);
  std::vector<uint8_t> sdf(size_t(side) * size_t(side));
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x) {
      const float px = float(x) + 0.5f - float(side) / 2, py = float(y) + 0.5f - float(side) / 2;
      const float d = icon_distance(kind, px, py, float(kIconRadius));
      sdf[size_t(y) * size_t(side) + size_t(x)] = uint8_t(std::clamp(float(kOnEdge) - d * kDistScale, 0.0f, 255.0f));
    }
  const float h = float(side) / 2;
  Glyph g = store(sdf.data(), side, side, -h, -h, 0);
  return icons_[key] = g;
}

std::vector<uint32_t> utf8_decode(const std::string& s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    size_t n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 6) {
      cp = c & 0x1f;
      n = 2;
    } else if ((c >> 4) == 14) {
      cp = c & 0x0f;
      n = 3;
    } else if ((c >> 3) == 30) {
      cp = c & 0x07;
      n = 4;
    } else {
      out.push_back(0xfffd);
      ++i;
      continue;
    }
    if (i + n > s.size()) {
      out.push_back(0xfffd);
      break;
    }
    bool ok = true;
    for (size_t k = 1; k < n; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc >> 6) != 2) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3f);
    }
    if (!ok) {
      out.push_back(0xfffd);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

}  // namespace gsa::text
