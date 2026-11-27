// Interface tokens from the website (:root in index.html, dark theme).
#pragma once

#include <imgui.h>

#include "style.hpp"

namespace gsa::ui {

inline constexpr const char* kGround = "#070d12";
inline constexpr const char* kLand = "#0d171e";
inline constexpr const char* kInk = "#e3eef3";
inline constexpr const char* kMuted = "#8aa0ae";
inline constexpr const char* kFaint = "#5d7383";
inline constexpr const char* kLine = "#22394a";
inline constexpr const char* kPanel = "#0b161d";
inline constexpr const char* kHover = "#13252f";
inline constexpr const char* kAccent = "#7df9ff";
inline constexpr const char* kAccentInk = "#03161a";
inline constexpr const char* kWarn = "#ffb020";
inline constexpr const char* kOk = "#3ddc84";

inline constexpr float kRailW = 60;
inline constexpr float kSideW = 380;
inline constexpr float kStatusH = 32;

inline ImVec4 col(const char* h, float a = 1.0f) {
  const auto c = style::hex(h, a);
  return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a);
}
inline ImU32 u32(const char* h, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(col(h, a)); }
inline ImU32 u32(style::Rgba c) { return IM_COL32(c.r, c.g, c.b, c.a); }

}  // namespace gsa::ui
