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
  if (p.contains("layer")) {
    // a feature of a website layer: tag from its group, kind from the layer
    std::string lid = p["layer"].get<std::string>();
    if (lid == "parcel") {
      // a plot: the cadastre and, where a roll matched, its value (no owner details anywhere)
      const auto* g = layers::group("prop");
      const auto s = [&](const char* k) { return p.contains(k) ? prop_text(p[k]) : std::string(); };
      const auto rand = [&](const char* k) {
        return p.contains(k) && p[k].is_number() ? "R" + find::fmt_num(p[k].get<double>()) : std::string();
      };
      std::string sub = s("town");
      if (!s("muni").empty()) sub += (sub.empty() ? "" : " \xc2\xb7 ") + s("muni");
      card_head("PLOTS", g ? g->colour : kAccent, "PLOT", s("label").empty() ? "Plot" : s("label"), sub,
                "Cadastre: Surveyor-General; values: municipal valuation rolls");
      fn_strip({"HERE", "NEAR", "REACH", "MEAS", "GO", "COPY", "PNG"});
      const std::pair<const char*, std::string> rows[] = {
          {"Market value", rand("value")},  {"Roll", s("roll")},          {"Category", s("cat")},
          {"Use", s("use")},                {"Last sale", rand("sale")},  {"Sale date", s("sale_date")},
          {"Units on roll", s("units")},    {"Address", s("address")},    {"Zoning", s("zoning")},
          {"Scheme", s("scheme")},          {"Type", s("type")},
          {"Area", p.contains("area") && p["area"].is_number() ? find::fmt_num(p["area"].get<double>()) + " m\xc2\xb2" : ""},
          {"SG code", s("sg21")}};
      for (const auto& [k, v] : rows)
        if (!v.empty()) kv_row(k, v);
      if (!p.contains("value")) kv_row("Market value", "no roll matched");
      return;
    }
    const layers::Def* d = nullptr;
    for (const auto& x : layers::catalogue())
      if (layers::mvt_name(x.id) == lid) d = &x;
    if (lid == "wards") d = layers::find("wards");
    const auto* g = d ? layers::group(d->group) : nullptr;
    std::string tag = g ? upper(g->name) : "LAYER";
    if (tag.size() > 6) tag = tag.substr(0, 5);
    card_head(tag.c_str(), g ? g->colour : kAccent, d ? upper(d->name).c_str() : lid.c_str(), p.value("name", d ? d->name : "Feature"),
              p.value("detail", std::string()), d ? d->about : "");
    fn_strip({"HERE", "NEAR", "REACH", "MEAS", "GO", "COPY", "PNG"});
    char ll[64];
    std::snprintf(ll, sizeof ll, "%.5f, %.5f", c.lat, c.lon);
    kv_row("Position", ll);
    std::string prov;
    const std::string muni = muni_name_at(geo::mx(c.lon), geo::my(c.lat), &prov);
    if (!muni.empty()) kv_row("Municipality", muni);
    static const std::set<std::string> skip = {"name", "detail", "layer"};
    for (const auto& [k, v] : p.items()) {
      if (skip.count(k)) continue;
      kv_row(k.c_str(), prop_text(v));
    }
    return;
  }
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
  // the website's spot summary from the Find data: town, dam, free capacity, airport, port, your conditions
  if (fdata_) {
    const auto lines = find::spot(*fdata_, c.lon, c.lat, rules_);
    if (!lines.empty()) section(font_mono_, "AROUND THIS SPOT");
    for (const auto& [head, text] : lines) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(col(kInk), "%s", text.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PushFont(font_small_);
      ImGui::TextColored(col(kFaint), "%s", head.c_str());
      ImGui::PopFont();
      ImGui::Spacing();
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
  if (chip("Find places where ...", !find_rank_, ImVec2(tw, 26))) find_rank_ = false;
  ImGui::SameLine(0, 4);
  if (chip("Rank what's in view", find_rank_, ImVec2(tw, 26))) find_rank_ = true;
  ImGui::PopFont();
  ImGui::Spacing();
  auto copy_csv = [&](const std::vector<Row>& rows) {
    auto q = [](const std::string& v) {
      if (v.find_first_of("\",") == std::string::npos) return v;
      std::string o = "\"";
      for (char ch : v) o += ch == '"' ? std::string("\"\"") : std::string(1, ch);
      return o + "\"";
    };
    std::string csv = "name,value,detail\n";
    for (const auto& r : rows) csv += q(r.name) + "," + q(r.value) + "," + q(r.sub) + "\n";
    ImGui::SetClipboardText(csv.c_str());
    toast(std::to_string(rows.size()) + " rows copied as CSV");
  };

  if (find_rank_) {
    // Rank: the website's RANK list; substations and power stations come from the drawn grid
    const auto& defs = find::rank_defs();
    const find::RankDef* cur = &defs[0];
    for (const auto& d : defs)
      if (rank_key_ == d.k) cur = &d;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##rank", cur->label)) {
      for (const auto& d : defs)
        if (ImGui::Selectable(d.label, cur == &d)) {
          rank_key_ = d.k;
          if (opt_.screenshot.empty()) {
            settings_->data()["rank"] = rank_key_;
            settings_->save();
          }
        }
      ImGui::EndCombo();
    }
    std::vector<Row> rows;
    if (!cur->needs_data) {
      rows = rows_in_view(cur->k);
    } else if (fdata_) {
      double x0, y0, x1, y1;
      cam_.to_world(side_width(), 0, x0, y0);
      cam_.to_world(cam_.w, cam_.h - kStatusH, x1, y1);
      const double b[4] = {geo::lon_of(x0), geo::lat_of(y1), geo::lon_of(x1), geo::lat_of(y0)};
      for (const auto& r : find::rank(*fdata_, cur->k, b)) {
        Row w;
        w.name = r.name;
        w.value = r.value;
        w.sub = r.sub;
        w.x = cur->ascending ? 0 : r.x;  // no bars when lowest comes first
        w.colour = r.colour;
        w.lon = r.lon;
        w.lat = r.lat;
        w.area = r.area;
        rows.push_back(std::move(w));
      }
    }
    ImGui::PushFont(font_mono_);
    section(font_mono_, ("IN VIEW \xc2\xb7 " + std::to_string(rows.size())).c_str());
    if (!rows.empty() && chip("CSV", false, ImVec2(44, 20))) copy_csv(rows);
    ImGui::PopFont();
    if (cur->needs_data && !fdata_) ImGui::TextColored(col(kFaint), "Needs the Find data pack (find.json).");
    else if (rows.empty())
      ImGui::TextColored(col(kFaint), cur->needs_data ? "Nothing of this kind in view. Zoom out or pan."
                                                      : "Put the grid on the map (key 2) and zoom in to rank it.");
    if (!rows.empty()) rows_list(rows, "rank", -24);
    else ImGui::Dummy(ImVec2(0, std::max(0.0f, ImGui::GetContentRegionAvail().y - 24)));
    ImGui::TextColored(col(kFaint), "The list follows the map as you pan and zoom.");
    return;
  }

  if (!fdata_) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), fdata_req_.valid() ? "Loading the Find data ..."
                                                       : "Find needs the Find data pack (find.json in the packs folder).");
    ImGui::PopTextWrapPos();
    return;
  }
  const auto& d = *fdata_;
  {
    const std::string head = rules_.empty() ? "CONDITIONS FROM ANY TOPIC, PER MUNICIPALITY"
                                            : std::to_string(fpass_.size()) + " OF " + std::to_string(d.munis.size()) +
                                                  " MUNICIPALITIES MATCH";
    section(font_mono_, head.c_str());
  }
  // the conditions, each editable in place
  bool changed = false;
  int remove = -1;
  for (size_t i = 0; i < rules_.size(); ++i) {
    auto& r = rules_[i];
    const find::Def* def = find::def(r.k);
    ImGui::PushID(int(i));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + 58), u32(kHover), 6);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + 6));
    ImGui::TextColored(col(kInk), "%s", find::text(r).c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 26, p0.y + 4));
    if (ImGui::SmallButton("x")) remove = int(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove condition");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + 28));
    for (size_t fi = 0; fi < def->fields.size(); ++fi) {
      const auto& f = def->fields[fi];
      const std::string n = f.f;
      if (fi) ImGui::SameLine(0, 10);
      ImGui::PushID(f.f);
      if (n == "p") {
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##p", r.p.c_str())) {
          for (const auto& party : find::parties())
            if (ImGui::Selectable(party.c_str(), party == r.p)) {
              r.p = party;
              changed = true;
            }
          ImGui::EndCombo();
        }
      } else {
        double* v = n == "v" ? &r.v : n == "km" ? &r.km : n == "pct" ? &r.pct : &r.mw;
        ImGui::SetNextItemWidth(def->fields.size() > 1 ? 82.0f : 140.0f);
        if (ImGui::InputDouble("##v", v, 0, 0, "%.0f")) changed = true;
        *v = std::max(0.0, *v);
        ImGui::SameLine(0, 4);
        ImGui::TextColored(col(kFaint), "%s", f.unit);
      }
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 62));
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::PopID();
  }
  if (remove >= 0) {
    rules_.erase(rules_.begin() + remove);
    changed = true;
  }
  if (changed) set_rules(rules_);

  // add a condition, grouped like the website's select; or start from a preset
  const float hw = (ImGui::GetContentRegionAvail().x - 6) / 2;
  ImGui::SetNextItemWidth(hw);
  if (ImGui::BeginCombo("##add", "+ Add a condition")) {
    for (const char* g : {"People", "Property", "Elections", "Water", "Places", "Power", "Transport"}) {
      ImGui::TextColored(col(kFaint), "%s", g);
      for (const auto& def : find::defs())
        if (std::string(def.group) == g && ImGui::Selectable(def.label)) {
          auto r = rules_;
          r.push_back(find::make(def.k));
          set_rules(r);
        }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine(0, 6);
  ImGui::SetNextItemWidth(hw);
  if (ImGui::BeginCombo("##preset", "Start from ...")) {
    for (const auto& p : find::presets())
      if (ImGui::Selectable(p.name)) {
        set_rules(p.rules);
        toast(std::string(p.name) + ": " + std::to_string(fpass_.size()) + " municipalities match");
      }
    ImGui::EndCombo();
  }
  if (rules_.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kMuted), "Add a condition, or start from a preset such as Solar farm or Data centre. "
                                    "Municipalities that meet every condition stay lit and the rest go dark.");
    ImGui::PopTextWrapPos();
    return;
  }

  std::vector<Row> rows;
  for (const auto& m : d.munis) {
    if (!fpass_.count(m.code)) continue;
    Row r;
    r.name = m.name;
    r.area = m.code;
    r.x = std::max(0.0, m.pop);
    r.value = find::fmt_num(m.pop);
    r.sub = m.prov + " \xc2\xb7 " + find::why(d, m, rules_);
    r.colour = u32(kAccent);
    rows.push_back(std::move(r));
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.x > b.x; });
  ImGui::PushFont(font_mono_);
  section(font_mono_, "MUNICIPALITIES THAT MATCH \xc2\xb7 BY PEOPLE");
  if (!rows.empty() && chip("CSV", false, ImVec2(44, 20))) copy_csv(rows);
  ImGui::SameLine(0, 4);
  if (chip("Clear", false, ImVec2(56, 20))) {
    set_rules({});
    ImGui::PopFont();
    return;
  }
  ImGui::PopFont();
  if (rows.empty()) ImGui::TextColored(col(kFaint), "No municipality meets every condition. Loosen one.");
  else rows_list(rows, "found", -40);
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(col(kFaint), "Distances run from the municipal boundary. Click a municipality to fly there, then "
                                  "right-click a spot to check it exactly.");
  ImGui::PopTextWrapPos();
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
    // in view or off screen, as on the website
    {
      double sx, sy;
      cam_.to_screen(geo::mx(e.value("lon", 0.0)), geo::my(e.value("lat", 0.0)), sx, sy);
      const bool iv = sx >= side_width() && sx <= cam_.w && sy >= 0 && sy <= cam_.h - kStatusH;
      const ImVec2 dc(p.x + cw - 40, p.y + 19);
      if (iv) dl->AddCircleFilled(dc, 4, u32(kAccent));
      else dl->AddCircle(dc, 4, u32(kFaint), 0, 1.2f);
    }
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
  if (!w.empty()) {
    ImGui::Spacing();
    ImGui::PushFont(font_mono_);
    if (chip("Fit all", false, ImVec2(80, 26))) {
      double x0 = 180, y0 = 90, x1 = -180, y1 = -90;
      for (const auto& e : w) {
        x0 = std::min(x0, e.value("lon", 0.0));
        x1 = std::max(x1, e.value("lon", 0.0));
        y0 = std::min(y0, e.value("lat", 0.0));
        y1 = std::max(y1, e.value("lat", 0.0));
      }
      fly_to_bbox(x0 - 0.05, y0 - 0.05, x1 + 0.05, y1 + 0.05);
    }
    ImGui::SameLine(0, 4);
    if (chip("Export CSV", false, ImVec2(100, 26))) {
      std::string csv = "name,kind,kv,mw,lat,lon\n";
      for (const auto& e : w) {
        std::string n = e.value("name", "");
        if (n.find_first_of(",\"") != std::string::npos) {
          std::string q = "\"";
          for (char ch : n) q += ch == '"' ? std::string("\"\"") : std::string(1, ch);
          n = q + "\"";
        }
        char b[160];
        std::snprintf(b, sizeof b, ",%s,%.0f,%.0f,%.5f,%.5f\n", e.value("kind", "").c_str(), e.value("kv", 0.0),
                      e.value("mw", 0.0), e.value("lat", 0.0), e.value("lon", 0.0));
        csv += n + b;
      }
      ImGui::SetClipboardText(csv.c_str());
      toast(std::to_string(w.size()) + " watched facilities copied as CSV");
    }
    ImGui::PopFont();
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


// ------------------------------------------------------------------ filter (the website's drawFilters)

void App::ui_panel_filter() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head("FILTER", kAccent, "F3", "Filters", "Dim or hide what does not match.", "");
  ImGui::PushFont(font_mono_);
  const float tw = (ImGui::GetContentRegionAvail().x - 4) / 2;
  if (chip("Power and grid", !filter_vote_tab_, ImVec2(tw, 26))) filter_vote_tab_ = false;
  ImGui::SameLine(0, 4);
  if (chip("Elections", filter_vote_tab_, ImVec2(tw, 26))) filter_vote_tab_ = true;
  ImGui::PopFont();

  layers::Filters f = filter_;
  bool changed = false;
  // one group of toggle chips, with "Any" to clear it
  auto group = [&](const char* title, auto& set, const auto& values, auto label, auto swatch) {
    ImGui::Spacing();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    section(font_mono_, title);
    if (!set.empty()) {
      const ImVec2 after = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetContentRegionAvail().x - 40, p.y + 2));
      ImGui::PushID(title);
      if (ImGui::SmallButton("Any")) {
        set.clear();
        changed = true;
      }
      ImGui::PopID();
      ImGui::SetCursorScreenPos(after);
    }
    ImGui::PushFont(font_mono_);
    float x = 0;
    const float avail = ImGui::GetContentRegionAvail().x;
    for (const auto& v : values) {
      const std::string l = label(v);
      const uint32_t sw = swatch(v);
      const float bw = ImGui::CalcTextSize(l.c_str()).x + (sw ? 30.0f : 16.0f);
      if (x > 0 && x + bw > avail) x = 0;
      else if (x > 0) ImGui::SameLine(0, 4);
      x += bw + 4;
      const bool on = set.count(v) != 0;
      ImGui::PushID(l.c_str());
      const ImVec2 c0 = ImGui::GetCursorScreenPos();
      if (chip(sw ? ("   " + l).c_str() : l.c_str(), on, ImVec2(bw, 24))) {
        if (on) set.erase(v);
        else set.insert(v);
        changed = true;
      }
      if (sw) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(c0.x + 8, c0.y + 8), ImVec2(c0.x + 16, c0.y + 16), sw, 2);
      ImGui::PopID();
    }
    ImGui::PopFont();
  };
  auto none = [](const auto&) { return uint32_t(0); };
  if (!filter_vote_tab_) {
    const std::vector<int> vb = {0, 1, 2, 3, 4, 5};
    static const char* const vcol[6] = {"#b0207e", "#e0302d", "#ef7f1a", "#d9b310", "#2aa36b", "#8a97a3"};
    group("LINE VOLTAGE (KV)", f.volt, vb, [](int i) { return std::string(layers::kVoltBins[i]); },
          [](int i) { return u32(vcol[i]); });
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "Applies to power lines and substations. Untagged means OpenStreetMap has no voltage for it.");
    ImGui::PopTextWrapPos();
    std::vector<std::string> fk;
    for (const auto& [k, l] : layers::fuels()) fk.push_back(k);
    group("POWER STATIONS BY FUEL", f.fuel, fk,
          [](const std::string& k) {
            for (const auto& [key, l] : layers::fuels())
              if (key == k) return l;
            return k;
          },
          [](const std::string& k) { return u32(style::plant(k)); });
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "Most wind and many solar farms are mapped as areas rather than single points, so few show here.");
    ImGui::PopTextWrapPos();
  } else {
    group("WINNING PARTY, 2026", f.party, layers::top_parties(),
          [](const std::string& p) { return p == "other" ? std::string("Other") : p; },
          [](const std::string& p) { return u32(style::party(p == "other" ? "" : p, 1.0f)); });
    const std::vector<int> tb = {0, 1, 2, 3, 4};
    group("TURNOUT", f.turn, tb, [](int i) { return std::string(layers::kTurnBins[i]); }, none);
    std::vector<std::string> pv(std::begin(layers::kProvinces), std::end(layers::kProvinces));
    group("PROVINCE", f.prov, pv, [](const std::string& p) { return p; }, none);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "Filters the 2026 ward winners (Votes, key 3). Winning margins need the full ward results pack.");
    ImGui::PopTextWrapPos();
    if (!lstate_.shows("c:ward")) {
      ImGui::Spacing();
      if (chip("Show ward winners", false, ImVec2(160, 24))) {
        lstate_.add("c:ward");
        apply_layers();
      }
    }
  }
  ImGui::Spacing();
  if (ImGui::Checkbox("Hide what's filtered out (instead of dimming it)", &f.hide)) changed = true;
  if (changed) set_filter(f);

  // summary of what is active
  std::vector<std::string> parts;
  auto join_set = [](const auto& set, auto label) {
    std::string s;
    for (const auto& v : set) s += (s.empty() ? "" : ", ") + label(v);
    return s;
  };
  if (!filter_.volt.empty()) parts.push_back("lines " + join_set(filter_.volt, [](int i) { return std::string(layers::kVoltBins[i]); }));
  if (!filter_.fuel.empty()) parts.push_back("stations " + join_set(filter_.fuel, [](const std::string& s) { return s; }));
  if (!filter_.party.empty()) parts.push_back("won by " + join_set(filter_.party, [](const std::string& s) { return s; }));
  if (!filter_.turn.empty()) parts.push_back("turnout " + join_set(filter_.turn, [](int i) { return std::string(layers::kTurnBins[i]); }));
  if (!filter_.prov.empty()) parts.push_back("in " + join_set(filter_.prov, [](const std::string& s) { return s; }));
  section(font_mono_, "FILTER RESULT");
  ImGui::PushTextWrapPos(0);
  if (parts.empty()) ImGui::TextColored(col(kMuted), "Nothing filtered. Pick chips above.");
  else {
    std::string s;
    for (const auto& p : parts) s += (s.empty() ? "" : "; ") + p;
    ImGui::TextColored(col(kInk), "%s", s.c_str());
    if (chip("Clear all filters", false, ImVec2(150, 24))) {
      layers::Filters cleared;
      cleared.hide = filter_.hide;
      set_filter(cleared);
    }
  }
  ImGui::PopTextWrapPos();
}

}  // namespace gsa
