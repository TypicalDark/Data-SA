// Sidebar content: the home card, area, facility and spot cards (type tag, title, function
// strip, tabs), and the rail panels (Find and Rank, List, Areas, Views, Watch, Keys).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

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

std::string fmt_num(double v) {
  char b[32];
  const double a = std::abs(v);
  if (a >= 1e9) std::snprintf(b, sizeof b, "%.1fbn", v / 1e9);
  else if (a >= 1e6) std::snprintf(b, sizeof b, "%.2fm", v / 1e6);
  else if (a >= 1e4) std::snprintf(b, sizeof b, "%.0fk", v / 1e3);
  else if (a >= 100) std::snprintf(b, sizeof b, "%.0f", v);
  else std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}

std::string fmt_rand(double v) { return "R " + fmt_num(v); }

std::string fmt_km(double km) {
  char b[32];
  std::snprintf(b, sizeof b, km < 10 ? "%.1f km" : "%.0f km", km);
  return b;
}

double km_between(double lon0, double lat0, double lon1, double lat1) {
  const double r = geo::kPi / 180, x = (lon1 - lon0) * r * std::cos((lat0 + lat1) / 2 * r), y = (lat1 - lat0) * r;
  return 6371.0 * std::hypot(x, y);
}

std::optional<double> metric(const json& area, const std::string& name) {
  if (!area.contains("metrics") || !area["metrics"].is_array()) return std::nullopt;
  for (const auto& m : area["metrics"])
    if (m.value("metric", "") == name) return m["v"].get<double>();  // newest first per metric
  return std::nullopt;
}

const char* area_tag_colour(const std::string& kind) {
  if (kind == "COUNTRY") return "#7df9ff";
  if (kind == "PROV") return "#a3b9c8";
  if (kind == "MUNI" || kind == "METRO" || kind == "DIST") return "#3ddc84";
  return "#8aa0ae";
}

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::toupper);
  return s;
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

void section(ImFont* f, const char* t) {
  ImGui::Spacing();
  ImGui::PushFont(f);
  ImGui::TextColored(col(kMuted), "%s", t);
  ImGui::PopFont();
}

bool chip(const char* label, bool on, ImVec2 size = ImVec2(0, 0), bool enabled = true) {
  ImGui::PushStyleColor(ImGuiCol_Border, on ? col(kAccent) : col(kLine));
  ImGui::PushStyleColor(ImGuiCol_Text, !enabled ? col(kFaint) : on ? col(kInk) : col(kMuted));
  ImGui::PushStyleColor(ImGuiCol_Button, on ? col(kHover) : ImVec4(0, 0, 0, 0));
  ImGui::BeginDisabled(!enabled);
  const bool r = ImGui::Button(label, size);
  ImGui::EndDisabled();
  ImGui::PopStyleColor(3);
  return r;
}

}  // namespace

// ------------------------------------------------------------------ data

const json* App::area_data(const std::string& id) {
  auto it = areas_.find(id);
  if (it != areas_.end()) return it->second.contains("error") ? nullptr : &it->second;
  if (!area_req_.count(id)) area_req_[id] = fetch("/v1/areas/" + api::url_encode(id));
  return nullptr;
}

std::vector<const json*> App::all_munis() {
  std::vector<const json*> out;
  const json* za = area_data("ZA");
  if (!za || !za->contains("children")) return out;
  for (const auto& p : (*za)["children"]) {
    const json* pa = area_data(p["id"].get<std::string>());
    if (!pa || !pa->contains("children")) continue;
    for (const auto& m : (*pa)["children"]) out.push_back(&m);
  }
  return out;
}

// ------------------------------------------------------------------ card parts

bool App::close_button() {
  const ImVec2 keep = ImGui::GetCursorPos();
  const float w = ImGui::GetWindowContentRegionMax().x;
  ImGui::SetCursorPos(ImVec2(w - 22, keep.y - 2));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool click = ImGui::InvisibleButton("##close", ImVec2(22, 22));
  const bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetTooltip("Close (Esc)");
  icons::draw(ImGui::GetWindowDrawList(), icons::Id::Close, ImVec2(p.x + 11, p.y + 11), 13, hov ? u32(kInk) : u32(kMuted), 1.4f);
  ImGui::SetCursorPos(keep);
  return click;
}

void App::card_head(const char* tag, const char* tag_colour, const char* kind, const std::string& title, const std::string& sub,
                    const std::string& data_line) {
  auto* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 ts = font_mono_->CalcTextSizeA(10.5f, FLT_MAX, 0, tag);
  dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + ts.x + 10, p.y + 17), u32(tag_colour), 3);
  dl->AddText(font_mono_, 10.5f, ImVec2(p.x + 5, p.y + 2.5f), u32(kAccentInk), tag);
  dl->AddText(font_mono_, 11, ImVec2(p.x + ts.x + 18, p.y + 2.5f), u32(kMuted), kind);
  ImGui::Dummy(ImVec2(ts.x + 10, 20));
  ImGui::PushFont(font_title_);
  ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 10);
  ImGui::TextUnformatted(title.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  if (!sub.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kMuted), "%s", sub.c_str());
    ImGui::PopTextWrapPos();
  }
  if (!data_line.empty()) {
    ImGui::PushFont(font_small_);
    ImGui::TextColored(col(kFaint), "Data: %s", data_line.c_str());
    ImGui::PopFont();
  }
}

void App::fn_strip(const std::vector<std::string>& codes) {
  ImGui::Spacing();
  ImGui::PushFont(font_mono_);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7, 4));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3);
  const float avail = ImGui::GetContentRegionAvail().x;
  float x = 0;
  for (const auto& code : codes) {
    const auto* s = commands::find(code);
    const bool ready = s && s->ready;
    const float bw = ImGui::CalcTextSize(code.c_str()).x + 14;
    if (x > 0 && x + bw > avail) x = 0;
    else if (x > 0) ImGui::SameLine(0, 4);
    x += bw + 4;
    ImGui::PushID(code.c_str());
    if (chip(code.c_str(), false, ImVec2(bw, 0), ready)) run_fn(code);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && s)
      ImGui::SetTooltip("%s%s", s->about, ready ? "" : "\nComes in a later milestone");
    ImGui::PopID();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
  ImGui::Spacing();
}

void App::kv_row(const char* k, const std::string& v) {
  const float w = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddText(font_small_, 12.5f, ImVec2(p.x, p.y + 3), u32(kMuted), k);
  const ImVec2 vs = font_mono_->CalcTextSizeA(12, FLT_MAX, 0, v.c_str());
  dl->AddText(font_mono_, 12, ImVec2(p.x + w - vs.x, p.y + 3), u32(kInk), v.c_str());
  dl->AddLine(ImVec2(p.x, p.y + 23), ImVec2(p.x + w, p.y + 23), u32(kLine, 0.6f));
  ImGui::Dummy(ImVec2(w, 24));
}

// ------------------------------------------------------------------ cards

void App::ui_home() {
  const layers::Topic* t = nullptr;
  for (const auto& x : layers::topics())
    if (lstate_.topic == x.id) t = &x;
  if (!t) t = &layers::topics().front();
  card_head("TOPIC", kAccent, (std::string("KEY ") + t->key).c_str(), t->name, t->blurb, "");
  // what the topic put on the map
  int drawn = 0, waiting = 0;
  for (const auto& id : lstate_.on) (avail_.count(id) ? drawn : waiting)++;
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  if (waiting)
    ImGui::TextColored(col(kWarn), "%d of %d layers in this topic need their data packs, which come with milestone B.", waiting,
                       drawn + waiting);
  ImGui::PopTextWrapPos();
  fn_strip({"FIND", "RANK", "LIST", "AREAS", "SAVE", "PNG"});

  section(font_mono_, "TOPICS");
  ImGui::PushFont(font_mono_);
  float x = 0;
  const float avail = ImGui::GetContentRegionAvail().x;
  for (const auto& tp : layers::topics()) {
    const std::string label = std::string(tp.key) + " " + tp.name;
    const float bw = ImGui::CalcTextSize(label.c_str()).x + 14;
    if (x > 0 && x + bw > avail) x = 0;
    else if (x > 0) ImGui::SameLine(0, 4);
    x += bw + 4;
    if (chip(label.c_str(), lstate_.topic == tp.id, ImVec2(bw, 24))) set_topic(tp.id);
  }
  ImGui::PopFont();

  section(font_mono_, "START FROM");
  const json* za = area_data("ZA");
  if (ImGui::Selectable("South Africa")) open_area("ZA");
  if (za && za->contains("children")) {
    for (const auto& c : (*za)["children"]) {
      const std::string id = c["id"], name = c["name"];
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable(("   " + name + "##" + id).c_str())) open_area(id);
      if (c["metrics"].contains("pop_est")) {
        const std::string v = fmt_num(c["metrics"]["pop_est"].get<double>()) + " people";
        const float vw = font_mono_->CalcTextSizeA(11.5f, FLT_MAX, 0, v.c_str()).x;
        ImGui::GetWindowDrawList()->AddText(font_mono_, 11.5f, ImVec2(p.x + ImGui::GetContentRegionAvail().x - vw, p.y + 2),
                                            u32(kMuted), v.c_str());
      }
    }
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(col(kFaint),
                     "Click the map to open a place, right-click for what is here. Type a name or a command in the search box: "
                     "\"GO soweto\", \"apollo DES\", \"ADD dams\".");
  ImGui::PopTextWrapPos();
}

void App::ui_area(const std::string& id) {
  if (close_button()) {
    close_top();
    return;
  }
  auto it = areas_.find(id);
  if (it == areas_.end()) {
    ImGui::TextColored(col(kMuted), "Loading %s ...", id.c_str());
    return;
  }
  const json& a = it->second;
  if (a.contains("error")) {
    ImGui::TextColored(col(kWarn), "%s", a["error"].get<std::string>().c_str());
    return;
  }
  // breadcrumb
  if (a.contains("breadcrumb") && !a["breadcrumb"].empty()) {
    ImGui::PushFont(font_small_);
    bool first = true;
    for (const auto& b : a["breadcrumb"]) {
      if (!first) {
        ImGui::SameLine(0, 2);
        ImGui::TextColored(col(kFaint), "\xe2\x80\xba");
        ImGui::SameLine(0, 2);
      }
      first = false;
      const std::string bid = b["id"];
      ImGui::PushStyleColor(ImGuiCol_Text, col(kMuted));
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
      if (ImGui::SmallButton((b["name"].get<std::string>() + "##bc" + bid).c_str())) open_area(bid);
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }
    ImGui::PopFont();
  }
  const std::string kind = a.value("kind", "AREA");
  const char* kicker = kind == "COUNTRY" ? "COUNTRY" : kind == "PROV" ? "PROVINCE" : kind == "WARD" ? "WARD" : "MUNICIPALITY";
  std::string sub = id;
  if (a.contains("breadcrumb") && !a["breadcrumb"].empty()) sub += " \xc2\xb7 in " + a["breadcrumb"].back().value("name", "");
  card_head(kind.c_str(), area_tag_colour(kind), kicker, a.value("name", id), sub, "Stats SA 2022 estimates, municipal rolls GV2025");

  // facts
  const auto pop = metric(a, "pop_est");
  const double km2 = a.contains("area_km2") && a["area_km2"].is_number() ? a["area_km2"].get<double>() : 0;
  const auto val = metric(a, "value_median");
  const auto parcels = metric(a, "parcels");
  const auto hh = metric(a, "households");
  struct Fact {
    std::string n, l;
  };
  std::vector<Fact> facts;
  if (pop) facts.push_back({fmt_num(*pop), "people"});
  if (pop && km2 > 0) facts.push_back({fmt_num(*pop / km2), "people per km\xc2\xb2"});
  if (hh) facts.push_back({fmt_num(*hh), "households"});
  if (val) facts.push_back({fmt_rand(*val), "median value"});
  if (parcels) facts.push_back({fmt_num(*parcels), "valued plots"});
  if (km2 > 0) facts.push_back({fmt_num(km2), "km\xc2\xb2"});
  if (!facts.empty()) {
    ImGui::Spacing();
    const float w = (ImGui::GetContentRegionAvail().x - 12) / 3;
    for (size_t i = 0; i < facts.size() && i < 6; ++i) {
      if (i % 3) ImGui::SameLine(0, 6);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddRect(p, ImVec2(p.x + w, p.y + 50), u32(kLine), 4);
      dl->AddText(font_head_, 15, ImVec2(p.x + 9, p.y + 7), u32(kInk), facts[i].n.c_str());
      dl->AddText(font_small_, 11.5f, ImVec2(p.x + 9, p.y + 29), u32(kMuted), facts[i].l.c_str());
      ImGui::Dummy(ImVec2(w, 50));
    }
  }
  fn_strip({"DES", "HERE", "NEAR", "REACH", "MEAS", "WATCH", "GO", "COPY", "PNG"});

  // the parts of this area, ranked by the measure of the topic
  if (a.contains("children") && !a["children"].empty()) {
    const bool by_val = lstate_.topic == "prop";
    const char* measure = by_val ? "value_median" : "pop_est";
    std::vector<Row> rows;
    for (const auto& c : a["children"]) {
      Row r;
      r.name = c["name"];
      r.area = c["id"];
      const bool has = c["metrics"].contains(measure);
      r.x = c["metrics"].value(measure, 0.0);
      r.value = !has ? "no data" : by_val ? fmt_rand(r.x) : fmt_num(r.x);
      r.colour = style::hex(by_val ? "#de4968" : "#3ddc84").packed();
      rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& p, const Row& q) { return p.x > q.x; });
    section(font_mono_, (std::to_string(rows.size()) + (by_val ? " PARTS BY MEDIAN VALUE" : " PARTS BY PEOPLE")).c_str());
    rows_list(rows, "kids", 0);
  }
}

void App::ui_facility(Card& c) {
  if (close_button()) {
    close_top();
    return;
  }
  const json& p = c.props;
  const std::string kind = p.value("kind", std::string());
  const bool station = kind == "power_station" || p.value("tag", "") == "PLANT";
  const double kv = p.contains("kv") && p["kv"].is_number() ? p["kv"].get<double>() : 0;
  const double mw = p.contains("mw") && p["mw"].is_number() ? p["mw"].get<double>() : 0;
  std::string sub;
  char b[64];
  if (station) {
    if (p.contains("fuel")) sub = p["fuel"].get<std::string>();
    if (mw > 0) {
      std::snprintf(b, sizeof b, "%s%.0f MW", sub.empty() ? "" : " \xc2\xb7 ", mw);
      sub += b;
    }
  } else if (kv > 0) {
    std::snprintf(b, sizeof b, "%.0f kV", kv);
    sub = b;
  }
  if (p.contains("operator") && p["operator"].is_string()) sub += (sub.empty() ? "" : " \xc2\xb7 ") + p["operator"].get<std::string>();
  if (sub.empty() && p.contains("detail")) sub = p["detail"].get<std::string>();
  const std::string tag = station ? "PLANT" : "SUB";
  std::string tcs = kWarn;
  if (station) {
    const auto pc = style::plant(p.value("fuel", ""));
    char hx[16];
    std::snprintf(hx, sizeof hx, "#%02x%02x%02x", pc.r, pc.g, pc.b);
    tcs = hx;
  }
  card_head(tag.c_str(), tcs.c_str(), station ? "POWER STATION" : "SUBSTATION", p.value("name", c.id), sub,
            "OpenStreetMap power data, NTCSA GCCA 2025");
  fn_strip({"DES", "HERE", "NEAR", "TR", "FAIL", "FEED", "PATH", "REACH", "MEAS", "WATCH", "GO", "COPY", "PNG"});

  // tabs
  static const char* tabs[] = {"Details", "Connections", "Area served"};
  ImGui::PushFont(font_mono_);
  const float tw = (ImGui::GetContentRegionAvail().x - 8) / 3;
  for (int i = 0; i < 3; ++i) {
    if (i) ImGui::SameLine(0, 4);
    if (chip(tabs[i], c.tab == i, ImVec2(tw, 26))) c.tab = i;
  }
  ImGui::PopFont();
  ImGui::Spacing();
  if (c.tab == 0) {
    char ll[64];
    std::snprintf(ll, sizeof ll, "%.5f, %.5f", c.lat, c.lon);
    kv_row("Position", ll);
    std::string prov;
    const std::string muni = muni_name_at(geo::mx(c.lon), geo::my(c.lat), &prov);
    if (!muni.empty()) kv_row("Municipality", muni);
    if (!prov.empty()) kv_row("Province", prov);
    static const std::set<std::string> skip = {"id", "name", "tag", "detail", "kind"};
    for (const auto& [k, v] : p.items()) {
      if (skip.count(k)) continue;
      std::string key = k;
      if (!key.empty()) key[0] = char(std::toupper(static_cast<unsigned char>(key[0])));
      std::replace(key.begin(), key.end(), '_', ' ');
      std::string val = prop_text(v);
      if (k == "kv") val += " kV";
      if (k == "mw") val += " MW";
      kv_row(key.c_str(), val);
    }
  } else {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kMuted), "%s",
                       c.tab == 1 ? "The lines and substations this one connects to, with TR to trace them. Comes with milestone D."
                                  : "The supply zone this one feeds and who lives in it, with FEED. Comes with milestone D.");
    ImGui::PopTextWrapPos();
  }
}

void App::ui_spot(Card& c) {
  if (close_button()) {
    close_top();
    return;
  }
  const double wx = geo::mx(c.lon), wy = geo::my(c.lat);
  const auto* m = map_->pick(wx, wy, "municipality", std::max(cam_.zoom, 6.0));
  const auto* pv = map_->pick(wx, wy, "province", std::max(cam_.zoom, 6.0));
  char ll[64];
  std::snprintf(ll, sizeof ll, "%.5f, %.5f", c.lat, c.lon);
  const std::string title = m ? m->str("name") : std::string("Outside South Africa");
  std::string sub = ll;
  if (pv) sub = pv->str("name") + " \xc2\xb7 " + sub;
  card_head("SPOT", kAccent, "WHAT IS HERE", title, sub, "");
  fn_strip({"NEAR", "REACH", "MEAS", "GO", "COPY", "PNG"});

  if (m) {
    const std::string id = m->str("id");
    section(font_mono_, "THE MUNICIPALITY");
    if (const json* a = area_data(id)) {
      const auto pop = metric(*a, "pop_est");
      const double km2 = a->contains("area_km2") && (*a)["area_km2"].is_number() ? (*a)["area_km2"].get<double>() : 0;
      if (pop) kv_row("People", fmt_num(*pop));
      if (pop && km2 > 0) kv_row("People per km\xc2\xb2", fmt_num(*pop / km2));
      if (const auto v = metric(*a, "value_median")) kv_row("Median property value", fmt_rand(*v));
      if (const auto v = metric(*a, "parcels")) kv_row("Valued plots", fmt_num(*v));
      if (ImGui::SmallButton(("Open " + title).c_str())) open_area(id, false);
    } else {
      ImGui::TextColored(col(kMuted), "Loading ...");
    }
  }
  // the nearest grid points among what the map has loaded
  struct Near {
    double km;
    const map_labels::FeatureRef* r;
  };
  std::vector<Near> subs, plants;
  for (const auto& r : map_->drawn_facilities()) {
    const double km = km_between(c.lon, c.lat, geo::lon_of(r.wx), geo::lat_of(r.wy));
    (r.feature().str("kind") == "power_station" ? plants : subs).push_back({km, &r});
  }
  auto show = [&](std::vector<Near>& v, const char* head) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end(), [](const Near& a, const Near& b) { return a.km < b.km; });
    section(font_mono_, head);
    for (size_t i = 0; i < v.size() && i < 3; ++i) {
      const auto& f = v[i].r->feature();
      std::string name = f.str("name");
      if (name.empty()) name = "(no name)";
      std::string detail = fmt_km(v[i].km);
      if (const auto kv = f.num("kv")) detail = std::to_string(int(*kv)) + " kV \xc2\xb7 " + detail;
      if (const auto mw = f.num("mw")) detail = std::to_string(int(*mw)) + " MW \xc2\xb7 " + detail;
      ImGui::PushID(v[i].r);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      if (ImGui::Selectable("##n", false, 0, ImVec2(0, 24))) {
        open_facility(*v[i].r);
        ImGui::PopID();
        return;
      }
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddText(ImVec2(p.x + 2, p.y + 3), u32(kInk), name.c_str());
      const float dw = font_mono_->CalcTextSizeA(11.5f, FLT_MAX, 0, detail.c_str()).x;
      dl->AddText(font_mono_, 11.5f, ImVec2(p.x + w - dw, p.y + 5), u32(kMuted), detail.c_str());
      ImGui::PopID();
    }
  };
  show(subs, "NEAREST SUBSTATIONS IN VIEW");
  show(plants, "NEAREST POWER STATIONS IN VIEW");
  if (subs.empty() && plants.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "Put Substations or Power stations on the map (key 2) to see the nearest ones here.");
    ImGui::PopTextWrapPos();
  }
}

// ------------------------------------------------------------------ lists

void App::rows_list(std::vector<Row>& rows, const char* id, float height) {
  ImGui::BeginChild(id, ImVec2(0, height));
  double top = 0;
  for (const auto& r : rows) top = std::max(top, r.x);
  const auto& drawn = map_->drawn_facilities();
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    ImGui::PushID(int(i));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = r.sub.empty() ? 30.0f : 42.0f;
    if (ImGui::Selectable("##r", false, 0, ImVec2(0, h))) {
      if (!r.area.empty()) open_area(r.area);
      else if (r.pick >= 0 && size_t(r.pick) < drawn.size()) {
        open_facility(drawn[size_t(r.pick)]);
        fly_to(r.lon, r.lat, std::max(cam_.zoom, 11.0));
      } else if (r.lon != 0) fly_to(r.lon, r.lat, r.zoom > 0 ? r.zoom : std::max(cam_.zoom, 11.0));
    }
    auto* dl = ImGui::GetWindowDrawList();
    const float vw = font_mono_->CalcTextSizeA(12, FLT_MAX, 0, r.value.c_str()).x;
    dl->PushClipRect(p, ImVec2(p.x + w - vw - 10, p.y + h), true);
    dl->AddText(ImVec2(p.x + 2, p.y + 1), u32(kInk), r.name.c_str());
    dl->PopClipRect();
    dl->AddText(font_mono_, 12, ImVec2(p.x + w - vw - 2, p.y + 3), u32(kMuted), r.value.c_str());
    if (!r.sub.empty()) dl->AddText(font_small_, 11.5f, ImVec2(p.x + 2, p.y + 19), u32(kFaint), r.sub.c_str());
    const float by = p.y + h - 7;
    dl->AddRectFilled(ImVec2(p.x + 2, by), ImVec2(p.x + w - 2, by + 3), u32(kLine), 2);
    if (top > 0 && r.x > 0)
      dl->AddRectFilled(ImVec2(p.x + 2, by), ImVec2(p.x + 2 + float((w - 4) * r.x / top), by + 3),
                        r.colour ? r.colour : u32(kAccent), 2);
    ImGui::PopID();
  }
  if (rows.empty()) ImGui::TextColored(col(kFaint), "Nothing to list here yet.");
  ImGui::EndChild();
}

std::vector<App::Row> App::rows_in_view(const std::string& layer) {
  std::vector<Row> rows;
  double x0, y0, x1, y1;
  cam_.to_world(side_width(), 0, x0, y0);
  cam_.to_world(cam_.w, cam_.h - kStatusH, x1, y1);
  if (layer == "subs" || layer == "plants") {
    const auto& drawn = map_->drawn_facilities();
    for (size_t i = 0; i < drawn.size(); ++i) {
      const auto& f = drawn[i].feature();
      const bool st = f.str("kind") == "power_station";
      if (st != (layer == "plants")) continue;
      Row r;
      r.name = f.str("name");
      if (r.name.empty()) r.name = "(no name)";
      r.pick = int(i);
      r.lon = geo::lon_of(drawn[i].wx);
      r.lat = geo::lat_of(drawn[i].wy);
      if (st) {
        r.x = f.num("mw").value_or(0);
        r.value = r.x > 0 ? std::to_string(int(r.x)) + " MW" : "? MW";
        r.sub = f.str("fuel");
        r.colour = u32(style::plant(f.str("fuel")));
      } else {
        r.x = f.num("kv").value_or(0);
        r.value = r.x > 0 ? std::to_string(int(r.x)) + " kV" : "? kV";
        r.colour = u32(style::volt(r.x));
      }
      rows.push_back(r);
    }
  } else {
    // municipalities whose centre is in view
    for (const json* m : all_munis()) {
      if (!m->contains("center")) continue;
      const double wx = geo::mx((*m)["center"][0].get<double>()), wy = geo::my((*m)["center"][1].get<double>());
      if (wx < x0 || wx > x1 || wy < y0 || wy > y1) continue;
      const json& mt = (*m)["metrics"];
      const double km2 = m->value("area_km2", 0.0);
      Row r;
      r.name = (*m)["name"];
      r.area = (*m)["id"];
      if (layer == "val") {
        if (!mt.contains("value_median")) continue;
        r.x = mt["value_median"];
        r.value = fmt_rand(r.x);
        r.colour = style::hex("#de4968").packed();
      } else if (layer == "dens") {
        if (!mt.contains("pop_est") || km2 <= 0) continue;
        r.x = mt["pop_est"].get<double>() / km2;
        r.value = fmt_num(r.x) + " /km\xc2\xb2";
        r.colour = style::hex("#ffb020").packed();
      } else {
        if (!mt.contains("pop_est")) continue;
        r.x = mt["pop_est"];
        r.value = fmt_num(r.x);
        r.colour = style::hex("#3ddc84").packed();
      }
      rows.push_back(r);
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.x > b.x; });
  return rows;
}

// ------------------------------------------------------------------ panels

void App::ui_panel_find() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head(find_rank_ ? "RANK" : "FIND", kAccent, find_rank_ ? "SHIFT N" : "KEY N",
            find_rank_ ? "Rank what's in view" : "Find places where ...", "", "");
  ImGui::PushFont(font_mono_);
  const float tw = (ImGui::GetContentRegionAvail().x - 4) / 2;
  if (chip("Find places", !find_rank_, ImVec2(tw, 26))) find_rank_ = false;
  ImGui::SameLine(0, 4);
  if (chip("Rank in view", find_rank_, ImVec2(tw, 26))) find_rank_ = true;
  ImGui::PopFont();
  ImGui::Spacing();
  if (find_rank_) {
    ui_panel_list(true);
    return;
  }
  // presets, as on the website; the parts that need distances wait for the find pack
  struct Preset {
    const char* name;
    FindQuery q;
    const char* needs;
  };
  static const Preset presets[] = {
      {"Solar farm", {0, 0, 0, 0, 0, "Solar farm"}, "within 40 km of a substation with 100 MW free"},
      {"Data centre", {300, 0, 0, 0, 0, "Data centre"}, "within 15 km of 20 MW free and 60 km of an airport"},
      {"Warehouse near a port", {0, 0, 0, 0, 0, "Warehouse near a port"}, "within 120 km of a port and 20 km of a town"},
      {"Affordable, busy towns", {100, 0, 0, 0, 600000, "Affordable, busy towns"}, ""},
      {"Water-secure areas", {0, 0, 0, 0, 0, "Water-secure areas"}, "within 30 km of a dam over 75% full"},
  };
  section(font_mono_, "PRESETS");
  ImGui::PushFont(font_mono_);
  float x = 0;
  const float avail = ImGui::GetContentRegionAvail().x;
  for (const auto& p : presets) {
    const float bw = ImGui::CalcTextSize(p.name).x + 14;
    if (x > 0 && x + bw > avail) x = 0;
    else if (x > 0) ImGui::SameLine(0, 4);
    x += bw + 4;
    if (chip(p.name, find_.preset == p.name, ImVec2(bw, 24))) find_ = p.q;
    if (p.needs[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("Also %s: needs the find pack (milestone B)", p.needs);
  }
  ImGui::PopFont();

  section(font_mono_, "CONDITIONS");
  auto num = [&](const char* label, double& v, double step, const char* unit) {
    ImGui::PushID(label);
    ImGui::TextColored(col(kMuted), "%s", label);
    ImGui::SameLine(170);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 50);
    if (ImGui::InputDouble("##v", &v, step, step * 10, "%.0f")) find_.preset.clear();
    v = std::max(0.0, v);
    ImGui::SameLine();
    ImGui::TextColored(col(kFaint), "%s", unit);
    ImGui::PopID();
  };
  num("People per km\xc2\xb2 at least", find_.dens_min, 10, "");
  num("People per km\xc2\xb2 at most", find_.dens_max, 10, "0=any");
  num("People at least", find_.pop_min, 10000, "");
  num("Median value at least", find_.val_min, 50000, "R");
  num("Median value at most", find_.val_max, 50000, "R");
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(col(kFaint), "Near a substation, dam, town, airport or port, and party won, come with the find pack (milestone B).");
  ImGui::PopTextWrapPos();
  if (chip("Reset", false, ImVec2(70, 24))) find_ = FindQuery{};

  // results over municipalities
  std::vector<Row> rows;
  const auto munis = all_munis();
  for (const json* m : munis) {
    const json& mt = (*m)["metrics"];
    const double pop = mt.value("pop_est", -1.0), val = mt.value("value_median", -1.0), km2 = m->value("area_km2", 0.0);
    const double dens = pop >= 0 && km2 > 0 ? pop / km2 : -1;
    if (find_.dens_min > 0 && dens < find_.dens_min) continue;
    if (find_.dens_max > 0 && (dens < 0 || dens > find_.dens_max)) continue;
    if (find_.pop_min > 0 && pop < find_.pop_min) continue;
    if (find_.val_min > 0 && val < find_.val_min) continue;
    if (find_.val_max > 0 && (val < 0 || val > find_.val_max)) continue;
    Row r;
    r.name = (*m)["name"];
    r.area = (*m)["id"];
    r.x = pop;
    r.value = pop >= 0 ? fmt_num(pop) : "no data";
    std::string s;
    if (dens >= 0) s = fmt_num(dens) + " per km\xc2\xb2";
    if (val >= 0) s += (s.empty() ? "" : " \xc2\xb7 ") + fmt_rand(val) + " median";
    r.sub = s;
    r.colour = style::hex("#3ddc84").packed();
    rows.push_back(r);
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.x > b.x; });
  section(font_mono_, munis.empty() ? "LOADING MUNICIPALITIES ..."
                                    : (std::to_string(rows.size()) + " OF " + std::to_string(munis.size()) + " MUNICIPALITIES MATCH").c_str());
  rows_list(rows, "found", 0);
}

void App::ui_panel_list(bool rank) {
  if (!rank) {
    if (close_button()) {
      panel_ = Panel::None;
      return;
    }
    card_head("LIST", kAccent, "F4", "What's in view", "Everything on the map in this view, biggest first.", "");
    ImGui::Spacing();
  }
  struct Opt {
    const char* id;
    const char* name;
  };
  static const Opt opts[] = {{"subs", "Substations"}, {"plants", "Power stations"}, {"pop", "People"}, {"dens", "Density"}, {"val", "Value"}};
  if (list_layer_.empty()) list_layer_ = lstate_.shows("subs") ? "subs" : "pop";
  ImGui::PushFont(font_mono_);
  const float tw = (ImGui::GetContentRegionAvail().x - 16) / 5;
  for (size_t i = 0; i < 5; ++i) {
    if (i) ImGui::SameLine(0, 4);
    if (chip(opts[i].name, list_layer_ == opts[i].id, ImVec2(tw, 24))) list_layer_ = opts[i].id;
  }
  ImGui::PopFont();
  auto rows = rows_in_view(list_layer_);
  const bool fac = list_layer_ == "subs" || list_layer_ == "plants";
  section(font_mono_, (std::to_string(rows.size()) + (fac ? " DRAWN IN VIEW" : " MUNICIPALITIES IN VIEW")).c_str());
  if (fac && rows.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "Put the grid on the map (key 2) and zoom in to list substations and power stations.");
    ImGui::PopTextWrapPos();
  }
  rows_list(rows, "inview", 0);
}

void App::ui_panel_areas() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head("AREAS", kAccent, "PROVINCES AND MUNICIPALITIES", "Areas", "", "");
  const json* za = area_data("ZA");
  if (!za) {
    ImGui::TextColored(col(kMuted), "Loading ...");
    return;
  }
  ImGui::BeginChild("##areas");
  for (const auto& p : (*za)["children"]) {
    const std::string pid = p["id"], pname = p["name"];
    ImGui::PushStyleColor(ImGuiCol_Header, col(kHover));
    const bool open = ImGui::TreeNodeEx(pid.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s", pname.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) open_area(pid);
    if (open) {
      if (ImGui::Selectable(("Open " + pname + "##o" + pid).c_str())) open_area(pid);
      if (const json* pa = area_data(pid)) {
        for (const auto& m : (*pa)["children"]) {
          const std::string mid = m["id"], mname = m["name"];
          const ImVec2 q = ImGui::GetCursorScreenPos();
          if (ImGui::Selectable(("##" + mid).c_str(), false, 0, ImVec2(0, 22))) open_area(mid);
          auto* dl = ImGui::GetWindowDrawList();
          dl->AddText(ImVec2(q.x + 4, q.y + 2), u32(kInk), mname.c_str());
          if (m["metrics"].contains("pop_est")) {
            const std::string v = fmt_num(m["metrics"]["pop_est"].get<double>());
            const float vw = font_mono_->CalcTextSizeA(11.5f, FLT_MAX, 0, v.c_str()).x;
            dl->AddText(font_mono_, 11.5f, ImVec2(q.x + ImGui::GetContentRegionAvail().x - vw, q.y + 4), u32(kMuted), v.c_str());
          }
        }
      } else {
        ImGui::TextColored(col(kMuted), "Loading ...");
      }
      ImGui::TreePop();
    }
  }
  ImGui::EndChild();
}

void App::ui_panel_views() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head("VIEWS", kAccent, "F6 \xc2\xb7 CTRL S SAVES", "Saved views", "A view keeps the place, zoom, topic and layers.", "");
  ImGui::Spacing();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
  const bool enter = ImGui::InputTextWithHint("##vn", "Name this view", view_name_, sizeof view_name_, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine(0, 6);
  if (chip("Save", true, ImVec2(80, 0)) || enter) {
    save_view(view_name_);
    view_name_[0] = 0;
  }
  auto& v = settings_->data()["views"];
  if (!v.is_array()) v = json::array();
  section(font_mono_, (std::to_string(v.size()) + " SAVED").c_str());
  int remove = -1;
  for (size_t i = 0; i < v.size(); ++i) {
    const auto& e = v[i];
    ImGui::PushID(int(i));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable("##v", false, 0, ImVec2(w - 30, 38))) load_view(e.value("name", ""));
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(p.x + 2, p.y + 2), u32(kInk), e.value("name", "").c_str());
    char d[96];
    std::snprintf(d, sizeof d, "%s \xc2\xb7 z%.1f \xc2\xb7 %zu layers", e.value("topic", "start").c_str(), e.value("zoom", 5.0),
                  e.value("on", std::vector<std::string>{}).size());
    dl->AddText(font_small_, 11.5f, ImVec2(p.x + 2, p.y + 20), u32(kFaint), d);
    ImGui::SameLine(0, 4);
    const ImVec2 x0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##del", ImVec2(24, 38))) remove = int(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this view");
    icons::draw(dl, icons::Id::Close, ImVec2(x0.x + 12, x0.y + 19), 12, u32(kMuted), 1.3f);
    ImGui::PopID();
  }
  if (remove >= 0) {
    v.erase(size_t(remove));
    save_settings();
  }
  if (v.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "No saved views yet. Type SAVE name in the search box, or press Ctrl S.");
    ImGui::PopTextWrapPos();
  }
}

void App::ui_panel_watch() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head("WATCH", kAccent, "SHIFT W \xc2\xb7 * ADDS", "Watchlist", "Substations and power stations you keep an eye on.", "");
  auto& w = settings_->data()["watch"];
  if (!w.is_array()) w = json::array();
  section(font_mono_, (std::to_string(w.size()) + " WATCHED").c_str());
  int remove = -1;
  for (size_t i = 0; i < w.size(); ++i) {
    const auto& e = w[i];
    ImGui::PushID(int(i));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cw = ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable("##w", false, 0, ImVec2(cw - 30, 38))) {
      Card c;
      c.kind = Card::Facility;
      c.id = e.value("id", "");
      c.lon = e.value("lon", 0.0);
      c.lat = e.value("lat", 0.0);
      c.props = {{"name", e.value("name", "")}, {"kind", e.value("kind", "")}};
      if (e.value("kv", 0.0) > 0) c.props["kv"] = e["kv"];
      if (e.value("mw", 0.0) > 0) c.props["mw"] = e["mw"];
      cards_.push_back(c);
      panel_ = Panel::None;
      fly_to(c.lon, c.lat, std::max(cam_.zoom, 11.0));
    }
    auto* dl = ImGui::GetWindowDrawList();
    const bool st = e.value("kind", "") == "power_station";
    const std::string tag = st ? "PLANT" : "SUB";
    dl->AddText(font_mono_, 10.5f, ImVec2(p.x + 2, p.y + 4), st ? u32(kOk) : u32(kWarn), tag.c_str());
    dl->AddText(ImVec2(p.x + 46, p.y + 2), u32(kInk), e.value("name", "").c_str());
    char d[64];
    if (st) std::snprintf(d, sizeof d, "%.0f MW", e.value("mw", 0.0));
    else std::snprintf(d, sizeof d, "%.0f kV", e.value("kv", 0.0));
    dl->AddText(font_small_, 11.5f, ImVec2(p.x + 46, p.y + 20), u32(kFaint), d);
    ImGui::SameLine(0, 4);
    const ImVec2 x0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##del", ImVec2(24, 38))) remove = int(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop watching");
    icons::draw(dl, icons::Id::Close, ImVec2(x0.x + 12, x0.y + 19), 12, u32(kMuted), 1.3f);
    ImGui::PopID();
  }
  if (remove >= 0) {
    w.erase(size_t(remove));
    save_settings();
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(col(kFaint),
                     w.empty() ? "Nothing watched yet. Open a substation and press WATCH, or *."
                               : "Alarms on load shedding and outages for watched places come with live feeds (milestone F).");
  ImGui::PopTextWrapPos();
}

void App::ui_panel_keys() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head("KEYS", kAccent, "? OR F1", "Keys and commands", "", "");
  section(font_mono_, "KEYS");
  static const std::pair<const char*, const char*> keys[] = {
      {"1 to 0", "Topics: start, grid, vote, water, plots, land, places, move, nature, risk"},
      {"/ or Ctrl K", "Search, or type a command"},
      {"N / Shift N", "Find places / rank what is in view"},
      {"F2", "Add a layer"},
      {"F4", "List what is in view"},
      {"F6 / Ctrl S", "Saved views / save this view"},
      {"M or F7", "Measure"},
      {"W", "What is here (map centre); right-click anywhere"},
      {"Shift W / *", "Watchlist / watch this place"},
      {"[", "Hide or show the sidebar"},
      {"+ - Home", "Zoom in, out, whole country"},
      {"Arrows", "Pan"},
      {"Esc", "Close the top card or panel"},
  };
  for (const auto& [k, v] : keys) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddText(font_mono_, 11.5f, ImVec2(p.x, p.y + 3), u32(kAccent), k);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 100, p.y + 1));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kInk), "%s", v);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }
  section(font_mono_, "COMMANDS");
  for (const auto& s : commands::all()) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    const std::string code = std::string(s.code) + (s.arg[0] ? std::string(" ") + s.arg : "");
    dl->AddText(font_mono_, 11.5f, ImVec2(p.x, p.y + 3), s.ready ? u32(kAccent) : u32(kFaint), code.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x + 100, p.y + 1));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(s.ready ? col(kInk) : col(kFaint), "%s%s", s.about, s.ready ? "" : " (later)");
    ImGui::PopTextWrapPos();
  }
}

void App::ui_panel_later(const char* title, const char* what, const char* milestone) {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head(upper(title).c_str(), kAccent, "COMING", title, what, "");
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(col(kWarn), "This panel comes with milestone %s of the desktop build.", milestone);
  ImGui::PopTextWrapPos();
}

}  // namespace gsa
