// Speed benchmark: updates per second with the null log, Chance on and
// Calc off, or with --log the real log (reset before each update), Chance and
// Calc off. Build it in the release preset (-O3, no asserts).
//
// Builds every battle first, each between two teams drawn at random from the
// teams of every file, pooled, then plays them one after another with uniformly random legal
// choices. The clock starts at the first battle's first update and stops
// after the last battle's last update, so parsing and building aren't timed.
//
// Usage: bench [--battles N] [--seed N] [--log] TEAM_FILE...

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
using Logged = Options<protocol::FixedLog, NullChance, NullCalc>;

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

struct Count {
  uint64_t updates = 0;
  uint64_t turns = 0;
};

// Plays every battle to the end. `before` runs before each update.
template <typename O, typename F>
Count play(std::vector<Battle>& all, Cheap& rng, O& o, F before) {
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  Count count;
  for (Battle& battle : all) {
    before();
    Result r = update(battle, Choice::pass(), Choice::pass(), o);
    count.updates++;
    while (!r.over()) {
      const uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
      const uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
      before();
      r = update(battle, c1[rng.range(n1)], c2[rng.range(n2)], o);
      count.updates++;
    }
    count.turns += battle.turn;
  }
  return count;
}

} // namespace

int main(int argc, char** argv) {
  uint64_t battles = 10000;
  uint64_t seed = 1;
  bool log = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--battles" && i + 1 < argc) {
      battles = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--seed" && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--log") {
      log = true;
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
    std::fprintf(stderr, "usage: bench [--battles N] [--seed N] [--log] TEAM_FILE... "
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

  std::vector<uint8_t> buffer(protocol::MAX_LOG_SIZE);
  protocol::Writer writer{buffer.data(), buffer.size(), 0};
  Count count;
  const auto start = std::chrono::steady_clock::now();
  if (log) {
    Logged o{protocol::fixed_log(writer), {}, {}};
    count = play(all, rng, o, [&] { writer.reset(); });
  } else {
    Bench o{};
    count = play(all, rng, o, [] {});
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  const uint64_t updates = count.updates, turns = count.turns;
  std::printf("bench%s: %llu battles, %llu updates in %.3f s: %.0f updates/s "
              "(%.1f turns per battle)\n",
              log ? " --log" : "", (unsigned long long)battles, (unsigned long long)updates,
              seconds, double(updates) / seconds,
              double(turns) / double(battles));
  return 0;
}
