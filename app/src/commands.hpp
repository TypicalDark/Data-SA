// The command line in the search box, as on the website: an UPPERCASE first word (or a leading
// ":") is a command, anything else searches. "name CODE" finds the subject, then runs the
// function on it ("apollo DES", "vaal dam NEAR").
#pragma once

#include <string>
#include <vector>

namespace gsa::commands {

struct Spec {
  const char* code;
  const char* arg;    // what the argument is, empty if none
  const char* about;
  bool ready;         // false: listed, but comes in a later milestone
};

const std::vector<Spec>& all();
const Spec* find(const std::string& code);
// Commands whose code or text starts with what has been typed, for the suggestion list.
std::vector<const Spec*> suggest(const std::string& typed, size_t max = 8);

struct Parsed {
  enum Kind { Search, Command, SubjectFunction } kind = Search;
  std::string code;     // Command, SubjectFunction
  std::string arg;      // Command: the rest of the line; SubjectFunction: the subject's name
  std::string text;     // Search: the query
};

Parsed parse(const std::string& line);

}  // namespace gsa::commands
