// Live planes and ships from the back-end's /v1/live (milestone F): positions polled with ETags,
// trails from the back-end's position history for the open item and every watched one, a Watch
// button that keeps an item's history long term, and the Live panel.
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "app.hpp"
#include "ui_theme.hpp"

namespace gsa {

using nlohmann::json;
using namespace ui;

namespace {

double wall_now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string ago(double secs) {
  if (secs < 0) secs = 0;
  if (secs < 90) return std::to_string(int(secs)) + " s ago";
  if (secs < 5400) return std::to_string(int(secs / 60)) + " min ago";
  if (secs < 172800) return std::to_string(int(secs / 3600)) + " h ago";
  return std::to_string(int(secs / 86400)) + " days ago";
}

double num(const json& j, const char* k, double def = -1) {
  auto it = j.find(k);
  return it != j.end() && it->is_number() ? it->get<double>() : def;
}

// AIS ship type groups (ITU-R M.1371): colour and name
std::pair<const char*, const char*> ship_class(int t) {
  if (t >= 70 && t <= 79) return {"#3ddc84", "cargo"};
  if (t >= 80 && t <= 89) return {"#ff5d5d", "tanker"};
  if (t >= 60 && t <= 69) return {"#5aa9ff", "passenger"};
  if (t == 30) return {"#ffb020", "fishing"};
  if (t == 31 || t == 32 || t == 52) return {"#c98bd0", "tug"};
  if (t == 36 || t == 37) return {"#e6edf3", "sailing or pleasure"};
  return {"#9fb3c8", "other"};
}

const char* plane_colour(const json& extra, double alt) {
  if (extra.value("gnd", false) || (alt >= 0 && alt < 60)) return "#8aa0ae";
  if (alt < 0) return "#e6edf3";
  if (alt < 3000) return "#3ddc84";
  if (alt < 9000) return "#7df9ff";
  return "#f2f7fa";
}

double fix_km(const std::array<double, 6>& a, const std::array<double, 6>& b) {
  const double r = geo::kPi / 180, x = (b[1] - a[1]) * r * std::cos((a[2] + b[2]) / 2 * r), y = (b[2] - a[2]) * r;
  return 6371.0 * std::hypot(x, y);
}

}  // namespace

std::future<api::Response> App::live_get(const std::string& path, const std::string& etag) {
  const auto ep = opt_.endpoint;
  return std::async(std::launch::async, [ep, path, etag]() {
    api::Client c(ep);
    return c.get(path, etag);
  });
}

void App::live_send(const std::string& method, const std::string& path, const std::string& body) {
  const auto ep = opt_.endpoint;
  live_sends_.push_back(std::async(std::launch::async, [ep, method, path, body]() {
    api::Client c(ep);
    return c.send(method, path, body);
  }));
}

std::string App::live_key(const std::string& kind, const std::string& id) { return kind + "/" + id; }

const App::LiveItem* App::live_find(const std::string& kind, const std::string& id) const {
  const auto& f = kind == "plane" ? planes_ : ships_;
  for (const auto& it : f.items)
    if (it.id == id) return &it;
  for (const auto& it : watched_)
    if (it.kind == kind && it.id == id) return &it;
  return nullptr;
}

bool App::live_is_watched(const std::string& kind, const std::string& id) const {
  return std::any_of(watched_.begin(), watched_.end(), [&](const LiveItem& w) { return w.kind == kind && w.id == id; });
}

void App::live_set(bool planes, bool ships) {
  const bool was = planes_.on;
  planes_.on = planes;
  ships_.on = ships;
  planes_.next = ships_.next = 0;
  if (planes != was) live_send("POST", "/v1/live/poller", planes ? R"({"on":true})" : R"({"on":false})");
  status_next_ = 0;
  if (opt_.screenshot.empty()) {
    settings_->data()["live"] = json{{"planes", planes}, {"ships", ships}};
    settings_->save();
  }
}

void App::live_watch(const std::string& kind, const std::string& id, bool on, const std::string& label) {
  const std::string path = "/v1/live/watch/" + kind + "/" + api::url_encode(id);
  if (on) {
    live_send("PUT", path, json{{"label", label}}.dump());
    if (const auto* it = live_find(kind, id); it && !live_is_watched(kind, id)) {
      LiveItem w = *it;
      w.watched = true;
      watched_.push_back(w);
    }
    toast("Watching " + (label.empty() ? id : label) + ": its track is kept for a year");
  } else {
    live_send("DELETE", path);
    watched_.erase(std::remove_if(watched_.begin(), watched_.end(), [&](const LiveItem& w) { return w.kind == kind && w.id == id; }),
                   watched_.end());
    toast("No longer watching " + (label.empty() ? id : label));
  }
  watch_next_ = glfwGetTime() + 1.0;
}

void App::live_poll() {
  const double t = glfwGetTime();
  // drop finished sends
  live_sends_.erase(std::remove_if(live_sends_.begin(), live_sends_.end(),
                                   [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
                    live_sends_.end());
  // latest positions for the view (bbox snapped to half degrees so pans reuse the cached answer)
  for (LiveFeed* f : {&planes_, &ships_}) {
    const bool plane = f == &planes_;
    if (f->req.valid() && f->req.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      const auto r = f->req.get();
      if (r.status == 200) {
        const auto j = json::parse(r.body, nullptr, false);
        if (j.is_object()) {
          f->items.clear();
          for (const auto& x : j.value("items", json::array())) {
            LiveItem it;
            it.kind = plane ? "plane" : "ship";
            it.id = x.value("id", "");
            it.name = x.value("name", "");
            it.lon = num(x, "lon", 0);
            it.lat = num(x, "lat", 0);
            it.alt = num(x, "alt");
            it.spd = num(x, "spd");
            it.hdg = num(x, "hdg");
            it.t = num(x, "t", 0);
            it.watched = x.value("watched", false);
            for (auto e = x.begin(); e != x.end(); ++e)
              if (!std::set<std::string>{"id", "name", "lon", "lat", "alt", "spd", "hdg", "t", "watched", "src"}.count(e.key()))
                it.extra[e.key()] = e.value();
            if (x.contains("src")) it.extra["src"] = x["src"];
            if (!it.id.empty()) f->items.push_back(std::move(it));
          }
          f->sources = j.value("sources", json::array());
          f->etag = r.etag;
          f->note.clear();
          f->fetched = wall_now();
        }
      } else if (r.status == 304) {
        f->note.clear();
      } else if (r.status == 503) {
        f->note = "The back-end runs without live feeds (start it with --live).";
      } else if (r.status != 0) {
        f->note = "Live " + std::string(plane ? "planes" : "ships") + ": HTTP " + std::to_string(r.status);
      } else {
        f->note = "No answer from the back-end: " + r.error;
      }
    }
    if (!f->on || f->req.valid() || t < f->next) continue;
    double x0, y0, x1, y1;
    cam_.to_world(side_width(), 0, x0, y0);
    cam_.to_world(cam_.w, cam_.h, x1, y1);
    const double lon0 = std::floor(geo::lon_of(x0) * 2) / 2 - 0.5, lon1 = std::ceil(geo::lon_of(x1) * 2) / 2 + 0.5;
    const double lat0 = std::floor(geo::lat_of(y1) * 2) / 2 - 0.5, lat1 = std::ceil(geo::lat_of(y0) * 2) / 2 + 0.5;
    char q[160];
    std::snprintf(q, sizeof q, "/v1/live/%s?bbox=%.1f,%.1f,%.1f,%.1f&max_age=%d", plane ? "planes" : "ships",
                  std::max(-180.0, lon0), std::max(-85.0, lat0), std::min(180.0, lon1), std::min(85.0, lat1),
                  plane ? 300 : 3600);
    if (q != f->query) f->etag.clear();
    f->query = q;
    f->req = live_get(q, f->etag);
    f->next = t + (plane ? 10 : 20);
  }
  // the watch list
  if (watch_req_.valid() && watch_req_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto r = watch_req_.get();
    if (r.status == 200) {
      const auto j = json::parse(r.body, nullptr, false);
      const json& arr = j.is_array() ? j : j.value("items", json::array());
      watched_.clear();
      for (const auto& x : arr) {
        LiveItem it;
        it.kind = x.value("kind", "");
        it.id = x.value("id", "");
        const json& last = x.contains("last") && x["last"].is_object() ? x["last"] : x;
        it.name = x.value("label", std::string());
        if (it.name.empty()) it.name = last.value("name", std::string());
        it.lon = num(last, "lon", 0);
        it.lat = num(last, "lat", 0);
        it.alt = num(last, "alt");
        it.spd = num(last, "spd");
        it.hdg = num(last, "hdg");
        it.t = num(last, "t", 0);
        it.watched = true;
        if (!it.id.empty()) watched_.push_back(std::move(it));
      }
    }
  }
  if ((planes_.on || ships_.on || panel_ == Panel::Live) && !watch_req_.valid() && t >= watch_next_) {
    watch_req_ = live_get("/v1/live/watch");
    watch_next_ = t + 60;
  }
  // back-end status for the Live panel
  if (status_req_.valid() && status_req_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto r = status_req_.get();
    live_status_ = r.status == 200 ? json::parse(r.body, nullptr, false) : json{{"error", r.status ? std::to_string(r.status) : r.error}};
  }
  if (panel_ == Panel::Live && !status_req_.valid() && t >= status_next_) {
    status_req_ = live_get("/v1/live/status");
    status_next_ = t + 15;
  }
  // trails: the open live card and every watched item
  std::set<std::string> want;
  if (const Card& c = cards_.back(); c.kind == Card::Facility && c.props.value("layer", "") == "live")
    want.insert(live_key(c.props.value("kind", ""), c.props.value("id", "")));
  for (const auto& k : kept_)
    if (k.card.kind == Card::Facility && k.card.props.value("layer", "") == "live")
      want.insert(live_key(k.card.props.value("kind", ""), k.card.props.value("id", "")));
  if (planes_.on || ships_.on)
    for (const auto& w : watched_) want.insert(live_key(w.kind, w.id));
  for (auto it = tracks_.begin(); it != tracks_.end();) {
    if (!want.count(it->first) && !(it->second.req.valid())) it = tracks_.erase(it);
    else ++it;
  }
  for (const auto& key : want) {
    auto& tr = tracks_[key];
    if (tr.req.valid() && tr.req.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      const auto r = tr.req.get();
      if (r.status == 200) {
        const auto j = json::parse(r.body, nullptr, false);
        if (j.is_object()) {
          tr.fixes.clear();
          for (const auto& f : j.value("fixes", json::array())) {
            if (!f.is_array() || f.size() < 3) continue;
            std::array<double, 6> a{};
            for (size_t i = 0; i < 6; ++i) a[i] = i < f.size() && f[i].is_number() ? f[i].get<double>() : -1;
            tr.fixes.push_back(a);
          }
          tr.name = j.value("name", "");
        }
      }
    }
    if (!tr.req.valid() && t >= tr.next) {
      const auto slash = key.find('/');
      char q[200];
      std::snprintf(q, sizeof q, "/v1/live/%s/%s/track?since=%.0f&limit=3000", key.substr(0, slash).c_str(),
                    api::url_encode(key.substr(slash + 1)).c_str(), wall_now() - 86400);
      tr.req = live_get(q);
      tr.next = t + 30;
    }
  }
}

const App::LiveItem* App::live_pick(double sx, double sy) const {
  const LiveItem* best = nullptr;
  double bd = 10;
  for (const LiveFeed* f : {&planes_, &ships_}) {
    if (!f->on) continue;
    for (const auto& it : f->items) {
      double px, py;
      cam_.to_screen(geo::mx(it.lon), geo::my(it.lat), px, py);
      const double d = std::hypot(px - sx, py - sy);
      if (d < bd) bd = d, best = &it;
    }
  }
  return best;
}

void App::open_live(const LiveItem& it) {
  Card c;
  c.kind = Card::Facility;
  c.lon = it.lon;
  c.lat = it.lat;
  c.id = live_key(it.kind, it.id);
  c.props = {{"layer", "live"}, {"kind", it.kind}, {"id", it.id}, {"name", it.name.empty() ? it.id : it.name},
             {"tag", it.kind == "plane" ? "PLANE" : "SHIP"}};
  if (cards_.back().kind == Card::Facility && cards_.back().props.value("layer", "") == "live") cards_.back() = c;
  else cards_.push_back(c);
  panel_ = Panel::None;
  side_open_ = true;
  tracks_[c.id].next = 0;
}

void App::live_draw() {
  if (!planes_.on && !ships_.on) return;
  auto* dl = ImGui::GetBackgroundDrawList();
  const float x0 = side_width();
  dl->PushClipRect(ImVec2(x0, 0), ImVec2(float(cam_.w), float(cam_.h) - kStatusH), true);
  const double now = wall_now();
  // trails, oldest faintest
  for (const auto& [key, tr] : tracks_) {
    if (tr.fixes.size() < 2) continue;
    const bool plane = key.rfind("plane/", 0) == 0;
    if ((plane && !planes_.on) || (!plane && !ships_.on)) continue;
    const auto c = style::hex(plane ? kAccent : kWarn);
    const double t0 = tr.fixes.front()[0], span = std::max(1.0, now - t0);
    double px = 0, py = 0;
    for (size_t i = 0; i < tr.fixes.size(); ++i) {
      const auto& f = tr.fixes[i];
      double sx, sy;
      cam_.to_screen(geo::mx(f[1]), geo::my(f[2]), sx, sy);
      // a gap of more than 20 minutes (planes) or 3 hours (ships) breaks the line
      const bool gap = i > 0 && f[0] - tr.fixes[i - 1][0] > (plane ? 1200 : 10800);
      if (i > 0 && !gap) {
        const float a = float(0.18 + 0.75 * std::clamp((f[0] - t0) / span, 0.0, 1.0));
        dl->AddLine(ImVec2(float(px), float(py)), ImVec2(float(sx), float(sy)), IM_COL32(c.r, c.g, c.b, int(a * 255)), 2.0f);
      }
      px = sx, py = sy;
    }
  }
  // name everything once zoomed in, or sooner when only a few are about
  const bool labels_all = cam_.zoom >= 9.5 || (cam_.zoom >= 7.5 && planes_.items.size() + ships_.items.size() < 60);
  auto label = [&](float x, float y, const std::string& s, ImU32 colour, bool ring) {
    const float dx = ring ? 14.0f : 8.0f;
    dl->AddText(font_mono_, 11, ImVec2(x + dx + 1, y - 7), IM_COL32(4, 10, 14, 220), s.c_str());
    dl->AddText(font_mono_, 11, ImVec2(x + dx, y - 8), colour, s.c_str());
  };
  std::string sel_kind, sel_id;
  if (const Card& c = cards_.back(); c.kind == Card::Facility && c.props.value("layer", "") == "live")
    sel_kind = c.props.value("kind", ""), sel_id = c.props.value("id", "");
  for (const LiveFeed* f : {&ships_, &planes_}) {
    if (!f->on) continue;
    for (const auto& it : f->items) {
      double sx, sy;
      cam_.to_screen(geo::mx(it.lon), geo::my(it.lat), sx, sy);
      if (sx < x0 - 20 || sx > cam_.w + 20 || sy < -20 || sy > cam_.h + 20) continue;
      const float x = float(sx), y = float(sy);
      const double h = (it.hdg >= 0 ? it.hdg : 0) * geo::kPi / 180, s = std::sin(h), c = -std::cos(h);
      auto P = [&](double fwd, double side) { return ImVec2(float(x + s * fwd - c * side), float(y + c * fwd + s * side)); };
      const bool watched = it.watched || live_is_watched(it.kind, it.id);
      const bool sel = it.kind == sel_kind && it.id == sel_id;
      const bool stale = now - it.t > (it.kind == "plane" ? 120 : 1800);
      ImU32 col32;
      if (it.kind == "plane") {
        const auto cc = style::hex(plane_colour(it.extra, it.alt));
        col32 = IM_COL32(cc.r, cc.g, cc.b, stale ? 120 : 255);
        const ImVec2 pts[4] = {P(8, 0), P(-6, 5.5), P(-3, 0), P(-6, -5.5)};
        dl->AddConvexPolyFilled(pts, 4, IM_COL32(4, 10, 14, 200));
        const ImVec2 in[4] = {P(6.5, 0), P(-4.8, 4.3), P(-2.3, 0), P(-4.8, -4.3)};
        dl->AddConvexPolyFilled(in, 4, col32);
      } else {
        const int type = int(num(it.extra, "type", 0));
        const auto cc = style::hex(ship_class(type).first);
        col32 = IM_COL32(cc.r, cc.g, cc.b, stale ? 120 : 255);
        const bool moving = it.spd > 0.5;
        if (moving && it.hdg >= 0) {
          const ImVec2 pts[5] = {P(7, 0), P(2, 3.5), P(-5, 3.5), P(-5, -3.5), P(2, -3.5)};
          dl->AddConvexPolyFilled(pts, 5, col32);
          dl->AddPolyline(pts, 5, IM_COL32(4, 10, 14, 220), ImDrawFlags_Closed, 1.0f);
        } else {
          dl->AddCircleFilled(ImVec2(x, y), 3.5f, col32);
          dl->AddCircle(ImVec2(x, y), 3.5f, IM_COL32(4, 10, 14, 220), 0, 1.0f);
        }
      }
      if (watched || sel) dl->AddCircle(ImVec2(x, y), 11, sel ? u32(kAccent) : IM_COL32(255, 176, 32, 220), 0, 1.6f);
      if (sel || watched || (labels_all && f->items.size() < 400)) {
        std::string l = it.name.empty() ? it.id : it.name;
        if (it.kind == "plane" && it.alt >= 0 && (sel || watched)) {
          char fl[16];
          std::snprintf(fl, sizeof fl, " FL%03d", int(std::lround(it.alt * 3.28084 / 100)));
          l += fl;
        }
        label(x, y, l, sel ? u32(kAccent) : watched ? u32(kWarn) : col32, sel || watched);
      }
    }
  }
  dl->PopClipRect();
}

void App::ui_live_card(Card& c) {
  const std::string kind = c.props.value("kind", ""), id = c.props.value("id", "");
  const LiveItem* it = live_find(kind, id);
  if (it) c.lon = it->lon, c.lat = it->lat;  // the card follows the item
  const std::string name = it && !it->name.empty() ? it->name : c.props.value("name", id);
  const bool plane = kind == "plane";
  const bool watched = live_is_watched(kind, id);
  std::string sub = plane ? "Aircraft" : "Vessel";
  if (it) {
    if (plane && it->extra.contains("type")) sub += " \xc2\xb7 " + it->extra["type"].get<std::string>();
    if (!plane) sub += std::string(" \xc2\xb7 ") + ship_class(int(num(it->extra, "type", 0))).second;
  }
  card_head(plane ? "PLANE" : "SHIP", plane ? kAccent : kWarn, watched ? "LIVE \xc2\xb7 WATCHED" : "LIVE", name, sub, "");
  if (!in_kept_) {
    ImGui::PushFont(font_mono_);
    if (chip(watched ? "Watching \xc2\xb7 stop" : "Watch", watched, ImVec2(130, 26))) live_watch(kind, id, !watched, name);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(watched ? "Stop keeping this track" : "Keep this one's track for a year and draw its trail whenever live is on");
    ImGui::SameLine(0, 4);
    if (chip("Follow", false, ImVec2(70, 26)) && it) fly_to(it->lon, it->lat, std::max(cam_.zoom, plane ? 9.0 : 11.0));
    ImGui::PopFont();
  }
  ImGui::Spacing();
  auto kv = [&](const char* k, const std::string& v) {
    if (v.empty()) return;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddText(font_small_, 12.5f, ImVec2(p.x, p.y + 3), u32(kMuted), k);
    const ImVec2 vs = font_mono_->CalcTextSizeA(12, FLT_MAX, 0, v.c_str());
    dl->AddText(font_mono_, 12, ImVec2(p.x + w - vs.x, p.y + 3), u32(kInk), v.c_str());
    ImGui::Dummy(ImVec2(w, 21));
  };
  char b[96];
  if (it) {
    const double now = wall_now();
    kv("Last seen", ago(now - it->t));
    std::snprintf(b, sizeof b, "%.4f, %.4f", it->lat, it->lon);
    kv("Position", b);
    if (plane) {
      if (it->extra.value("gnd", false)) kv("Altitude", "on the ground");
      else if (it->alt >= 0) {
        std::snprintf(b, sizeof b, "%s m (FL%03d)", find::fmt_num(it->alt).c_str(), int(std::lround(it->alt * 3.28084 / 100)));
        kv("Altitude", b);
      }
      if (it->spd >= 0) kv("Ground speed", find::fmt_num(it->spd * 3.6) + " km/h (" + find::fmt_num(it->spd * 1.94384) + " kt)");
      if (const double vr = num(it->extra, "vr", -9999); vr > -9999) kv("Climb", find::fmt_num(vr) + " m/s");
    } else {
      if (it->spd >= 0) {
        std::snprintf(b, sizeof b, "%.1f kn", it->spd * 1.94384);
        kv("Speed", b);
      }
      if (it->extra.contains("dest") && it->extra["dest"].is_string()) kv("Destination", it->extra["dest"].get<std::string>());
      if (const double l = num(it->extra, "length"); l > 0) kv("Length", find::fmt_num(l) + " m");
      if (it->extra.contains("nav") && it->extra["nav"].is_string()) kv("Status", it->extra["nav"].get<std::string>());
    }
    if (it->hdg >= 0) kv("Heading", std::to_string(int(std::lround(it->hdg))) + "\xc2\xb0");
    for (const char* k : {"reg", "callsign", "imo"})
      if (it->extra.contains(k) && (it->extra[k].is_string() || it->extra[k].is_number()))
        kv(k == std::string("reg") ? "Registration" : k == std::string("imo") ? "IMO" : "Call sign",
           it->extra[k].is_string() ? it->extra[k].get<std::string>() : it->extra[k].dump());
    kv(plane ? "ICAO address" : "MMSI", id);
    if (it->extra.contains("src") && it->extra["src"].is_string()) kv("Source", it->extra["src"].get<std::string>());
  } else {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kMuted), (planes_.on || ships_.on) ? "Not in the latest feed for this view. Its trail shows what the "
                                                                 "back-end last heard."
                                                               : "Turn on live planes or ships (LIVE) to see where it is now.");
    ImGui::PopTextWrapPos();
  }
  // the trail
  section(font_mono_, "TRACK, LAST 24 HOURS");
  auto tit = tracks_.find(live_key(kind, id));
  if (tit == tracks_.end() || tit->second.fixes.empty()) {
    ImGui::TextColored(col(kFaint), "Loading the track ...");
  } else {
    const auto& fx = tit->second.fixes;
    double km = 0;
    for (size_t i = 1; i < fx.size(); ++i) km += fix_km(fx[i - 1], fx[i]);
    kv("Positions held", find::fmt_num(double(fx.size())));
    kv("First of them", ago(wall_now() - fx.front()[0]));
    kv("Distance covered", find::fmt_km(km));
    if (plane) {
      double top = -1;
      for (const auto& f : fx) top = std::max(top, f[3]);
      if (top > 0) kv("Highest", find::fmt_num(top) + " m");
    }
    ImGui::PushTextWrapPos(0);
    ImGui::PushFont(font_small_);
    ImGui::TextColored(col(kFaint), watched ? "Watched: the back-end keeps this track for a year."
                                            : "Unwatched tracks are kept for 24 hours. Watch it to keep its history.");
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
  }
  fn_strip({"KEEP", "NEAR", "MEAS", "COPY", "PNG"});
}

void App::ui_panel_live() {
  if (close_button()) {
    panel_ = Panel::None;
    return;
  }
  card_head("LIVE", kAccent, "F5", "Live planes and ships", "Positions through your back-end; trails from its history.", "");
  ImGui::PushFont(font_mono_);
  const float bw = (ImGui::GetContentRegionAvail().x - 4) / 2;
  bool p = planes_.on, s = ships_.on;
  if (chip(p ? "Planes on" : "Planes", p, ImVec2(bw, 28))) p = !p;
  ImGui::SameLine(0, 4);
  if (chip(s ? "Ships on" : "Ships", s, ImVec2(bw, 28))) s = !s;
  ImGui::PopFont();
  if (p != planes_.on || s != ships_.on) live_set(p, s);
  // what the back-end says
  section(font_mono_, "FEEDS");
  auto line = [&](const char* k, const std::string& v, const char* c = kInk) {
    ImGui::TextColored(col(kMuted), "%s", k);
    ImGui::SameLine(110);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(c), "%s", v.c_str());
    ImGui::PopTextWrapPos();
  };
  if (live_status_.is_object() && live_status_.contains("error")) {
    line("Back-end", "no answer (" + live_status_["error"].dump() + ")", kWarn);
  } else if (live_status_.is_object() && !live_status_.value("enabled", false)) {
    line("Back-end", "live feeds off (needs --live)", kWarn);
  } else if (live_status_.is_object() && live_status_.contains("planes")) {
    const auto& pl = live_status_["planes"];
    const bool run = pl.value("running", false);
    std::string v = run ? "polling every " + find::fmt_num(pl.value("interval_s", 20.0)) + " s" : "off";
    if (pl.contains("source") && pl["source"].is_string()) v += " \xc2\xb7 " + pl["source"].get<std::string>();
    if (pl.contains("last_ok") && pl["last_ok"].is_number()) v += " \xc2\xb7 " + ago(wall_now() - pl["last_ok"].get<double>());
    line("Planes", v, run ? kInk : kFaint);
    if (pl.contains("error") && pl["error"].is_string() && !pl["error"].get<std::string>().empty())
      line("", pl["error"].get<std::string>(), kWarn);
    const json st = live_status_.value("store", json::object()).value("kinds", json::object());
    for (const char* k : {"ship", "plane"}) {
      if (!st.contains(k) || !st[k].is_object()) continue;
      const auto& o = st[k];
      std::string v2 = std::to_string(long(num(o, "latest", 0))) + " known";
      if (const double m = num(o, "modified", 0); m > 0) v2 += " \xc2\xb7 newest fix " + ago(wall_now() - m);
      line(k == std::string("ship") ? "Ships kept" : "Planes kept", v2);
    }
  } else {
    ImGui::TextColored(col(kFaint), "Asking the back-end ...");
  }
  for (const LiveFeed* f : {&planes_, &ships_})
    if (!f->note.empty()) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(col(kWarn), "%s", f->note.c_str());
      ImGui::PopTextWrapPos();
    }
  if (planes_.on || ships_.on) {
    section(font_mono_, "IN VIEW");
    int np = 0, ns = 0;
    double x0, y0, x1, y1;
    cam_.to_world(side_width(), 0, x0, y0);
    cam_.to_world(cam_.w, cam_.h, x1, y1);
    auto in = [&](const LiveItem& it) {
      const double x = geo::mx(it.lon), y = geo::my(it.lat);
      return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    };
    std::vector<const LiveItem*> list;
    for (const auto& it : planes_.items)
      if (in(it)) ++np, list.push_back(&it);
    for (const auto& it : ships_.items)
      if (in(it)) ++ns, list.push_back(&it);
    if (planes_.on) line("Planes", std::to_string(np));
    if (ships_.on) line("Ships", std::to_string(ns));
    // the newest first, each one opens its card
    std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->t > b->t; });
    auto* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const double now = wall_now();
    for (size_t i = 0; i < list.size() && i < 8; ++i) {
      const auto& it = *list[i];
      ImGui::PushID(int(i));
      const ImVec2 q = ImGui::GetCursorScreenPos();
      const bool click = ImGui::Selectable("##it", false, 0, ImVec2(0, 24));
      ImGui::PopID();
      const bool plane = it.kind == "plane";
      dl->AddText(font_mono_, 10.5f, ImVec2(q.x + 2, q.y + 5), u32(kFaint), plane ? "PLANE" : "SHIP");
      dl->AddText(ImVec2(q.x + 50, q.y + 3), u32(kInk), (it.name.empty() ? it.id : it.name).c_str());
      std::string d;
      if (plane && it.alt >= 0) d = std::to_string(int(std::lround(it.alt * 3.28084 / 100))) + "00 ft \xc2\xb7 ";
      if (!plane && it.spd >= 0) d = std::to_string(int(std::lround(it.spd * 1.94384))) + " kn \xc2\xb7 ";
      d += ago(now - it.t);
      const float dw = font_mono_->CalcTextSizeA(11.5f, FLT_MAX, 0, d.c_str()).x;
      dl->AddText(font_mono_, 11.5f, ImVec2(q.x + w - dw, q.y + 5), u32(kMuted), d.c_str());
      if (click) {
        open_live(it);
        return;
      }
    }
    if (list.size() > 8) ImGui::TextColored(col(kFaint), "and %d more", int(list.size() - 8));
  }
  section(font_mono_, ("WATCHED \xc2\xb7 " + std::to_string(watched_.size())).c_str());
  if (watched_.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kFaint), "Click a plane or ship, then Watch: the back-end keeps its track for a year and the map "
                                    "draws its trail whenever live is on.");
    ImGui::PopTextWrapPos();
  }
  for (size_t i = 0; i < watched_.size(); ++i) {
    const auto& w = watched_[i];
    ImGui::PushID(int(i));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##w", false, 0, ImVec2(0, 34))) {
      const LiveItem copy = w;
      open_live(copy);
      fly_to(copy.lon, copy.lat, std::max(cam_.zoom, 8.0));
      ImGui::PopID();
      break;
    }
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddText(font_ui_, 13.5f, ImVec2(q.x + 4, q.y + 2), u32(kInk), (w.name.empty() ? w.id : w.name).c_str());
    const std::string sub = (w.kind == "plane" ? "plane \xc2\xb7 " : "ship \xc2\xb7 ") +
                            (w.t > 0 ? "last seen " + ago(wall_now() - w.t) : std::string("not seen yet"));
    dl->AddText(font_small_, 12, ImVec2(q.x + 4, q.y + 19), u32(kMuted), sub.c_str());
    ImGui::PopID();
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::PushFont(font_small_);
  ImGui::TextColored(col(kFaint), "Planes: adsb.lol (ODbL), then OpenSky (non-commercial use), polled by your back-end only "
                                  "while Planes is on. Ships: AIS through tools/live/ais_feed.py with your aisstream.io "
                                  "key. Unwatched history is kept for 24 hours.");
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
}

}  // namespace gsa
