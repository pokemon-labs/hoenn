// Random self-play soak: plays many battles between random pairs of teams
// with uniformly random legal choices, to catch crashes, sanitizer reports,
// failed asserts and broken invariants. Build it in the debug preset (asserts,
// ASan, UBSan) for that; release only measures speed.
//
// Each game is a function of its own seed (the team pair, the battle's RNG
// seed and every choice), so a failing game replays alone with --replay.
// When a game dies to an assert or a sanitizer, its seed is printed first.
//
// Usage: soak [--seed N] [--games N] [--replay GAME_SEED] team files...

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

// Only when ASan is on: the header can exist without the runtime linked.
#if defined(__SANITIZE_ADDRESS__)
#define PKMN_SOAK_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PKMN_SOAK_ASAN 1
#endif
#endif
#if defined(PKMN_SOAK_ASAN) && defined(__has_include)
#if __has_include(<sanitizer/common_interface_defs.h>)
#include <sanitizer/common_interface_defs.h>
#define PKMN_SOAK_SANITIZER 1
#endif
#endif

#include "common/options.hpp"
#include "common/protocol.hpp"
#include "gen3/helpers.hpp"
#include "gen3/mechanics.hpp"
#include "gen3/showdown.hpp"

using namespace pkmn;
using namespace pkmn::gen3;

namespace {

using Teams = std::vector<std::vector<helpers::Pokemon>>;
using Observed = Options<protocol::NullLog, Chance<>, Calc>;

// The game in progress, for the crash handlers. Written before each game.
char current[128] = "no game started";

void report() {
  const char header[] = "\nsoak: died in ";
  (void)!write(2, header, sizeof(header) - 1);
  (void)!write(2, current, std::strlen(current));
  (void)!write(2, "\n", 1);
}

extern "C" void on_abort(int) {
  report();
  std::signal(SIGABRT, SIG_DFL);
  std::abort();
}

int failures = 0;

#define CHECK(cond, game_seed)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("game %llu: CHECK failed: %s\n",                             \
                  (unsigned long long)(game_seed), #cond);                     \
      failures++;                                                              \
      return {};                                                               \
    }                                                                          \
  } while (0)

struct Game {
  uint64_t updates;
  uint16_t turns;
  Result::Type result;
};

// One game from its seed. The draws come in a fixed order (team pair, battle
// seed, then P1's choice before P2's), so a seed replays the same way under
// any compiler.
Game play(const Teams& teams, uint64_t game_seed) {
  RNG rng{game_seed};
  const uint32_t a = rng.range(uint32_t(teams.size()));
  const uint32_t b = rng.range(uint32_t(teams.size()));
  std::snprintf(current, sizeof(current), "game seed %llu (teams %u and %u)",
                (unsigned long long)game_seed, a, b);

  Battle battle = *helpers::init_battle(rng.next(), teams[a], teams[b]);
  Observed o{};
  Result r = update(battle, Choice::pass(), Choice::pass(), o);
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  uint64_t updates = 1;
  while (!r.over()) {
    const uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
    const uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
    CHECK(n1 >= 1 && n2 >= 1, game_seed);
    const Choice x = c1[rng.range(n1)];
    const Choice y = c2[rng.range(n2)];
    r = update(battle, x, y, o);
    updates++;
    CHECK(battle.valid(), game_seed);
    CHECK(battle.turn <= Battle::MAX_TURNS, game_seed);
    const auto& p = o.chance.probability;
    CHECK(p.p >= 1 && p.p <= p.q, game_seed);
  }
  return {updates, battle.turn, r.type()};
}

Teams load(const std::vector<std::string>& files) {
  Teams teams;
  RNG rng{0};
  for (const std::string& file : files) {
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    for (const auto& team : showdown::parse_teams(ss.str(), rng)) {
      if (team.value) teams.push_back(team.value->pokemon);
    }
  }
  return teams;
}

} // namespace

int main(int argc, char** argv) {
  uint64_t seed = 1;
  uint64_t games = 100;
  bool replay = false;
  uint64_t replay_seed = 0;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--seed" && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--games" && i + 1 < argc) {
      games = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--replay" && i + 1 < argc) {
      replay = true;
      replay_seed = std::strtoull(argv[++i], nullptr, 10);
    } else {
      files.push_back(arg);
    }
  }

  const Teams teams = load(files);
  if (teams.empty()) {
    std::fprintf(stderr, "soak: no teams (pass Showdown export files)\n");
    return 2;
  }

  std::signal(SIGABRT, on_abort);
#ifdef PKMN_SOAK_SANITIZER
  __sanitizer_set_death_callback(report);
#endif

  if (replay) {
    const Game g = play(teams, replay_seed);
    std::printf("game seed %llu: %llu updates, turn %u, result %u\n",
                (unsigned long long)replay_seed,
                (unsigned long long)g.updates, g.turns, unsigned(g.result));
    return failures ? 1 : 0;
  }

  RNG seeds{seed};
  uint64_t updates = 0, turns = 0, capped = 0;
  uint64_t outcomes[5] = {};
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < games; i++) {
    const Game g = play(teams, seeds.next());
    updates += g.updates;
    turns += g.turns;
    if (g.turns >= Battle::MAX_TURNS) capped++;
    outcomes[unsigned(g.result) % 5]++;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  std::printf("soak: %llu games, %llu updates in %.1f s (%.0f updates/s), "
              "%.0f turns per game\n",
              (unsigned long long)games, (unsigned long long)updates, seconds,
              double(updates) / seconds, double(turns) / double(games));
  std::printf("      P1 won %llu, P2 won %llu, ties %llu (%llu at the turn cap), "
              "failures %d\n",
              (unsigned long long)outcomes[Result::Win],
              (unsigned long long)outcomes[Result::Lose],
              (unsigned long long)outcomes[Result::Tie],
              (unsigned long long)capped, failures);
  return failures ? 1 : 0;
}
