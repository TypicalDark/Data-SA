#include "app.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace gsa {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---- design tokens (v7 prototype, A2 Control Room)
ImVec4 col(const char* h, float a = 1.0f) {
  const auto c = style::hex(h, a);
  return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a);
}
ImU32 u32(const char* h, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(col(h, a)); }
ImU32 u32(style::Rgba c) { return IM_COL32(c.r, c.g, c.b, c.a); }
const char* kChrome = "#141b22";
const char* kChrome2 = "#1a222b";
const char* kLine = "#27313b";
const char* kLine2 = "#36414d";
const char* kInk = "#e6edf3";
const char* kInk2 = "#9aa7b4";
const char* kInk3 = "#6b7886";
const char* kAccent = "#35d0f0";
const char* kWarn = "#ffb020";

double now_s() { return glfwGetTime(); }

std::string fmt_num(double n) {
  char b[64];
  if (n >= 1e9) std::snprintf(b, sizeof b, "%.1f bn", n / 1e9);
  else if (n >= 1e7) std::snprintf(b, sizeof b, "%.0f m", n / 1e6);
  else if (n >= 1e6) std::snprintf(b, sizeof b, "%.1f m", n / 1e6);
  else if (n >= 1e4) std::snprintf(b, sizeof b, "%.0f k", n / 1e3);
  else std::snprintf(b, sizeof b, "%.0f", n);
  return b;
}
std::string fmt_rand(double n) { return "R " + fmt_num(n); }

const char* kind_colour(const std::string& k) {
  if (k == "PROV") return "#8794a1";
  if (k == "MUNI") return "#c98bd0";
  if (k == "WARD") return "#2e9e4f";
  if (k == "SUB") return "#e0302d";
  if (k == "PLANT") return "#e0a030";
  if (k == "TOWN") return "#cfd7df";
  if (k == "PLOT") return "#de4968";
  return "#6b7886";
}

// Small coloured type tag, like the prototype's .ytag
void type_tag(const std::string& kind) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 ts = ImGui::CalcTextSize(kind.c_str());
  const float pad = 4;
  ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ts.x + 2 * pad, p.y + ts.y + 2), u32(kind_colour(kind)), 3);
  ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + pad, p.y + 1), u32("#0b0f13"), kind.c_str());
  ImGui::Dummy(ImVec2(ts.x + 2 * pad, ts.y + 2));
}

std::optional<double> metric(const json& area, const std::string& name) {
  if (!area.contains("metrics")) return std::nullopt;
  for (const auto& m : area["metrics"])
    if (m.value("metric", "") == name) return m["v"].get<double>();  // newest first per metric
  return std::nullopt;
}

style::Topic topic_by_name(const std::string& n) {
  for (const auto& t : style::topics()) {
    std::string a = t.name, b = n;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    if (a.rfind(b, 0) == 0) return t.id;
  }
  return style::Topic::Overview;
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
  init_imgui();

  // what the back-end serves decides what the map can draw
  std::string err;
  if (auto v = api_->get_json("/v1/version", &err)) version_ = *v;
  else backend_error_ = "Back-end not reachable: " + err;
  if (auto packs = api_->get_json("/v1/packs", &err)) {
    for (const auto& p : *packs) {
      if (p.value("kind", "") != "tiles" || p.value("tile_type", "") != "mvt") continue;
      const std::string id = p["id"];
      if (id == "admin" || id == "grid") packs_.push_back({id, p.value("minzoom", 0), p.value("maxzoom", 14)});
    }
    std::sort(packs_.begin(), packs_.end(), [](const auto& a, const auto& b) { return a.pack < b.pack; });
  }

  if (!opt_.topic.empty()) set_topic(topic_by_name(opt_.topic));
  if (opt_.lon && opt_.lat) {
    cam_.cx = geo::mx(*opt_.lon);
    cam_.cy = geo::my(*opt_.lat);
  }
  if (opt_.zoom) cam_.zoom = *opt_.zoom;
  if (!opt_.lon) {
    // centre the country in the part of the window the panel leaves free
    cam_.w = opt_.width;
    cam_.cx -= panel_width() / 2 / cam_.scale();
  }
  if (!opt_.select.empty()) open_area(opt_.select);
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
  const float ui = std::max(1.0f, sx);

  const fs::path fonts = fs::path(opt_.assets_dir) / "fonts";
  auto load = [&](const char* file, float px) -> ImFont* {
    const auto p = fonts / file;
    if (fs::exists(p)) return io.Fonts->AddFontFromFileTTF(p.string().c_str(), px * ui);
    return nullptr;
  };
  font_ui_ = load("SpaceGrotesk-Regular.ttf", 14.0f);
  if (!font_ui_) {
    std::fprintf(stderr, "fonts not found in %s, using the built-in font\n", fonts.string().c_str());
    ImFontConfig c;
    c.SizePixels = 13.0f * ui;
    font_ui_ = io.Fonts->AddFontDefault(&c);
  }
  font_mono_ = load("IBMPlexMono-Regular.ttf", 12.0f);
  font_title_ = load("SpaceGrotesk-Medium.ttf", 17.0f);
  if (!font_mono_) font_mono_ = font_ui_;
  if (!font_title_) font_title_ = font_ui_;
  io.FontGlobalScale = 1.0f / ui;  // sizes above are in physical pixels

  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 8;
  s.FrameRounding = 6;
  s.PopupRounding = 8;
  s.ScrollbarRounding = 6;
  s.GrabRounding = 4;
  s.WindowBorderSize = 1;
  s.FrameBorderSize = 1;
  s.WindowPadding = ImVec2(12, 10);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(8, 6);
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = col(kChrome);
  c[ImGuiCol_PopupBg] = col(kChrome);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_Border] = col(kLine2);
  c[ImGuiCol_Text] = col(kInk);
  c[ImGuiCol_TextDisabled] = col(kInk3);
  c[ImGuiCol_FrameBg] = col(kChrome);
  c[ImGuiCol_FrameBgHovered] = col(kChrome2);
  c[ImGuiCol_FrameBgActive] = col(kChrome2);
  c[ImGuiCol_Button] = col(kChrome2);
  c[ImGuiCol_ButtonHovered] = col("#222c37");
  c[ImGuiCol_ButtonActive] = col("#2a3642");
  c[ImGuiCol_Header] = col(kChrome2);
  c[ImGuiCol_HeaderHovered] = col("#222c37");
  c[ImGuiCol_HeaderActive] = col("#2a3642");
  c[ImGuiCol_Separator] = col(kLine);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = col(kLine2);
  c[ImGuiCol_NavHighlight] = col(kAccent);
  c[ImGuiCol_PlotHistogram] = col(kAccent);
  c[ImGuiCol_CheckMark] = col(kAccent);
  c[ImGuiCol_TextSelectedBg] = col(kAccent, 0.35f);

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

  for (auto& t : loader_->take()) map_->add(t);
  auto need = map_->needed(cam_, packs_);
  if (need != last_need_) {
    loader_->want(need);
    last_need_ = need;
  }
  poll_async();
  map_->set_topic(topic_);

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  handle_input();
  blockers_.clear();
  ui();
  ImGui::Render();

  // labels go where the interface left room; screenshots skip the fades
  const double now = now_s();
  const double dt = opt_.screenshot.empty() ? std::min(0.1, now - last_frame_t_) : -1.0;
  last_frame_t_ = now;
  map_->prepare(cam_, packs_, blockers_, dt);

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
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vk_->cmd());

  // scripted screenshot: wait until no tiles are missing or in flight and areas have loaded
  const bool idle = need.empty() && loader_->in_flight() == 0 && area_req_.empty() && !fly_ && !map_->labels_animating();
  settled_frames_ = idle ? settled_frames_ + 1 : 0;
  const bool shoot = !opt_.screenshot.empty() && (settled_frames_ > 8 || (opt_.max_frames && frames_ >= opt_.max_frames));
  if (shoot) {
    std::vector<uint8_t> px;
    vk_->end_frame(&px);
    const auto e = vk_->extent();
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    if (!stbi_write_png(opt_.screenshot.c_str(), int(e.width), int(e.height), 4, px.data(), int(e.width * 4)))
      std::fprintf(stderr, "cannot write %s\n", opt_.screenshot.c_str());
    else
      std::fprintf(stderr, "wrote %s (%ux%u, %zu tiles, %zu vertices)\n", opt_.screenshot.c_str(), e.width, e.height,
                   map_->tile_count(), map_->vertex_count());
    glfwSetWindowShouldClose(win_, GLFW_TRUE);
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
  }
  const std::string q = query_;
  if (q != last_query_ && !search_req_.valid()) {
    last_query_ = q;
    if (q.size() >= 2) {
      const auto ep = opt_.endpoint;
      search_req_ = std::async(std::launch::async, [ep, q]() {
        api::Client c(ep);
        return c.search(q, 10);
      });
    } else {
      hits_.clear();
    }
  }
}

// ------------------------------------------------------------------ actions

void App::set_topic(style::Topic t) {
  topic_ = t;
  topic_menu_ = false;
  if (stack_.size() == 1) panel_open_ = true;
}

void App::open_area(const std::string& id, bool fly) {
  if (stack_.back().kind != Page::Area || stack_.back().id != id) stack_.push_back({Page::Area, id});
  panel_open_ = true;
  fly_on_load_ = fly ? id : std::string();
  if (fly && areas_.count(id) && areas_[id].contains("bbox")) {
    const auto& b = areas_[id]["bbox"];
    fly_to_bbox(b[0], b[1], b[2], b[3]);
    fly_on_load_.clear();
  }
  if (!areas_.count(id) && !area_req_.count(id)) area_req_[id] = fetch("/v1/areas/" + api::url_encode(id));
}

float App::panel_width() const { return panel_open_ ? 372.0f : 0.0f; }

void App::fly_to(double lon, double lat, double zoom) {
  // centre the target in the part of the screen the panel does not cover
  geo::Camera to = cam_;
  to.zoom = zoom;
  const double off = panel_width() / 2 / to.scale();
  fly_ = Fly{now_s(), 0.7, cam_, geo::mx(lon) - off, geo::my(lat), zoom};
}

void App::fly_to_bbox(double lon0, double lat0, double lon1, double lat1, double max_zoom) {
  const double w = std::max(200.0, cam_.w - panel_width() - 120), h = std::max(200.0, cam_.h - 160);
  const double dx = std::max(1e-6, geo::mx(lon1) - geo::mx(lon0)), dy = std::max(1e-6, geo::my(lat0) - geo::my(lat1));
  const double z = std::clamp(std::log2(std::min(w / (dx * geo::kTilePx), h / (dy * geo::kTilePx))), 3.0, max_zoom);
  fly_to((lon0 + lon1) / 2, geo::lat_of((geo::my(lat0) + geo::my(lat1)) / 2), z);
}

// ------------------------------------------------------------------ input

void App::handle_input() {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  if (!io.WantCaptureMouse) {
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
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      fly_.reset();
      cam_.zoom_at(1.0, m.x, m.y);
    }
    // hover: name of the municipality under the cursor
    double wx, wy;
    cam_.to_world(m.x, m.y, wx, wy);
    const auto* f = map_->pick(wx, wy, "municipality", cam_.zoom);
    hover_name_ = f ? f->str("name") : "";
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
          cam_.pan_px(dx, dy);
        }
      }
      drag_x_ = m.x;
      drag_y_ = m.y;
    } else {
      dragging_ = false;
      if (!drag_moved_ && !io.WantCaptureMouse) {
        double wx, wy;
        cam_.to_world(m.x, m.y, wx, wy);
        const auto* f = map_->pick(wx, wy, "municipality", cam_.zoom);
        if (f) open_area(f->str("id"), false);
      }
    }
  }

  if (!io.WantTextInput) {
    for (const auto& t : style::topics())
      if (ImGui::IsKeyPressed(ImGuiKey(int(ImGuiKey_1) + int(t.id)), false)) set_topic(t.id);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) cam_.zoom_at(0.5, cam_.w / 2, cam_.h / 2);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) cam_.zoom_at(-0.5, cam_.w / 2, cam_.h / 2);
    if (ImGui::IsKeyPressed(ImGuiKey_0, false)) fly_to(25.2, -28.8, 5.4);
    if (ImGui::IsKeyPressed(ImGuiKey_Slash, false)) focus_search_ = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) panel_open_ = !panel_open_;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
      if (stack_.size() > 1) stack_.pop_back();
    }
    const double step = 120;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) cam_.pan_px(step * io.DeltaTime * 6, 0);
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) cam_.pan_px(-step * io.DeltaTime * 6, 0);
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) cam_.pan_px(0, step * io.DeltaTime * 6);
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) cam_.pan_px(0, -step * io.DeltaTime * 6);
  }
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) focus_search_ = true;
}

// ------------------------------------------------------------------ UI

void App::ui() {
  ui_top();
  if (panel_open_) ui_panel();
  ui_key();
  ui_status();
  if (!hover_name_.empty() && !dragging_ && opt_.screenshot.empty()) {
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(hover_name_.c_str());
    ImGui::EndTooltip();
  }
  if (!toast_.empty() && now_s() < toast_until_) {
    ImGui::SetNextWindowPos(ImVec2(float(cam_.w) / 2, float(cam_.h) - 44), ImGuiCond_Always, ImVec2(0.5f, 1));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(toast_.c_str());
    block_window();
    ImGui::End();
  }
}

void App::block_window() {
  // the map keeps its labels out of every interface window
  const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
  blockers_.push_back({p.x - 4, p.y - 4, p.x + sz.x + 4, p.y + sz.y + 4});
}

void App::ui_top() {
  const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground;
  // topic switcher
  ImGui::SetNextWindowPos(ImVec2(10, 10));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##topic", nullptr, fl);
  const auto& ti = style::info(topic_);
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    std::string label = std::string("     ") + ti.name + "  v";
    ImGui::PushStyleColor(ImGuiCol_Button, col(kChrome));
    if (ImGui::Button(label.c_str(), ImVec2(0, 36))) topic_menu_ = !topic_menu_;
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 12, p.y + 13), ImVec2(p.x + 22, p.y + 23), u32(ti.swatch), 3);
  }
  block_window();
  ImGui::End();
  ImGui::PopStyleVar();

  if (topic_menu_) {
    ImGui::SetNextWindowPos(ImVec2(10, 52));
    ImGui::SetNextWindowSize(ImVec2(340, 0));
    ImGui::Begin("##topicmenu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    for (const auto& t : style::topics()) {
      ImGui::PushID(int(t.id));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##t", t.id == topic_, 0, ImVec2(0, 40))) set_topic(t.id);
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 4, p.y + 6), ImVec2(p.x + 14, p.y + 16), u32(t.swatch), 3);
      ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 24, p.y + 2), u32(kInk), t.name);
      ImGui::GetWindowDrawList()->AddText(font_mono_, 12, ImVec2(p.x + 300, p.y + 4), u32(kInk3), t.key);
      ImGui::PushClipRect(p, ImVec2(p.x + 290, p.y + 40), true);
      ImGui::GetWindowDrawList()->AddText(font_ui_, 12, ImVec2(p.x + 24, p.y + 21), u32(kInk2), t.blurb);
      ImGui::PopClipRect();
      ImGui::PopID();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) topic_menu_ = false;
    block_window();
    ImGui::End();
  }

  // search box, centred
  const float sw = std::min(440.0f, float(cam_.w) - 420);
  ImGui::SetNextWindowPos(ImVec2(float(cam_.w) / 2 - sw / 2, 10));
  ImGui::SetNextWindowSize(ImVec2(sw, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##search", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 10));
  ImGui::SetNextItemWidth(sw);
  if (focus_search_) {
    ImGui::SetKeyboardFocusHere();
    focus_search_ = false;
  }
  const bool enter = ImGui::InputTextWithHint("##q", "Search places, areas, substations  ( / )", query_, sizeof query_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  const bool active = ImGui::IsItemActive() || ImGui::IsItemFocused();
  ImGui::PopStyleVar(2);
  block_window();
  ImGui::End();
  ImGui::PopStyleVar();

  auto choose = [&](const api::SearchHit& h) {
    if (h.ref.rfind("area:", 0) == 0) open_area(h.ref.substr(5));
    else if (h.lon && h.lat) fly_to(*h.lon, *h.lat, std::max(cam_.zoom, 9.0));
    query_[0] = 0;
    hits_.clear();
    ImGui::SetWindowFocus(nullptr);
  };
  if (!hits_.empty() && query_[0]) {
    if (active) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) hit_sel_ = std::min(hit_sel_ + 1, int(hits_.size()) - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) hit_sel_ = std::max(hit_sel_ - 1, 0);
    }
    if (enter) {
      choose(hits_[size_t(hit_sel_)]);
      return;
    }
    ImGui::SetNextWindowPos(ImVec2(float(cam_.w) / 2 - sw / 2, 52));
    ImGui::SetNextWindowSize(ImVec2(sw, 0));
    ImGui::Begin("##hits", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing);
    for (size_t i = 0; i < hits_.size(); ++i) {
      const auto& h = hits_[i];
      ImGui::PushID(int(i));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##h", int(i) == hit_sel_, 0, ImVec2(0, 34))) {
        choose(h);
        ImGui::PopID();
        break;
      }
      ImGui::SetCursorScreenPos(ImVec2(p.x + 4, p.y + 2));
      ImGui::PushFont(font_mono_);
      type_tag(h.kind);
      ImGui::PopFont();
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddText(ImVec2(p.x + 64, p.y), u32(kInk), h.label.c_str());
      dl->AddText(font_ui_, 12, ImVec2(p.x + 64, p.y + 18), u32(kInk3), h.detail.c_str());
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 38));
      ImGui::PopID();
    }
    block_window();
    ImGui::End();
  } else if (enter && query_[0] && hits_.empty()) {
    toast_ = "Nothing found for that search";
    toast_until_ = now_s() + 2.5;
  }

  // right: panel toggle
  ImGui::SetNextWindowPos(ImVec2(float(cam_.w) - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##topr", nullptr, fl);
  if (ImGui::Button(panel_open_ ? "Hide panel" : "Panel", ImVec2(0, 36))) panel_open_ = !panel_open_;
  block_window();
  ImGui::End();
  ImGui::PopStyleVar();
}

void App::ui_panel() {
  ImGui::SetNextWindowPos(ImVec2(10, 56));
  const bool area = stack_.back().kind == Page::Area;
  if (area) ImGui::SetNextWindowSize(ImVec2(360, std::max(200.0f, float(cam_.h) - 100)));
  else ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(360, std::max(200.0f, float(cam_.h) - 100)));
  ImGui::Begin("##panel", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | (area ? 0 : ImGuiWindowFlags_AlwaysAutoResize));
  const Page page = stack_.back();
  if (stack_.size() > 1) {
    if (ImGui::SmallButton("< Back")) {
      stack_.pop_back();
      block_window();
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Esc");
  }
  if (page.kind == Page::TopicHome) ui_topic_home();
  else ui_area(page.id);
  block_window();
  ImGui::End();
}

void App::ui_topic_home() {
  const auto& t = style::info(topic_);
  ImGui::PushFont(font_mono_);
  ImGui::TextColored(col(kInk3), "TOPIC %s", t.key);
  ImGui::PopFont();
  ImGui::PushFont(font_title_);
  ImGui::TextUnformatted(t.name);
  ImGui::PopFont();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(col(kInk2), "%s", t.blurb);
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  if (t.pack_needed[0]) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(kWarn));
    ImGui::PushTextWrapPos(0);
    ImGui::Text("The %s data pack is not installed in this build yet. It comes with the data pack step.", t.pack_needed);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::PushFont(font_mono_);
  ImGui::TextColored(col(kInk3), "START FROM");
  ImGui::PopFont();
  if (ImGui::Selectable("South Africa", false)) open_area("ZA");
  if (areas_.count("ZA") && areas_["ZA"].contains("children")) {
    for (const auto& c : areas_["ZA"]["children"]) {
      const std::string id = c["id"], name = c["name"];
      if (ImGui::Selectable(("   " + name + "##" + id).c_str(), false)) open_area(id);
    }
  } else if (!area_req_.count("ZA")) {
    area_req_["ZA"] = fetch("/v1/areas/ZA");
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(col(kInk3), "Click a municipality to open it. Keys 1 to 7 switch topic, / searches, Tab hides this panel.");
  ImGui::PopTextWrapPos();
}

void App::ui_area(const std::string& id) {
  auto it = areas_.find(id);
  if (it == areas_.end()) {
    ImGui::TextColored(col(kInk2), "Loading %s ...", id.c_str());
    return;
  }
  const json& a = it->second;
  if (a.contains("error")) {
    ImGui::TextColored(col(kWarn), "%s", a["error"].get<std::string>().c_str());
    return;
  }
  // breadcrumb
  if (a.contains("breadcrumb")) {
    bool first = true;
    for (const auto& b : a["breadcrumb"]) {
      if (!first) {
        ImGui::SameLine(0, 4);
        ImGui::TextColored(col(kInk3), ">");
        ImGui::SameLine(0, 4);
      }
      first = false;
      ImGui::PushStyleColor(ImGuiCol_Text, col(kInk3));
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 1));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
      const std::string bid = b["id"];
      if (ImGui::SmallButton((b["name"].get<std::string>() + "##bc" + bid).c_str())) open_area(bid);
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }
  }
  ImGui::PushFont(font_mono_);
  type_tag(a.value("kind", "AREA"));
  ImGui::SameLine();
  ImGui::TextColored(col(kInk3), "%s", id.c_str());
  ImGui::PopFont();
  ImGui::PushFont(font_title_);
  ImGui::TextWrapped("%s", a.value("name", id).c_str());
  ImGui::PopFont();

  // facts
  const auto pop = metric(a, "pop_est");
  const double km2 = a.contains("area_km2") && a["area_km2"].is_number() ? a["area_km2"].get<double>() : 0;
  const auto val = metric(a, "value_median");
  const auto parcels = metric(a, "parcels");
  struct Fact {
    std::string n, l;
  };
  std::vector<Fact> facts;
  if (pop) facts.push_back({fmt_num(*pop), "people"});
  if (pop && km2 > 0) facts.push_back({fmt_num(*pop / km2), "people per km2"});
  if (val) facts.push_back({fmt_rand(*val), "median value"});
  if (parcels) facts.push_back({fmt_num(*parcels), "valued properties"});
  if (km2 > 0) facts.push_back({fmt_num(km2), "km2"});
  if (!facts.empty()) {
    ImGui::Spacing();
    const float w = (ImGui::GetContentRegionAvail().x - 16) / 3;
    for (size_t i = 0; i < facts.size() && i < 6; ++i) {
      if (i % 3) ImGui::SameLine(0, 8);
      ImGui::PushStyleColor(ImGuiCol_ChildBg, col(kChrome2));
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6);
      ImGui::BeginChild(int(100 + i), ImVec2(w, 52), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
      ImGui::SetCursorPos(ImVec2(8, 6));
      ImGui::PushFont(font_title_);
      ImGui::TextUnformatted(facts[i].n.c_str());
      ImGui::PopFont();
      ImGui::SetCursorPosX(8);
      ImGui::TextColored(col(kInk2), "%s", facts[i].l.c_str());
      ImGui::EndChild();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
    }
  }

  // function strip (Bloomberg-style subject + function model)
  ImGui::Spacing();
  struct Fn {
    const char* code;
    const char* name;
    bool ready;
  };
  static const Fn fns[] = {{"GO", "go to", true},      {"LIST", "list", true},     {"COPY", "copy id", true},
                           {"DES", "describe", false}, {"CHK", "check", false},    {"NEAR", "nearby", false},
                           {"RANK", "rank", false},    {"CMP", "compare", false},  {"MEAS", "measure", false}};
  ImGui::PushFont(font_mono_);
  float x = 0;
  const float avail = ImGui::GetContentRegionAvail().x;
  for (const auto& f : fns) {
    const std::string label = std::string(f.code) + " " + f.name;
    const float bw = ImGui::CalcTextSize(label.c_str()).x + 16;
    if (x > 0 && x + bw > avail) x = 0;
    else if (x > 0) ImGui::SameLine(0, 4);
    x += bw + 4;
    ImGui::BeginDisabled(!f.ready);
    if (ImGui::Button(label.c_str())) {
      const std::string c = f.code;
      if (c == "GO" && a.contains("bbox")) {
        const auto& b = a["bbox"];
        fly_to_bbox(b[0], b[1], b[2], b[3]);
      } else if (c == "COPY") {
        glfwSetClipboardString(win_, id.c_str());
        toast_ = "Copied " + id;
        toast_until_ = now_s() + 2;
      } else if (c == "LIST") {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndDisabled();
    if (!f.ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Comes in a later build");
  }
  ImGui::PopFont();

  // children list with bars, coloured by the current topic's measure
  if (a.contains("children") && !a["children"].empty()) {
    ImGui::Spacing();
    ImGui::Separator();
    const char* measure = topic_ == style::Topic::Property ? "value_median" : "pop_est";
    double top = 1;
    for (const auto& c : a["children"]) top = std::max(top, c["metrics"].value(measure, 0.0));
    ImGui::PushFont(font_mono_);
    ImGui::TextColored(col(kInk3), "%zu %s", a["children"].size(), measure[0] == 'v' ? "BY MEDIAN VALUE" : "BY PEOPLE");
    ImGui::PopFont();
    std::vector<const json*> kids;
    for (const auto& c : a["children"]) kids.push_back(&c);
    std::sort(kids.begin(), kids.end(), [&](const json* p, const json* q) {
      return (*p)["metrics"].value(measure, -1.0) > (*q)["metrics"].value(measure, -1.0);
    });
    ImGui::BeginChild("kids", ImVec2(0, 0));
    for (const json* c : kids) {
      const std::string cid = (*c)["id"], name = (*c)["name"];
      const bool has = (*c)["metrics"].contains(measure);
      const double v = (*c)["metrics"].value(measure, 0.0);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      if (ImGui::Selectable(("##" + cid).c_str(), false, 0, ImVec2(0, 30))) open_area(cid);
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddText(ImVec2(p.x + 4, p.y), u32(kInk), name.c_str());
      const std::string vs = !has ? "no data" : measure[0] == 'v' ? fmt_rand(v) : fmt_num(v);
      const float vw = font_mono_->CalcTextSizeA(12, FLT_MAX, 0, vs.c_str()).x;
      dl->AddText(font_mono_, 12, ImVec2(p.x + w - vw - 4, p.y + 2), u32(kInk2), vs.c_str());
      dl->AddRectFilled(ImVec2(p.x + 4, p.y + 22), ImVec2(p.x + w - 4, p.y + 25), u32(kLine), 2);
      dl->AddRectFilled(ImVec2(p.x + 4, p.y + 22), ImVec2(p.x + 4 + float((w - 8) * v / top), p.y + 25),
                        u32(style::info(topic_).swatch), 2);
    }
    ImGui::EndChild();
  }
}

void App::ui_key() {
  const auto lg = style::legend(topic_);
  const float w = 252;
  ImGui::SetNextWindowPos(ImVec2(float(cam_.w) - 10, float(cam_.h) - 38), ImGuiCond_Always, ImVec2(1, 1));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::Begin("##key", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PushFont(font_mono_);
  ImGui::TextColored(col(kInk3), "KEY  ON THE MAP");
  ImGui::PopFont();
  if (lg.items.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col(kInk2), topic_ == style::Topic::Overview ? "Nothing coloured. Pick a topic (1 to 7)."
                                                                     : "No layer for this topic in the installed packs.");
    ImGui::PopTextWrapPos();
  } else {
    ImGui::TextUnformatted(lg.title.c_str());
    for (const auto& it : lg.items) {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 10, p.y + 14), u32(it.colour), 2);
      ImGui::SetCursorScreenPos(ImVec2(p.x + 18, p.y));
      ImGui::TextColored(col(kInk2), "%s", it.label.c_str());
    }
    if (!lg.note.empty()) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(col(kInk3), "%s", lg.note.c_str());
      ImGui::PopTextWrapPos();
    }
    if (topic_ != style::Topic::Energy && topic_ != style::Topic::Overview) {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      auto* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 10, p.y + 14), u32("#232b35"), 2);
      ImGui::SetCursorScreenPos(ImVec2(p.x + 18, p.y));
      ImGui::TextColored(col(kInk2), "no data yet");
    }
  }
  block_window();
  ImGui::End();
}

void App::ui_status() {
  ImGui::SetNextWindowPos(ImVec2(0, float(cam_.h) - 28));
  ImGui::SetNextWindowSize(ImVec2(float(cam_.w), 28));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 6));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col("#0b0f13", 0.88f));
  ImGui::PushStyleColor(ImGuiCol_Border, col(kLine));
  ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);
  ImGui::PushFont(font_mono_);
  const ImVec2 m = ImGui::GetIO().MousePos;
  double wx, wy;
  cam_.to_world(m.x, m.y, wx, wy);
  ImGui::TextColored(col(kInk2), "z %.1f   %.3f, %.3f", cam_.zoom, geo::lat_of(wy), geo::lon_of(wx));
  ImGui::SameLine(0, 24);
  // scale bar: km per 100 px at the cursor latitude
  const double km = 100 / cam_.scale() * 40075.0 * std::cos(geo::lat_of(wy) * geo::kPi / 180);
  ImGui::TextColored(col(kInk2), "100 px = %s km", km >= 10 ? fmt_num(km).c_str() : (std::to_string(km).substr(0, 4)).c_str());
  std::string right;
  if (!backend_error_.empty()) right = backend_error_;
  else {
    right = "back-end " + version_.value("backend", "?") + "  api v" + version_.value("api", "?") + "  packs";
    if (version_.contains("packs"))
      for (const auto& [k, v] : version_["packs"].items()) right += " " + k + "@" + v.get<std::string>();
    right += "   " + vk_->device_name();
    if (loader_->in_flight()) right += "   loading " + std::to_string(loader_->in_flight());
  }
  const float rw = ImGui::CalcTextSize(right.c_str()).x;
  ImGui::SameLine(std::max(0.0f, float(cam_.w) - rw - 24));
  ImGui::TextColored(backend_error_.empty() ? col(kInk3) : col(kWarn), "%s", right.c_str());
  ImGui::PopFont();
  block_window();
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}

}  // namespace gsa
