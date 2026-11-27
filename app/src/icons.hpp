// Line icons for the interface, drawn with ImGui's draw list on a 24-unit grid like the
// website's SVG icons.
#pragma once

#include <imgui.h>

#include "layers.hpp"

namespace gsa::icons {

enum class Id {
  Globe, Bolt, Bars, Drop, House, Layers, Filter, Find, List, Map, Plane, Star, Bookmark, Ruler, Clock, Chart,
  Keyboard, Moon, Search, Eye, EyeOff, Close, Plus, Minus, Home, ChevronLeft, ChevronRight, Leaf, Wheat, Pin, Alert, Cube
};

// Draws icon `id` centred in a box of `size` pixels at `centre`.
void draw(ImDrawList* dl, Id id, ImVec2 centre, float size, ImU32 colour, float thickness = 1.6f);
// A layer's swatch (circle, square, line ...) in its colour.
void swatch(ImDrawList* dl, layers::Swatch s, ImVec2 centre, float size, ImU32 colour);

}  // namespace gsa::icons
