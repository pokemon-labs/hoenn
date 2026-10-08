// The chance-key test (CLAUDE.md, "Testing"): the choices and Chance's
// Actions must determine the resulting state exactly, as search keys its
// chance nodes by them.
//
// Random self-play games, as soak. At every state visited, for each pair of
// legal choices, run `update` many times, each from a fresh RNG seed and
// after randomize_hidden, and map the key (c1, c2, Actions) to the value: the
// battle without its seed and true hidden counters (mask_hidden), the
// Durations, the Result and the probability. A key that maps to two values is
// a collision: some draw that changes the state isn't in Actions, or two
// draws share a field.
//
// It can only find collisions among branches the samples reach.
//
// Usage: check_chance [--seed N] [--games N] [--samples N] [--every N]
//                     [--replay GAME_SEED] team files...

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "common/options.hpp"
#include "common/protocol.hpp"
#include "gen3/data/names.hpp"
#include "gen3/helpers.hpp"
#include "gen3/mechanics.hpp"
#include "gen3/showdown.hpp"

using namespace pkmn;
using namespace pkmn::gen3;
using namespace pkmn::gen3::names;

namespace {

using Teams = std::vector<std::vector<helpers::Pokemon>>;
using Observed = Options<protocol::NullLog, Chance<>, NullCalc>;

using Key = std::tuple<uint8_t, uint8_t, std::array<uint8_t, sizeof(Actions)>>;

struct Value {
  Battle battle;
  Durations durations;
  Result result;
  double probability;

  friend bool operator==(const Value&, const Value&) = default;
};

struct Stats {
  uint64_t states = 0;
  uint64_t pairs = 0;
  uint64_t keys = 0;
  uint64_t max_keys = 0;
  uint64_t collisions = 0;
};

Stats stats;

void print_key(const Key& key) {
  std::printf("  choices %u %u, Actions (offset:value)",
              unsigned(std::get<0>(key)), unsigned(std::get<1>(key)));
  const auto& bytes = std::get<2>(key);
  for (size_t i = 0; i < bytes.size(); i++) {
    if (bytes[i] != 0) std::printf(" %zu:%u", i, unsigned(bytes[i]));
  }
  std::printf("\n");
}

// The state a collision happened at: each side's active Pokémon.
void print_state(const Battle& battle, const Durations& durations) {
  for (int s = 0; s < 2; s++) {
    const Side& side = battle.sides[s];
    const ActivePokemon& a = side.active;
    const Pokemon& p = side.get(1);
    std::printf("  p%d %.*s hp %u/%u status %u ability %.*s item %u, moves",
                s + 1, int(SPECIES_IDS[uint16_t(a.species)].size()),
                SPECIES_IDS[uint16_t(a.species)].data(), p.hp, p.stats.hp,
                p.status.bits, int(ABILITY_IDS[uint8_t(a.ability)].size()),
                ABILITY_IDS[uint8_t(a.ability)].data(), unsigned(p.item));
    for (const MoveSlot& m : a.moves) {
      const std::string_view id = MOVE_IDS[uint16_t(m.id)];
      std::printf(" %.*s", int(id.size()), id.data());
    }
    const Durations::Side& d = durations.get(Player(s));
    std::printf(", sleep counts");
    for (uint8_t c : d.sleep) std::printf(" %u", c);
    std::printf("\n");
  }
}

// Names the parts of two values that differ.
void print_diff(const Value& a, const Value& b) {
  std::printf("  differs in:");
  if (a.result != b.result) std::printf(" result");
  if (a.probability != b.probability) std::printf(" probability");
  if (!(a.durations == b.durations)) std::printf(" durations");
  if (a.battle.turn != b.battle.turn) std::printf(" turn");
  if (!(a.battle.field == b.battle.field)) std::printf(" field");
  if (!(a.battle.request == b.battle.request)) std::printf(" request");
  for (int s = 0; s < 2; s++) {
    const Side& x = a.battle.sides[s];
    const Side& y = b.battle.sides[s];
    for (int i = 0; i < 6; i++) {
      if (!(x.pokemon[i] == y.pokemon[i])) {
        std::printf(" p%d.pokemon[%d](hp %u/%u status %u/%u)", s + 1, i,
                    x.pokemon[i].hp, y.pokemon[i].hp, x.pokemon[i].status.bits,
                    y.pokemon[i].status.bits);
      }
    }
    if (!(x.active.volatiles == y.active.volatiles)) {
      std::printf(" p%d.volatiles", s + 1);
    }
    if (!(x.active.boosts == y.active.boosts)) std::printf(" p%d.boosts", s + 1);
    if (!(x.active == y.active)) std::printf(" p%d.active", s + 1);
    if (!(x.conditions == y.conditions)) std::printf(" p%d.conditions", s + 1);
    if (std::memcmp(x.order, y.order, sizeof(x.order)) != 0) {
      std::printf(" p%d.order", s + 1);
    }
    if (!(x == y)) std::printf(" (p%d side)", s + 1);
  }
  std::printf("\n");
}

// Every choice pair at one state, `samples` times each. Returns false on a
// collision.
bool check_state(const Battle& battle, const Durations& durations, Result r,
                 RNG& rng, uint32_t samples, uint64_t game_seed) {
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  const uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
  const uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
  stats.states++;
  bool ok = true;
  for (uint8_t i = 0; i < n1; i++) {
    for (uint8_t j = 0; j < n2; j++) {
      std::map<Key, Value> seen;
      for (uint32_t k = 0; k < samples; k++) {
        Battle copy = battle;
        copy.rng.seed = rng.next();
        randomize_hidden(copy, durations);
        Observed o{};
        o.chance.durations = durations;
        const Result result = update(copy, c1[i], c2[j], o);
        mask_hidden(copy);
        Key key{c1[i].bits, c2[j].bits, {}};
        std::memcpy(std::get<2>(key).data(), &o.chance.actions,
                    sizeof(Actions));
        const Value value{copy, o.chance.durations, result,
                          o.chance.probability.p / o.chance.probability.q};
        const auto [it, inserted] = seen.try_emplace(key, value);
        if (!inserted && !(it->second == value)) {
          std::printf("game %llu, turn %u: collision\n",
                      (unsigned long long)game_seed, battle.turn);
          print_state(battle, durations);
          print_key(key);
          print_diff(it->second, value);
          stats.collisions++;
          ok = false;
          break;
        }
      }
      stats.pairs++;
      stats.keys += seen.size();
      if (seen.size() > stats.max_keys) stats.max_keys = seen.size();
      if (!ok) return false;
    }
  }
  return true;
}

// One game from its seed, checking every `every`th state. Stops at the
// first collision.
void play(const Teams& teams, uint64_t game_seed, uint32_t samples,
          uint32_t every) {
  RNG rng{game_seed};
  const uint32_t a = rng.range(uint32_t(teams.size()));
  const uint32_t b = rng.range(uint32_t(teams.size()));
  Battle battle = *helpers::init_battle(rng.next(), teams[a], teams[b]);
  Observed o{};
  RNG sampler{rng.next()};
  Result r = update(battle, Choice::pass(), Choice::pass(), o);
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  for (uint32_t n = 0; !r.over(); n++) {
    if (n % every == 0 &&
        !check_state(battle, o.chance.durations, r, sampler, samples,
                     game_seed)) {
      return;
    }
    const uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
    const uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
    const Choice x = c1[rng.range(n1)];
    const Choice y = c2[rng.range(n2)];
    r = update(battle, x, y, o);
  }
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
  uint64_t games = 10;
  uint32_t samples = 64;
  uint32_t every = 1;
  bool replay = false;
  uint64_t replay_seed = 0;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--seed" && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--games" && i + 1 < argc) {
      games = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--samples" && i + 1 < argc) {
      samples = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--every" && i + 1 < argc) {
      every = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--replay" && i + 1 < argc) {
      replay = true;
      replay_seed = std::strtoull(argv[++i], nullptr, 10);
    } else {
      files.push_back(arg);
    }
  }
  if (every == 0) every = 1;

  const Teams teams = load(files);
  if (teams.empty()) {
    std::fprintf(stderr, "check_chance: no teams (pass Showdown export files)\n");
    return 2;
  }

  if (replay) {
    play(teams, replay_seed, samples, every);
  } else {
    RNG seeds{seed};
    for (uint64_t i = 0; i < games; i++) play(teams, seeds.next(), samples, every);
  }
  std::printf("check_chance: %llu states, %llu choice pairs, %.1f keys per "
              "pair (max %llu), %llu collisions\n",
              (unsigned long long)stats.states,
              (unsigned long long)stats.pairs,
              stats.pairs ? double(stats.keys) / double(stats.pairs) : 0.0,
              (unsigned long long)stats.max_keys,
              (unsigned long long)stats.collisions);
  return stats.collisions ? 1 : 0;
}
