// Layer catalogue, topics, map style, the command line and settings.
#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <set>

#include "commands.hpp"
#include "layers.hpp"
#include "settings.hpp"

using namespace gsa;

TEST_CASE("catalogue matches the website: 71 layers in nine groups, 5 base toggles, ten topics") {
  int n = 0;
  std::set<std::string> ids;
  for (const auto& d : layers::catalogue()) {
    CHECK(ids.insert(d.id).second);  // unique ids
    if (std::string(d.group) != "base") {
      ++n;
      CHECK(layers::group(d.group) != nullptr);
    }
  }
  CHECK(n == 71);
  CHECK(layers::catalogue().size() == 76);
  CHECK(layers::groups().size() == 9);
  CHECK(layers::topics().size() == 10);
  for (const auto& t : layers::topics())
    for (const char* l : t.layers) CHECK_MESSAGE(layers::find(l) != nullptr, l);
}

TEST_CASE("loose layer names") {
  REQUIRE(layers::match("substations"));
  CHECK(std::string(layers::match("substations")->id) == "subs");
  REQUIRE(layers::match("subs"));
  CHECK(layers::match("zzzz-no-such-layer") == nullptr);
}

TEST_CASE("layer state: one choropleth, hide, remove, topics") {
  layers::State s;
  s.add("c:pop");
  s.add("subs");
  s.add("c:val");  // replaces c:pop
  CHECK(std::count(s.on.begin(), s.on.end(), "c:pop") == 0);
  CHECK(s.focus == "c:val");
  CHECK(s.shows("subs"));
  s.toggle_hidden("subs");
  CHECK(!s.shows("subs"));
  CHECK(layers::map_style(s).fill == style::Topic::Property);
  CHECK(!layers::map_style(s).subs);
  s.remove("c:val");
  CHECK(s.focus != "c:val");
  CHECK(layers::map_style(s).fill == style::Topic::Overview);

  const auto avail = layers::available({"admin", "grid"});
  CHECK(!avail.count("streets"));
  const auto with_base = layers::available({"admin", "base"});
  CHECK(with_base.count("streets"));
  CHECK(with_base.count("pois"));
  {
    layers::State b;
    CHECK(layers::map_style(b).roads);
    CHECK(layers::map_style(b).pois);
    b.roads = false;
    CHECK(!layers::map_style(b).roads);
  }
  // base map style: motorways sit wider than tracks and tracks come in last
  CHECK(style::road_half_width(0, false) > style::road_half_width(7, false));
  CHECK(style::road_minz(7, false) > style::road_minz(0, false));
  CHECK(style::land_use("park", false).a == 255);
  CHECK(style::land_use("unknown", false).a == 0);
  CHECK(style::land_use("unknown", true).a == 255);
  // plots: from the parcels pack, fetched only while shown, coloured by the website's value bands
  {
    const std::map<std::string, std::vector<std::string>> pl = {{"parcels", {"parcel"}}};
    const auto pa = layers::available({"admin", "parcels"}, pl);
    CHECK(pa.count("parcels"));
    layers::State ps;
    CHECK(!layers::packs_needed(ps, pl).count("parcels"));
    ps.add("parcels");
    CHECK(layers::packs_needed(ps, pl).count("parcels"));
    const auto pm = layers::map_style(ps);
    REQUIRE(!pm.extra.empty());
    CHECK(pm.extra.back().mode == layers::Extra::Plots);
    CHECK(pm.extra.back().layer == "parcel");
    // the store's per-province packs, used when there is no national pack
    const std::map<std::string, std::vector<std::string>> prov = {{"parcels-gp", {"parcel"}}, {"parcels-wc", {"parcel"}}};
    CHECK(layers::available({"admin", "parcels-gp"}, prov).count("parcels"));
    CHECK(layers::packs_needed(ps, prov).size() == 2);
    auto both = prov;
    both["parcels"] = {"parcel"};
    CHECK(layers::packs_needed(ps, both) == std::set<std::string>{"parcels"});
    CHECK(style::value_band(50000).packed() == style::hex("#c6dbef").packed());
    CHECK(style::value_band(1500000).packed() == style::hex("#f2a65a").packed());
    CHECK(style::value_band(9e6).packed() == style::hex("#9c1f1f").packed());
  }
  s.set_topic("grid", avail);
  const auto ms = layers::map_style(s);
  CHECK(ms.lines);
  CHECK(ms.subs);
  CHECK(s.count_in("grid") >= 3);
  s.set_topic("start", avail);
  CHECK(!layers::map_style(s).lines);
}

TEST_CASE("command line: commands, name + function, search") {
  auto p = commands::parse("GO soweto");
  CHECK(p.kind == commands::Parsed::Command);
  CHECK(p.code == "GO");
  CHECK(p.arg == "soweto");
  p = commands::parse("apollo DES");
  CHECK(p.kind == commands::Parsed::SubjectFunction);
  CHECK(p.code == "DES");
  CHECK(p.arg == "apollo");
  p = commands::parse("Cape Town");
  CHECK(p.kind == commands::Parsed::Search);
  CHECK(p.text == "Cape Town");
  p = commands::parse(":go vaal");
  CHECK(p.kind == commands::Parsed::Command);
  CHECK(p.code == "GO");
  CHECK(commands::parse("ADD dams").code == "ADD");
  CHECK(!commands::suggest("GO").empty());
  CHECK(commands::find("TR") != nullptr);
  CHECK(!commands::find("TR")->ready);
}

TEST_CASE("settings round trip") {
  const auto path = (std::filesystem::temp_directory_path() / "gsa-test-settings" / "settings.json").string();
  std::filesystem::remove_all(std::filesystem::path(path).parent_path());
  {
    settings::Store s(path);
    CHECK(s.data().is_object());
    s.data()["views"] = nlohmann::json::array({{{"name", "Home"}, {"zoom", 5.4}}});
    CHECK(s.save());
  }
  settings::Store again(path);
  REQUIRE(again.data().contains("views"));
  CHECK(again.data()["views"][0]["name"] == "Home");
  std::filesystem::remove_all(std::filesystem::path(path).parent_path());
}

TEST_CASE("website packs: available layers, packs to fetch, draw order") {
  const std::map<std::string, std::vector<std::string>> pl = {
      {"admin", {"province", "municipality", "label"}},
      {"web-nature", {"x_pa", "x_wetl"}},
      {"web-vote", {"wards"}},
  };
  const auto a = layers::available({"admin", "web-nature", "web-vote"}, pl);
  CHECK(a.count("x:pa"));
  CHECK(a.count("x:wetl"));
  CHECK(a.count("wards"));
  CHECK(a.count("c:ward"));
  CHECK(!a.count("x:cba"));
  CHECK(layers::mvt_name("x:pa") == "x_pa");

  layers::State s;
  s.add("x:wetl");
  s.add("x:pa");  // top of the list
  s.add("c:ward");
  auto need = layers::packs_needed(s, pl);
  CHECK(need.count("web-nature"));
  CHECK(need.count("web-vote"));
  const auto ms = layers::map_style(s);
  REQUIRE(ms.extra.size() == 3);
  CHECK(ms.extra.back().mode == layers::Extra::Party);  // c:ward was added last, so it is on top
  CHECK(ms.extra[0].layer == "x_wetl");               // bottom of the list draws first
  s.toggle_hidden("x:pa");
  CHECK(layers::map_style(s).extra.size() == 2);
  s.remove("c:ward");
  CHECK(!layers::packs_needed(s, pl).count("web-vote"));
}

TEST_CASE("image packs: terrain and raster layers") {
  CHECK(layers::raster_pack("x:fire") == "raster-fire");
  CHECK(layers::raster_pack("relief") == "raster-relief");
  CHECK(layers::raster_pack("x:pa").empty());
  const std::map<std::string, std::vector<std::string>> pl = {
      {"admin", {"province"}}, {"raster-fire", {}}, {"raster-relief", {}}, {"web-move", {"x_ships"}}, {"raster-ships", {}}};
  const auto a = layers::available({"admin", "raster-fire", "raster-relief", "web-move", "raster-ships"}, pl);
  CHECK(a.count("x:fire"));
  CHECK(a.count("relief"));
  CHECK(!a.count("x:flood"));

  layers::State s;
  s.terrain = false;
  CHECK(layers::packs_needed(s, pl).empty());
  s.terrain = true;
  s.add("x:fire");
  s.add("x:ships");
  const auto need = layers::packs_needed(s, pl);
  CHECK(need.count("raster-relief"));
  CHECK(need.count("raster-fire"));
  CHECK(need.count("raster-ships"));
  CHECK(need.count("web-move"));  // ports are vectors in the same layer
  const auto ms = layers::map_style(s);
  REQUIRE(ms.rasters.size() == 3);
  CHECK(ms.rasters[0] == "raster-relief");  // terrain under everything
  CHECK(ms.rasters[1] == "raster-fire");
  CHECK(ms.rasters[2] == "raster-ships");   // top of the list draws last
  CHECK(layers::raster_opacity("raster-fire", 5) > layers::raster_opacity("raster-fire", 12));
  CHECK(layers::raster_opacity("raster-relief", 12) == 0.0f);  // terrain shading fades out when zoomed in
}

TEST_CASE("filters: voltage bins, fuels, wards") {
  CHECK(layers::volt_bin(400) == 0);
  CHECK(layers::volt_bin(275) == 1);
  CHECK(layers::volt_bin(132) == 2);
  CHECK(layers::volt_bin(66) == 3);
  CHECK(layers::volt_bin(22) == 4);
  CHECK(layers::volt_bin(0) == 5);
  CHECK(layers::fuel_key("Oil") == "diesel");
  CHECK(layers::fuel_key("biogas") == "biomass");
  CHECK(layers::fuel_key("tidal") == "other");
  layers::Filters f;
  CHECK(layers::pass_volt(f, 88));
  f.volt = {0};
  CHECK(layers::pass_volt(f, 765));
  CHECK(!layers::pass_volt(f, 132));
  f.party = {"DA", "other"};
  CHECK(layers::pass_ward(f, "DA", 50, "CPT"));
  CHECK(layers::pass_ward(f, "Good", 50, "CPT"));  // small parties count as other
  CHECK(!layers::pass_ward(f, "ANC", 50, "CPT"));
  f.turn = {2};
  CHECK(!layers::pass_ward(f, "DA", 61, "CPT"));
  f.turn.clear();
  f.prov = {"WC"};
  f.prov_munis = {"CPT"};
  CHECK(layers::pass_ward(f, "DA", 50, "CPT"));
  CHECK(!layers::pass_ward(f, "DA", 50, "JHB"));
  layers::State s;
  auto ms = layers::map_style(s);
  ms.filter = f;
  CHECK(ms != layers::map_style(s));  // a filter change rebuilds the meshes
}
