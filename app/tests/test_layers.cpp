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
