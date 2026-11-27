#include "icons.hpp"

#include <cmath>

namespace gsa::icons {

namespace {

struct Pen {
  ImDrawList* dl;
  ImVec2 o;  // top-left of the 24-unit box
  float k;   // pixels per unit
  ImU32 c;
  float t;
  ImVec2 p(float x, float y) const { return ImVec2(o.x + x * k, o.y + y * k); }
  void line(float x0, float y0, float x1, float y1) const { dl->AddLine(p(x0, y0), p(x1, y1), c, t); }
  void poly(std::initializer_list<float> xy, bool closed) const {
    ImVec2 pts[32];
    int n = 0;
    for (auto it = xy.begin(); it != xy.end() && n < 32; it += 2) pts[n++] = p(*it, *(it + 1));
    dl->AddPolyline(pts, n, c, closed ? ImDrawFlags_Closed : ImDrawFlags_None, t);
  }
  void circle(float x, float y, float r) const { dl->AddCircle(p(x, y), r * k, c, 0, t); }
  void dot(float x, float y, float r) const { dl->AddCircleFilled(p(x, y), r * k, c); }
  void rect(float x0, float y0, float x1, float y1, float round = 0) const { dl->AddRect(p(x0, y0), p(x1, y1), c, round * k, 0, t); }
  void arc(float x, float y, float r, float a0, float a1) const {
    dl->PathArcTo(p(x, y), r * k, a0, a1, 16);
    dl->PathStroke(c, ImDrawFlags_None, t);
  }
};

constexpr float kPi = 3.14159265f;

}  // namespace

void draw(ImDrawList* dl, Id id, ImVec2 centre, float size, ImU32 colour, float thickness) {
  const float k = size / 24.0f;
  const Pen g{dl, ImVec2(centre.x - size / 2, centre.y - size / 2), k, colour, thickness};
  switch (id) {
    case Id::Globe:
      g.circle(12, 12, 8);
      g.line(4, 12, 20, 12);
      dl->AddBezierCubic(g.p(12, 4), g.p(8, 8), g.p(8, 16), g.p(12, 20), colour, thickness);
      dl->AddBezierCubic(g.p(12, 4), g.p(16, 8), g.p(16, 16), g.p(12, 20), colour, thickness);
      break;
    case Id::Bolt: g.poly({13, 3, 6, 13, 11.5f, 13, 10, 21, 18, 10, 12.5f, 10, 13, 3}, true); break;
    case Id::Bars:
      g.line(6, 20, 6, 12);
      g.line(10, 20, 10, 6);
      g.line(14, 20, 14, 9);
      g.line(18, 20, 18, 14);
      g.line(4, 20, 20, 20);
      break;
    case Id::Drop:
      dl->AddBezierCubic(g.p(12, 3.5f), g.p(9, 8), g.p(6, 11), g.p(6, 14.5f), colour, thickness);
      dl->AddBezierCubic(g.p(12, 3.5f), g.p(15, 8), g.p(18, 11), g.p(18, 14.5f), colour, thickness);
      g.arc(12, 14.5f, 6, 0, kPi);
      break;
    case Id::House: g.poly({4, 11, 12, 4, 20, 11, 18, 11, 18, 20, 6, 20, 6, 11}, true); break;
    case Id::Layers:
      g.poly({12, 4, 21, 9, 12, 14, 3, 9}, true);
      g.poly({3, 13, 12, 18, 21, 13}, false);
      break;
    case Id::Filter: g.poly({4, 5, 20, 5, 14, 12, 14, 19, 10, 17, 10, 12}, true); break;
    case Id::Find:
      g.circle(10, 10, 6);
      g.line(14.5f, 14.5f, 20, 20);
      g.line(7, 10, 13, 10);
      g.line(10, 7, 10, 13);
      break;
    case Id::List:
      for (float y : {6.0f, 12.0f, 18.0f}) {
        g.dot(5, y, 1.1f);
        g.line(9, y, 20, y);
      }
      break;
    case Id::Map: g.poly({3, 6, 9, 4, 15, 6, 21, 4, 21, 18, 15, 20, 9, 18, 3, 20}, true);
      g.line(9, 4, 9, 18);
      g.line(15, 6, 15, 20);
      break;
    case Id::Plane:
      g.poly({12, 3, 13.5f, 9, 21, 13, 21, 14.5f, 13.5f, 12.5f, 13, 18, 15.5f, 20, 15.5f, 21, 12, 20, 8.5f, 21, 8.5f, 20, 11, 18, 10.5f,
              12.5f, 3, 14.5f, 3, 13, 10.5f, 9},
             true);
      break;
    case Id::Star: {
      float pts[20];
      for (int i = 0; i < 10; ++i) {
        const float r = i % 2 ? 3.6f : 8.5f, a = -kPi / 2 + float(i) * kPi / 5;
        pts[2 * i] = 12 + r * std::cos(a);
        pts[2 * i + 1] = 12.5f + r * std::sin(a);
      }
      ImVec2 v[10];
      for (int i = 0; i < 10; ++i) v[i] = g.p(pts[2 * i], pts[2 * i + 1]);
      dl->AddPolyline(v, 10, colour, ImDrawFlags_Closed, thickness);
      break;
    }
    case Id::Bookmark: g.poly({7, 4, 17, 4, 17, 20, 12, 16, 7, 20}, true); break;
    case Id::Ruler:
      g.poly({3, 16, 16, 3, 21, 8, 8, 21}, true);
      g.line(7, 12, 9, 14);
      g.line(10, 9, 12, 11);
      g.line(13, 6, 15, 8);
      break;
    case Id::Clock:
      g.circle(12, 12, 8);
      g.line(12, 12, 12, 7);
      g.line(12, 12, 15.5f, 14);
      break;
    case Id::Chart:
      g.line(4, 20, 20, 20);
      g.line(7, 20, 7, 13);
      g.line(12, 20, 12, 8);
      g.line(17, 20, 17, 11);
      break;
    case Id::Keyboard:
      g.rect(3, 7, 21, 17, 2);
      for (float x : {7.0f, 10.0f, 13.0f, 16.0f}) g.dot(x, 10.5f, 0.8f);
      g.line(8, 14, 16, 14);
      break;
    case Id::Moon:
      g.arc(12, 12, 8, kPi * 0.35f, kPi * 1.85f);
      dl->AddBezierCubic(g.p(15.2f, 4.7f), g.p(10, 7), g.p(11, 16), g.p(19.2f, 15.3f), colour, thickness);
      break;
    case Id::Search:
      g.circle(10.5f, 10.5f, 6);
      g.line(15, 15, 20, 20);
      break;
    case Id::Eye:
      dl->AddBezierQuadratic(g.p(3, 12), g.p(12, 3), g.p(21, 12), colour, thickness);
      dl->AddBezierQuadratic(g.p(3, 12), g.p(12, 21), g.p(21, 12), colour, thickness);
      g.circle(12, 12, 2.6f);
      break;
    case Id::EyeOff:
      dl->AddBezierQuadratic(g.p(3, 12), g.p(12, 3), g.p(21, 12), colour, thickness);
      dl->AddBezierQuadratic(g.p(3, 12), g.p(12, 21), g.p(21, 12), colour, thickness);
      g.line(5, 19, 19, 5);
      break;
    case Id::Close:
      g.line(7, 7, 17, 17);
      g.line(17, 7, 7, 17);
      break;
    case Id::Plus:
      g.line(12, 6, 12, 18);
      g.line(6, 12, 18, 12);
      break;
    case Id::Minus: g.line(6, 12, 18, 12); break;
    case Id::Home: g.poly({5, 11, 12, 5, 19, 11, 19, 19, 14, 19, 14, 14, 10, 14, 10, 19, 5, 19}, true); break;
    case Id::ChevronLeft: g.poly({14, 6, 8, 12, 14, 18}, false); break;
    case Id::ChevronRight: g.poly({10, 6, 16, 12, 10, 18}, false); break;
    case Id::Leaf:
      dl->AddBezierQuadratic(g.p(5, 19), g.p(5, 5), g.p(19, 5), colour, thickness);
      dl->AddBezierQuadratic(g.p(5, 19), g.p(19, 19), g.p(19, 5), colour, thickness);
      g.line(5, 19, 14, 10);
      break;
    case Id::Wheat:
      g.line(12, 21, 12, 5);
      for (float y : {7.0f, 11.0f, 15.0f}) {
        g.line(12, y + 2, 8, y);
        g.line(12, y + 2, 16, y);
      }
      break;
    case Id::Pin:
      g.circle(12, 10, 3);
      dl->AddBezierCubic(g.p(12, 21), g.p(6, 14), g.p(5, 10), g.p(7, 6.5f), colour, thickness);
      dl->AddBezierCubic(g.p(12, 21), g.p(18, 14), g.p(19, 10), g.p(17, 6.5f), colour, thickness);
      g.arc(12, 10, 5.8f, kPi * 1.18f, kPi * 1.82f);
      break;
    case Id::Alert:
      g.poly({12, 4, 21, 20, 3, 20}, true);
      g.line(12, 10, 12, 14);
      g.dot(12, 17, 0.9f);
      break;
    case Id::Cube:
      g.poly({12, 3, 20, 7.5f, 20, 16.5f, 12, 21, 4, 16.5f, 4, 7.5f}, true);
      g.poly({4, 7.5f, 12, 12, 20, 7.5f}, false);
      g.line(12, 12, 12, 21);
      break;
  }
}

void swatch(ImDrawList* dl, layers::Swatch s, ImVec2 c, float size, ImU32 col) {
  const float h = size / 2;
  switch (s) {
    case layers::Swatch::Circle: dl->AddCircleFilled(c, h * 0.7f, col); break;
    case layers::Swatch::Square: dl->AddRectFilled(ImVec2(c.x - h * 0.62f, c.y - h * 0.62f), ImVec2(c.x + h * 0.62f, c.y + h * 0.62f), col); break;
    case layers::Swatch::Line: dl->AddLine(ImVec2(c.x - h * 0.85f, c.y + h * 0.45f), ImVec2(c.x + h * 0.85f, c.y - h * 0.45f), col, 2.2f); break;
    case layers::Swatch::Fill: dl->AddRectFilled(ImVec2(c.x - h * 0.85f, c.y - h * 0.6f), ImVec2(c.x + h * 0.85f, c.y + h * 0.6f), col, 2); break;
    case layers::Swatch::Diamond:
      dl->AddQuadFilled(ImVec2(c.x, c.y - h * 0.8f), ImVec2(c.x + h * 0.8f, c.y), ImVec2(c.x, c.y + h * 0.8f), ImVec2(c.x - h * 0.8f, c.y), col);
      break;
    case layers::Swatch::Plane:
      dl->AddTriangleFilled(ImVec2(c.x + h * 0.8f, c.y), ImVec2(c.x - h * 0.7f, c.y - h * 0.6f), ImVec2(c.x - h * 0.7f, c.y + h * 0.6f), col);
      break;
  }
}

}  // namespace gsa::icons
