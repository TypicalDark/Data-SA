#include <doctest/doctest.h>

#include <fstream>

#include "find.hpp"

using namespace gsa;

namespace {
const find::Data& data() {
  static const find::Data d = [] {
    std::ifstream in(std::string(GSA_DEVDATA) + "/packs/find.json");
    return find::parse(nlohmann::json::parse(in));
  }();
  return d;
}
}  // namespace

TEST_CASE("find data parses like the website's load()") {
  const auto& d = data();
  CHECK(d.munis.size() == 213);
  CHECK(d.cap.size() > 100);
  CHECK(d.dams.size() > 200);
  CHECK(!d.air.empty());
  CHECK(!d.places.empty());
  const auto* jhb = d.muni("JHB");
  REQUIRE(jhb);
  CHECK(jhb->pop > 4e6);
  CHECK(jhb->dens > 1000);
  CHECK(d.muni_at(28.047, -26.204) == jhb);  // Johannesburg's centre
  CHECK(d.muni_at(10, 10) == nullptr);
}

TEST_CASE("conditions, presets and why") {
  const auto& d = data();
  const auto* jhb = d.muni("JHB");
  REQUIRE(jhb);
  CHECK(find::pass(d, *jhb, find::make("densMin")));
  CHECK(!find::pass(d, *jhb, find::make("densMax")));
  auto town = find::make("nearTown");
  CHECK(find::pass(d, *jhb, town));  // Johannesburg itself is a town inside
  CHECK(find::why(d, *jhb, {town}).find("inside") != std::string::npos);

  const auto* solar = find::preset("solar");
  REQUIRE(solar);
  const auto pass = find::run(d, solar->rules);
  CHECK(pass.size() > 10);
  CHECK(pass.size() < 213);
  CHECK(find::run(d, {}).empty());

  // stricter numbers never let more through
  auto r = find::make("nearCap");
  r.km = 10;
  CHECK(find::run(d, {r}).size() <= find::run(d, solar->rules).size());

  auto party = find::make("party");
  party.p = "DA";
  const auto da = find::run(d, {party});
  CHECK(da.count("CPT"));
  CHECK(find::text(find::make("nearCap")) == "Within 40 km of a substation with 100 MW free");
  CHECK(find::text(find::make("valMax")) == "Median property value, at most R800 000");

  const auto j = find::to_json(solar->rules);
  CHECK(find::rules_from(j) == solar->rules);
}

TEST_CASE("rank and spot") {
  const auto& d = data();
  const double sa[4] = {16, -35, 33, -22};
  const auto dams = find::rank(d, "dams", sa);
  REQUIRE(dams.size() > 100);
  CHECK(dams.front().x <= dams.back().x);  // lowest first
  const auto pop = find::rank(d, "mpop", sa);
  CHECK(pop.front().area == "JHB");
  const auto lines = find::spot(d, 18.42, -33.92, {find::make("nearPort")});
  REQUIRE(lines.size() >= 4);
  CHECK(lines.front().second.rfind("City of Cape Town", 0) == 0);
  CHECK(lines.back().first == "Your Find conditions");
  CHECK(lines.back().second == "Meets all 1");
  CHECK(find::fmt_num(1234567) == "1 234 567");
  CHECK(find::fmt_km(0.45) == "450 m");
}
