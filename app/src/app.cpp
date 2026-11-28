#include "app.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "commands.hpp"
#include "ui_theme.hpp"

namespace gsa {

using nlohmann::json;
using namespace ui;
namespace fs = std::filesystem;

namespace {

double now_s() { return glfwGetTime(); }

// old topic names from the first build still work on the command line and in --topic
std::string topic_id(const std::string& n) {
  std::string s = n;
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  static const std::map<std::string, std::string> old = {
      {"overview", "start"}, {"energy", "grid"}, {"power", "grid"},     {"elections", "vote"}, {"votes", "vote"},
      {"people", "vote"},    {"property", "prop"}, {"plots", "prop"},   {"transport", "move"}, {"travel", "move"},
      {"farms", "land"},     {"hazards", "risk"}};
  if (auto it = old.find(s); it != old.end()) return it->second;
  for (const auto& t : layers::topics())
    if (s == t.id) return s;
  return "start";
}

}  // namespace

// ------------------------------------------------------------------ setup

App::App(AppOptions opt) : opt_(std::move(opt)) {
  if (!glfwInit()) throw std::runtime_error("glfwInit failed");
  if (!glfwVulkanSupported()) throw std::runtime_error("no Vulkan loader or driver found");
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  win_ = glfwCreateWindow(opt_.width, opt_.height, "Grid of SA", nullptr, nullptr);
  if (!win_) throw std::runtime_error("cannot open a window");
  glfwSetWindowUserPointer(win_, this);
  vk_ = std::make_unique<vk::Context>(win_, opt_.validation);
  map_ = std::make_unique<MapRenderer>(*vk_);
  if (!map_->load_fonts(opt_.assets_dir)) std::fprintf(stderr, "map label fonts missing under %s/fonts\n", opt_.assets_dir.c_str());
  api_ = std::make_unique<api::Client>(opt_.endpoint);
  loader_ = std::make_unique<TileLoader>(opt_.endpoint, 4);
  // scripted runs never read or write the user's settings
  settings_ = std::make_unique<settings::Store>(
      !opt_.settings_path.empty() ? opt_.settings_path
      : opt_.screenshot.empty()   ? settings::default_path()
                                  : (fs::temp_directory_path() / "gsa-screenshot-settings.json").string());
  init_imgui();

  // what the back-end serves decides what the map can draw
  std::string err;
  if (auto v = api_->get_json("/v1/version", &err)) version_ = *v;
  else backend_error_ = "Back-end not reachable: " + err;
  std::vector<std::string> pack_ids;
  if (auto packs = api_->get_json("/v1/packs", &err)) {
    for (const auto& p : *packs) {
      if (p.value("kind", "") != "tiles") continue;
      const std::string id = p["id"];
      const std::string type = p.value("tile_type", "");
      // vector packs, and image packs ("raster-...") as PNG
      if (type != "mvt" && !(type == "png" && is_raster_pack(id)) && id != "buildings") continue;
      // plots draw from 12.5 as on the website, buildings from 13
      pack_ids.push_back(id);
      if (id == "admin" || id == "base" || id == "buildings" || layers::is_plot_pack(id) || id == "grid" || id.rfind("web-", 0) == 0 ||
          is_raster_pack(id))
        all_packs_.push_back({id, p.value("minzoom", 0), p.value("maxzoom", 14),
                              id == "buildings" ? 13.0 : layers::is_plot_pack(id) ? 12.5 : 0.0});
      auto& names = pack_layers_[id];
      for (const auto& l : p.value("layers", json::array())) {
        if (l.is_string()) names.push_back(l.get<std::string>());
        else if (l.is_object() && l.contains("id")) names.push_back(l["id"].get<std::string>());
      }
    }
    // draw order: municipalities at the bottom, the base map, buildings, website layers, then the grid
    auto rank = [](const std::string& id) {
      return id == "admin" ? 0 : id == "base" ? 1 : id == "buildings" ? 2 : id == "grid" ? 4 : 3;
    };
    std::sort(all_packs_.begin(), all_packs_.end(),
              [&](const auto& a, const auto& b) { return rank(a.pack) != rank(b.pack) ? rank(a.pack) < rank(b.pack) : a.pack < b.pack; });
  }
  avail_ = layers::available(pack_ids, pack_layers_);
  fdata_req_ = fetch("/v1/data/find");

  const auto& sd = settings_->data();
  side_open_ = sd.value("side_open", true);
  if (opt_.screenshot.empty() || !opt_.settings_path.empty()) {  // screenshots read an explicit settings file
    rules_ = find::rules_from(sd.value("find_rules", json::array()));
    if (sd.contains("base") && sd["base"].is_object()) {
      const auto& b = sd["base"];
      lstate_.roads = b.value("roads", lstate_.roads);
      lstate_.terrain = b.value("terrain", lstate_.terrain);
      lstate_.names = b.value("names", lstate_.names);
      lstate_.buildings = b.value("buildings", lstate_.buildings);
      lstate_.landmarks = b.value("landmarks", lstate_.landmarks);
    }
    if (sd.contains("filter") && sd["filter"].is_object()) {
      const auto& j = sd["filter"];
      try {
        filter_.volt = j.value("volt", std::set<int>{});
        filter_.fuel = j.value("fuel", std::set<std::string>{});
        filter_.party = j.value("party", std::set<std::string>{});
        filter_.turn = j.value("turn", std::set<int>{});
        filter_.prov = j.value("prov", std::set<std::string>{});
        filter_.hide = j.value("hide", false);
      } catch (const std::exception&) {
        filter_ = {};
      }
    }
    rank_key_ = sd.value("rank", rank_key_);
    try {
      cmp_ = sd.value("cmp", std::vector<std::string>{});
      if (cmp_.size() > 4) cmp_.resize(4);
      for (const auto& k : sd.value("kept", json::array())) {
        Kept x;
        x.card.kind = Card::Kind(std::clamp(k.value("kind", 0), 0, int(Card::Near)));
        x.card.id = k.value("id", "");
        x.card.lon = k.value("lon", 0.0);
        x.card.lat = k.value("lat", 0.0);
        x.card.props = k.value("props", json::object());
        x.title = k.value("title", "");
        x.kind = k.value("kd", "");
        x.open = k.value("open", true);
        if (x.card.kind == Card::Home || kept_.size() >= 2) continue;
        if (x.card.kind == Card::Area) area_data(x.card.id);
        kept_.push_back(x);
        kept_all_.push_back(x);
      }
    } catch (const std::exception&) {
      cmp_.clear();
      kept_.clear();
    }
  }
  if (opt_.screenshot.empty() && sd.contains("topic")) set_topic(sd.value("topic", "start"));
  else set_topic(opt_.topic.empty() ? "start" : topic_id(opt_.topic));
  if (!opt_.topic.empty()) set_topic(topic_id(opt_.topic));

  cam_.w = opt_.width;
  cam_.h = opt_.height;
  if (opt_.lon && opt_.lat) {
    cam_.cx = geo::mx(*opt_.lon);
    cam_.cy = geo::my(*opt_.lat);
  }
  if (opt_.zoom) cam_.zoom = *opt_.zoom;
  if (opt_.pitch) cam_.pitch = pitch_target_ = std::clamp(*opt_.pitch, 0.0, geo::kMaxPitch);
  if (!opt_.lon) cam_.cx -= side_width() / 2 / cam_.scale();  // centre the country in the free map area
  if (!opt_.select.empty()) open_area(opt_.select, !opt_.lon);
  if (opt_.spot) open_spot(opt_.spot->first, opt_.spot->second);
  if (const auto colon = opt_.measure.find(':'); colon != std::string::npos) {
    const std::string mode = opt_.measure.substr(0, colon);
    measure_mode_ = mode == "area" ? measure::Mode::Area : mode == "circle" ? measure::Mode::Circle : measure::Mode::Path;
    std::vector<double> v;
    std::string rest = opt_.measure.substr(colon + 1);
    for (size_t at = 0; at < rest.size();) {
      const size_t comma = rest.find(',', at);
      v.push_back(std::atof(rest.substr(at, comma - at).c_str()));
      at = comma == std::string::npos ? rest.size() : comma + 1;
    }
    open_panel(Panel::Measure);
    for (size_t i = 0; i + 1 < v.size(); i += 2) measure_.push_back({geo::mx(v[i]), geo::my(v[i + 1])});
    measure_done_ = measure_.size() >= 2;
  }
  if (!opt_.panel.empty()) {
    const std::string p = opt_.panel;
    if (p == "rank") {
      find_tab_ = 1;
      open_panel(Panel::Find);
    } else if (p == "cmp") {
      find_tab_ = 2;
      open_panel(Panel::Find);
    } else if (p == "hist") open_panel(Panel::History);
    else if (p == "find") open_panel(Panel::Find);
    else if (p == "list") open_panel(Panel::List);
    else if (p == "filter") open_panel(Panel::Filter);
    else if (p == "areas") open_panel(Panel::Areas);
    else if (p == "views") open_panel(Panel::Views);
    else if (p == "watch") open_panel(Panel::Watch);
    else if (p == "keys") open_panel(Panel::Keys);
    else if (p == "catalogue") catalogue_open_ = true;
  }
  // a scripted run: commands separated by ";" run one per frame once the map is up
  for (size_t a = 0; a <= opt_.command.size();) {
    size_t b = opt_.command.find(';', a);
    if (b == std::string::npos) b = opt_.command.size();
    const std::string c = opt_.command.substr(a, b - a);
    if (c.find_first_not_of(' ') != std::string::npos) script_.push_back(c.substr(c.find_first_not_of(' ')));
    a = b + 1;
  }
}

App::~App() {
  if (vk_) vkDeviceWaitIdle(vk_->device());
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  if (imgui_pool_) vkDestroyDescriptorPool(vk_->device(), imgui_pool_, nullptr);
  loader_.reset();
  map_.reset();
  vk_.reset();
  if (win_) glfwDestroyWindow(win_);
  glfwTerminate();
}

void App::init_imgui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // layout is fixed by design, nothing to remember
  float sx = 1, sy = 1;
  glfwGetWindowContentScale(win_, &sx, &sy);
  const float scale = std::max(1.0f, sx);

  const fs::path fonts = fs::path(opt_.assets_dir) / "fonts";
  // Latin plus the few symbols the interface uses (middle dot, arrows, multiplication sign, ellipsis)
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x2193, 0x2212, 0x2212, 0x203A, 0x203A, 0};
  auto load = [&](const char* file, float px) -> ImFont* {
    const auto p = fonts / file;
    if (fs::exists(p)) return io.Fonts->AddFontFromFileTTF(p.string().c_str(), px * scale, nullptr, ranges);
    return nullptr;
  };
  font_ui_ = load("SpaceGrotesk-Regular.ttf", 13.5f);
  if (!font_ui_) {
    std::fprintf(stderr, "fonts not found in %s, using the built-in font\n", fonts.string().c_str());
    ImFontConfig c;
    c.SizePixels = 13.0f * scale;
    font_ui_ = io.Fonts->AddFontDefault(&c);
  }
  font_small_ = load("SpaceGrotesk-Regular.ttf", 12.0f);
  font_mono_ = load("IBMPlexMono-Regular.ttf", 11.5f);
  font_title_ = load("SpaceGrotesk-Medium.ttf", 21.0f);
  font_head_ = load("SpaceGrotesk-Medium.ttf", 15.0f);
  for (ImFont** f : {&font_small_, &font_mono_, &font_title_, &font_head_})
    if (!*f) *f = font_ui_;
  io.FontGlobalScale = 1.0f / scale;  // sizes above are in physical pixels

  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0;
  s.ChildRounding = 0;
  s.FrameRounding = 4;
  s.PopupRounding = 6;
  s.ScrollbarRounding = 4;
  s.ScrollbarSize = 8;
  s.GrabRounding = 4;
  s.WindowBorderSize = 0;
  s.FrameBorderSize = 1;
  s.WindowPadding = ImVec2(14, 12);
  s.FramePadding = ImVec2(9, 6);
  s.ItemSpacing = ImVec2(6, 6);
  s.SelectableTextAlign = ImVec2(0, 0.5f);
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = col(kPanel);
  c[ImGuiCol_PopupBg] = col(kPanel);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_Border] = col(kLine);
  c[ImGuiCol_Text] = col(kInk);
  c[ImGuiCol_TextDisabled] = col(kFaint);
  c[ImGuiCol_FrameBg] = col(kGround);
  c[ImGuiCol_FrameBgHovered] = col(kHover);
  c[ImGuiCol_FrameBgActive] = col(kHover);
  c[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ButtonHovered] = col(kHover);
  c[ImGuiCol_ButtonActive] = col("#1a3340");
  c[ImGuiCol_Header] = col(kHover);
  c[ImGuiCol_HeaderHovered] = col(kHover);
  c[ImGuiCol_HeaderActive] = col("#1a3340");
  c[ImGuiCol_Separator] = col(kLine);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = col(kLine);
  c[ImGuiCol_ScrollbarGrabHovered] = col("#2f4c60");
  c[ImGuiCol_NavHighlight] = col(kAccent);
  c[ImGuiCol_CheckMark] = col(kAccent);
  c[ImGuiCol_TextSelectedBg] = col(kAccent, 0.3f);
  c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TabHovered] = col(kHover);
  c[ImGuiCol_TabSelected] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TabSelectedOverline] = col(kAccent);

  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pi.maxSets = 16;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = sizes;
  vk::check(vkCreateDescriptorPool(vk_->device(), &pi, nullptr, &imgui_pool_), "imgui descriptor pool");

  ImGui_ImplGlfw_InitForVulkan(win_, true);
  ImGui_ImplVulkan_InitInfo ii{};
  ii.Instance = vk_->instance();
  ii.PhysicalDevice = vk_->physical();
  ii.Device = vk_->device();
  ii.QueueFamily = vk_->queue_family();
  ii.Queue = vk_->queue();
  ii.DescriptorPool = imgui_pool_;
  ii.RenderPass = vk_->render_pass();
  ii.MinImageCount = 2;
  ii.ImageCount = std::max(2u, vk_->image_count());
  ii.MSAASamples = vk_->samples();
  ImGui_ImplVulkan_Init(&ii);
}

// ------------------------------------------------------------------ loop

int App::run() {
  while (!glfwWindowShouldClose(win_)) {
    glfwPollEvents();
    frame();
    ++frames_;
    if (opt_.max_frames && frames_ >= opt_.max_frames && opt_.screenshot.empty()) break;
  }
  if (opt_.screenshot.empty()) save_settings();
  return 0;
}

void App::frame() {
  int ww, wh;
  glfwGetWindowSize(win_, &ww, &wh);
  cam_.w = ww;
  cam_.h = wh;

  if (fly_) {
    const double k = std::clamp((now_s() - fly_->t0) / fly_->dur, 0.0, 1.0);
    const double e = k < 0.5 ? 2 * k * k : 1 - std::pow(-2 * k + 2, 2) / 2;
    cam_.cx = fly_->from.cx + (fly_->cx - fly_->from.cx) * e;
    cam_.cy = fly_->from.cy + (fly_->cy - fly_->from.cy) * e;
    cam_.zoom = fly_->from.zoom + (fly_->zoom - fly_->from.zoom) * e;
    if (k >= 1) fly_.reset();
  }
  if (cam_.pitch != pitch_target_) {
    const double step = std::clamp(ImGui::GetIO().DeltaTime * 160.0, 0.5, 20.0);
    cam_.pitch = std::abs(pitch_target_ - cam_.pitch) <= step ? pitch_target_
                                                                : cam_.pitch + (pitch_target_ > cam_.pitch ? step : -step);
  }

  const double f0 = now_s();
  if (prof_.t_start == 0) prof_.t_start = f0;
  if (!script_.empty() && frames_ >= 2 && !fly_ && pending_fn_.empty()) {
    run_command(script_.front());
    script_.pop_front();
  }
  if (report_pending_ && report_ready()) report_now();
  for (auto& t : loader_->take()) map_->add(t);
  const double f1 = now_s();
  auto need = map_->needed(cam_, packs_);
  if (need.empty() && !fly_) need = map_->needed(cam_, packs_, true);
  if (need != last_need_) {
    loader_->want(need);
    last_need_ = need;
  }
  poll_async();
  map_->set_style(map_style_now());

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  handle_input();
  blockers_.clear();
  ui();
  track_changes();
  ImGui::Render();

  // labels go where the interface left room; screenshots skip the fades
  const double now = now_s();
  const double dt = opt_.screenshot.empty() ? std::min(0.1, now - last_frame_t_) : -1.0;
  last_frame_t_ = now;
  const double f2 = now_s();
  map_->prepare(cam_, packs_, blockers_, dt);
  {
    const double f3 = now_s(), ms = (f3 - f0) * 1000;
    prof_.add_ms += (f1 - f0) * 1000;
    prof_.prep_ms += (f3 - f2) * 1000;
    prof_.worst_ms = std::max(prof_.worst_ms, ms);
    prof_.slow += ms > 33;
  }

  const auto bg = style::background();
  const float clear[4] = {bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1.0f};
  if (!vk_->begin_frame(clear, [this](VkCommandBuffer cmd) { map_->upload(cmd); })) return;
  float dpr = 1;
  {
    int fw, fh;
    glfwGetFramebufferSize(win_, &fw, &fh);
    dpr = ww ? float(fw) / float(ww) : 1.0f;
  }
  map_->draw(vk_->cmd(), cam_, dpr);
  if (!(want_png_ && on_png_)) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vk_->cmd());  // reports: map only

  // scripted screenshot: wait until no tiles are missing or in flight and areas have loaded
  const bool idle = script_.empty() && !report_pending_ && !want_png_ && map_->needed(cam_, packs_).empty() && loader_->in_flight() == 0 && !map_->meshing() && area_req_.empty() && !fly_ &&
                    !map_->labels_animating() && !search_req_.valid() && pending_fn_.empty();
  settled_frames_ = idle ? settled_frames_ + 1 : 0;
  if (idle && prof_.t_settled == 0) prof_.t_settled = now_s();
  if (!idle) prof_.t_settled = 0;
  const bool shoot = !opt_.screenshot.empty() && (settled_frames_ > 8 || (opt_.max_frames && frames_ >= opt_.max_frames));
  if (shoot || want_png_) {
    std::vector<uint8_t> px;
    vk_->end_frame(&px);
    const auto e = vk_->extent();
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    if (!shoot && on_png_) {
      auto cb = std::move(on_png_);
      on_png_ = nullptr;
      want_png_ = false;
      cb(px, e.width, e.height);
      return;
    }
    std::string out = opt_.screenshot;
    if (!shoot) {
      // PNG command: a dated file in the user's Pictures (or home) folder
      const char* home = std::getenv(
#ifdef _WIN32
          "USERPROFILE"
#else
          "HOME"
#endif
      );
      fs::path dir = home ? fs::path(home) / "Pictures" : fs::current_path();
      std::error_code ec;
      if (!fs::exists(dir, ec)) dir = home ? fs::path(home) : fs::current_path();
      char name[64];
      const std::time_t t = std::time(nullptr);
      std::strftime(name, sizeof name, "GridOfSA-%Y%m%d-%H%M%S.png", std::localtime(&t));
      out = (dir / name).string();
      want_png_ = false;
    }
    if (!stbi_write_png(out.c_str(), int(e.width), int(e.height), 4, px.data(), int(e.width * 4))) {
      std::fprintf(stderr, "cannot write %s\n", out.c_str());
      toast("Could not save the image");
    } else {
      std::fprintf(stderr, "wrote %s (%ux%u, %zu tiles, %zu vertices)\n", out.c_str(), e.width, e.height, map_->tile_count(),
                   map_->vertex_count());
      if (shoot)
        std::fprintf(stderr, "load: settled after %.0f ms, %d frames, worst frame %.0f ms, %d frames over 33 ms, "
                     "tile adds %.0f ms, labels %.0f ms\n",
                     (prof_.t_settled - prof_.t_start) * 1000, frames_, prof_.worst_ms, prof_.slow, prof_.add_ms,
                     prof_.prep_ms);
      if (!shoot) toast("Saved " + out, 4);
    }
    if (shoot) glfwSetWindowShouldClose(win_, GLFW_TRUE);
    return;
  }
  vk_->end_frame();
}

App::JsonFuture App::fetch(const std::string& path) {
  const auto ep = opt_.endpoint;
  return std::async(std::launch::async, [ep, path]() {
    api::Client c(ep);
    return c.get_json(path);
  });
}

void App::poll_async() {
  if (fdata_req_.valid() && fdata_req_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    if (auto j = fdata_req_.get()) {
      try {
        fdata_ = find::parse(*j);
        set_rules(rules_);
        set_filter(filter_);  // provinces need the municipality list
        if (!find_toast_.empty()) toast(find_toast_ + ": " + std::to_string(fpass_.size()) + " municipalities match");
        find_toast_.clear();
      } catch (const std::exception& e) {
        toast(std::string("Find data unreadable: ") + e.what());
      }
    }
  }
  for (auto it = area_req_.begin(); it != area_req_.end();) {
    if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto j = it->second.get();
      areas_[it->first] = j ? *j : json{{"error", "Could not load this area"}};
      if (it->first == fly_on_load_ && j && j->contains("bbox")) {
        const auto& b = (*j)["bbox"];
        fly_to_bbox(b[0], b[1], b[2], b[3]);
        fly_on_load_.clear();
      }
      it = area_req_.erase(it);
    } else {
      ++it;
    }
  }
  if (search_req_.valid() && search_req_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    hits_ = search_req_.get();
    hit_sel_ = 0;
    if (!pending_fn_.empty()) {
      // "name CODE": open the best hit, then run the function on it
      const std::string code = pending_fn_;
      pending_fn_.clear();
      if (hits_.empty()) {
        toast(code + ": nothing found for " + last_query_);
      } else {
        const auto h = hits_.front();
        if (h.ref.rfind("area:", 0) == 0) open_area(h.ref.substr(5));
        else if (h.lon && h.lat) {
          Card c;
          c.kind = Card::Facility;
          c.id = h.ref;
          c.lon = *h.lon;
          c.lat = *h.lat;
          c.props = {{"name", h.label}, {"detail", h.detail}, {"tag", h.kind}};
          if (cards_.back().kind != Card::Home || cards_.size() > 1) cards_.push_back(c);
          else cards_.back() = c;
          panel_ = Panel::None;
          fly_to(*h.lon, *h.lat, std::max(cam_.zoom, 11.0));
        }
        run_fn(code);
      }
      hits_.clear();
      query_[0] = 0;
      last_query_.clear();
    }
  }
  const std::string q = query_;
  const auto parsed = commands::parse(q);
  const std::string text = parsed.kind == commands::Parsed::Search ? parsed.text : std::string();
  if (text != last_query_ && !search_req_.valid() && pending_fn_.empty()) {
    last_query_ = text;
    if (text.size() >= 2) {
      const auto ep = opt_.endpoint;
      search_req_ = std::async(std::launch::async, [ep, text]() {
        api::Client c(ep);
        return c.search(text, 10);
      });
    } else {
      hits_.clear();
    }
  }
}

// ------------------------------------------------------------------ actions

void App::set_topic(const std::string& id) {
  lstate_.set_topic(id, avail_);
  // keep only what this build can draw at the top; the rest stays listed as needing a pack
  std::stable_partition(lstate_.on.begin(), lstate_.on.end(), [&](const auto& l) { return avail_.count(l) != 0; });
  apply_layers();
}

layers::MapStyle App::map_style_now() const {
  auto ms = layers::map_style(lstate_);
  ms.find_on = fdata_ && !rules_.empty();
  if (ms.find_on) ms.find_pass = fpass_;
  ms.filter = filter_;
  ms.extrude = cam_.pitch > 5;  // buildings stand up once the map is tilted, as on the website
  // ward filters: municipal winners under the wards would show through where wards are hidden
  if (filter_.vote_on() && lstate_.shows("c:ward") && !lstate_.shows("c:muni")) ms.fill = style::Topic::Overview;
  return ms;
}

void App::set_rules(std::vector<find::Rule> r) {
  rules_ = std::move(r);
  fpass_ = fdata_ ? find::run(*fdata_, rules_) : std::set<std::string>{};
  if (opt_.screenshot.empty()) {
    settings_->data()["find_rules"] = find::to_json(rules_);
    settings_->save();
  }
}

void App::set_filter(const layers::Filters& f) {
  filter_ = f;
  filter_.prov_munis.clear();
  if (fdata_)
    for (const auto& m : fdata_->munis) {
      // the Find data uses KZN, EC, LP, GT, WC, NW, MP, FS, NC; the website's filter says LIM
      const std::string p = m.prov == "LP" ? "LIM" : m.prov;
      if (filter_.prov.count(p)) filter_.prov_munis.insert(m.code);
    }
  if (opt_.screenshot.empty()) {
    auto& j = settings_->data()["filter"];
    j = json{{"volt", filter_.volt}, {"fuel", filter_.fuel}, {"party", filter_.party},
             {"turn", filter_.turn}, {"prov", filter_.prov}, {"hide", filter_.hide}};
    settings_->save();
  }
}

void App::apply_layers() {
  map_->set_style(map_style_now());
  // only fetch the website packs whose layers are on
  const auto want = layers::packs_needed(lstate_, pack_layers_);
  packs_.clear();
  const bool base = lstate_.roads || lstate_.landmarks || lstate_.names;
  for (const auto& p : all_packs_) {
    if (p.pack == "base" && !base) continue;
    if (p.pack == "buildings" && !lstate_.buildings) continue;
    const bool on_demand = p.pack.rfind("web-", 0) == 0 || is_raster_pack(p.pack) || layers::is_plot_pack(p.pack);
    if (!on_demand || want.count(p.pack)) packs_.push_back(p);
  }
}

void App::open_area(const std::string& id, bool fly) {
  Card c;
  c.kind = Card::Area;
  c.id = id;
  if (cards_.back().kind == Card::Area && cards_.back().id == id) {
  } else if (cards_.size() == 1 && cards_.back().kind == Card::Home) cards_.push_back(c);
  else cards_.push_back(c);
  panel_ = Panel::None;
  side_open_ = true;
  fly_on_load_ = fly ? id : std::string();
  if (fly && areas_.count(id) && areas_[id].contains("bbox")) {
    const auto& b = areas_[id]["bbox"];
    fly_to_bbox(b[0], b[1], b[2], b[3]);
    fly_on_load_.clear();
  }
  if (!areas_.count(id) && !area_req_.count(id)) area_req_[id] = fetch("/v1/areas/" + api::url_encode(id));
}

void App::open_facility(const map_labels::FeatureRef& r) {
  const auto& f = r.feature();
  Card c;
  c.kind = Card::Facility;
  c.id = f.str("id");
  c.lon = geo::lon_of(r.wx);
  c.lat = geo::lat_of(r.wy);
  for (const auto& [k, v] : f.props) {
    if (const auto* s = std::get_if<std::string>(&v)) c.props[k] = *s;
    else if (const auto* d = std::get_if<double>(&v)) c.props[k] = *d;
    else if (const auto* i = std::get_if<int64_t>(&v)) c.props[k] = *i;
    else if (const auto* b = std::get_if<bool>(&v)) c.props[k] = *b;
  }
  if (cards_.back().kind == Card::Facility && cards_.back().id == c.id) return;
  cards_.push_back(c);
  panel_ = Panel::None;
  side_open_ = true;
}

void App::open_web(const MapRenderer::WebHit& h) {
  const auto& f = *h.feature;
  Card c;
  c.kind = Card::Facility;
  c.lon = geo::lon_of(h.wx);
  c.lat = geo::lat_of(h.wy);
  for (const auto& [k, v] : f.props) {
    if (const auto* s = std::get_if<std::string>(&v)) c.props[k] = *s;
    else if (const auto* d = std::get_if<double>(&v)) c.props[k] = *d;
    else if (const auto* i = std::get_if<int64_t>(&v)) c.props[k] = *i;
    else if (const auto* b = std::get_if<bool>(&v)) c.props[k] = *b;
  }
  c.props["layer"] = h.layer;
  c.id = h.layer + ":" + c.props.value("name", std::string()) + ":" + std::to_string(c.lon) + std::to_string(c.lat);
  if (cards_.back().kind == Card::Facility && cards_.back().id == c.id) return;
  cards_.push_back(c);
  panel_ = Panel::None;
  side_open_ = true;
}

void App::open_spot(double lon, double lat) {
  Card c;
  c.kind = Card::Spot;
  c.lon = lon;
  c.lat = lat;
  if (cards_.back().kind == Card::Spot) cards_.back() = c;
  else cards_.push_back(c);
  panel_ = Panel::None;
  side_open_ = true;
}

void App::open_panel(Panel p) {
  panel_ = panel_ == p ? Panel::None : p;
  if (p == Panel::Measure) {
    measuring_ = panel_ == Panel::Measure;
    if (measuring_) measure_reset();
  }
  side_open_ = true;
}

std::vector<measure::LonLat> App::measure_ll() const {
  std::vector<measure::LonLat> out;
  for (const auto& [wx, wy] : measure_) out.push_back({geo::lon_of(wx), geo::lat_of(wy)});
  return out;
}

void App::measure_click(double sx, double sy, double wx, double wy) {
  using measure::Mode;
  if (measure_done_) measure_reset();
  // clicking an own point: the first closes an area, the last finishes a path
  for (size_t i = 0; i < measure_.size(); ++i) {
    double px, py;
    cam_.to_screen(measure_[i].first, measure_[i].second, px, py);
    if (std::hypot(px - sx, py - sy) >= 10) continue;
    if (measure_mode_ == Mode::Area && i == 0 && measure_.size() >= 3) {
      measure_done_ = true;
      return;
    }
    if (measure_mode_ != Mode::Circle && i + 1 == measure_.size() && measure_.size() >= 2) {
      measure_done_ = true;
      return;
    }
  }
  measure_.push_back({wx, wy});
  if (measure_mode_ == Mode::Circle && measure_.size() == 2) measure_done_ = true;
}

void App::close_top() {
  if (catalogue_open_) {
    catalogue_open_ = false;
    return;
  }
  if (panel_ != Panel::None) {
    if (panel_ == Panel::Measure) measuring_ = false;
    panel_ = Panel::None;
    return;
  }
  if (cards_.size() > 1) cards_.pop_back();
}

float App::side_w() const {
  return panel_ == Panel::Find && find_tab_ == 2 && cmp_.size() >= 3 && cam_.w >= 1100 ? 560.0f : kSideW;
}
float App::side_width() const { return kRailW + (side_open_ ? side_w() : 0.0f); }

void App::fly_to(double lon, double lat, double zoom) {
  // centre the target in the part of the window the sidebar leaves free
  geo::Camera to = cam_;
  to.zoom = zoom;
  const double off = side_width() / 2 / to.scale();
  fly_on_load_.clear();  // a newer fly wins over an area card's fly once its outline loads
  fly_ = Fly{now_s(), 0.7, cam_, geo::mx(lon) - off, geo::my(lat), zoom};
}

void App::fly_to_bbox(double lon0, double lat0, double lon1, double lat1, double max_zoom) {
  const double w = std::max(200.0, cam_.w - side_width() - 120), h = std::max(200.0, cam_.h - 160);
  const double dx = std::max(1e-6, geo::mx(lon1) - geo::mx(lon0)), dy = std::max(1e-6, geo::my(lat0) - geo::my(lat1));
  const double z = std::clamp(std::log2(std::min(w / (dx * geo::kTilePx), h / (dy * geo::kTilePx))), 3.0, max_zoom);
  fly_to((lon0 + lon1) / 2, geo::lat_of((geo::my(lat0) + geo::my(lat1)) / 2), z);
}

void App::toast(const std::string& s, double secs) {
  toast_ = s;
  toast_until_ = now_s() + secs;
}

bool App::subject_pos(double& lon, double& lat) const {
  const Card& c = cards_.back();
  if (c.kind == Card::Facility || c.kind == Card::Spot || c.kind == Card::Near) {
    lon = c.lon;
    lat = c.lat;
    return true;
  }
  if (c.kind == Card::Area) {
    auto it = areas_.find(c.id);
    if (it != areas_.end() && it->second.contains("bbox")) {
      const auto& b = it->second["bbox"];
      lon = (b[0].get<double>() + b[2].get<double>()) / 2;
      lat = (b[1].get<double>() + b[3].get<double>()) / 2;
      return true;
    }
    // not loaded yet: the Find data has every municipality's outline
    if (fdata_)
      if (const auto* m = fdata_->muni(c.id)) {
        lon = (m->bb[0] + m->bb[2]) / 2;
        lat = (m->bb[1] + m->bb[3]) / 2;
        return true;
      }
  }
  return false;
}

std::string App::subject_name() const {
  const Card& c = cards_.back();
  if (c.kind == Card::Facility) return c.props.value("name", c.id);
  if (c.kind == Card::Area) {
    auto it = areas_.find(c.id);
    if (it != areas_.end() && it->second.contains("name")) return it->second.value("name", c.id);
    if (fdata_)
      if (const auto* m = fdata_->muni(c.id)) return m->name;
    return c.id;
  }
  if (c.kind == Card::Spot) {
    const std::string m = muni_name_at(geo::mx(c.lon), geo::my(c.lat));
    return m.empty() ? "this spot" : "a spot in " + m;
  }
  if (c.kind == Card::Near) return c.props.value("label", std::string("this spot"));
  return "South Africa";
}

std::string App::muni_name_at(double wx, double wy, std::string* prov) const {
  const auto* m = map_->pick(wx, wy, "municipality", std::max(cam_.zoom, 6.0));
  if (prov) {
    const auto* p = map_->pick(wx, wy, "province", std::max(cam_.zoom, 6.0));
    *prov = p ? p->str("name") : "";
  }
  return m ? m->str("name") : "";
}

void App::watch_current() {
  const Card& c = cards_.back();
  if (c.kind != Card::Facility) {
    toast("WATCH: open a substation or power station first");
    return;
  }
  auto& w = settings_->data()["watch"];
  if (!w.is_array()) w = json::array();
  for (const auto& e : w)
    if (e.value("id", "") == c.id) {
      toast(c.props.value("name", c.id) + " is already on the watchlist");
      return;
    }
  w.push_back({{"id", c.id}, {"name", c.props.value("name", c.id)}, {"kind", c.props.value("kind", "")},
               {"kv", c.props.value("kv", 0.0)}, {"mw", c.props.value("mw", 0.0)}, {"lon", c.lon}, {"lat", c.lat}});
  save_settings();
  toast("Watching " + c.props.value("name", c.id));
}

void App::save_view(const std::string& name) {
  auto& v = settings_->data()["views"];
  if (!v.is_array()) v = json::array();
  const std::string n = name.empty() ? "View " + std::to_string(v.size() + 1) : name;
  json e = {{"name", n},
            {"lon", geo::lon_of(cam_.cx)},
            {"lat", geo::lat_of(cam_.cy)},
            {"zoom", cam_.zoom},
            {"topic", lstate_.topic},
            {"on", lstate_.on},
            {"hidden", std::vector<std::string>(lstate_.hidden.begin(), lstate_.hidden.end())},
            {"focus", lstate_.focus}};
  if (cards_.back().kind == Card::Area) e["area"] = cards_.back().id;
  // views keep the Find conditions and the Find / Rank tab, as on the website
  if (!rules_.empty()) e["find"] = find::to_json(rules_);
  e["find_tab"] = find_tab_ == 1 ? "rank" : find_tab_ == 2 ? "cmp" : "find";
  e["rank"] = rank_key_;
  for (auto& x : v)
    if (x.value("name", "") == n) {
      x = e;
      save_settings();
      toast("Updated view " + n);
      return;
    }
  v.push_back(e);
  save_settings();
  toast("Saved view " + n);
}

bool App::load_view(const std::string& name) {
  const auto& v = settings_->data().value("views", json::array());
  for (const auto& e : v) {
    std::string n = e.value("name", ""), q = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    if (n.rfind(q, 0) != 0) continue;
    lstate_.topic = e.value("topic", "start");
    lstate_.on = e.value("on", std::vector<std::string>{});
    const auto h = e.value("hidden", std::vector<std::string>{});
    lstate_.hidden = std::set<std::string>(h.begin(), h.end());
    lstate_.focus = e.value("focus", "");
    set_rules(find::rules_from(e.value("find", json::array())));
    {
      const std::string ft = e.value("find_tab", "find");
      find_tab_ = ft == "rank" ? 1 : ft == "cmp" ? 2 : 0;
    }
    rank_key_ = e.value("rank", rank_key_);
    apply_layers();
    fly_ = Fly{now_s(), 0.7, cam_, geo::mx(e.value("lon", 25.2)), geo::my(e.value("lat", -28.8)), e.value("zoom", 5.4)};
    if (e.contains("area")) open_area(e["area"], false);
    toast("View " + e.value("name", ""));
    return true;
  }
  toast("No saved view called " + name);
  return false;
}

void App::save_settings() {
  if (!opt_.screenshot.empty()) return;
  auto& d = settings_->data();
  d["side_open"] = side_open_;
  d["topic"] = lstate_.topic;
  d["cmp"] = cmp_;
  {
    json k = json::array();
    for (const auto& x : kept_)
      k.push_back({{"kind", int(x.card.kind)}, {"id", x.card.id}, {"lon", x.card.lon}, {"lat", x.card.lat},
                   {"props", x.card.props}, {"title", x.title}, {"kd", x.kind}, {"open", x.open}});
    d["kept"] = k;
  }
  d["base"] = json{{"roads", lstate_.roads}, {"terrain", lstate_.terrain}, {"names", lstate_.names},
                   {"buildings", lstate_.buildings}, {"landmarks", lstate_.landmarks}};
  settings_->save();
}

void App::png() { want_png_ = true; }

// The functions of the subject + function model (DES, NEAR, GO ...), from the card's strip,
// the command line, or "name CODE".
void App::run_fn(const std::string& code) {
  double lon = 0, lat = 0;
  const bool has_pos = subject_pos(lon, lat);
  if (code == "DES") {
    panel_ = Panel::None;
    side_open_ = true;
  } else if (code == "GO") {
    const Card& c = cards_.back();
    if (c.kind == Card::Area && areas_.count(c.id) && areas_[c.id].contains("bbox")) {
      const auto& b = areas_[c.id]["bbox"];
      fly_to_bbox(b[0], b[1], b[2], b[3]);
    } else if (has_pos) fly_to(lon, lat, std::max(cam_.zoom, 13.0));
  } else if (code == "HERE") {
    if (has_pos) open_spot(lon, lat);
    else open_spot(geo::lon_of(cam_.cx), geo::lat_of(cam_.cy));
  } else if (code == "NEAR") {
    if (!has_pos) {
      lon = geo::lon_of(cam_.cx);
      lat = geo::lat_of(cam_.cy);
    }
    std::string label = subject_name();
    if (cards_.back().kind == Card::Home || label.empty()) label = "Map centre";
    label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
    open_near(lon, lat, 25, label);
  } else if (code == "CHK") {
    if (!has_pos) {
      lon = geo::lon_of(cam_.cx);
      lat = geo::lat_of(cam_.cy);
    }
    open_spot(lon, lat);
    fly_to(lon, lat, std::max(cam_.zoom, 12.0));
  } else if (code == "KEEP") {
    keep_current();
  } else if (code == "CMP") {
    const std::string m = current_muni_code();
    if (!m.empty()) cmp_add(m);
    find_tab_ = 2;
    panel_ = Panel::Find;
    side_open_ = true;
  } else if (code == "REPORT") {
    report();
  } else if (code == "HIST") {
    panel_ = Panel::History;
    side_open_ = true;
  } else if (code == "MEAS") {
    measuring_ = true;
    panel_ = Panel::Measure;
    measure_reset();
    if (has_pos) measure_.push_back({geo::mx(lon), geo::my(lat)});
  } else if (code == "WATCH") {
    watch_current();
  } else if (code == "COPY") {
    if (!has_pos) {
      lon = geo::lon_of(cam_.cx);
      lat = geo::lat_of(cam_.cy);
    }
    char b[64];
    std::snprintf(b, sizeof b, "%.5f, %.5f", lat, lon);
    glfwSetClipboardString(win_, b);
    toast(std::string("Coordinates copied: ") + b);
  } else if (code == "PNG") {
    png();
  } else if (code == "LIST") {
    open_panel(Panel::List);
  } else {
    const auto* s = commands::find(code);
    toast(code + (s ? std::string(": ") + s->about + " comes in a later milestone" : ": not a function"));
  }
}

bool App::run_command(const std::string& line) {
  const auto p = commands::parse(line);
  if (p.kind == commands::Parsed::Search) return false;
  history_.push_back(line);
  if (p.kind == commands::Parsed::SubjectFunction) {
    // find the subject, then run the function once it opens
    pending_fn_ = p.code;
    last_query_ = p.arg;
    const auto ep = opt_.endpoint;
    const std::string q = p.arg;
    hits_.clear();
    search_req_ = std::async(std::launch::async, [ep, q]() {
      api::Client c(ep);
      return c.search(q, 5);
    });
    return true;
  }
  const std::string& c = p.code;
  const std::string& a = p.arg;
  const auto layer_arg = [&]() -> const layers::Def* {
    const auto* d = layers::match(a);
    if (!d) toast(c + ": no layer called " + a);
    return d;
  };
  if (c == "GO") {
    if (a.empty()) run_fn("GO");
    else run_command(a + " GO");
  } else if (c == "Z") {
    try {
      fly_ = Fly{now_s(), 0.4, cam_, cam_.cx, cam_.cy, std::clamp(std::stod(a), 3.0, 16.0)};
    } catch (...) {
      toast("Z needs a zoom level, 3 to 16");
    }
  } else if (c == "FIT") {
    fly_to(25.2, -28.8, 5.4);
  } else if (c == "ADD" || c == "SHOW") {
    if (const auto* d = layer_arg()) {
      if (c == "SHOW" && lstate_.hidden.erase(d->id)) {
      } else lstate_.add(d->id);
      apply_layers();
      if (!avail_.count(d->id)) toast(std::string(d->name) + " needs its data pack, which is not installed yet");
    }
  } else if (c == "RM") {
    if (const auto* d = layer_arg()) lstate_.remove(d->id);
    apply_layers();
  } else if (c == "HIDE") {
    if (const auto* d = layer_arg()) lstate_.hidden.insert(d->id);
    apply_layers();
  } else if (c == "CLR") {
    lstate_.clear();
    apply_layers();
  } else if (c == "START" || c == "GRID" || c == "VOTE" || c == "WATER" || c == "PLOTS") {
    set_topic(c == "PLOTS" ? "prop" : topic_id(c));
  } else if (c == "FIND") {
    find_tab_ = 0;
    panel_ = Panel::Find;
    side_open_ = true;
    if (const auto* pre = find::preset(a)) {
      set_rules(pre->rules);
      if (fdata_) toast(std::string(pre->name) + ": " + std::to_string(fpass_.size()) + " municipalities match");
      else find_toast_ = pre->name;  // once the Find data arrives
    }
  } else if (c == "RANK") {
    // "RANK dams", "RANK capacity": pick the list by its key or the start of its name
    std::string q = a;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    if (!q.empty())
      for (const auto& d : find::rank_defs()) {
        std::string l = d.label;
        std::transform(l.begin(), l.end(), l.begin(), ::tolower);
        if (q == d.k || l.rfind(q, 0) == 0 || (q == "capacity" && std::string(d.k) == "cap")) {
          rank_key_ = d.k;
          break;
        }
      }
    find_tab_ = 1;
    panel_ = Panel::Find;
    side_open_ = true;
  } else if (c == "FILTER") {
    panel_ = Panel::Filter;
    side_open_ = true;
    filter_vote_tab_ = a == "vote" || a == "party" || (a.empty() && lstate_.topic == "vote");
  } else if (c == "LIST") {
    panel_ = Panel::List;
    side_open_ = true;
  } else if (c == "AREAS") {
    panel_ = Panel::Areas;
    side_open_ = true;
  } else if (c == "WL") {
    panel_ = Panel::Watch;
    side_open_ = true;
  } else if (c == "SAVE") {
    save_view(a);
  } else if (c == "VIEW") {
    if (a.empty()) {
      panel_ = Panel::Views;
      side_open_ = true;
    } else load_view(a);
  } else if (c == "X") {
    close_top();
  } else if (c == "HELP") {
    help_code_ = a;
    std::transform(help_code_.begin(), help_code_.end(), help_code_.begin(), ::toupper);
    panel_ = Panel::Keys;
    side_open_ = true;
  } else if (c == "NEAR" && !a.empty()) {
    double lon = 0, lat = 0;
    if (!subject_pos(lon, lat)) lon = geo::lon_of(cam_.cx), lat = geo::lat_of(cam_.cy);
    double km = 25;
    try {
      km = std::clamp(std::stod(a), 1.0, 200.0);
    } catch (...) {
    }
    std::string label = subject_name();
    label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
    open_near(lon, lat, km, cards_.back().kind == Card::Home ? "Map centre" : label);
  } else {
    run_fn(c);
  }
  return true;
}

// ------------------------------------------------------------------ input

void App::handle_input() {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const bool over_map = !io.WantCaptureMouse && m.x > side_width();
  if (over_map) {
    if (io.MouseWheel != 0) {
      fly_.reset();
      cam_.zoom_at(io.MouseWheel * 0.35, m.x, m.y);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      dragging_ = true;
      drag_moved_ = false;
      drag_x_ = m.x;
      drag_y_ = m.y;
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !measuring_) {
      fly_.reset();
      cam_.zoom_at(1.0, m.x, m.y);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      double wx, wy;
      cam_.to_world(m.x, m.y, wx, wy);
      open_spot(geo::lon_of(wx), geo::lat_of(wy));
    }
    // hover: the facility or municipality under the cursor
    double wx, wy;
    cam_.to_world(m.x, m.y, wx, wy);
    if (const auto* r = map_->pick_icon(m.x, m.y)) {
      hover_name_ = r->feature().str("name");
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else if (const auto h = map_->pick_web(m.x, m.y, cam_)) {
      hover_name_ = h->feature->str("name");
      if (hover_name_.empty()) hover_name_ = h->feature->str("detail");
    } else {
      const auto* f = map_->pick(wx, wy, "municipality", cam_.zoom);
      hover_name_ = f ? f->str("name") : "";
    }
  } else {
    hover_name_.clear();
  }
  if (dragging_) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const double dx = m.x - drag_x_, dy = m.y - drag_y_;
      if (std::abs(dx) + std::abs(dy) > 0) {
        if (std::hypot(m.x - io.MouseClickedPos[0].x, m.y - io.MouseClickedPos[0].y) > 3) drag_moved_ = true;
        if (drag_moved_) {
          fly_.reset();
          if (io.KeyCtrl) {  // Ctrl+drag tilts the map
            cam_.pitch = pitch_target_ = std::clamp(cam_.pitch - dy * 0.3, 0.0, geo::kMaxPitch);
          } else {
            cam_.pan_px(dx, dy);
          }
        }
      }
      drag_x_ = m.x;
      drag_y_ = m.y;
    } else {
      dragging_ = false;
      if (!drag_moved_ && !io.WantCaptureMouse) {
        double wx, wy;
        cam_.to_world(m.x, m.y, wx, wy);
        if (measuring_) {
          measure_click(m.x, m.y, wx, wy);
        } else if (const auto* r = map_->pick_icon(m.x, m.y)) {
          open_facility(*r);
        } else if (const auto h = map_->pick_web(m.x, m.y, cam_)) {
          open_web(*h);
        } else if (const auto* f = map_->pick(wx, wy, "municipality", cam_.zoom)) {
          open_area(f->str("id"), false);
        }
      }
    }
  }

  // undo and redo work everywhere except while typing
  if (!io.WantTextInput && io.KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redo() : undo();
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
  }
  if (!io.WantTextInput) {
    const bool shift = io.KeyShift;
    if (shift && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
      // Shift F clears every filter (undo brings them back)
      set_rules({});
      layers::Filters f;
      f.hide = filter_.hide;
      set_filter(f);
      toast("Every filter cleared \xc2\xb7 Ctrl Z brings them back");
    }
    static const ImGuiKey digits[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
                                      ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_0};
    const auto& tps = layers::topics();
    for (size_t i = 0; i < tps.size() && i < 10; ++i)
      if (ImGui::IsKeyPressed(digits[i], false) && !io.KeyCtrl) set_topic(tps[i].id);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) cam_.zoom_at(0.5, cam_.w / 2, cam_.h / 2);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) cam_.zoom_at(-0.5, cam_.w / 2, cam_.h / 2);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) fly_to(25.2, -28.8, 5.4);
    if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) && !shift) focus_search_ = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) && shift) open_panel(Panel::Keys);  // "?"
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) side_open_ = !side_open_;
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
      find_tab_ = shift ? 1 : 0;
      panel_ = Panel::Find;
      side_open_ = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) open_panel(Panel::Measure);
    if (ImGui::IsKeyPressed(ImGuiKey_W, false) && !shift) open_spot(geo::lon_of(cam_.cx), geo::lat_of(cam_.cy));
    if (ImGui::IsKeyPressed(ImGuiKey_W, false) && shift) open_panel(Panel::Watch);
    if (ImGui::IsKeyPressed(ImGuiKey_KeypadMultiply, false) || (shift && ImGui::IsKeyPressed(ImGuiKey_8, false))) watch_current();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) catalogue_open_ = !catalogue_open_;
    if (measuring_ && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
        measure_.size() >= 2)
      measure_done_ = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) open_panel(Panel::Filter);
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) open_panel(Panel::List);
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) open_panel(Panel::Views);
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) open_panel(Panel::Measure);
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) open_panel(Panel::Keys);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
      if (measuring_ && ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !measure_.empty()) {
        measure_.pop_back();
        measure_done_ = false;
      }
      else close_top();
    }
    const double step = 120;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) cam_.pan_px(step * io.DeltaTime * 6, 0);
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) cam_.pan_px(-step * io.DeltaTime * 6, 0);
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) cam_.pan_px(0, step * io.DeltaTime * 6);
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) cam_.pan_px(0, -step * io.DeltaTime * 6);
  }
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) focus_search_ = true;
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save_view("");
}

}  // namespace gsa
