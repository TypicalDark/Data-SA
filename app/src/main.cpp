// gsa: the Grid of SA desktop app.
//
//   gsa                                  start the bundled back-end on the default data folder
//   gsa --data DIR                       same, with another data folder
//   gsa --api 127.0.0.1:7878             use a back-end that is already running (token in GSA_TOKEN)
//
// Scripted runs: --screenshot out.png [--topic people] [--select WC053] [--at lon,lat,zoom] [--size 1600x1000]
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "app.hpp"
#include "backend_process.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path exe_dir(const char* argv0) {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(buf).parent_path();
#else
  std::error_code ec;
  auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::absolute(argv0).parent_path() : p.parent_path();
#endif
}

fs::path default_data_dir(const fs::path& here) {
  if (const char* d = std::getenv("GSA_DATA")) return d;
  if (fs::exists(here / "devdata" / "packs")) return here / "devdata";  // internal builds ship the dev pack
#ifdef GSA_DEV_DATA
  if (fs::exists(GSA_DEV_DATA)) return GSA_DEV_DATA;  // development builds use the repo's devdata/
#endif
#ifdef _WIN32
  if (const char* l = std::getenv("LOCALAPPDATA")) return fs::path(l) / "GridOfSA";
#endif
  if (const char* x = std::getenv("XDG_DATA_HOME")) return fs::path(x) / "GridOfSA";
  if (const char* h = std::getenv("HOME")) return fs::path(h) / ".local/share/GridOfSA";
  return "GridOfSA";
}

int usage(const char* why) {
  if (why) std::fprintf(stderr, "gsa: %s\n", why);
  std::fprintf(stderr,
               "usage: gsa [--data DIR | --api HOST:PORT] [--backend PATH] [--size WxH] [--validation]\n"
               "           [--screenshot out.png] [--topic NAME] [--select AREA_ID] [--at LON,LAT,ZOOM] [--frames N]\n"
               "           [--panel find|rank|list|areas|views|watch|keys|catalogue] [--command LINE] [--spot LON,LAT]\n"
               "           [--settings FILE]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  gsa::AppOptions opt;
  std::string api, data, backend;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) std::exit(usage(("missing value for " + a).c_str()));
      return argv[++i];
    };
    if (a == "--api") api = val();
    else if (a == "--data") data = val();
    else if (a == "--backend") backend = val();
    else if (a == "--validation") opt.validation = true;
    else if (a == "--screenshot") opt.screenshot = val();
    else if (a == "--topic") opt.topic = val();
    else if (a == "--select") opt.select = val();
    else if (a == "--panel") opt.panel = val();
    else if (a == "--command") opt.command = val();
    else if (a == "--settings") opt.settings_path = val();
    else if (a == "--spot") {
      double lon = 0, lat = 0;
      if (std::sscanf(val().c_str(), "%lf,%lf", &lon, &lat) != 2) return usage("--spot wants LON,LAT");
      opt.spot = std::make_pair(lon, lat);
    }
    else if (a == "--frames") opt.max_frames = std::atoi(val().c_str());
    else if (a == "--assets") opt.assets_dir = val();
    else if (a == "--size") {
      const auto v = val();
      if (std::sscanf(v.c_str(), "%dx%d", &opt.width, &opt.height) != 2) return usage("--size wants WxH");
    } else if (a == "--at") {
      double lon, lat, z;
      if (std::sscanf(val().c_str(), "%lf,%lf,%lf", &lon, &lat, &z) != 3) return usage("--at wants LON,LAT,ZOOM");
      opt.lon = lon, opt.lat = lat, opt.zoom = z;
    } else if (a == "-h" || a == "--help") return usage(nullptr), 0;
    else return usage(("unknown option " + a).c_str());
  }
  const fs::path here = exe_dir(argv[0]);
  if (opt.assets_dir.empty()) opt.assets_dir = (here / "assets").string();

  try {
    std::unique_ptr<gsa::BackendProcess> child;
    if (!api.empty()) {
      const auto colon = api.rfind(':');
      if (colon == std::string::npos) return usage("--api wants HOST:PORT");
      opt.endpoint.host = api.substr(0, colon);
      opt.endpoint.port = std::atoi(api.substr(colon + 1).c_str());
      if (const char* t = std::getenv("GSA_TOKEN")) opt.endpoint.token = t;
    } else {
      // shell role: start our own back-end on a random port with a one-time token
#ifdef _WIN32
      const fs::path be = backend.empty() ? here / "gsa-backend.exe" : fs::path(backend);
#else
      const fs::path be = backend.empty() ? here / "gsa-backend" : fs::path(backend);
#endif
      const fs::path dd = data.empty() ? default_data_dir(here) : fs::path(data);
      std::fprintf(stderr, "gsa: starting back-end %s on %s\n", be.string().c_str(), dd.string().c_str());
      child = std::make_unique<gsa::BackendProcess>(be, dd);
      opt.endpoint = {"127.0.0.1", child->port(), child->token()};
    }
    gsa::App app(opt);
    return app.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "gsa: %s\n", e.what());
#ifdef _WIN32
    MessageBoxA(nullptr, e.what(), "Grid of SA", MB_ICONERROR);
#endif
    return 1;
  }
}
