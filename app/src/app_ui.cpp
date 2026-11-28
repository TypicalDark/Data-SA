// The website's frame around the map: icon rail, docked sidebar (search, card or panel, the
// On the map list), layer catalogue, breadcrumb, map buttons, status bar, measure overlay.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <ctime>
#include <functional>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "app.hpp"
#include "commands.hpp"
#include "icons.hpp"
#include "ui_theme.hpp"

namespace gsa {

using nlohmann::json;
using namespace ui;

namespace {

constexpr ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;


}  // namespace

void App::block_window() {
  // the map keeps its labels out of every interface window
  const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
  blockers_.push_back({p.x - 4, p.y - 4, p.x + sz.x + 4, p.y + sz.y + 4});
}

void App::ui() {
  // the open card's place: a ring on the map, as the website marks the selection
  if (const Card& c = cards_.back(); c.kind == Card::Facility || c.kind == Card::Spot) {
    double sx, sy;
    cam_.to_screen(geo::mx(c.lon), geo::my(c.lat), sx, sy);
    auto* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 p{float(sx), float(sy)};
    dl->AddCircle(p, 11, u32(kGround, 0.8f), 0, 4.5f);
    dl->AddCircle(p, 11, u32(kAccent), 0, 2.0f);
    if (c.kind == Card::Spot) dl->AddCircleFilled(p, 3, u32(kAccent));
  }
  if (const Card& c = cards_.back(); c.kind == Card::Near) {
    // the NEAR circle on the ground
    const double km = c.props.value("km", 25.0), dlat = km / 111.0, dlon = dlat / std::cos(c.lat * geo::kPi / 180);
    std::vector<ImVec2> ring;
    for (int i = 0; i < 96; ++i) {
      const double t = i * 2 * geo::kPi / 96;
      double sx, sy;
      cam_.to_screen(geo::mx(c.lon + dlon * std::cos(t)), geo::my(c.lat + dlat * std::sin(t)), sx, sy);
      ring.push_back(ImVec2(float(sx), float(sy)));
    }
    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddPolyline(ring.data(), int(ring.size()), u32(kAccent, 0.7f), ImDrawFlags_Closed, 1.6f);
    double sx, sy;
    cam_.to_screen(geo::mx(c.lon), geo::my(c.lat), sx, sy);
    dl->AddCircleFilled(ImVec2(float(sx), float(sy)), 3.5f, u32(kAccent));
  }
  live_draw();
  ui_measure();
  ui_rail();
  ui_side();
  ui_crumb();
  ui_filter_strip();
  ui_controls();
  ui_status();
  if (catalogue_open_) ui_catalogue();
  if (!hover_name_.empty() && !dragging_ && opt_.screenshot.empty()) {
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(hover_name_.c_str());
    ImGui::EndTooltip();
  }
  ui_toast();
}

// ------------------------------------------------------------------ rail

void App::ui_rail() {
  const float h = float(cam_.h) - kStatusH;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(kRailW, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(kPanel));
  ImGui::Begin("##rail", nullptr, kFixed);
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(kRailW - 0.5f, 0), ImVec2(kRailW - 0.5f, h), u32(kLine));
  // logo
  dl->AddRect(ImVec2(12, 10), ImVec2(48, 46), u32(kAccent), 7, 0, 1.5f);
  const ImVec2 ls = font_head_->CalcTextSizeA(13, FLT_MAX, 0, "SA");
  dl->AddText(font_head_, 13, ImVec2(30 - ls.x / 2, 28 - ls.y / 2), u32(kAccent), "SA");

  struct Item {
    icons::Id icon;
    const char* label;
    std::function<bool()> active;
    std::function<void()> act;
    const char* tip;
  };
  auto topic = [&](icons::Id i, const char* label, const char* id, const char* tip) {
    return Item{i, label, [this, id] { return lstate_.topic == id; }, [this, id] { set_topic(id); }, tip};
  };
  auto panel = [&](icons::Id i, const char* label, Panel p, const char* tip) {
    return Item{i, label, [this, p] { return panel_ == p; }, [this, p] { open_panel(p); }, tip};
  };
  std::vector<Item> top = {
      topic(icons::Id::Globe, "START", "start", "Overview, nothing coloured (1)"),
      topic(icons::Id::Bolt, "GRID", "grid", "Power and grid (2)"),
      topic(icons::Id::Bars, "VOTE", "vote", "Votes and people (3)"),
      topic(icons::Id::Drop, "WATER", "water", "Water (4)"),
      topic(icons::Id::House, "PLOTS", "prop", "Property (5)"),
  };
  std::vector<Item> mid = {
      Item{icons::Id::Layers, "LAYERS", [this] { return catalogue_open_; }, [this] { catalogue_open_ = !catalogue_open_; },
           "Add layers from every topic (F2)"},
      panel(icons::Id::Filter, "FILTER", Panel::Filter, "Filter what is drawn (F3)"),
      Item{icons::Id::Find, "FIND", [this] { return panel_ == Panel::Find; },
           [this] {
             find_tab_ = 0;
             open_panel(Panel::Find);
           },
           "Find places where... (N)"},
      panel(icons::Id::List, "LIST", Panel::List, "List what is in view (F4)"),
      panel(icons::Id::Map, "AREAS", Panel::Areas, "Provinces and municipalities"),
      panel(icons::Id::Plane, "LIVE", Panel::Live, "Live planes and ships (F5)"),
      panel(icons::Id::Star, "WATCH", Panel::Watch, "Watchlist (Shift W)"),
      panel(icons::Id::Bookmark, "VIEWS", Panel::Views, "Saved views (F6)"),
      panel(icons::Id::Ruler, "MEASURE", Panel::Measure, "Measure (M)"),
      panel(icons::Id::Clock, "REACH", Panel::Reach, "Travel-time reach (F8)"),
      panel(icons::Id::Chart, "ANALYSE", Panel::Analyse, "Analyse an area (F9)"),
  };
  std::vector<Item> bottom = {
      panel(icons::Id::Keyboard, "KEYS", Panel::Keys, "Keys and commands (?)"),
      Item{icons::Id::Moon, "THEME", [] { return false; }, [this] { toast("Light and terminal themes come with the base map (milestone E)"); },
           "Theme"},
  };
  const float n = float(top.size() + mid.size() + bottom.size());
  const float ih = std::clamp((h - 64 - 24) / n, 34.0f, 44.0f);
  auto draw_items = [&](std::vector<Item>& items, float y) {
    for (auto& it : items) {
      ImGui::SetCursorPos(ImVec2(4, y));
      ImGui::PushID(it.label);
      const bool click = ImGui::InvisibleButton("##r", ImVec2(52, ih - 2));
      const bool hov = ImGui::IsItemHovered();
      const bool act = it.active();
      ImGui::PopID();
      const ImVec2 a(4, y), b(56, y + ih - 2);
      if (act || hov) dl->AddRectFilled(a, b, u32(kHover), 6);
      if (act) dl->AddRectFilled(ImVec2(0, y + 6), ImVec2(2.5f, y + ih - 8), u32(kAccent));
      const ImU32 c = act ? u32(kAccent) : hov ? u32(kInk) : u32(kMuted);
      icons::draw(dl, it.icon, ImVec2(30, y + ih * 0.38f), ih * 0.42f, c, 1.5f);
      const ImVec2 ts = font_mono_->CalcTextSizeA(9, FLT_MAX, 0, it.label);
      dl->AddText(font_mono_, 9, ImVec2(30 - ts.x / 2, y + ih * 0.66f), c, it.label);
      if (hov) ImGui::SetTooltip("%s", it.tip);
      if (click) it.act();
      y += ih;
    }
    return y;
  };
  float y = draw_items(top, 58);
  dl->AddLine(ImVec2(14, y + 4), ImVec2(46, y + 4), u32(kLine));
  y = draw_items(mid, y + 8);
  draw_items(bottom, std::max(y + 4, h - ih * float(bottom.size()) - 6));
  block_window();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ sidebar

void App::ui_side() {
  const float h = float(cam_.h) - kStatusH;
  if (!side_open_) {
    ImGui::SetNextWindowPos(ImVec2(kRailW, h / 2 - 22));
    ImGui::SetNextWindowSize(ImVec2(16, 44));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##sidetab", nullptr, kFixed);
    if (ImGui::InvisibleButton("##open", ImVec2(16, 44))) side_open_ = true;
    icons::draw(ImGui::GetWindowDrawList(), icons::Id::ChevronRight, ImVec2(kRailW + 8, h / 2), 14,
                ImGui::IsItemHovered() ? u32(kInk) : u32(kMuted));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the sidebar ( [ )");
    block_window();
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }
  const float x = kRailW, w = side_w();
  ImGui::SetNextWindowPos(ImVec2(x, 0));
  ImGui::SetNextWindowSize(ImVec2(w, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(kPanel, 0.97f));
  ImGui::Begin("##side", nullptr, kFixed | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(x + w - 0.5f, 0), ImVec2(x + w - 0.5f, h), u32(kLine));
  block_window();
  const float search_h = 79;
  // the On the map dock folds to its header while a panel needs the height (short windows), or
  // whenever the user folds it
  if (panel_ != dock_panel_) {
    dock_panel_ = panel_;
    dock_force_open_ = false;
  }
  const bool auto_fold = panel_ != Panel::None && h < 860 && panel_ != Panel::Filter;
  dock_folded_now_ = dock_fold_ || (auto_fold && !dock_force_open_);
  // open: tall enough for the rows on the map (header, tabs and base row are 132 px), at most 42% of the window
  const float dock_max = std::clamp(h * (h < 860 ? 0.36f : 0.42f), 230.0f, 330.0f);
  const float dock_h = dock_folded_now_ ? 40.0f : std::clamp(132.0f + dock_rows_used_ + 8.0f, std::min(190.0f, dock_max), dock_max);
  ui_search(x, 0, w);
  dl->AddLine(ImVec2(x, search_h), ImVec2(x + w, search_h), u32(kLine));
  ui_main(x, search_h + 1, w, h - search_h - dock_h - 1);
  dl->AddLine(ImVec2(x, h - dock_h), ImVec2(x + w, h - dock_h), u32(kLine));
  ui_dock(x, h - dock_h, w, dock_h);
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  // the tab that hides the sidebar, on its edge
  ImGui::SetNextWindowPos(ImVec2(x + w, h / 2 - 22));
  ImGui::SetNextWindowSize(ImVec2(14, 44));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(kPanel));
  ImGui::Begin("##sidetab", nullptr, kFixed);
  if (ImGui::InvisibleButton("##close", ImVec2(14, 44))) side_open_ = false;
  icons::draw(ImGui::GetWindowDrawList(), icons::Id::ChevronLeft, ImVec2(x + w + 7, h / 2), 12,
              ImGui::IsItemHovered() ? u32(kInk) : u32(kMuted));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide the sidebar ( [ )");
  block_window();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void App::ui_search(float x, float y, float w) {
  auto* dl = ImGui::GetWindowDrawList();
  const float bx = x + 14, by = y + 12, bw = w - 28, bh = 34;
  ImGui::SetCursorScreenPos(ImVec2(bx, by));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32, 9));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, col(kGround));
  ImGui::SetNextItemWidth(bw);
  if (focus_search_) {
    ImGui::SetKeyboardFocusHere();
    focus_search_ = false;
  }
  const bool enter = ImGui::InputTextWithHint("##q", "Search places, or type a command: GO Soweto", query_, sizeof query_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  search_active_ = ImGui::IsItemActive();
  const bool focused = ImGui::IsItemFocused();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  icons::draw(dl, icons::Id::Search, ImVec2(bx + 16, by + bh / 2), 15, u32(kMuted));
  // the "/" key hint
  const ImVec2 k0(bx + bw - 24, by + 9), k1(bx + bw - 9, by + bh - 9);
  dl->AddRect(k0, k1, u32(kLine), 3);
  dl->AddText(font_mono_, 10, ImVec2(k0.x + 4, k0.y + 1), u32(kMuted), "/");
  const std::string asof = "Data as of " + version_.value("data_as_of", std::string("25 Nov 2026")) + " \xc2\xb7 press / to search";
  dl->AddText(font_mono_, 11, ImVec2(bx, by + bh + 9), u32(kMuted), asof.c_str());

  if (enter && query_[0]) {
    const std::string line = query_;
    const auto p = commands::parse(line);
    if (p.kind != commands::Parsed::Search) {
      run_command(line);
      if (pending_fn_.empty()) {
        query_[0] = 0;
        hits_.clear();
      }
      ImGui::SetWindowFocus(nullptr);
    } else if (!hits_.empty()) {
      const auto h = hits_[size_t(std::clamp(hit_sel_, 0, int(hits_.size()) - 1))];
      run_command(h.label + " GO");
      query_[0] = 0;
      hits_.clear();
      ImGui::SetWindowFocus(nullptr);
    } else if (const auto t = tasks_for(line); !t.empty()) {
      query_[0] = 0;
      run_command(t[0].command);
      ImGui::SetWindowFocus(nullptr);
    } else {
      toast("Nothing found for " + line);
    }
  }
  if (focused && !hits_.empty()) {
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) hit_sel_ = std::min(hit_sel_ + 1, int(hits_.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) hit_sel_ = std::max(hit_sel_ - 1, 0);
  }
  if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    query_[0] = 0;
    hits_.clear();
    ImGui::SetWindowFocus(nullptr);
  }
}

void App::ui_main(float x, float y, float w, float h) {
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
  ImGui::BeginChild("##main", ImVec2(w - 1, h), ImGuiChildFlags_AlwaysUseWindowPadding);
  const std::string q = query_;
  const auto parsed = commands::parse(q);
  // an all-capitals first word that starts a command code shows the command list
  const std::string first = q.substr(0, q.find(' '));
  const bool caps = !first.empty() && std::all_of(first.begin(), first.end(), [](unsigned char ch) { return std::isupper(ch); });
  const bool cmd_list = parsed.kind == commands::Parsed::Command || (!q.empty() && q[0] == ':') ||
                        (caps && q.find(' ') == std::string::npos && !commands::suggest(q, 1).empty());
  if (!q.empty() && cmd_list) {
    // command suggestions while a command is being typed
    ImGui::PushFont(font_mono_);
    ImGui::TextColored(col(kMuted), "COMMANDS");
    ImGui::PopFont();
    for (const auto* s : commands::suggest(q, 12)) {
      ImGui::PushID(s->code);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##c", false, 0, ImVec2(0, 26))) {
        std::snprintf(query_, sizeof query_, "%s ", s->code);
        focus_search_ = true;
      }
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddText(font_mono_, 11.5f, ImVec2(p.x + 2, p.y + 6), s->ready ? u32(kInk) : u32(kFaint), s->code);
      dl->AddText(font_small_, 12, ImVec2(p.x + 56, p.y + 5), s->ready ? u32(kMuted) : u32(kFaint), s->about);
      ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "A name, then a function also works: \"apollo DES\", \"tshwane NEAR\".");
    ImGui::PopTextWrapPos();
  } else if (query_[0] && (!hits_.empty() || !tasks_for(q).empty())) {
    // what you may want to do: the ranking, preset or tool that fits the words
    const auto tasks = tasks_for(q);
    if (!tasks.empty()) {
      ImGui::PushFont(font_mono_);
      ImGui::TextColored(col(kMuted), "DO");
      ImGui::PopFont();
      for (size_t i = 0; i < tasks.size(); ++i) {
        ImGui::PushID(int(i) + 500);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##t", false, 0, ImVec2(0, 26))) {
          const std::string cmd = tasks[i].command;
          query_[0] = 0;
          hits_.clear();
          run_command(cmd);
          ImGui::PopID();
          break;
        }
        auto* dl = ImGui::GetWindowDrawList();
        const std::string code = tasks[i].command.substr(0, tasks[i].command.find(' '));
        const ImVec2 ts = font_mono_->CalcTextSizeA(10, FLT_MAX, 0, code.c_str());
        dl->AddRect(ImVec2(p.x, p.y + 5), ImVec2(p.x + ts.x + 10, p.y + 19), u32(kAccent), 3);
        dl->AddText(font_mono_, 10, ImVec2(p.x + 5, p.y + 6), u32(kAccent), code.c_str());
        dl->AddText(ImVec2(p.x + ts.x + 18, p.y + 4), u32(kInk), tasks[i].label.c_str());
        ImGui::PopID();
      }
      ImGui::Spacing();
    }
    if (!hits_.empty()) {
      ImGui::PushFont(font_mono_);
      ImGui::TextColored(col(kMuted), "PLACES");
      ImGui::PopFont();
    }
    for (size_t i = 0; i < hits_.size(); ++i) {
      const auto& hit = hits_[i];
      ImGui::PushID(int(i));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##h", int(i) == hit_sel_, 0, ImVec2(0, 38))) {
        run_command(hit.label + " GO");
        query_[0] = 0;
        hits_.clear();
        ImGui::PopID();
        break;
      }
      auto* dl = ImGui::GetWindowDrawList();
      const char* tc = hit.kind == "SUB" || hit.kind == "PLANT" ? "#ffb020" : hit.kind == "PROV" ? "#9fb3c8" : "#2e9e4f";
      const ImVec2 ts = font_mono_->CalcTextSizeA(10, FLT_MAX, 0, hit.kind.c_str());
      dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + ts.x + 10, p.y + 18), u32(tc), 3);
      dl->AddText(font_mono_, 10, ImVec2(p.x + 5, p.y + 5), u32("#0b0f13"), hit.kind.c_str());
      dl->AddText(ImVec2(p.x + ts.x + 18, p.y + 2), u32(kInk), hit.label.c_str());
      dl->AddText(font_small_, 12, ImVec2(p.x + ts.x + 18, p.y + 20), u32(kMuted), hit.detail.c_str());
      ImGui::PopID();
    }
  } else if (panel_ != Panel::None) {
    switch (panel_) {
      case Panel::Find: ui_panel_find(); break;
      case Panel::List: ui_panel_list(false); break;
      case Panel::Areas: ui_panel_areas(); break;
      case Panel::Views: ui_panel_views(); break;
      case Panel::Watch: ui_panel_watch(); break;
      case Panel::Keys: ui_panel_keys(); break;
      case Panel::History: ui_panel_history(); break;
      case Panel::Measure: {
        if (close_button()) {
          measuring_ = false;
          panel_ = Panel::None;
          break;
        }
        card_head("MEAS", kAccent, "M", "Measure", "", "");
        using measure::Mode;
        ImGui::PushFont(font_mono_);
        const float mw = (ImGui::GetContentRegionAvail().x - 12) / 4;
        const std::pair<Mode, const char*> modes[] = {{Mode::Path, "Path"}, {Mode::Area, "Area"}, {Mode::Circle, "Circle"}};
        for (const auto& [mo, label] : modes) {
          if (chip(label, measure_mode_ == mo, ImVec2(mw, 26))) {
            measure_mode_ = mo;
            measure_reset();
          }
          ImGui::SameLine(0, 4);
        }
        chip("Along roads", false, ImVec2(mw, 26), false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Routes along roads come with Reach (milestone F)");
        ImGui::PopFont();
        ImGui::Spacing();
        const auto ll = measure_ll();
        ImGui::PushFont(font_title_);
        if (measure_mode_ == Mode::Path && ll.size() >= 2) ImGui::TextUnformatted(measure::fmt_dist(measure::length(ll)).c_str());
        if (measure_mode_ == Mode::Area && ll.size() >= 3) ImGui::TextUnformatted(measure::fmt_area(measure::area_m2(ll)).c_str());
        if (measure_mode_ == Mode::Circle && ll.size() == 2)
          ImGui::TextUnformatted((measure::fmt_dist(measure::metres(ll[0], ll[1])) + " radius").c_str());
        ImGui::PopFont();
        if (measure_mode_ == Mode::Path)
          for (size_t i = 1; i < ll.size(); ++i) {
            char b[16];
            std::snprintf(b, sizeof b, "%d\xc2\xb0", int(std::lround(measure::bearing(ll[i - 1], ll[i]))));
            kv_row((std::to_string(i) + ". " + measure::fmt_dist(measure::metres(ll[i - 1], ll[i]))).c_str(), b);
          }
        if (measure_mode_ == Mode::Area && ll.size() >= 3) {
          auto ring = ll;
          ring.push_back(ll[0]);
          ImGui::TextColored(col(kMuted), "Perimeter %s%s", measure::fmt_dist(measure::length(ring)).c_str(),
                             measure_done_ ? "" : " \xc2\xb7 click the first point to close");
        }
        if (measure_mode_ == Mode::Circle && ll.size() == 2) {
          const double r = measure::metres(ll[0], ll[1]);
          ImGui::TextColored(col(kMuted), "Area %s \xc2\xb7 circumference %s", measure::fmt_area(3.14159265358979 * r * r).c_str(),
                             measure::fmt_dist(2 * 3.14159265358979 * r).c_str());
        }
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(col(kFaint), "%s", measure_done_ ? "Click the map to start again." : measure::hint(measure_mode_, ll.size()).c_str());
        ImGui::TextColored(col(kFaint), "Backspace removes the last point, Esc ends.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::PushFont(font_mono_);
        if (!ll.empty() && chip("Clear", false, ImVec2(70, 26))) measure_reset();
        if (!measure_done_ && ll.size() >= 2 && measure_mode_ != Mode::Circle) {
          ImGui::SameLine(0, 4);
          if (chip("Finish", false, ImVec2(70, 26))) measure_done_ = true;
        }
        if (measure_done_) {
          ImGui::SameLine(0, 4);
          if (chip("Copy", false, ImVec2(70, 26))) {
            glfwSetClipboardString(win_, measure::summary(measure_mode_, ll).c_str());
            toast("Copied " + measure::summary(measure_mode_, ll));
          }
          ImGui::SameLine(0, 4);
          if (chip("Copy GeoJSON", false, ImVec2(120, 26))) {
            glfwSetClipboardString(win_, measure::geojson(measure_mode_, ll).c_str());
            toast("GeoJSON copied");
          }
        }
        ImGui::PopFont();
        break;
      }
      case Panel::Filter: ui_panel_filter(); break;
      case Panel::Layers: ui_panel_later("Layers", "Layer settings.", "C"); break;
      case Panel::Live: ui_panel_live(); break;
      case Panel::Reach: ui_panel_later("Reach", "How far you get by car, transit, walking or cycling.", "F"); break;
      case Panel::Analyse: ui_panel_later("Analyse", "Totals for any area you draw: people, plots, grid, water.", "F"); break;
      case Panel::None: break;
    }
  } else {
    if (!kept_.empty()) ui_kept();
    Card& c = cards_.back();
    switch (c.kind) {
      case Card::Home: ui_home(); break;
      case Card::Area: ui_area(c.id); break;
      case Card::Facility: ui_facility(c); break;
      case Card::Spot: ui_spot(c); break;
      case Card::Near: ui_near(c); break;
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void App::ui_dock(float x, float y, float w, float h) {
  auto* dl = ImGui::GetWindowDrawList();
  const float pad = 8;
  // the header folds and unfolds the dock
  ImGui::SetCursorScreenPos(ImVec2(x + 4, y + 4));
  if (ImGui::InvisibleButton("##dockfold", ImVec2(150, 28))) {
    if (dock_folded_now_) {
      dock_fold_ = false;
      dock_force_open_ = true;
    } else {
      dock_fold_ = true;
    }
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(dock_folded_now_ ? "Show the layers on the map" : "Fold the layer list");
  {
    const float cx = x + 16, cy = y + 17;
    if (dock_folded_now_)
      dl->AddTriangleFilled(ImVec2(cx - 3, cy - 4), ImVec2(cx - 3, cy + 4), ImVec2(cx + 3, cy), u32(kMuted));
    else
      dl->AddTriangleFilled(ImVec2(cx - 4, cy - 2.5f), ImVec2(cx + 4, cy - 2.5f), ImVec2(cx, cy + 3.5f), u32(kMuted));
  }
  ImGui::SetCursorScreenPos(ImVec2(x + 26, y + 10));
  ImGui::PushFont(font_mono_);
  if (dock_folded_now_) ImGui::TextColored(col(kMuted), "ON THE MAP \xc2\xb7 %d", int(lstate_.on.size()));
  else ImGui::TextColored(col(kMuted), "ON THE MAP");
  ImGui::PopFont();
  // Clear all, + Add
  ImGui::SetCursorScreenPos(ImVec2(x + w - 162, y + 6));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 3));
  ImGui::PushFont(font_mono_);
  if (chip("Clear all", false, ImVec2(80, 22))) {
    lstate_.clear();
    apply_layers();
  }
  ImGui::PopFont();
  ImGui::SameLine(0, 10);
  ImGui::PushStyleColor(ImGuiCol_Text, col(kAccent));
  if (chip("+ Add", true, ImVec2(60, 22))) catalogue_open_ = !catalogue_open_;
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (dock_folded_now_) return;

  // group tabs, five to a row
  const float tw = (w - 2 * pad - 4 * 4) / 5, th = 24;
  std::vector<std::pair<std::string, std::string>> tabs = {{"on", "On map"}};
  for (const auto& g : layers::groups()) tabs.push_back({g.id, g.name});
  ImGui::PushFont(font_mono_);
  for (size_t i = 0; i < tabs.size(); ++i) {
    const float tx = x + pad + float(i % 5) * (tw + 4), ty = y + 36 + float(i / 5) * (th + 4);
    ImGui::SetCursorScreenPos(ImVec2(tx, ty));
    const int n = tabs[i].first == "on" ? int(lstate_.on.size()) : lstate_.count_in(tabs[i].first);
    std::string label = tabs[i].second;
    if (n) label += " " + std::to_string(n);
    label += "##t" + tabs[i].first;
    if (chip(label.c_str(), group_tab_ == tabs[i].first, ImVec2(tw, th))) group_tab_ = tabs[i].first;
  }
  ImGui::PopFont();

  // layer rows
  const float rows_y = y + 36 + 2 * (th + 4) + 4, base_h = 34;
  ImGui::SetCursorScreenPos(ImVec2(x + pad, rows_y));
  ImGui::BeginChild("##rows", ImVec2(w - 2 * pad, h - (rows_y - y) - base_h - 2));
  std::vector<const layers::Def*> rows;
  if (group_tab_ == "on") {
    for (const auto& id : lstate_.on)
      if (const auto* d = layers::find(id)) rows.push_back(d);
  } else {
    for (const auto& d : layers::catalogue())
      if (group_tab_ == d.group) rows.push_back(&d);
  }
  if (rows.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kMuted), "Nothing on the map. Pick a topic on the left, or + Add to put any layer on.");
    ImGui::PopTextWrapPos();
  }
  const float rw = ImGui::GetContentRegionAvail().x;
  for (const auto* d : rows) {
    ImGui::PushID(d->id);
    const bool on = std::find(lstate_.on.begin(), lstate_.on.end(), d->id) != lstate_.on.end();
    const bool hidden = lstate_.hidden.count(d->id) != 0;
    const bool have = avail_.count(d->id) != 0;
    const bool focus = lstate_.focus == d->id;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float bw = 30, rh = 34;
    auto* cdl = ImGui::GetWindowDrawList();
    // the row itself: click makes it the colour layer (or adds it in a group tab)
    if (ImGui::InvisibleButton("##row", ImVec2(rw - 2 * (bw + 4), rh))) {
      if (!on) lstate_.add(d->id);
      else lstate_.focus = d->id;
      apply_layers();
    }
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip("%s%s", d->about, have ? "" : "\nNeeds its data pack (milestone B)");
    cdl->AddRect(p, ImVec2(p.x + rw - 2 * (bw + 4), p.y + rh), focus ? u32(kAccent) : u32(kLine), 3);
    if (hov) cdl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + rw - 2 * (bw + 4) - 1, p.y + rh - 1), u32(kHover), 3);
    // swatch in a small tile, like the website's icon squares
    cdl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 26, p.y + 26), u32(kGround), 4);
    icons::swatch(cdl, d->swatch, ImVec2(p.x + 17, p.y + 17), 15, u32(d->colour, have ? 1.0f : 0.4f));
    const ImU32 tc = !have ? u32(kFaint) : hidden ? u32(kMuted) : u32(kInk);
    cdl->AddText(font_ui_, 13.5f, ImVec2(p.x + 36, p.y + (have ? 8.0f : 3.0f)), tc, d->name);
    if (!have) cdl->AddText(font_small_, 11, ImVec2(p.x + 36, p.y + 19), u32(kFaint), "needs its data pack");
    // eye and remove (or add) buttons
    ImGui::SameLine(0, 4);
    const ImVec2 e0 = ImGui::GetCursorScreenPos();
    if (on) {
      if (ImGui::InvisibleButton("##eye", ImVec2(bw, rh))) {
        lstate_.toggle_hidden(d->id);
        apply_layers();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(hidden ? "Show" : "Hide for now");
      cdl->AddRect(e0, ImVec2(e0.x + bw, e0.y + rh), u32(kLine), 3);
      icons::draw(cdl, hidden ? icons::Id::EyeOff : icons::Id::Eye, ImVec2(e0.x + bw / 2, e0.y + rh / 2), 15, u32(kMuted), 1.3f);
      ImGui::SameLine(0, 4);
      const ImVec2 x0 = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##rm", ImVec2(bw, rh))) {
        lstate_.remove(d->id);
        apply_layers();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take off the map");
      cdl->AddRect(x0, ImVec2(x0.x + bw, x0.y + rh), u32(kLine), 3);
      icons::draw(cdl, icons::Id::Close, ImVec2(x0.x + bw / 2, x0.y + rh / 2), 14, u32(kMuted), 1.3f);
    } else {
      ImGui::Dummy(ImVec2(bw, rh));
      ImGui::SameLine(0, 4);
      const ImVec2 x0 = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##add", ImVec2(bw, rh))) {
        lstate_.add(d->id);
        apply_layers();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put on the map");
      cdl->AddRect(x0, ImVec2(x0.x + bw, x0.y + rh), u32(kLine), 3);
      icons::draw(cdl, icons::Id::Plus, ImVec2(x0.x + bw / 2, x0.y + rh / 2), 14, u32(kAccent), 1.3f);
    }
    // the colour layer shows its legend under the row
    // (the topic card above already shows the key, so the dock leaves it out then)
    const bool topic_key = panel_ == Panel::None && !cards_.empty() && cards_.back().kind == Card::Home && lstate_.topic != "start";
    if (focus && have && !hidden && !topic_key) {
      const auto ms = layers::map_style(lstate_);
      style::Legend lg;
      if (layers::is_choropleth(d->id)) lg = style::legend(ms.fill);
      else if (std::string(d->id) == "lines" || std::string(d->id) == "subs") lg = style::legend(style::Topic::Energy);
      if (!lg.items.empty()) {
        ImGui::Indent(8);
        float lx = 0;
        for (const auto& it : lg.items) {
          const float iw = font_small_->CalcTextSizeA(11.5f, FLT_MAX, 0, it.label.c_str()).x + 22;
          if (lx > 0 && lx + iw < rw - 8) ImGui::SameLine(0, 6);
          else lx = 0;
          lx += iw + 6;
          const ImVec2 q = ImGui::GetCursorScreenPos();
          cdl->AddRectFilled(ImVec2(q.x, q.y + 4), ImVec2(q.x + 10, q.y + 14), u32(it.colour), 2);
          cdl->AddText(font_small_, 11.5f, ImVec2(q.x + 14, q.y + 2), u32(kMuted), it.label.c_str());
          ImGui::Dummy(ImVec2(iw, 17));
        }
        if (!lg.note.empty()) {
          ImGui::PushFont(font_small_);
          ImGui::PushTextWrapPos(rw - 8);
          ImGui::TextColored(col(kFaint), "%s", lg.note.c_str());
          ImGui::PopTextWrapPos();
          ImGui::PopFont();
        }
        ImGui::Unindent(8);
      }
    }
    ImGui::PopID();
  }
  dock_rows_used_ = group_tab_ == "on" ? ImGui::GetCursorPosY() : 1e4f;  // the dock shrinks to what it lists
  ImGui::EndChild();

  // base map toggles
  const float by = y + h - base_h + 4;
  dl->AddText(font_mono_, 10.5f, ImVec2(x + 12, by + 6), u32(kMuted), "BASE");
  ImGui::SetCursorScreenPos(ImVec2(x + 52, by));
  struct Base {
    const char* name;
    bool* v;
    bool ready;
  };
  Base base[] = {{"Roads", &lstate_.roads, avail_.count("streets") != 0},
                 {"Terrain", &lstate_.terrain, avail_.count("relief") != 0},
                 {"Names", &lstate_.names, true},
                 {"Buildings", &lstate_.buildings, avail_.count("buildings") != 0},
                 {"Landmarks", &lstate_.landmarks, avail_.count("pois") != 0}};
  for (auto& b : base) {
    const float bw = ImGui::CalcTextSize(b.name).x + 14;
    if (chip(b.name, *b.v && b.ready, ImVec2(bw, 24), true)) {
      if (b.ready) {
        *b.v = !*b.v;
        apply_layers();
        save_settings();
      } else if (b.v == &lstate_.terrain) toast("Terrain needs the relief image pack (raster-relief)");
      else if (b.v == &lstate_.buildings) toast("Buildings need the buildings pack (buildings)");
      else toast(std::string(b.name) + " need the base map pack (base)");
    }
    ImGui::SameLine(0, 5);
  }
  ImGui::NewLine();
}

// ------------------------------------------------------------------ catalogue

void App::ui_catalogue() {
  const float x = side_width() + 10, h = std::min(620.0f, float(cam_.h) - kStatusH - 40);
  ImGui::SetNextWindowPos(ImVec2(x, 20), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(420, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8);
  ImGui::Begin("##catalogue", &catalogue_open_, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PushFont(font_mono_);
  ImGui::TextColored(col(kMuted), "ADD A LAYER");
  ImGui::PopFont();
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 10);
  if (ImGui::SmallButton("x")) catalogue_open_ = false;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::InputTextWithHint("##cq", "Search 76 layers: solar, dams, crime ...", cat_query_, sizeof cat_query_);
  std::string q = cat_query_;
  std::transform(q.begin(), q.end(), q.begin(), ::tolower);
  ImGui::BeginChild("##cl");
  for (const auto& g : layers::groups()) {
    bool header = false;
    for (const auto& d : layers::catalogue()) {
      if (g.id != std::string(d.group)) continue;
      std::string n = d.name, a = d.about;
      std::transform(n.begin(), n.end(), n.begin(), ::tolower);
      std::transform(a.begin(), a.end(), a.begin(), ::tolower);
      if (!q.empty() && n.find(q) == std::string::npos && a.find(q) == std::string::npos) continue;
      if (!header) {
        ImGui::Spacing();
        ImGui::PushFont(font_mono_);
        ImGui::TextColored(col(g.colour), "%s", g.name);
        ImGui::PopFont();
        header = true;
      }
      ImGui::PushID(d.id);
      const bool on = std::find(lstate_.on.begin(), lstate_.on.end(), d.id) != lstate_.on.end();
      const bool have = avail_.count(d.id) != 0;
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##l", on, 0, ImVec2(0, 36))) {
        if (on) lstate_.remove(d.id);
        else lstate_.add(d.id);
        apply_layers();
      }
      auto* cdl = ImGui::GetWindowDrawList();
      icons::swatch(cdl, d.swatch, ImVec2(p.x + 12, p.y + 18), 15, u32(d.colour, have ? 1.0f : 0.4f));
      cdl->AddText(ImVec2(p.x + 28, p.y + 2), have ? u32(kInk) : u32(kMuted), d.name);
      cdl->AddText(font_small_, 11.5f, ImVec2(p.x + 28, p.y + 19), u32(kFaint), have ? d.about : "Needs its data pack (milestone B)");
      if (on) cdl->AddText(font_mono_, 10, ImVec2(p.x + 360, p.y + 4), u32(kAccent), "ON");
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) catalogue_open_ = false;
  block_window();
  ImGui::End();
  ImGui::PopStyleVar(2);
}

// ------------------------------------------------------------------ map chrome

void App::ui_crumb() {
  // where the centre of the map is: South Africa > province > municipality
  const double cx = side_width() + (cam_.w - side_width()) / 2, cy = (cam_.h - kStatusH) / 2;
  double wx, wy;
  cam_.to_world(cx, cy, wx, wy);
  std::string prov;
  const std::string muni = cam_.zoom >= 6 ? muni_name_at(wx, wy, &prov) : std::string();
  std::vector<std::string> parts = {"South Africa"};
  if (!prov.empty()) parts.push_back(prov);
  if (!muni.empty()) parts.push_back(muni);
  std::string text;
  for (size_t i = 0; i < parts.size(); ++i) text += (i ? "  \xe2\x80\xba  " : "") + parts[i];
  const ImVec2 ts = font_mono_->CalcTextSizeA(12, FLT_MAX, 0, text.c_str());
  const float w = ts.x + 30, x = float(cx) - w / 2;
  ImGui::SetNextWindowPos(ImVec2(x, 12));
  ImGui::SetNextWindowSize(ImVec2(w, 28));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::Begin("##crumb", nullptr, kFixed | ImGuiWindowFlags_NoInputs);
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(x, 12), ImVec2(x + w, 40), u32(kPanel, 0.94f), 14);
  dl->AddRect(ImVec2(x, 12), ImVec2(x + w, 40), u32(kLine), 14);
  float tx = x + 15;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      dl->AddText(font_mono_, 12, ImVec2(tx, 19), u32(kFaint), "  \xe2\x80\xba  ");
      tx += font_mono_->CalcTextSizeA(12, FLT_MAX, 0, "  \xe2\x80\xba  ").x;
    }
    dl->AddText(font_mono_, 12, ImVec2(tx, 19), i + 1 == parts.size() ? u32(kInk) : u32(kMuted), parts[i].c_str());
    tx += font_mono_->CalcTextSizeA(12, FLT_MAX, 0, parts[i].c_str()).x;
  }
  block_window();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void App::ui_controls() {
  const float s = 36, x = float(cam_.w) - s - 12;
  float y = float(cam_.h) - kStatusH - 14 - s;
  ImGui::SetNextWindowPos(ImVec2(x, y - 3 * s - 14));
  ImGui::SetNextWindowSize(ImVec2(s, 4 * s + 14));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::Begin("##ctrls", nullptr, kFixed);
  auto* dl = ImGui::GetWindowDrawList();
  auto button = [&](const char* id, float by, icons::Id icon, const char* text, const char* tip, bool enabled) {
    ImGui::SetCursorScreenPos(ImVec2(x, by));
    const bool click = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(x, by), ImVec2(x + s, by + s), u32(hov ? kHover : kPanel, 0.96f), 4);
    dl->AddRect(ImVec2(x, by), ImVec2(x + s, by + s), u32(kLine), 4);
    const ImU32 c = !enabled ? u32(kFaint) : hov ? u32(kInk) : u32(kInk, 0.85f);
    if (text) {
      const ImVec2 ts = font_head_->CalcTextSizeA(13, FLT_MAX, 0, text);
      dl->AddText(font_head_, 13, ImVec2(x + s / 2 - ts.x / 2, by + s / 2 - ts.y / 2), c, text);
    } else {
      icons::draw(dl, icon, ImVec2(x + s / 2, by + s / 2), 17, c, 1.5f);
    }
    if (hov) ImGui::SetTooltip("%s", tip);
    return click && enabled;
  };
  const float y0 = y - 3 * s - 14;
  if (button("##3d", y0, icons::Id::Cube, cam_.pitch > 1 ? "2D" : "3D", "Tilt the map (3D); Ctrl+drag to tilt by hand", true))
    pitch_target_ = cam_.pitch > 1 ? 0 : 50;
  if (button("##zi", y0 + s + 14, icons::Id::Plus, nullptr, "Zoom in (+)", true)) fly_ = Fly{glfwGetTime(), 0.25, cam_, cam_.cx, cam_.cy, std::min(16.0, cam_.zoom + 1)};
  if (button("##zo", y0 + 2 * s + 14, icons::Id::Minus, nullptr, "Zoom out (-)", true)) fly_ = Fly{glfwGetTime(), 0.25, cam_, cam_.cx, cam_.cy, std::max(3.0, cam_.zoom - 1)};
  if (button("##home", y0 + 3 * s + 14, icons::Id::Home, nullptr, "Whole country (Home)", true)) fly_to(25.2, -28.8, 5.4);
  block_window();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void App::ui_status() {
  const float y = float(cam_.h) - kStatusH, w = float(cam_.w);
  ImGui::SetNextWindowPos(ImVec2(0, y));
  ImGui::SetNextWindowSize(ImVec2(w, kStatusH));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(kPanel));
  ImGui::Begin("##status", nullptr, kFixed | ImGuiWindowFlags_NoInputs);
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(0, y + 0.5f), ImVec2(w, y + 0.5f), u32(kLine));
  const float ty = y + 10, fs = 11.5f;
  float x = 14;
  auto text = [&](const std::string& s, ImU32 c, float gap = 18) {
    dl->AddText(font_mono_, fs, ImVec2(x, ty), c, s.c_str());
    x += font_mono_->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str()).x + gap;
  };
  const size_t loading = loader_->in_flight() + last_need_.size();
  dl->AddCircleFilled(ImVec2(x + 3, ty + 7), 3.5f, loading ? u32(kWarn) : u32(kOk));
  x += 12;
  text(!backend_error_.empty() ? "BACK-END DOWN" : loading ? "LOADING " + std::to_string(loading) : "ALL LAYERS",
       backend_error_.empty() ? u32(kInk) : u32(kWarn));
  char zb[16];
  std::snprintf(zb, sizeof zb, "z%.1f", cam_.zoom);
  text(zb, u32(kMuted));
  // scale bar: a round distance about 70 to 140 px long
  {
    double wx, wy;
    cam_.to_world(cam_.w / 2, cam_.h / 2, wx, wy);
    const double km_per_px = 40075.0 * std::cos(geo::lat_of(wy) * geo::kPi / 180) / cam_.scale();
    static const double nice[] = {0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
    double km = nice[0];
    for (double n : nice)
      if (n / km_per_px <= 140) km = n;
    const float px = float(km / km_per_px);
    dl->AddLine(ImVec2(x, ty + 11), ImVec2(x + px, ty + 11), u32(kMuted));
    dl->AddLine(ImVec2(x, ty + 6), ImVec2(x, ty + 11), u32(kMuted));
    dl->AddLine(ImVec2(x + px, ty + 6), ImVec2(x + px, ty + 11), u32(kMuted));
    x += px + 8;
    text(km < 1 ? std::to_string(int(km * 1000)) + " m" : std::to_string(int(km)) + " km", u32(kMuted), 22);
  }
  // what is in view
  {
    int subs = 0, plants = 0;
    for (const auto& r : map_->drawn_facilities()) (r.feature().str("kind") == "power_station" ? plants : subs)++;
    if (subs || plants) {
      std::string s = "IN VIEW ";
      if (subs) s += std::to_string(subs) + " substations";
      if (subs && plants) s += " \xc2\xb7 ";
      if (plants) s += std::to_string(plants) + " stations";
      text(s, u32(kMuted), 22);
    }
  }
  if (measuring_) text("MEASURE \xc2\xb7 click to add points, Esc ends", u32(kWarn), 22);
  // right side: hints, clock, sources
  char clock[32];
  const std::time_t t = std::time(nullptr);
  std::strftime(clock, sizeof clock, "%H:%M", std::localtime(&t));
  std::string packs;
  if (version_.contains("packs"))
    for (const auto& [k, v] : version_["packs"].items()) packs += (packs.empty() ? "" : " \xc2\xb7 ") + k + " " + v.get<std::string>();
  const std::string right = "right-click: what is here \xc2\xb7 N: find \xc2\xb7 ? keys     " + std::string(clock) + "     " +
                            (packs.empty() ? "no packs" : packs) + " \xc2\xb7 " + vk_->device_name();
  const float rw = font_mono_->CalcTextSizeA(fs, FLT_MAX, 0, right.c_str()).x;
  if (x + 20 < w - rw - 14) dl->AddText(font_mono_, fs, ImVec2(w - rw - 14, ty), u32(kMuted), right.c_str());
  block_window();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void App::ui_measure() {
  if (!measuring_ && measure_.empty()) return;
  if (!measuring_) return;
  using measure::Mode;
  auto* dl = ImGui::GetBackgroundDrawList();
  const ImU32 ac = IM_COL32(125, 249, 255, 255), soft = IM_COL32(125, 249, 255, 180);
  std::vector<ImVec2> pts;
  for (const auto& [wx, wy] : measure_) {
    double sx, sy;
    cam_.to_screen(wx, wy, sx, sy);
    pts.push_back(ImVec2(float(sx), float(sy)));
  }
  const auto ll = measure_ll();
  // where the next click would go, while the mouse is over the map
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool over_map = !ImGui::GetIO().WantCaptureMouse && mouse.x > side_width() && !measure_done_;
  auto label = [&](ImVec2 p, const std::string& t, float size) {
    const ImVec2 ts = font_mono_->CalcTextSizeA(size, FLT_MAX, 0, t.c_str());
    const ImVec2 a(p.x - ts.x / 2 - 5, p.y - ts.y / 2 - 3), b(p.x + ts.x / 2 + 5, p.y + ts.y / 2 + 3);
    dl->AddRectFilled(a, b, u32(kGround, 0.85f), 4);
    dl->AddText(font_mono_, size, ImVec2(a.x + 5, a.y + 3), ac, t.c_str());
  };
  auto dashed = [&](ImVec2 a, ImVec2 b, ImU32 c) {
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    for (float d = 0; d < len; d += 10) {
      const float e = std::min(len, d + 6);
      dl->AddLine(ImVec2(a.x + (b.x - a.x) * d / len, a.y + (b.y - a.y) * d / len),
                  ImVec2(a.x + (b.x - a.x) * e / len, a.y + (b.y - a.y) * e / len), c, 2);
    }
  };
  if (measure_mode_ == Mode::Circle) {
    if (!pts.empty()) {
      const ImVec2 c = pts[0], e = pts.size() > 1 ? pts[1] : over_map ? mouse : c;
      const float r = std::hypot(e.x - c.x, e.y - c.y);
      if (r > 1) {
        dl->AddCircleFilled(c, r, IM_COL32(125, 249, 255, 36), 96);
        dl->AddCircle(c, r, ac, 96, 2);
        dashed(c, e, ac);
        double wx, wy;
        cam_.to_world(e.x, e.y, wx, wy);
        label(ImVec2((c.x + e.x) / 2, (c.y + e.y) / 2 - 12), measure::fmt_dist(measure::metres(ll[0], {geo::lon_of(wx), geo::lat_of(wy)})), 12);
      }
    }
  } else {
    if (measure_mode_ == Mode::Area && pts.size() >= 2) {
      auto ring = pts;
      if (over_map) ring.push_back(mouse);
      if (ring.size() >= 3) dl->AddConcavePolyFilled(ring.data(), int(ring.size()), IM_COL32(125, 249, 255, 40));
    }
    for (size_t i = 1; i < pts.size(); ++i) dl->AddLine(pts[i - 1], pts[i], ac, 2.5f);
    if (measure_mode_ == Mode::Area && measure_done_ && pts.size() >= 3) dl->AddLine(pts.back(), pts[0], ac, 2.5f);
    if (over_map && !pts.empty()) {
      dashed(pts.back(), mouse, soft);
      if (measure_mode_ == Mode::Area && pts.size() >= 2) dashed(mouse, pts[0], soft);
    }
    if (measure_mode_ == Mode::Path) {
      for (size_t i = 1; i < pts.size(); ++i)
        label(ImVec2((pts[i - 1].x + pts[i].x) / 2, (pts[i - 1].y + pts[i].y) / 2), measure::fmt_dist(measure::metres(ll[i - 1], ll[i])), 11.5f);
      if (over_map && !pts.empty()) {
        double wx, wy;
        cam_.to_world(mouse.x, mouse.y, wx, wy);
        auto with = ll;
        with.push_back({geo::lon_of(wx), geo::lat_of(wy)});
        label(ImVec2(mouse.x, mouse.y - 18), measure::fmt_dist(measure::length(with)), 12.5f);
      }
    }
  }
  for (const auto& p : pts) {
    dl->AddCircleFilled(p, 4.5f, u32(kGround));
    dl->AddCircle(p, 4.5f, ac, 0, 2);
  }
  // the result pill under the crumb, as on the website
  if (!ll.empty() || measuring_) {
    const std::string t = measure_done_ ? measure::summary(measure_mode_, ll) : measure::hint(measure_mode_, ll.size());
    const ImVec2 ts = (measure_done_ ? font_head_ : font_small_)->CalcTextSizeA(measure_done_ ? 15.0f : 13.0f, FLT_MAX, 0, t.c_str());
    const float cx = side_width() + (float(cam_.w) - side_width()) / 2;
    const ImVec2 a(cx - ts.x / 2 - 14, 58), b(cx + ts.x / 2 + 14, 58 + ts.y + 14);
    dl->AddRectFilled(a, b, u32(kPanel, 0.95f), 8);
    dl->AddRect(a, b, u32(kLine), 8);
    dl->AddText(measure_done_ ? font_head_ : font_small_, measure_done_ ? 15.0f : 13.0f, ImVec2(a.x + 14, a.y + 7),
                measure_done_ ? ac : u32(kInk), t.c_str());
  }
}

void App::ui_toast() {
  if (toast_.empty() || glfwGetTime() >= toast_until_) return;
  const ImVec2 ts = ImGui::CalcTextSize(toast_.c_str());
  const float cx = side_width() + (float(cam_.w) - side_width()) / 2;
  ImGui::SetNextWindowPos(ImVec2(cx - ts.x / 2 - 14, float(cam_.h) - kStatusH - 60));
  ImGui::SetNextWindowSize(ImVec2(ts.x + 28, 34));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 8));
  ImGui::Begin("##toast", nullptr, kFixed | ImGuiWindowFlags_NoInputs);
  ImGui::TextUnformatted(toast_.c_str());
  block_window();
  ImGui::End();
  ImGui::PopStyleVar(3);
}

}  // namespace gsa
