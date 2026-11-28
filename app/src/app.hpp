// The desktop app: window, input, map and the website's interface (icon rail, docked sidebar
// with search and command line, cards with type tags and functions, the On the map list,
// breadcrumb, map buttons and status bar).
#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api_client.hpp"
#include "geo.hpp"
#include "find.hpp"
#include "measure.hpp"
#include "layers.hpp"
#include "map_renderer.hpp"
#include "settings.hpp"
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
  std::string settings_path;   // empty = the per-user default
  // scripted runs (screenshots, smoke tests)
  std::string screenshot;      // write a PNG here once tiles are loaded, then quit
  std::string topic;           // start topic: start grid vote water prop land places move nature risk (or old names)
  std::string select;          // area id to open, for example WC053
  std::string panel;           // rail panel to open: find rank list areas views watch keys
  std::string command;         // command lines to run at start, separated by ";" ("apollo DES; KEEP")
  std::optional<double> lon, lat, zoom;
  std::optional<double> pitch;  // tilt in degrees (3D), 0 to 60
  std::optional<std::pair<double, double>> spot;  // right-click spot to open (lon, lat)
  std::string measure;  // "area:lon,lat,lon,lat,...": a finished measurement (for screenshots)
  int max_frames = 0;          // quit after this many frames (0 = run until closed)
};

class App {
 public:
  explicit App(AppOptions opt);
  ~App();
  int run();

 private:
  struct Card {
    enum Kind { Home, Area, Facility, Spot, Near } kind = Home;  // Near: props km, label
    std::string id;           // area id, or facility id
    nlohmann::json props;     // facility properties
    double lon = 0, lat = 0;  // facility or spot position
    int tab = 0;
  };
  enum class Panel { None, Find, List, Areas, Views, Watch, Keys, Measure, Filter, Layers, Live, Reach, Analyse, History };
  struct Row {  // one row of a list or ranking
    std::string name, value, sub;
    double x = 0;     // sort key / bar
    uint32_t colour = 0;
    double lon = 0, lat = 0, zoom = 0;
    std::string area;  // open this area instead of flying
    int pick = -1;     // index into map facilities, to open its card
  };
  using JsonFuture = std::future<std::optional<nlohmann::json>>;

  // setup and loop
  void init_imgui();
  void frame();
  void handle_input();
  void poll_async();
  JsonFuture fetch(const std::string& path);

  // interface
  void ui();
  void ui_rail();
  void ui_side();
  void ui_search(float x, float y, float w);
  void ui_main(float x, float y, float w, float h);
  void ui_dock(float x, float y, float w, float h);
  void ui_catalogue();
  void ui_crumb();
  void ui_controls();
  void ui_status();
  void ui_measure();
  void ui_toast();
  void block_window();
  // cards
  void ui_home();
  void ui_area(const std::string& id);
  void ui_facility(Card& c);
  void ui_spot(Card& c);
  void card_head(const char* tag, const char* tag_colour, const char* kind, const std::string& title,
                 const std::string& sub, const std::string& data_line);
  void fn_strip(const std::vector<std::string>& codes);
  void kv_row(const char* k, const std::string& v);
  bool close_button();
  // panels
  void ui_panel_find();
  void ui_panel_list(bool rank);
  void ui_panel_areas();
  void ui_panel_views();
  void ui_panel_watch();
  void ui_panel_keys();
  void ui_panel_later(const char* title, const char* what, const char* milestone);
  void rows_list(std::vector<Row>& rows, const char* id, float height);
  std::vector<Row> rows_in_view(const std::string& layer);
  std::vector<const nlohmann::json*> all_munis();  // every municipality, loading provinces as needed
  const nlohmann::json* area_data(const std::string& id);  // loaded area, or nullptr (and a request)

  // actions
  void set_topic(const std::string& id);
  void apply_layers();
  void open_area(const std::string& id, bool fly = true);
  void open_facility(const map_labels::FeatureRef& r);
  void open_web(const MapRenderer::WebHit& h);
  void open_spot(double lon, double lat);
  void open_panel(Panel p);
  void close_top();
  bool run_command(const std::string& line);
  void run_fn(const std::string& code);
  void save_view(const std::string& name);
  bool load_view(const std::string& name);
  void watch_current();

  // ---- website v70+ functions (app_extra.cpp)
  // KEEP: up to two cards stay open above the live card
  struct Kept {
    Card card;
    std::string title, kind;
    bool open = true;
  };
  std::vector<Kept> kept_;
  bool in_kept_ = false;  // drawing a kept card: no close button or function strip
  void keep_current();
  bool is_kept(const Card& c) const;
  void ui_kept();
  // Compare: up to four municipalities side by side
  std::vector<std::string> cmp_;
  void cmp_add(const std::string& code);
  void ui_compare();
  std::string current_muni_code() const;  // the open municipality card's code, or ""
  // undo / redo over layers, filters, Find conditions, Compare and kept cards
  struct Snap {
    layers::State l;
    layers::Filters f;
    std::vector<find::Rule> rules;
    std::vector<std::string> cmp;
    std::vector<std::string> kept;  // titles only: kept cards are restored by title from `kept_all`
    std::string label;
    double t = 0;
  };
  std::vector<Snap> undo_, redo_;
  std::optional<Snap> last_snap_;
  std::vector<Kept> kept_all_;  // every card kept this session, for undo
  Snap snap_now() const;
  void track_changes();
  void restore(const Snap& s);
  void undo();
  void redo();
  void ui_panel_history();
  // one strip of every active filter, over the map
  void ui_filter_strip();
  // REPORT: an HTML page of the open card with the map, in the Documents folder
  void report();
  void report_now();
  bool report_ready() const;
  bool report_pending_ = false;
  std::function<void(const std::vector<uint8_t>&, uint32_t, uint32_t)> on_png_;
  // CHK / NEAR: the everyday answers around a point
  void ui_near(Card& c);
  void open_near(double lon, double lat, double km, const std::string& label);
  float side_w() const;  // the docked panel's width (wider while Compare shows 3 or 4 places)
  // HELP CODE
  std::string help_code_;
  // search that understands tasks ("unemployment", "dam levels", "compare")
  struct Task {
    std::string label, command, why;
  };
  std::vector<Task> tasks_for(const std::string& q) const;
  // ---- live planes and ships (app_live.cpp), from the back-end's /v1/live
  struct LiveItem {
    std::string kind, id, name;  // kind: plane or ship
    double lon = 0, lat = 0, alt = -1, spd = -1, hdg = -1, t = 0;  // m, m/s, degrees, unix seconds
    bool watched = false;
    nlohmann::json extra = nlohmann::json::object();
  };
  struct LiveFeed {
    bool on = false;
    std::vector<LiveItem> items;
    nlohmann::json sources;
    std::string etag, query, note;
    double next = 0, fetched = 0;
    std::future<api::Response> req;
  };
  struct LiveTrack {
    std::vector<std::array<double, 6>> fixes;  // t, lon, lat, alt, spd, hdg
    std::string name;
    double next = 0;
    std::future<api::Response> req;
  };
  LiveFeed planes_, ships_;
  std::map<std::string, LiveTrack> tracks_;  // "plane/abc123"
  std::vector<LiveItem> watched_;
  double watch_next_ = 0, status_next_ = 0;
  std::future<api::Response> watch_req_, status_req_;
  std::vector<std::future<api::Response>> live_sends_;
  nlohmann::json live_status_;
  std::future<api::Response> live_get(const std::string& path, const std::string& etag = "");
  void live_send(const std::string& method, const std::string& path, const std::string& body = "");
  static std::string live_key(const std::string& kind, const std::string& id);
  const LiveItem* live_find(const std::string& kind, const std::string& id) const;
  bool live_is_watched(const std::string& kind, const std::string& id) const;
  void live_set(bool planes, bool ships);
  void live_watch(const std::string& kind, const std::string& id, bool on, const std::string& label);
  void live_poll();
  void live_draw();
  const LiveItem* live_pick(double sx, double sy) const;
  void open_live(const LiveItem& it);
  void ui_live_card(Card& c);
  void ui_panel_live();
  // card strip: the functions that fit, the rest under More
  std::vector<std::string> card_lines(const Card& c);  // facts of a card as "label: value" (REPORT)
  void save_settings();
  void fly_to_bbox(double lon0, double lat0, double lon1, double lat1, double max_zoom = 11);
  void fly_to(double lon, double lat, double zoom);
  void toast(const std::string& s, double secs = 2.5);
  float side_width() const;  // rail plus panel, logical pixels
  bool subject_pos(double& lon, double& lat) const;
  std::string subject_name() const;
  std::string muni_name_at(double wx, double wy, std::string* prov = nullptr) const;
  void png();

  AppOptions opt_;
  GLFWwindow* win_ = nullptr;
  std::unique_ptr<vk::Context> vk_;
  std::unique_ptr<MapRenderer> map_;
  std::unique_ptr<TileLoader> loader_;
  std::unique_ptr<api::Client> api_;
  std::unique_ptr<settings::Store> settings_;
  VkDescriptorPool imgui_pool_ = VK_NULL_HANDLE;
  ImFont* font_ui_ = nullptr;     // body, 13.5
  ImFont* font_small_ = nullptr;  // 12
  ImFont* font_mono_ = nullptr;   // 11.5
  ImFont* font_title_ = nullptr;  // card title, 21
  ImFont* font_head_ = nullptr;   // section heads, 15

  geo::Camera cam_;
  struct Fly {
    double t0, dur;
    geo::Camera from;
    double cx, cy, zoom;
  };
  std::optional<Fly> fly_;
  double pitch_target_ = 0;  // the tilt eases towards this
  bool dragging_ = false, drag_moved_ = false;
  double drag_x_ = 0, drag_y_ = 0;
  std::vector<PackZoom> packs_;      // tile packs drawn now: admin, the web packs in use, grid
  std::vector<PackZoom> all_packs_;  // every vector tile pack the back-end serves
  std::map<std::string, std::vector<std::string>> pack_layers_;  // pack id -> vector layer names
  nlohmann::json version_;
  std::string backend_error_;

  layers::State lstate_;
  std::set<std::string> avail_;
  bool side_open_ = true;
  Panel panel_ = Panel::None;
  int find_tab_ = 0;  // Find panel tab: 0 Find, 1 Rank, 2 Compare
  // Find and Rank (find.hpp) over the Find data pack
  std::optional<find::Data> fdata_;
  JsonFuture fdata_req_;
  std::vector<find::Rule> rules_;
  std::set<std::string> fpass_;
  std::string rank_key_ = "subs";
  std::string find_toast_;
  layers::Filters filter_;
  bool filter_vote_tab_ = false;
  void set_filter(const layers::Filters& f);
  void ui_panel_filter();
  void set_rules(std::vector<find::Rule> r);
  layers::MapStyle map_style_now() const;
  char view_name_[64] = {};
  std::string list_layer_;
  std::vector<Card> cards_{Card{}};
  std::string group_tab_ = "on";
  bool catalogue_open_ = false;
  char cat_query_[64] = {};
  std::map<std::string, nlohmann::json> areas_;
  std::map<std::string, JsonFuture> area_req_;

  char query_[200] = {};
  std::string last_query_;
  std::future<std::vector<api::SearchHit>> search_req_;
  std::vector<api::SearchHit> hits_;
  int hit_sel_ = 0;
  bool focus_search_ = false;
  bool search_active_ = false;
  std::string pending_fn_;  // "name CODE": run CODE once the subject opens
  std::vector<std::string> history_;

  bool measuring_ = false;
  std::vector<std::pair<double, double>> measure_;  // world points
  measure::Mode measure_mode_ = measure::Mode::Path;
  bool measure_done_ = false;
  std::vector<measure::LonLat> measure_ll() const;
  void measure_click(double sx, double sy, double wx, double wy);
  void measure_reset() {
    measure_.clear();
    measure_done_ = false;
  }

  std::string hover_name_;
  std::string toast_;
  double toast_until_ = 0;
  double last_frame_t_ = 0;
  std::vector<text::Rect> blockers_;  // interface windows this frame, logical pixels
  int frames_ = 0;
  int settled_frames_ = 0;
  struct Profile {  // printed with scripted screenshots
    double t_start = 0, t_settled = 0, worst_ms = 0, add_ms = 0, prep_ms = 0;
    int slow = 0;
  } prof_;
  std::vector<TileKey> last_need_;
  std::deque<std::string> script_;  // --command steps still to run
  std::string fly_on_load_;
  bool want_png_ = false;
};

}  // namespace gsa
