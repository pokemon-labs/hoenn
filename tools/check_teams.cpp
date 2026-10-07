// Parses every team in the given Showdown export files and builds a battle
// from each one, reporting the teams that fail. Run by the `check_teams` test
// on PKMN_TEAMS_DIR, or by hand: check_teams ../teams/adv/*.txt
//
// `--expect NAME` (repeatable) names a team that should fail. The exit status
// is 0 only if exactly the expected teams fail.

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include "gen3/showdown.hpp"

using namespace pkmn::gen3;

int main(int argc, char** argv) {
  std::set<std::string> expected, failed;
  pkmn::RNG rng{0};
  size_t ok = 0;
  for (int i = 1; i < argc; i++) {
    if (std::string_view(argv[i]) == "--expect" && i + 1 < argc) {
      expected.insert(argv[++i]);
      continue;
    }
    std::ifstream in(argv[i]);
    if (!in) {
      std::fprintf(stderr, "can't read %s\n", argv[i]);
      return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    for (const auto& team : showdown::parse_teams(text, rng)) {
      if (!team.value) {
        std::printf("%s: %s\n", argv[i], team.error.c_str());
        failed.insert(team.error.substr(0, team.error.find(": ")));
        continue;
      }
      const auto battle = helpers::init_battle(1, team.value->pokemon, team.value->pokemon);
      if (!battle || !battle->valid()) {
        std::printf("%s: %s: built an invalid battle\n", argv[i], team.value->name.c_str());
        failed.insert(team.value->name);
        continue;
      }
      ok++;
    }
  }
  std::printf("%zu teams parsed, %zu failed\n", ok, failed.size());
  bool good = true;
  for (const std::string& name : failed) {
    if (!expected.count(name)) {
      std::printf("unexpected failure: %s\n", name.c_str());
      good = false;
    }
  }
  for (const std::string& name : expected) {
    if (!failed.count(name)) {
      std::printf("expected to fail but didn't: %s\n", name.c_str());
      good = false;
    }
  }
  return good ? 0 : 1;
}
