#include "commands.hpp"

#include <algorithm>
#include <cctype>

namespace gsa::commands {

const std::vector<Spec>& all() {
  static const std::vector<Spec> s = {
      {"GO", "place", "Fly to a place, area or facility", true},
      {"Z", "zoom", "Set the zoom level, 3 to 16", true},
      {"FIT", "", "The whole country", true},
      {"ADD", "layer", "Put a layer on the map", true},
      {"RM", "layer", "Take a layer off the map", true},
      {"HIDE", "layer", "Hide a layer for now", true},
      {"SHOW", "layer", "Show a hidden layer", true},
      {"CLR", "", "Clear every layer", true},
      {"START", "", "Topic: overview", true},
      {"GRID", "", "Topic: power and grid", true},
      {"VOTE", "", "Topic: votes and people", true},
      {"WATER", "", "Topic: water", true},
      {"PLOTS", "", "Topic: property", true},
      {"FIND", "preset", "Find places where... (conditions across layers)", true},
      {"RANK", "", "Rank what is in view", true},
      {"FILTER", "", "Filters: line voltage, fuel, ward party, turnout, province", true},
      {"LIST", "", "List what is in view for the colour layer", true},
      {"AREAS", "", "Provinces and municipalities", true},
      {"DES", "", "Describe the open subject (the full card)", true},
      {"HERE", "", "What is here, at the centre of the map", true},
      {"NEAR", "", "Everything within 25 km", true},
      {"MEAS", "", "Measure distances on the map", true},
      {"WATCH", "", "Add the open facility to the watchlist", true},
      {"WL", "", "The watchlist", true},
      {"SAVE", "name", "Save this view", true},
      {"VIEW", "name", "Open a saved view", true},
      {"COPY", "", "Copy the coordinates of the open subject", true},
      {"PNG", "", "Save the map as an image", true},
      {"X", "", "Close the card or panel", true},
      {"HELP", "", "Keys and commands", true},
      {"TR", "", "Trace the network from here", false},
      {"FAIL", "", "Outage: what goes down if this fails", false},
      {"FEED", "", "Which stations feed it", false},
      {"PATH", "to", "Shortest path to another facility", false},
      {"REACH", "", "Travel-time reach", false},
      {"ANA", "", "Analyse an area", false},
      {"LIVE", "", "Live planes and ships", false},
      {"3D", "", "Terrain and tilt", false},
  };
  return s;
}

const Spec* find(const std::string& code) {
  for (const auto& s : all())
    if (code == s.code) return &s;
  return nullptr;
}

namespace {
std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  const auto b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}
bool is_code(const std::string& w) {
  if (w.empty() || w.size() > 6) return false;
  bool letter = false;
  for (unsigned char c : w) {
    if (std::isupper(c)) letter = true;
    else if (!std::isdigit(c)) return false;
  }
  return letter || w == "3D";
}
std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return s;
}
}  // namespace

std::vector<const Spec*> suggest(const std::string& typed, size_t max) {
  std::vector<const Spec*> out;
  std::string t = trim(typed);
  if (!t.empty() && t[0] == ':') t = trim(t.substr(1));
  const std::string word = upper(t.substr(0, t.find(' ')));
  for (const auto& s : all()) {
    if (out.size() >= max) break;
    if (word.empty() || std::string(s.code).rfind(word, 0) == 0) out.push_back(&s);
  }
  return out;
}

Parsed parse(const std::string& line) {
  Parsed p;
  std::string t = trim(line);
  bool forced = false;
  if (!t.empty() && t[0] == ':') {
    forced = true;
    t = trim(t.substr(1));
  }
  const auto sp = t.find(' ');
  const std::string first = t.substr(0, sp);
  if ((forced && !first.empty()) || (is_code(first) && find(first))) {
    p.kind = Parsed::Command;
    p.code = upper(first);
    p.arg = sp == std::string::npos ? "" : trim(t.substr(sp + 1));
    return p;
  }
  // "name CODE": a name with a lowercase letter, then a known code at the end
  const auto ls = t.find_last_of(' ');
  if (ls != std::string::npos) {
    const std::string last = t.substr(ls + 1), name = trim(t.substr(0, ls));
    const bool has_lower = std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::islower(c); });
    if (is_code(last) && find(last) && has_lower) {
      p.kind = Parsed::SubjectFunction;
      p.code = last;
      p.arg = name;
      return p;
    }
  }
  p.text = t;
  return p;
}

}  // namespace gsa::commands
