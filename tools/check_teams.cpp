// Parses every Showdown export team under a directory and builds a battle
// from each one, reporting the teams that fail.
//
// g++ -std=c++20 -g -fsanitize=address,undefined -Isrc tools/check_teams.cpp -o check_teams
// ./check_teams ../teams/adv/*.txt

#include <cstdio>
#include <fstream>
#include <sstream>

#include "gen3/showdown.hpp"

using namespace pkmn::gen3;

int main(int argc, char** argv) {
  pkmn::RNG rng{0};
  size_t ok = 0, failed = 0;
  for (int i = 1; i < argc; i++) {
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
        failed++;
        continue;
      }
      const auto battle = helpers::init_battle(1, team.value->pokemon, team.value->pokemon);
      if (!battle || !battle->valid()) {
        std::printf("%s: %s: built an invalid battle\n", argv[i], team.value->name.c_str());
        failed++;
        continue;
      }
      ok++;
    }
  }
  std::printf("%zu teams parsed, %zu failed\n", ok, failed);
  return failed != 0;
}
