// The desktop app: window, input, map and the v7 UI (topics, search, one panel, key, status bar).
#pragma once

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api_client.hpp"
#include "geo.hpp"
#include "map_renderer.hpp"
#include "style.hpp"
#include "tiles.hpp"
#include "vk_context.hpp"

struct GLFWwindow;
struct ImFont;

namespace gsa {

struct AppOptions {
  api::Endpoint endpoint;
  int width = 1600, height = 1000;
  bool validation = false;
  std::string assets_dir;
  // scripted runs (screenshots, smoke tests)
  std::string screenshot;      // write a PNG here once tiles are loaded, then quit
  std::string topic;           // start topic, by name
  std::string select;          // area id to open, for example WC053
  std::optional<double> lon, lat, zoom;
  int max_frames = 0;          // quit after this many frames (0 = run until closed)
};

class App {
 public:
  explicit App(AppOptions opt);
  ~App();
  int run();

 private:
  struct Page {
    enum Kind { TopicHome, Area } kind = TopicHome;
    std::string id;  // area id
  };
  using JsonFuture = std::future<std::optional<nlohmann::json>>;

  void init_imgui();
  void frame();
  void ui();
  void ui_top();
  void ui_panel();
  void ui_topic_home();
  void ui_area(const std::string& id);
  void ui_key();
  void ui_status();
  void block_window();
  void handle_input();
  void poll_async();

  void set_topic(style::Topic t);
  void open_area(const std::string& id, bool fly = true);
  void fly_to_bbox(double lon0, double lat0, double lon1, double lat1, double max_zoom = 11);
  void fly_to(double lon, double lat, double zoom);
  JsonFuture fetch(const std::string& path);
  float panel_width() const;

  AppOptions opt_;
  GLFWwindow* win_ = nullptr;
  std::unique_ptr<vk::Context> vk_;
  std::unique_ptr<MapRenderer> map_;
  std::unique_ptr<TileLoader> loader_;
  std::unique_ptr<api::Client> api_;
  VkDescriptorPool imgui_pool_ = VK_NULL_HANDLE;
  ImFont* font_ui_ = nullptr;
  ImFont* font_mono_ = nullptr;
  ImFont* font_title_ = nullptr;

  geo::Camera cam_;
  struct Fly {
    double t0, dur;
    geo::Camera from;
    double cx, cy, zoom;
  };
  std::optional<Fly> fly_;
  bool dragging_ = false, drag_moved_ = false;
  double drag_x_ = 0, drag_y_ = 0;
  std::vector<PackZoom> packs_;
  nlohmann::json version_;
  std::string backend_error_;

  style::Topic topic_ = style::Topic::Overview;
  bool topic_menu_ = false;
  bool panel_open_ = true;
  std::vector<Page> stack_{Page{}};
  std::map<std::string, nlohmann::json> areas_;
  std::map<std::string, JsonFuture> area_req_;

  char query_[200] = {};
  std::string last_query_;
  std::future<std::vector<api::SearchHit>> search_req_;
  std::vector<api::SearchHit> hits_;
  int hit_sel_ = 0;
  bool focus_search_ = false;

  std::string hover_name_;
  std::string toast_;
  double toast_until_ = 0;
  double last_frame_t_ = 0;
  std::vector<text::Rect> blockers_;  // interface windows this frame, logical pixels
  int frames_ = 0;
  int settled_frames_ = 0;
  std::vector<TileKey> last_need_;
  std::string fly_on_load_;
};

}  // namespace gsa
