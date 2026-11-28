// gsa-backend: serves the /v1 API from local data packs.
//
//   gsa-backend --data DIR [--host 127.0.0.1] [--port 0] [--origin URL] [--exit-with-stdin]
//               [--live] [--no-plane-poll] [--live-keep-hours 24] [--watch-keep-days 365] [--cache-mb 64]
//
// --live keeps live planes and ships in DIR/cache/live.sqlite: the latest positions, a 24 h history of
// everything and a long history of watched objects. Planes are polled from adsb.lol / OpenSky with the
// system curl (unless --no-plane-poll); ships arrive from tools/live/ais_feed.py via POST /v1/live/ingest.
//
// The token comes from the GSA_TOKEN environment variable (never the command line, so it
// does not show in process lists). With --port 0 a free port is picked. When the server is
// listening it prints one JSON line to stdout: {"event":"ready","port":N,"api":"1"}.
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gsa/api.hpp"
#include "gsa/live.hpp"

namespace {

httplib::Server* g_srv = nullptr;
extern "C" void on_signal(int) {
  if (g_srv) g_srv->stop();
}

int usage(const char* why) {
  if (why) std::cerr << "gsa-backend: " << why << "\n";
  std::cerr << "usage: gsa-backend --data DIR [--host 127.0.0.1] [--port 0] [--origin URL] [--threads N]\n"
               "                   [--exit-with-stdin] [--no-auth] [--live] [--no-plane-poll]\n"
               "                   [--live-keep-hours 24] [--watch-keep-days 365] [--cache-mb 64]\n"
               "Token: GSA_TOKEN environment variable.\n";
  return 2;
}

bool is_loopback(const std::string& h) { return h == "127.0.0.1" || h == "::1" || h == "localhost"; }

}  // namespace

int main(int argc, char** argv) {
  std::string data, host = "127.0.0.1", origin;
  int port = 0;
  bool exit_with_stdin = false, no_auth = false, live_on = false, plane_poll = true;
  double keep_hours = 24, watch_days = 365, cache_mb = 64;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage(("missing value for " + a).c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--data") data = val();
    else if (a == "--host") host = val();
    else if (a == "--port") port = std::atoi(val().c_str());
    else if (a == "--origin") origin = val();
    else if (a == "--threads") (void)val();  // reserved; httplib sizes its own pool
    else if (a == "--exit-with-stdin") exit_with_stdin = true;
    else if (a == "--no-auth") no_auth = true;
    else if (a == "--live") live_on = true;
    else if (a == "--no-plane-poll") plane_poll = false;
    else if (a == "--live-keep-hours") keep_hours = std::atof(val().c_str());
    else if (a == "--watch-keep-days") watch_days = std::atof(val().c_str());
    else if (a == "--cache-mb") cache_mb = std::atof(val().c_str());
    else if (a == "--version") {
      std::cout << gsa::kBackendVersion << "\n";
      return 0;
    } else if (a == "-h" || a == "--help") return usage(nullptr), 0;
    else return usage(("unknown option " + a).c_str());
  }
  if (data.empty()) return usage("--data is required");

  const char* tok = std::getenv("GSA_TOKEN");
  std::string token = tok ? tok : "";
  if (token.empty() && !no_auth) return usage("set GSA_TOKEN, or pass --no-auth for a local test run");
  if (no_auth && !is_loopback(host)) return usage("--no-auth is only allowed on a loopback address");

  std::shared_ptr<gsa::PackStore> store;
  try {
    store = std::make_shared<gsa::PackStore>(data);
  } catch (const std::exception& e) {
    std::cerr << "gsa-backend: cannot load packs: " << e.what() << "\n";
    return 1;
  }

  if (keep_hours < 1 || watch_days < 1 || cache_mb < 0) return usage("keep and cache sizes must be positive");
  std::shared_ptr<gsa::live::Store> live;
  std::shared_ptr<gsa::live::PlanePoller> poller;
  if (live_on) {
    gsa::live::Options lo;
    lo.db = std::filesystem::path(data) / "cache" / "live.sqlite";
    lo.keep_s = int64_t(keep_hours * 3600);
    lo.keep_watched_s = int64_t(watch_days * 86400);
    try {
      live = std::make_shared<gsa::live::Store>(lo);
    } catch (const std::exception& e) {
      std::cerr << "gsa-backend: cannot open the live store: " << e.what() << "\n";
      return 1;
    }
    if (plane_poll) poller = std::make_shared<gsa::live::PlanePoller>(live, gsa::live::curl_fetch);
  }

  httplib::Server srv;
  g_srv = &srv;
  gsa::ApiConfig cfg{store, no_auth ? std::string() : token, origin};
  cfg.live = live;
  cfg.poller = poller;
  cfg.cache_bytes = size_t(cache_mb * 1024 * 1024);
  gsa::mount_api(srv, cfg);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const int bound = port == 0 ? srv.bind_to_any_port(host) : (srv.bind_to_port(host, port) ? port : -1);
  if (bound <= 0) {
    std::cerr << "gsa-backend: cannot listen on " << host << ":" << port << "\n";
    return 1;
  }
  std::cerr << "gsa-backend " << gsa::kBackendVersion << ": " << store->current()->info.size() << " packs from " << data
            << ", listening on " << host << ":" << bound << "\n";
  std::cout << nlohmann::json{{"event", "ready"}, {"port", bound}, {"api", gsa::kApiVersion}}.dump() << std::endl;

  std::thread watcher;
  if (exit_with_stdin) {
    // The shell holds our stdin open; when it exits (or crashes) the pipe closes and we stop.
    watcher = std::thread([&srv] {
      std::string line;
      while (std::getline(std::cin, line)) {
      }
      srv.stop();
    });
    watcher.detach();
  }
  if (poller) poller->start();
  srv.listen_after_bind();
  if (poller) poller->stop();
  return 0;
}
