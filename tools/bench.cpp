// Speed benchmark: updates per second with the null log, Chance on and
// Calc off. Build it in the release preset (-O3, no asserts).
//
// Builds every battle first, each between two teams drawn at random from the
// teams of every file, pooled, then plays them one after another with uniformly random legal
// choices. The clock starts at the first battle's first update and stops
// after the last battle's last update, so parsing and building aren't timed.
//
// Usage: bench [--battles N] [--seed N] TEAM_FILE...

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "options.hpp"
#include "log.hpp"
#include "calc.hpp"
#include "chance.hpp"
#include "helpers.hpp"
#include "mechanics.hpp"
#include "showdown.hpp"

using namespace hoenn;

namespace {

using Bench = Options<protocol::NullLog, Chance<>, NullCalc>;

// xorshift64: cheap, and good enough for picking teams and choices.
struct Cheap {
  uint64_t s;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  // In [0, n), by multiply-shift (a negligible bias, no division).
  uint32_t range(uint32_t n) {
    return uint32_t((uint64_t(uint32_t(next() >> 32)) * n) >> 32);
  }
};

} // namespace

int main(int argc, char** argv) {
  uint64_t battles = 10000;
  uint64_t seed = 1;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--battles" && i + 1 < argc) {
      battles = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--seed" && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else {
      files.push_back(arg);
    }
  }

  std::vector<std::vector<helpers::Pokemon>> teams;
  RNG parse_rng{0};
  for (const std::string& file : files) {
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    for (const auto& team : showdown::parse_teams(ss.str(), parse_rng)) {
      if (team.value) teams.push_back(team.value->pokemon);
    }
  }
  if (teams.empty() || battles == 0) {
    std::fprintf(stderr, "usage: bench [--battles N] [--seed N] TEAM_FILE... "
                         "(no teams, or no battles)\n");
    return 2;
  }

  Cheap rng{seed ? seed : 1};
  std::vector<Battle> all;
  all.reserve(battles);
  for (uint64_t i = 0; i < battles; i++) {
    const uint32_t a = rng.range(uint32_t(teams.size()));
    const uint32_t b = rng.range(uint32_t(teams.size()));
    all.push_back(*helpers::init_battle(rng.next(), teams[a], teams[b]));
  }

  Bench o{};
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  uint64_t updates = 0, turns = 0;
  const auto start = std::chrono::steady_clock::now();
  for (Battle& battle : all) {
    Result r = update(battle, Choice::pass(), Choice::pass(), o);
    updates++;
    while (!r.over()) {
      const uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
      const uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
      r = update(battle, c1[rng.range(n1)], c2[rng.range(n2)], o);
      updates++;
    }
    turns += battle.turn;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  std::printf("bench: %llu battles, %llu updates in %.3f s: %.0f updates/s "
              "(%.1f turns per battle)\n",
              (unsigned long long)battles, (unsigned long long)updates,
              seconds, double(updates) / seconds,
              double(turns) / double(battles));
  return 0;
}
