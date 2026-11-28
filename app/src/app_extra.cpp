// The website's v70+ functions (UI parts 4 to 31): KEEP a card beside the next one, Compare
// municipalities, undo and redo with a history list, one strip of every active filter, REPORT,
// NEAR's everyday answers, CHK, HELP CODE and a search that understands tasks.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <stb_image_write.h>

#include "app.hpp"
#include "commands.hpp"
#include "ui_theme.hpp"

namespace gsa {

using nlohmann::json;
using namespace ui;
namespace fs = std::filesystem;

namespace {

double now_secs() { return glfwGetTime(); }

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string html_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string base64(const std::vector<uint8_t>& in) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t a = in[i], b = i + 1 < in.size() ? in[i + 1] : 0, c = i + 2 < in.size() ? in[i + 2] : 0;
    const uint32_t v = (a << 16) | (b << 8) | c;
    o += T[(v >> 18) & 63];
    o += T[(v >> 12) & 63];
    o += i + 1 < in.size() ? T[(v >> 6) & 63] : '=';
    o += i + 2 < in.size() ? T[v & 63] : '=';
  }
  return o;
}

std::string slug(const std::string& s) {
  std::string o;
  for (unsigned char c : lower(s)) {
    if (std::isalnum(c)) o += char(c);
    else if (!o.empty() && o.back() != '-') o += '-';
  }
  while (!o.empty() && o.back() == '-') o.pop_back();
  return o.substr(0, 40);
}

std::string today() {
  char b[16];
  const std::time_t t = std::time(nullptr);
  std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
  return b;
}

std::optional<double> metric(const json& area, const std::string& name) {
  if (!area.contains("metrics") || !area["metrics"].is_array()) return std::nullopt;
  for (const auto& m : area["metrics"])
    if (m.value("metric", "") == name && m.contains("v") && m["v"].is_number()) return m["v"].get<double>();
  return std::nullopt;
}

const char* kind_label(const std::string& k) {
  if (k == "COUNTRY") return "Country";
  if (k == "PROV") return "Province";
  if (k == "WARD") return "Ward";
  return "Municipality";
}

std::string prop_text(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "yes" : "no";
  if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
  if (v.is_number()) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v.get<double>());
    return b;
  }
  return v.dump();
}

}  // namespace

// ------------------------------------------------------------------ KEEP

bool App::is_kept(const Card& c) const {
  return std::any_of(kept_.begin(), kept_.end(), [&](const Kept& k) {
    return k.card.kind == c.kind && k.card.id == c.id && k.card.lon == c.lon && k.card.lat == c.lat;
  });
}

void App::keep_current() {
  const Card& c = cards_.back();
  if (c.kind == Card::Home || panel_ != Panel::None) {
    toast("Open a card first, then KEEP it");
    return;
  }
  for (size_t i = 0; i < kept_.size(); ++i)
    if (kept_[i].card.kind == c.kind && kept_[i].card.id == c.id && kept_[i].card.lon == c.lon && kept_[i].card.lat == c.lat) {
      kept_.erase(kept_.begin() + long(i));
      toast("No longer kept");
      return;
    }
  Kept k{c, subject_name(), "", true};
  if (c.kind == Card::Area) {
    auto it = areas_.find(c.id);
    k.kind = it != areas_.end() ? kind_label(it->second.value("kind", "MUNI")) : "Area";
  } else if (c.kind == Card::Facility) {
    k.kind = c.props.value("tag", c.props.value("layer", std::string("Place")));
  } else if (c.kind == Card::Near) {
    k.kind = "Nearby";
  } else {
    k.kind = "Spot";
    k.title = muni_name_at(geo::mx(c.lon), geo::my(c.lat));
    if (k.title.empty()) k.title = "This spot";
  }
  if (!k.title.empty()) k.title[0] = char(std::toupper(static_cast<unsigned char>(k.title[0])));
  kept_.insert(kept_.begin(), k);
  kept_all_.push_back(k);
  if (kept_.size() > 2) kept_.pop_back();
  toast(kept_.size() == 2 ? "Kept. Two at most: the oldest goes next time"
                          : "Kept. Open another card and both stay in view");
}

void App::ui_kept() {
  int remove = -1, reopen = -1;
  for (size_t i = 0; i < kept_.size(); ++i) {
    auto& k = kept_[i];
    ImGui::PushID(int(i) + 900);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 40), u32(kHover), 5);
    if (ImGui::InvisibleButton("##fold", ImVec2(w - 92, 40))) k.open = !k.open;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(k.open ? "Fold" : "Unfold");
    const std::string kicker = lower(k.kind) + " \xc2\xb7 kept";
    dl->AddText(font_mono_, 10.5f, ImVec2(p.x + 10, p.y + 5), u32(kFaint), kicker.c_str());
    dl->AddText(font_ui_, 14, ImVec2(p.x + 10, p.y + 19), u32(kInk), k.title.c_str());
    dl->AddText(font_mono_, 11, ImVec2(p.x + w - 112, p.y + 13), u32(kFaint), k.open ? "\xe2\x80\x93" : "+");
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - 88, p.y + 8));
    ImGui::PushFont(font_mono_);
    if (chip("Open", false, ImVec2(52, 24))) reopen = int(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make it the live card again");
    ImGui::SameLine(0, 4);
    if (chip("x", false, ImVec2(26, 24))) remove = int(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop keeping it");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 44));
    if (k.open) {
      ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, 210));
      ImGui::BeginChild("##kbody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
      in_kept_ = true;
      switch (k.card.kind) {
        case Card::Area: ui_area(k.card.id); break;
        case Card::Facility: ui_facility(k.card); break;
        case Card::Spot: ui_spot(k.card); break;
        case Card::Near: ui_near(k.card); break;
        case Card::Home: break;
      }
      in_kept_ = false;
      ImGui::EndChild();
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (reopen >= 0) {
    const Card c = kept_[size_t(reopen)].card;
    kept_.erase(kept_.begin() + reopen);
    if (c.kind == Card::Area) open_area(c.id);
    else {
      cards_.push_back(c);
      panel_ = Panel::None;
    }
  } else if (remove >= 0) {
    kept_.erase(kept_.begin() + remove);
  }
  if (!kept_.empty()) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), u32(kAccent, 0.5f));
    ImGui::Dummy(ImVec2(0, 6));
  }
}

// ------------------------------------------------------------------ Compare

std::string App::current_muni_code() const {
  const Card& c = cards_.back();
  if (c.kind != Card::Area || panel_ != Panel::None) return {};
  auto it = areas_.find(c.id);
  const std::string k = it != areas_.end() ? it->second.value("kind", "") : "";
  if (k == "PROV" || k == "COUNTRY" || k == "WARD") return {};
  return c.id;
}

void App::cmp_add(const std::string& code) {
  if (code.empty()) return;
  if (std::find(cmp_.begin(), cmp_.end(), code) != cmp_.end()) {
    toast("Already in Compare");
    return;
  }
  cmp_.push_back(code);
  if (cmp_.size() > 4) cmp_.erase(cmp_.begin());
  toast("Added to Compare (" + std::to_string(cmp_.size()) + " of 4)");
}

void App::ui_compare() {
  if (!fdata_) {
    ImGui::TextColored(col(kFaint), "Loading the municipal figures ...");
    return;
  }
  std::vector<const find::Muni*> ms;
  for (const auto& c : cmp_)
    if (const auto* m = fdata_->muni(c)) ms.push_back(m);
  if (ms.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kMuted), "Compare up to 4 municipalities side by side. Open a municipality and press CMP, "
                                    "or press Compare top 4 under your Find results.");
    ImGui::PopTextWrapPos();
    return;
  }
  ImGui::PushFont(font_mono_);
  section(font_mono_, (std::to_string(ms.size()) + (ms.size() > 1 ? " MUNICIPALITIES" : " MUNICIPALITY")).c_str());
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
  if (chip("CSV", false, ImVec2(44, 20))) {
    ImGui::SetClipboardText(find::compare_csv(ms).c_str());
    toast("Compare table copied as CSV");
  }
  ImGui::SameLine(0, 4);
  bool clear = chip("Clear", false, ImVec2(52, 20));
  ImGui::PopFont();
  if (clear) {
    cmp_.clear();
    return;
  }
  const auto rows = find::compare(ms);
  const float lw = ms.size() >= 3 ? 150.0f : 128.0f;
  std::string remove;
  if (ImGui::BeginTable("##cmp", int(ms.size()) + 1,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, lw);
    for (size_t i = 0; i < ms.size(); ++i) ImGui::TableSetupColumn(("##c" + std::to_string(i)).c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    for (size_t i = 0; i < ms.size(); ++i) {
      ImGui::TableNextColumn();
      ImGui::PushID(int(i));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 14);
      ImGui::PushStyleColor(ImGuiCol_Text, col(kAccent));
      ImGui::TextUnformatted(ms[i]->name.c_str());
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
      if (ImGui::IsItemClicked()) open_area(ms[i]->code);
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Open %s", ms[i]->name.c_str());
      }
      ImGui::PushFont(font_small_);
      ImGui::TextColored(col(kFaint), "%s", ms[i]->prov.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) remove = ms[i]->code;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take out of Compare");
      ImGui::PopFont();
      ImGui::PopID();
    }
    for (const auto& r : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushFont(font_small_);
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(col(kMuted), "%s", r.label.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopFont();
      for (size_t i = 0; i < r.cells.size(); ++i) {
        ImGui::TableNextColumn();
        ImGui::PushFont(font_mono_);
        const bool hi = int(i) == r.hi, lo = int(i) == r.lo;
        ImGui::TextColored(col(hi ? kOk : lo ? kWarn : kInk), "%s", r.cells[i].c_str());
        ImGui::PopFont();
        if (hi || lo) {  // a small triangle: up for the highest, down for the lowest
          const ImVec2 q = ImGui::GetItemRectMax(), q0 = ImGui::GetItemRectMin();
          const float cx = q.x + 7, cy = (q0.y + q.y) / 2;
          auto* dl = ImGui::GetWindowDrawList();
          if (hi) dl->AddTriangleFilled(ImVec2(cx - 3.5f, cy + 2.5f), ImVec2(cx + 3.5f, cy + 2.5f), ImVec2(cx, cy - 3), u32(kOk));
          else dl->AddTriangleFilled(ImVec2(cx - 3.5f, cy - 2.5f), ImVec2(cx + 3.5f, cy - 2.5f), ImVec2(cx, cy + 3), u32(kWarn));
        }
      }
    }
    ImGui::EndTable();
  }
  if (!remove.empty()) cmp_.erase(std::remove(cmp_.begin(), cmp_.end(), remove), cmp_.end());
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::PushFont(font_small_);
  ImGui::TextColored(col(kFaint), "Green is the highest figure in a row, amber the lowest. Jobs, income, education and "
                                  "services are Census 2011, weighted by 2022 ward population; values are municipal "
                                  "roll medians.");
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
  if (ms.size() < 4) {
    ImGui::Spacing();
    ImGui::TextColored(col(kMuted), "Add more: open a municipality and press CMP.");
  }
}

// ------------------------------------------------------------------ undo, redo, history

App::Snap App::snap_now() const {
  Snap s;
  s.l = lstate_;
  s.f = filter_;
  s.f.prov_munis.clear();
  s.rules = rules_;
  s.cmp = cmp_;
  for (const auto& k : kept_) s.kept.push_back(k.title);
  return s;
}

void App::track_changes() {
  Snap cur = snap_now();
  if (!last_snap_) {
    last_snap_ = cur;
    return;
  }
  const Snap& a = *last_snap_;
  if (a.l == cur.l && a.f == cur.f && a.rules == cur.rules && a.cmp == cur.cmp && a.kept == cur.kept) return;
  // what changed, in words
  std::string label;
  if (a.l.topic != cur.l.topic) {
    const auto t = std::find_if(layers::topics().begin(), layers::topics().end(),
                                 [&](const layers::Topic& x) { return cur.l.topic == x.id; });
    label = "Topic: " + std::string(t != layers::topics().end() ? t->name : cur.l.topic.c_str());
  } else if (a.l.on != cur.l.on) {
    std::vector<std::string> added, removed;
    for (const auto& id : cur.l.on)
      if (std::find(a.l.on.begin(), a.l.on.end(), id) == a.l.on.end()) added.push_back(id);
    for (const auto& id : a.l.on)
      if (std::find(cur.l.on.begin(), cur.l.on.end(), id) == cur.l.on.end()) removed.push_back(id);
    auto name = [](const std::string& id) {
      const auto* d = layers::find(id);
      return d ? std::string(d->name) : id;
    };
    if (cur.l.on.empty()) label = "Cleared the map";
    else if (added.size() == 1 && removed.size() <= 1) label = "Added " + name(added[0]);
    else if (removed.size() == 1 && added.empty()) label = "Removed " + name(removed[0]);
    else if (added.empty() && removed.empty()) label = "Moved " + name(cur.l.on.front()) + " to the top";
    else label = "Changed " + std::to_string(added.size() + removed.size()) + " layers";
  } else if (a.l.hidden != cur.l.hidden) {
    auto name = [](const std::string& id) {
      const auto* d = layers::find(id);
      return d ? std::string(d->name) : id;
    };
    for (const auto& id : cur.l.hidden)
      if (!a.l.hidden.count(id)) label = "Hid " + name(id);
    for (const auto& id : a.l.hidden)
      if (!cur.l.hidden.count(id)) label = "Showed " + name(id);
  } else if (a.l.focus != cur.l.focus) {
    label = "Colour layer";
  } else if (!(a.l == cur.l)) {
    label = "Base map";
  } else if (!(a.f == cur.f)) {
    label = "Filters";
  } else if (a.rules != cur.rules) {
    label = cur.rules.size() > a.rules.size()   ? "Find: added a condition"
            : cur.rules.size() < a.rules.size() ? (cur.rules.empty() ? "Find: cleared" : "Find: removed a condition")
                                                : "Find: changed a condition";
  } else if (a.cmp != cur.cmp) {
    label = cur.cmp.size() > a.cmp.size() ? "Compare: added" : cur.cmp.empty() ? "Compare: cleared" : "Compare: changed";
  } else {
    label = "Kept cards";
  }
  const double t = now_secs();
  // typing a number into a condition makes one step, not one per key
  const bool merge = !undo_.empty() && undo_.back().label == label && t - undo_.back().t < 1.5 &&
                     label.rfind("Find: changed", 0) == 0;
  if (!merge) {
    Snap prev = a;
    prev.label = label;
    prev.t = t;
    undo_.push_back(std::move(prev));
    if (undo_.size() > 60) undo_.erase(undo_.begin());
  } else {
    undo_.back().t = t;
  }
  redo_.clear();
  last_snap_ = cur;
}

void App::restore(const Snap& s) {
  lstate_ = s.l;
  apply_layers();
  set_filter(s.f);
  set_rules(s.rules);
  cmp_ = s.cmp;
  std::vector<Kept> k;
  for (const auto& title : s.kept) {
    for (auto it = kept_all_.rbegin(); it != kept_all_.rend(); ++it)
      if (it->title == title) {
        k.push_back(*it);
        break;
      }
  }
  kept_ = std::move(k);
  last_snap_ = snap_now();
}

void App::undo() {
  if (undo_.empty()) {
    toast("Nothing to undo");
    return;
  }
  Snap cur = snap_now();
  cur.label = undo_.back().label;
  cur.t = now_secs();
  const std::string label = undo_.back().label;
  redo_.push_back(std::move(cur));
  restore(undo_.back());
  undo_.pop_back();
  toast("Undone: " + label + " \xc2\xb7 Ctrl Shift Z redoes");
}

void App::redo() {
  if (redo_.empty()) {
    toast("Nothing to redo");
    return;
  }
  Snap cur = snap_now();
  cur.label = redo_.back().label;
  cur.t = now_secs();
  const std::string label = redo_.back().label;
  undo_.push_back(std::move(cur));
  restore(redo_.back());
  redo_.pop_back();
  toast("Redone: " + label);
}

void App::ui_panel_history() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head("HIST", kAccent, "CTRL Z", "History", "Every change to layers, filters, Find conditions, Compare and kept "
                                                  "cards. Click a step to go back to just before it.", "");
  ImGui::PushFont(font_mono_);
  const float bw = (ImGui::GetContentRegionAvail().x - 4) / 2;
  if (chip("Undo  Ctrl Z", false, ImVec2(bw, 26), !undo_.empty())) undo();
  ImGui::SameLine(0, 4);
  if (chip("Redo  Ctrl Shift Z", false, ImVec2(bw, 26), !redo_.empty())) redo();
  ImGui::PopFont();
  const double t = now_secs();
  auto ago = [&](double at) {
    const int s = int(t - at);
    return s < 60 ? std::to_string(s) + " s ago" : std::to_string(s / 60) + " min ago";
  };
  section(font_mono_, "STEPS, NEWEST FIRST");
  if (undo_.empty()) ImGui::TextColored(col(kFaint), "No changes yet in this session.");
  int go_back = -1;
  for (int i = int(undo_.size()) - 1; i >= 0; --i) {
    ImGui::PushID(i);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##step", false, 0, ImVec2(0, 26))) go_back = i;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go back to before this step");
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddText(font_ui_, 13.5f, ImVec2(p.x + 4, p.y + 4), u32(kInk), undo_[size_t(i)].label.c_str());
    const std::string a = ago(undo_[size_t(i)].t);
    const ImVec2 as = font_mono_->CalcTextSizeA(11, FLT_MAX, 0, a.c_str());
    dl->AddText(font_mono_, 11, ImVec2(p.x + ImGui::GetContentRegionAvail().x - as.x - 4, p.y + 6), u32(kFaint), a.c_str());
    ImGui::PopID();
  }
  if (go_back >= 0)
    while (int(undo_.size()) > go_back) undo();
  if (!redo_.empty()) {
    section(font_mono_, "UNDONE (REDO BRINGS THEM BACK)");
    for (int i = int(redo_.size()) - 1; i >= 0; --i) ImGui::TextColored(col(kFaint), "%s", redo_[size_t(i)].label.c_str());
  }
}

// ------------------------------------------------------------------ filter strip

void App::ui_filter_strip() {
  struct Chip {
    std::string text;
    std::function<void()> off;
  };
  std::vector<Chip> chips;
  for (size_t i = 0; i < rules_.size(); ++i)
    chips.push_back({"Find: " + find::text(rules_[i]), [this, i] {
                       auto r = rules_;
                       r.erase(r.begin() + long(i));
                       set_rules(r);
                     }});
  auto drop = [this](auto member, auto value) {
    return [this, member, value] {
      auto f = filter_;
      (f.*member).erase(value);
      set_filter(f);
    };
  };
  for (int v : filter_.volt) chips.push_back({std::string("Lines ") + layers::kVoltBins[v], drop(&layers::Filters::volt, v)});
  for (const auto& v : filter_.fuel) {
    std::string l = v;
    for (const auto& [k, n] : layers::fuels())
      if (k == v) l = n;
    chips.push_back({"Stations: " + l, drop(&layers::Filters::fuel, v)});
  }
  for (const auto& v : filter_.party) chips.push_back({"Wards won by " + v, drop(&layers::Filters::party, v)});
  for (int v : filter_.turn) chips.push_back({std::string("Turnout ") + layers::kTurnBins[v], drop(&layers::Filters::turn, v)});
  for (const auto& v : filter_.prov) chips.push_back({"Province " + v, drop(&layers::Filters::prov, v)});
  for (const auto& id : lstate_.hidden) {
    if (std::find(lstate_.on.begin(), lstate_.on.end(), id) == lstate_.on.end()) continue;
    const auto* d = layers::find(id);
    chips.push_back({std::string("Hidden: ") + (d ? d->name : id.c_str()), [this, id] {
                       lstate_.hidden.erase(id);
                       apply_layers();
                     }});
  }
  if (chips.empty()) return;

  const float x0 = side_width() + 12, maxw = float(cam_.w) - x0 - 70;
  ImGui::SetNextWindowPos(ImVec2(x0, 48));
  ImGui::SetNextWindowSize(ImVec2(maxw, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::Begin("##fstrip", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::PushFont(font_mono_);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
  float x = 0;
  int off = -1;
  for (size_t i = 0; i <= chips.size(); ++i) {
    const bool all = i == chips.size();
    const std::string label = all ? std::string("Clear all") : chips[i].text + "  \xc3\x97";
    const float w = ImGui::CalcTextSize(label.c_str()).x + 20;
    if (x > 0 && x + w > maxw) x = 0;
    else if (x > 0) ImGui::SameLine(0, 6);
    x += w + 6;
    ImGui::PushID(int(i));
    ImGui::PushStyleColor(ImGuiCol_Button, col(kPanel, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(kHover));
    ImGui::PushStyleColor(ImGuiCol_Border, all ? col(kLine) : col(kAccent, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, all ? col(kMuted) : col(kInk));
    if (ImGui::Button(label.c_str(), ImVec2(w, 0))) off = int(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(all ? "Clear every filter (Shift F); Ctrl Z brings them back" : "Remove this filter");
    ImGui::PopStyleColor(4);
    ImGui::PopID();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
  blockers_.push_back({ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                       ImGui::GetWindowPos().y + ImGui::GetWindowSize().y});
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  if (off == int(chips.size())) {
    set_rules({});
    auto f = filter_;
    f.volt.clear();
    f.fuel.clear();
    f.party.clear();
    f.turn.clear();
    f.prov.clear();
    set_filter(f);
    for (auto it = lstate_.hidden.begin(); it != lstate_.hidden.end();)
      it = std::find(lstate_.on.begin(), lstate_.on.end(), *it) != lstate_.on.end() ? lstate_.hidden.erase(it) : std::next(it);
    apply_layers();
    toast("Every filter cleared \xc2\xb7 Ctrl Z brings them back");
  } else if (off >= 0) {
    chips[size_t(off)].off();
  }
}

// ------------------------------------------------------------------ REPORT

std::vector<std::string> App::card_lines(const Card& c) {
  std::vector<std::string> L;  // "label\tvalue"; a line without a tab is a heading
  auto add = [&](const std::string& k, const std::string& v) {
    if (!v.empty()) L.push_back(k + "\t" + v);
  };
  auto muni_block = [&](const find::Muni* m) {
    if (!m) return;
    L.push_back("Jobs, income and services (Census 2011)");
    for (const auto& ms : find::measures()) {
      const double v = m->*ms.field;
      if (v >= 0 && (ms.census || std::string(ms.key) == "pv")) add(ms.label, find::fmt_measure(ms, v));
    }
    if (!m->win.empty())
      add("Most wards won", m->win + " " + std::to_string(m->win_wards) + " of " + std::to_string(m->wards));
  };
  if (c.kind == Card::Area) {
    auto it = areas_.find(c.id);
    if (it != areas_.end()) {
      const json& a = it->second;
      L.push_back("Figures");
      if (auto v = metric(a, "pop_est")) add("People", find::fmt_num(*v));
      const double km2 = a.contains("area_km2") && a["area_km2"].is_number() ? a["area_km2"].get<double>() : 0;
      if (auto v = metric(a, "pop_est"); v && km2 > 0) add("People per km\xc2\xb2", find::fmt_num(*v / km2));
      if (auto v = metric(a, "households")) add("Households", find::fmt_num(*v));
      if (auto v = metric(a, "value_median")) add("Median property value", "R" + find::fmt_num(*v));
      if (auto v = metric(a, "parcels")) add("Valued plots", find::fmt_num(*v));
      if (km2 > 0) add("Area", find::fmt_num(km2) + " km\xc2\xb2");
    }
    if (fdata_) muni_block(fdata_->muni(c.id));
  } else if (c.kind == Card::Facility) {
    L.push_back("Details");
    static const std::set<std::string> skip = {"name", "layer", "tag", "sg21"};
    for (auto it = c.props.begin(); it != c.props.end(); ++it)
      if (!skip.count(it.key()) && !it.value().is_null() && !it.value().is_object() && !it.value().is_array())
        add(it.key(), prop_text(it.value()));
    char ll[64];
    std::snprintf(ll, sizeof ll, "%.5f, %.5f", c.lat, c.lon);
    add("Position", ll);
  } else if (c.kind == Card::Spot || c.kind == Card::Near) {
    char ll[64];
    std::snprintf(ll, sizeof ll, "%.5f, %.5f", c.lat, c.lon);
    add("Position", ll);
    if (fdata_) {
      L.push_back("Around this spot");
      for (const auto& [h, t] : find::spot(*fdata_, c.lon, c.lat, rules_)) add(h, t);
      muni_block(fdata_->muni_at(c.lon, c.lat));
    }
  }
  return L;
}

void App::report() {
  const Card& c = cards_.back();
  if (c.kind == Card::Home || panel_ != Panel::None) {
    toast("Open a place first, then REPORT");
    return;
  }
  if (c.kind == Card::Area) area_data(c.id);
  report_pending_ = true;  // written once the map has settled (report_now)
  toast("Making the report ...");
}

bool App::report_ready() const {
  const Card& c = cards_.back();
  return !fly_ && !(c.kind == Card::Area && !areas_.count(c.id)) && map_->needed(cam_, packs_).empty() &&
         loader_->in_flight() == 0 && !map_->meshing();
}

void App::report_now() {
  const Card c = cards_.back();
  const std::string title = c.kind == Card::Spot ? [&] {
    std::string m = muni_name_at(geo::mx(c.lon), geo::my(c.lat));
    return m.empty() ? std::string("This spot") : "Spot in " + m;
  }()
                                                 : subject_name();
  report_pending_ = false;
  std::string kicker = "Place";
  if (c.kind == Card::Area) kicker = kind_label(areas_[c.id].value("kind", "MUNI"));
  else if (c.kind == Card::Spot) kicker = "Spot";
  else if (c.kind == Card::Near) kicker = "Within " + find::fmt_num(c.props.value("km", 25.0)) + " km";
  else if (c.kind == Card::Facility) kicker = c.props.value("tag", c.props.value("layer", std::string("Place")));
  const auto lines = card_lines(c);
  const float crop_x = side_width();
  // the map image arrives with the next frame; the page is written then
  on_png_ = [this, title, kicker, lines, crop_x](const std::vector<uint8_t>& px, uint32_t W, uint32_t H) {
    const float dpr = cam_.w > 0 ? float(W) / float(cam_.w) : 1.0f;
    const uint32_t x0 = std::min(W, uint32_t(crop_x * dpr)), y1 = H - std::min(H, uint32_t(kStatusH * dpr));
    const uint32_t w = W - x0, h = y1;
    std::vector<uint8_t> crop(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) std::memcpy(&crop[size_t(y) * w * 4], &px[(size_t(y) * W + x0) * 4], size_t(w) * 4);
    std::vector<uint8_t> png;
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
      auto* v = static_cast<std::vector<uint8_t>*>(ctx);
      v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }, &png, int(w), int(h), 4, crop.data(), int(w * 4));
    const std::string d = today();
    std::string body;
    bool open = false;
    for (const auto& l : lines) {
      const auto tab = l.find('\t');
      if (tab == std::string::npos) {
        if (open) body += "</table>";
        body += "<h2>" + html_escape(l) + "</h2><table>";
        open = true;
      } else {
        if (!open) body += "<table>", open = true;
        body += "<tr><th>" + html_escape(l.substr(0, tab)) + "</th><td>" + html_escape(l.substr(tab + 1)) + "</td></tr>";
      }
    }
    if (open) body += "</table>";
    const std::string html =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>" + html_escape(title) +
        " \xc2\xb7 Grid of SA report</title><style>body{font:15px/1.45 system-ui,sans-serif;color:#13202a;max-width:820px;"
        "margin:24px auto;padding:0 20px}.k{color:#5d7383;font:12px ui-monospace,monospace;text-transform:uppercase;"
        "letter-spacing:.06em}h1{font-size:26px;margin:2px 0}h2{font-size:15px;margin:20px 0 6px;color:#2b4252}"
        "img{width:100%;border:1px solid #cfdbe3;border-radius:4px;margin:14px 0}table{width:100%;border-collapse:"
        "collapse}th,td{text-align:left;padding:5px 0;border-bottom:1px solid #e3ebf0;vertical-align:top}th{font-weight:"
        "400;color:#5d7383;width:45%}td{font-variant-numeric:tabular-nums}footer{margin-top:24px;border-top:1px solid "
        "#cfdbe3;padding-top:10px;color:#5d7383;font-size:12px}</style></head><body><div class=\"k\">" +
        html_escape(kicker) + " \xc2\xb7 Grid of SA \xc2\xb7 " + d + "</div><h1>" + html_escape(title) + "</h1><img alt=\"Map of " + html_escape(title) +
        " as shown on screen\" src=\"data:image/png;base64," + base64(png) + "\">" + body +
        "<footer>Made with the Grid of SA desktop app on " + d +
        ". Sources: Stats SA (Census 2011, 2022 estimates), municipal valuation rolls, IEC, NTCSA, DWS, OpenStreetMap "
        "contributors and the others named in the app. No owner names or ID numbers are held.</footer></body></html>";
    const char* home = std::getenv(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    fs::path dir = home ? fs::path(home) / "Documents" : fs::current_path();
    std::error_code ec;
    if (!fs::exists(dir, ec)) dir = home ? fs::path(home) : fs::current_path();
    const fs::path out = dir / ("report-" + slug(title) + "-" + d + ".html");
    std::ofstream f(out, std::ios::binary);
    f << html;
    if (f) toast("Saved " + out.string() + " \xc2\xb7 open it and print to PDF", 5);
    else toast("Could not save the report in " + dir.string());
  };
  want_png_ = true;
}

// ------------------------------------------------------------------ NEAR and CHK

void App::open_near(double lon, double lat, double km, const std::string& label) {
  Card c;
  c.kind = Card::Near;
  c.lon = lon;
  c.lat = lat;
  c.props = {{"km", km}, {"label", label}};
  if (cards_.back().kind == Card::Near) cards_.back() = c;
  else cards_.push_back(c);
  panel_ = Panel::None;
  side_open_ = true;
  const double dlat = km / 111.0, dlon = dlat / std::cos(lat * geo::kPi / 180);
  fly_to_bbox(lon - dlon, lat - dlat, lon + dlon, lat + dlat, 13);
}

void App::ui_near(Card& c) {
  if (close_button()) {
    close_top();
    return;
  }
  const double km = c.props.value("km", 25.0);
  const std::string label = c.props.value("label", std::string("This spot"));
  char ll[64];
  std::snprintf(ll, sizeof ll, "%.3f\xc2\xb0 S %.3f\xc2\xb0 E", std::abs(c.lat), c.lon);
  card_head("NEAR", kAccent, ("WITHIN " + find::fmt_num(km) + " KM").c_str(), label, ll, "");
  if (!in_kept_) {
    ImGui::PushFont(font_mono_);
    const float bw = (ImGui::GetContentRegionAvail().x - 12) / 4;
    for (double r : {10.0, 25.0, 50.0, 100.0}) {
      if (r != 10.0) ImGui::SameLine(0, 4);
      if (chip((find::fmt_num(r) + " km").c_str(), km == r, ImVec2(bw, 24))) {
        open_near(c.lon, c.lat, r, label);
        ImGui::PopFont();
        return;
      }
    }
    ImGui::PopFont();
  }
  struct Item {
    std::string name, sub;
    double km, lon, lat;
  };
  auto within = [&](const std::vector<find::Site>& v, size_t n, auto sub) {
    std::vector<Item> out;
    for (const auto& s : v) {
      const double d = find::km_between(c.lon, c.lat, s.lon, s.lat);
      if (d <= km) out.push_back({s.name, sub(s), d, s.lon, s.lat});
    }
    std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.km < b.km; });
    if (out.size() > n) out.resize(n);
    return out;
  };
  auto list = [&](const char* head, const std::vector<Item>& items) {
    if (items.empty()) return;
    section(font_mono_, head);
    for (size_t i = 0; i < items.size(); ++i) {
      const auto& it = items[i];
      ImGui::PushID((std::string(head) + std::to_string(i)).c_str());
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      if (ImGui::Selectable("##it", false, 0, ImVec2(0, 22))) fly_to(it.lon, it.lat, std::max(cam_.zoom, 13.0));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fly there");
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddText(font_ui_, 13.5f, ImVec2(p.x + 2, p.y + 3), u32(kInk), it.name.c_str());
      if (!it.sub.empty()) {
        const float nx = font_ui_->CalcTextSizeA(13.5f, FLT_MAX, 0, it.name.c_str()).x;
        dl->AddText(font_small_, 12, ImVec2(p.x + 10 + nx, p.y + 4), u32(kFaint), it.sub.c_str());
      }
      const std::string d = find::fmt_km(it.km);
      const ImVec2 ds = font_mono_->CalcTextSizeA(11.5f, FLT_MAX, 0, d.c_str());
      dl->AddText(font_mono_, 11.5f, ImVec2(p.x + w - ds.x, p.y + 4), u32(kMuted), d.c_str());
      ImGui::PopID();
    }
  };
  // services from the base map's landmarks that are loaded around the circle
  std::vector<Item> sch, cli, pol;
  {
    std::set<std::string> seen;
    map_->points("base", "poi", [&](const mvt::Feature& f, double wx, double wy) {
      const std::string k = f.str("k"), n = f.str("name");
      std::vector<Item>* dst = k == "school" ? &sch : (k == "hospital" || k == "clinic") ? &cli : k == "police" ? &pol : nullptr;
      if (!dst) return;
      const double lon = geo::lon_of(wx), lat = geo::lat_of(wy), d = find::km_between(c.lon, c.lat, lon, lat);
      if (d > km) return;
      char key[96];
      std::snprintf(key, sizeof key, "%s|%.3f|%.3f", n.c_str(), lon, lat);
      if (!seen.insert(key).second) return;
      dst->push_back({n.empty() ? (k == "school" ? "School" : k == "police" ? "Police station" : "Clinic") : n,
                      k == "hospital" ? "hospital" : "", d, lon, lat});
    });
    for (auto* v : {&sch, &cli, &pol}) std::sort(v->begin(), v->end(), [](const Item& a, const Item& b) { return a.km < b.km; });
  }
  if (fdata_) {
    const auto& d = *fdata_;
    // headline figures
    double mw = 0;
    int stations = 0;
    for (const auto& s : d.st)
      if (find::km_between(c.lon, c.lat, s.lon, s.lat) <= km) ++stations, mw += s.a;
    struct Fact {
      std::string n, l;
    };
    const Fact facts[3] = {{std::to_string(sch.size()), "schools on the map"},
                           {std::to_string(cli.size()), "clinics, hospitals"},
                           {std::to_string(stations), stations ? find::fmt_num(mw) + " MW stations" : "power stations"}};
    ImGui::Spacing();
    const float w = (ImGui::GetContentRegionAvail().x - 12) / 3;
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine(0, 6);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddRect(p, ImVec2(p.x + w, p.y + 48), u32(kLine), 4);
      dl->AddText(font_head_, 15, ImVec2(p.x + 9, p.y + 6), u32(kInk), facts[i].n.c_str());
      dl->AddText(font_small_, 11.5f, ImVec2(p.x + 9, p.y + 28), u32(kMuted), facts[i].l.c_str());
      ImGui::Dummy(ImVec2(w, 48));
    }
    list("TOWNS", within(d.places, 5, [](const find::Site&) { return std::string(); }));
  }
  list("SCHOOLS", std::vector<Item>(sch.begin(), sch.begin() + long(std::min<size_t>(5, sch.size()))));
  list("CLINICS AND HOSPITALS", std::vector<Item>(cli.begin(), cli.begin() + long(std::min<size_t>(5, cli.size()))));
  list("POLICE", std::vector<Item>(pol.begin(), pol.begin() + long(std::min<size_t>(3, pol.size()))));
  if (fdata_) {
    const auto& d = *fdata_;
    list("DAMS", within(d.dams, 4, [](const find::Site& s) { return find::fmt_num(s.a) + "% full"; }));
    list("AIRPORTS", within(d.air, 3, [](const find::Site& s) { return s.t; }));
    list("PORTS", within(d.ports, 2, [](const find::Site& s) { return s.t; }));
    list("GRID ROOM (NTCSA 2025)",
         within(d.cap, 3, [](const find::Site& s) { return s.a > 0 ? find::fmt_num(s.a) + " MW free" : "full"; }));
    list("POWER STATIONS", within(d.st, 3, [](const find::Site& s) { return find::fmt_num(s.a) + " MW " + s.t; }));
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::PushFont(font_small_);
  ImGui::TextColored(col(kFaint), "Straight-line distances. Schools, clinics and police come from the base map's "
                                  "landmarks loaded around the circle (OpenStreetMap), so zoom in for the full count.");
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
  if (!in_kept_) fn_strip({"KEEP", "REPORT", "CHK", "MEAS", "COPY", "PNG"});
}

// ------------------------------------------------------------------ task search

std::vector<App::Task> App::tasks_for(const std::string& query) const {
  const std::string q = lower(query);
  if (q.size() < 3) return {};
  struct T {
    std::vector<const char*> words;
    const char* label;
    const char* command;
  };
  static const std::vector<T> table = {
      {{"unemploy", "jobless", "jobs"}, "Rank municipalities by unemployment", "RANK mun"},
      {{"income", "poor", "poorest", "richest", "wealth"}, "Rank municipalities by household income", "RANK minc"},
      {{"matric", "education", "school"}, "Rank municipalities by adults with matric", "RANK mmat"},
      {{"internet", "remote work", "fibre"}, "Rank municipalities by homes with internet", "RANK mnet"},
      {{"informal", "backlog", "shack"}, "Find service backlogs (informal homes, no piped water)", "FIND service"},
      {{"cheap", "afford", "property value", "expensive", "house price"}, "Rank municipalities by median property value", "RANK mval"},
      {{"dam level", "dams", "water level", "drought"}, "Rank dams in view, lowest first", "RANK dams"},
      {{"capacity", "grid room", "connect", "free mw"}, "Rank substations by free grid capacity", "RANK cap"},
      {{"solar", "pv"}, "Find places for a solar farm", "FIND solar"},
      {{"data centre", "data center"}, "Find places for a data centre", "FIND data"},
      {{"population", "people", "biggest"}, "Rank municipalities by people", "RANK mpop"},
      {{"airport"}, "Rank airports in view", "RANK air"},
      {{"compare", "side by side", "versus", " vs "}, "Compare municipalities side by side", "CMP"},
      {{"report", "print", "pdf"}, "Save a report of the open card", "REPORT"},
      {{"measure", "distance", "how far"}, "Measure on the map", "MEAS"},
      {{"near", "close to", "nearby", "clinic", "hospital", "police"}, "What is nearby: towns, schools, clinics, dams", "NEAR"},
      {{"undo", "history", "go back"}, "History of changes (undo, redo)", "HIST"},
      {{"filter", "voltage", "turnout"}, "Filters: voltage, fuel, ward party, turnout, province", "FILTER"},
      {{"watch", "alert"}, "The watchlist", "WL"},
      {{"keep", "pin card", "two cards"}, "Keep this card while you open another", "KEEP"},
      {{"check", "is this a good"}, "Check this place", "CHK"},
      {{"plots", "erf", "stand", "valuation"}, "Topic: property plots and values", "PLOTS"},
      {{"election", "vote", "ward"}, "Topic: votes and people", "VOTE"},
      {{"substation", "power line", "eskom", "grid"}, "Topic: power and grid", "GRID"},
  };
  std::vector<Task> out;
  for (const auto& t : table) {
    for (const char* w : t.words) {
      const std::string ws = w;
      // a word in the query, or the query is the start of a task word ("unem")
      if (q.find(ws) != std::string::npos || (q.size() >= 4 && ws.rfind(q, 0) == 0)) {
        out.push_back({t.label, t.command, ws});
        break;
      }
    }
    if (out.size() >= 3) break;
  }
  return out;
}

}  // namespace gsa
