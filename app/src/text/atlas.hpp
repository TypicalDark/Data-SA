// Signed distance field glyph and icon atlas for map text (one R8 texture).
//
// Glyphs are rendered once at a base size as distance fields (stb_truetype) and drawn at any size
// with a sharp edge and a halo from the same texture. Icons are distance fields computed from
// their shapes, so they scale and take halos the same way.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gsa::text {

struct Glyph {
  bool valid = false;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // texture coordinates
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // quad relative to the pen on the baseline, base-size pixels, y down
  float advance = 0;                     // base-size pixels
};

enum class Icon { Square, Circle, Diamond, Triangle };

class Atlas {
 public:
  static constexpr int kSize = 1024;        // texture is kSize x kSize, one byte per texel
  static constexpr int kBase = 24;          // glyph base size in pixels
  static constexpr int kPad = 6;            // distance field reach outside a glyph, base pixels
  static constexpr int kIconRadius = 16;    // icon base radius in pixels
  static constexpr uint8_t kOnEdge = 160;   // texel value on the outline
  static constexpr float kDistScale = 160.0f / float(kPad);  // texel value per base pixel of distance

  Atlas();
  ~Atlas();
  Atlas(const Atlas&) = delete;
  Atlas& operator=(const Atlas&) = delete;

  // Loads a TrueType font; returns its id or -1.
  int add_font(const std::string& path);
  int font_count() const { return int(fonts_.size()); }

  // The glyph for a code point, rendered on first use. Unknown code points fall back to '?'.
  const Glyph& glyph(int font, uint32_t codepoint);
  const Glyph& icon(Icon kind);
  // Kerning between two code points, base pixels.
  float kern(int font, uint32_t a, uint32_t b) const;
  // Ascent and descent (negative), base pixels.
  float ascent(int font) const;
  float descent(int font) const;

  const std::vector<uint8_t>& pixels() const { return pixels_; }
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }
  // Times the atlas filled up and was cleared; glyph references from before are stale.
  int generation() const { return generation_; }

 private:
  struct Font;
  bool place(int w, int h, int& x, int& y);
  void reset();
  Glyph store(const uint8_t* sdf, int w, int h, float xoff, float yoff, float advance);

  std::vector<Font*> fonts_;
  std::map<std::pair<int, uint32_t>, Glyph> glyphs_;
  std::map<int, Glyph> icons_;
  std::vector<uint8_t> pixels_;
  int shelf_x_ = 1, shelf_y_ = 1, shelf_h_ = 0;
  bool dirty_ = true;
  int generation_ = 0;
};

// Decodes UTF-8; invalid bytes become U+FFFD.
std::vector<uint32_t> utf8_decode(const std::string& s);

}  // namespace gsa::text
