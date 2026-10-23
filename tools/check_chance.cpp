// The chance-key test (docs/TESTING.md): the choices and Chance's
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
// It can only find collisions among branches the samples reach. Each key
// found is then replayed with its Actions forced through Calc, so a branch
// the free samples saw once is still compared (the hidden ends aren't
// forceable, so a replay may land elsewhere; `untested` counts what stays
// uncompared). The free samples also test the probabilities: the keys'
// probabilities sum to at most 1 and leave little unseen, and each key's
// frequency is consistent with its probability. As the samples go through
// randomize_hidden, that also checks it against the probabilities `update`
// reports (consistency, not truth: both could share one wrong belief).
//
// --unmask NAME (seed, sleep, confusion, binding, uproar, thrash, disable,
// encore; repeatable) leaves that part out of mask_hidden, an ablation that
// should produce collisions wherever the effect is active.
//
// Usage: check_chance [--seed N] [--games N] [--samples N] [--replays N]
//                     [--every N] [--unmask NAME]... [--replay GAME_SEED]
//                     team files...

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "options.hpp"
#include "log.hpp"
#include "calc.hpp"
#include "data/names.hpp"
#include "helpers.hpp"
#include "mechanics.hpp"
#include "showdown.hpp"

using namespace hoenn;
using namespace hoenn::names;

namespace {

using Teams = std::vector<std::vector<helpers::Pokemon>>;
using Observed = Options<protocol::NullLog, Chance<>, NullCalc>;

// Raw bytes, as search keys on them, so no padding may differ.
static_assert(std::has_unique_object_representations_v<Actions>);
using Key = std::tuple<uint8_t, uint8_t, std::array<uint8_t, sizeof(Actions)>>;

struct Value {
  Battle battle;
  Durations durations;
  Result result;
  double probability;

  // Search never expands a terminal state, so two ends must agree only on
  // the Result and the probability (docs/TESTING.md, "Terminal states"): the
  // winner's HP may differ.
  friend bool operator==(const Value& a, const Value& b) {
    if (a.result.over() || b.result.over()) {
      return a.result == b.result && a.probability == b.probability;
    }
    return a.battle == b.battle && a.durations == b.durations &&
           a.result == b.result && a.probability == b.probability;
  }
};

struct Totals {
  uint64_t states = 0;
  uint64_t pairs = 0;
  uint64_t keys = 0;
  uint64_t max_keys = 0;
  uint64_t collisions = 0;
  // Keys compared with nothing: seen once, and no forced replay reached them.
  uint64_t untested = 0;
  // Forced replays that reached another key: the hidden ticks (sleep,
  // confusion and binding ends) aren't forced, and randomize_hidden varies.
  uint64_t unreproduced = 0;
  // Probability failures: the sum over a pair's keys (against held-out
  // draws), a key's frequency.
  uint64_t mass = 0;
  uint64_t frequency = 0;
  // The probability the keys found (before the held-out draws) missed,
  // summed over pairs.
  double missing = 0;
  // Calibration over the whole run. Per bin, the held-out draws that landed
  // on a key with probability in [2^-(b+1), 2^-b), and the binomial mean and
  // variance of that count; the last bin is the keys not yet found, whose
  // mass is 1 - sum. The held-out draws are independent of how the keys
  // were found, so the count is Binomial(samples, the bin's mass) in each
  // pair, and the sums over pairs are close to normal.
  static constexpr int BINS = 42;
  static constexpr int UNSEEN = BINS - 1;
  double observed[BINS] = {};
  double expected[BINS] = {};
  double variance[BINS] = {};
};

constexpr int bin(double p) {
  int b = 0;
  while (b < Totals::UNSEEN - 1 && p < 0.5) p *= 2, b++;
  return b;
}

Totals stats;

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
  std::printf("  results %u/%u, differs in:", unsigned(a.result.type()),
              unsigned(b.result.type()));
  if (a.result != b.result) std::printf(" result");
  if (a.probability != b.probability) std::printf(" probability");
  if (!(a.durations == b.durations)) std::printf(" durations");
  if (a.battle.turn != b.battle.turn) std::printf(" turn");
  if (!(a.battle.field == b.battle.field)) std::printf(" field");
  if (!(a.battle.request == b.battle.request)) {
    std::printf(" request(bytes");
    const auto* xr = reinterpret_cast<const unsigned char*>(&a.battle.request);
    const auto* yr = reinterpret_cast<const unsigned char*>(&b.battle.request);
    for (size_t i = 0; i < sizeof(Request); i++) {
      if (xr[i] != yr[i]) std::printf(" %zu:%u/%u", i, xr[i], yr[i]);
    }
    std::printf(")");
  }
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
    if (!(x.active == y.active)) {
      std::printf(" p%d.active(bytes", s + 1);
      const auto* xa = reinterpret_cast<const unsigned char*>(&x.active);
      const auto* ya = reinterpret_cast<const unsigned char*>(&y.active);
      for (size_t i = 0; i < sizeof(ActivePokemon); i++) {
        if (xa[i] != ya[i]) std::printf(" %zu:%u/%u", i, xa[i], ya[i]);
      }
      std::printf(")");
    }
    if (!(x.conditions == y.conditions)) std::printf(" p%d.conditions", s + 1);
    if (std::memcmp(x.order, y.order, sizeof(x.order)) != 0) {
      std::printf(" p%d.order", s + 1);
    }
    if (!(x == y)) std::printf(" (p%d side)", s + 1);
  }
  std::printf("\n");
}

// The hidden parts of the battle the value leaves out, as mask_hidden, each
// of which --unmask can leave in. An ablation: leaving one in should make
// collisions appear wherever that effect is active, which shows the test can
// see it.
enum Mask : uint16_t {
  SEED = 1 << 0,
  SLEEP = 1 << 1,
  CONFUSION = 1 << 2,
  BINDING = 1 << 3,
  UPROAR = 1 << 4,
  THRASH = 1 << 5,
  DISABLE = 1 << 6,
  ENCORE = 1 << 7,
  ALL = (1 << 8) - 1,
};

constexpr std::pair<const char*, Mask> MASK_NAMES[] = {
    {"seed", SEED},       {"sleep", SLEEP},   {"confusion", CONFUSION},
    {"binding", BINDING}, {"uproar", UPROAR}, {"thrash", THRASH},
    {"disable", DISABLE}, {"encore", ENCORE},
};

uint16_t masked = ALL;

void mask(Battle& battle) {
  if (masked == ALL) return mask_hidden(battle);
  const auto one = [](uint8_t c) { return uint8_t(c != 0); };
  if (masked & SEED) battle.rng = {};
  for (Side& side : battle.sides) {
    if (masked & SLEEP) {
      for (Pokemon& mon : side.pokemon) {
        if (mon.status.asleep() && !mon.status.self_inflicted()) {
          mon.status = Status::slp(1);
        }
      }
    }
    ActivePokemon& a = side.active;
    Volatiles& v = a.volatiles;
    if (masked & CONFUSION) v.confusion = one(v.confusion);
    if (masked & BINDING) v.wrapped = one(v.wrapped);
    if ((masked & UPROAR) && v.kind == Volatiles::Kind::Uproar) v.lock = one(v.lock);
    if ((masked & THRASH) && v.kind == Volatiles::Kind::Thrash) v.lock = one(v.lock);
    if (masked & DISABLE) a.volatiles.disable_timer = one(a.volatiles.disable_timer);
    if (masked & ENCORE) a.volatiles.encore_timer = one(a.volatiles.encore_timer);
  }
}

// One update from the state, from a fresh seed after randomize_hidden.
template <typename O>
std::pair<Key, Value> sample(const Battle& battle, const Durations& durations,
                             Choice x, Choice y, RNG& rng, O& o) {
  Battle copy = battle;
  copy.rng.seed = rng.next();
  randomize_hidden(copy, durations);
  o.chance.durations = durations;
  const Result result = update(copy, x, y, o);
  mask(copy);
  Key key{x.bits, y.bits, {}};
  std::memcpy(std::get<2>(key).data(), &o.chance.actions, sizeof(Actions));
  return {key, Value{copy, o.chance.durations, result,
                     o.chance.probability.p / o.chance.probability.q}};
}

using Forced = Options<protocol::NullLog, Chance<>, Calc>;

// The chance of a Binomial(n, p) count at least as far from n * p as k, at
// most exp(-n KL(k/n || p)) (the Chernoff bound, either tail).
double tail(uint32_t k, uint32_t n, double p) {
  const double x = double(k) / double(n);
  const auto term = [](double a, double b) {
    return a == 0 ? 0.0 : a * std::log(a / b);
  };
  if (p >= 1) return k == n ? 1.0 : 0.0;
  if (p <= 0) return k == 0 ? 1.0 : 0.0;
  const double kl = term(x, p) + term(1 - x, 1 - p);
  return std::exp(-double(n) * kl);
}

// The false-alarm rate allowed per check: millions of checks per run.
constexpr double ALPHA = 1e-10;

// The chance of a count at least as far from its mean as `o`, on o's side.
// A sum of binomials: with a small variance (a deep bin, mean far below 1) a
// normal approximation is far too narrow, so use Poisson(e) there, which is
// exact in the limit and wider than the truth otherwise.
double bin_tail(double o, double e, double v) {
  if (v >= 100) return 0.5 * std::erfc(std::fabs(o - e) / std::sqrt(2 * v));
  if (e <= 0) return o > 0 ? 0.0 : 1.0;
  const auto pmf = [e](double k) {
    return std::exp(k * std::log(e) - e - std::lgamma(k + 1));
  };
  double sum = 0;
  if (o > e) {
    // The terms fall at least geometrically past the mean.
    for (double k = o; k < o + 1e6; k++) {
      const double t = pmf(k);
      sum += t;
      if (t <= sum * 1e-17) break;
    }
  } else {
    for (double k = 0; k <= o; k++) sum += pmf(k);
  }
  return std::min(sum, 1.0);
}

// A calibration bin fails below this tail chance. With ~40 bins, about
// 4e-6 false alarms per run.
constexpr double BIN_ALPHA = 1e-7;

// Every choice pair at one state. First `samples` free updates, which give
// the frequencies; then each key found is replayed `replays` times with its
// Actions forced through Calc, so that a key the free samples saw once is
// still compared. Then the probabilities: each key's free count must be
// consistent with its probability, and the keys found (disjoint branches)
// must sum to at most 1. The mass they leave, 1 - sum, can be large (many
// small branches go unseen), so it's tested on `samples` held-out draws: the
// number landing on a key not yet found must be consistent with it. Returns
// false on a failure.
bool check_state(const Battle& battle, const Durations& durations, Result r,
                 RNG& rng, uint32_t samples, uint32_t replays,
                 uint64_t game_seed) {
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  const uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
  const uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
  stats.states++;
  for (uint8_t i = 0; i < n1; i++) {
    for (uint8_t j = 0; j < n2; j++) {
      struct Entry {
        Value value;
        uint32_t free = 0;
        uint32_t total = 0;
        // Found before the held-out draws, which are classified against
        // only these.
        bool before = false;
      };
      std::map<Key, Entry> seen;
      const auto add = [&](const std::pair<Key, Value>& kv, bool free) {
        auto [it, inserted] = seen.try_emplace(kv.first, Entry{kv.second});
        it->second.total++;
        if (free) it->second.free++;
        if (inserted || it->second.value == kv.second) return true;
        std::printf("game %llu, turn %u: collision%s\n",
                    (unsigned long long)game_seed, battle.turn,
                    free ? "" : " (forced replay)");
        print_state(battle, durations);
        print_key(kv.first);
        print_diff(it->second.value, kv.second);
        stats.collisions++;
        return false;
      };

      for (uint32_t k = 0; k < samples; k++) {
        Observed o{};
        if (!add(sample(battle, durations, c1[i], c2[j], rng, o), true)) {
          return false;
        }
      }
      std::vector<Key> keys;
      for (const auto& [key, e] : seen) keys.push_back(key);
      for (const Key& key : keys) {
        for (uint32_t k = 0; k < replays; k++) {
          Forced o{};
          std::memcpy(&o.calc.overrides, std::get<2>(key).data(),
                      sizeof(Actions));
          const auto kv = sample(battle, durations, c1[i], c2[j], rng, o);
          if (kv.first != key) stats.unreproduced++;
          if (!add(kv, false)) return false;
        }
      }

      double total = 0;
      bool ok = true;
      for (const auto& [key, e] : seen) {
        total += e.value.probability;
        if (e.total == 1) stats.untested++;
        // A key expected less than once isn't tested alone: Beat Up has
        // about 3e7 keys, so some unlikely key turns up, even twice, by
        // chance (docs/TESTING.md, "Probability calibration"). The pooled
        // calibration bins still test such keys.
        const bool rare = samples * e.value.probability < 1;
        if (!rare && tail(e.free, samples, e.value.probability) < ALPHA) {
          std::printf("game %llu, turn %u: key seen %u of %u free samples, "
                      "probability %g\n",
                      (unsigned long long)game_seed, battle.turn, e.free,
                      samples, e.value.probability);
          print_state(battle, durations);
          print_key(key);
          stats.frequency++;
          ok = false;
        }
      }
      const double rest = std::clamp(1 - total, 0.0, 1.0);
      double mass[Totals::BINS] = {};
      for (auto& [key, e] : seen) {
        mass[bin(e.value.probability)] += e.value.probability;
        e.before = true;
      }
      mass[Totals::UNSEEN] = rest;
      uint32_t hits[Totals::BINS] = {};
      for (uint32_t k = 0; k < samples; k++) {
        Observed o{};
        const auto kv = sample(battle, durations, c1[i], c2[j], rng, o);
        const auto it = seen.find(kv.first);
        hits[it == seen.end() || !it->second.before
                 ? Totals::UNSEEN
                 : bin(it->second.value.probability)]++;
        if (!add(kv, false)) return false;
      }
      const uint32_t fresh = hits[Totals::UNSEEN];
      for (int b = 0; b < Totals::BINS; b++) {
        const double m = std::min(mass[b], 1.0);
        stats.observed[b] += hits[b];
        stats.expected[b] += samples * m;
        stats.variance[b] += samples * m * (1 - m);
      }
      if (total > 1 + 1e-9 || tail(fresh, samples, rest) < ALPHA) {
        std::printf("game %llu, turn %u: choices %u %u, the %zu keys' "
                    "probabilities sum to %.12g, and %u of %u held-out "
                    "samples found new keys\n",
                    (unsigned long long)game_seed, battle.turn,
                    unsigned(c1[i].bits), unsigned(c2[j].bits), seen.size(),
                    total, fresh, samples);
        print_state(battle, durations);
        if (std::getenv("HOENN_DUMP_KEYS")) {
          for (const auto& [key, e] : seen) {
            std::printf("  p %.6g free %u", e.value.probability, e.free);
            print_key(key);
          }
        }
        stats.mass++;
        ok = false;
      }
      stats.missing += 1 - total;
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
          uint32_t replays, uint32_t every) {
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
                     replays, game_seed)) {
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
  uint32_t replays = 4;
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
    } else if (arg == "--replays" && i + 1 < argc) {
      replays = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--unmask" && i + 1 < argc) {
      const std::string name = argv[++i];
      bool found = false;
      for (const auto& [n, m] : MASK_NAMES) {
        if (name == n) masked &= uint16_t(~m), found = true;
      }
      if (!found) {
        std::fprintf(stderr, "check_chance: unknown mask %s\n", name.c_str());
        return 2;
      }
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
    play(teams, replay_seed, samples, replays, every);
  } else {
    RNG seeds{seed};
    for (uint64_t i = 0; i < games; i++) play(teams, seeds.next(), samples, replays, every);
  }
  std::printf("check_chance: %llu states, %llu choice pairs, %.1f keys per "
              "pair (max %llu), %llu collisions\n",
              (unsigned long long)stats.states,
              (unsigned long long)stats.pairs,
              stats.pairs ? double(stats.keys) / double(stats.pairs) : 0.0,
              (unsigned long long)stats.max_keys,
              (unsigned long long)stats.collisions);
  std::printf("  %llu keys never compared (%.2f%%), %llu forced replays "
              "landed elsewhere\n",
              (unsigned long long)stats.untested,
              stats.keys ? 100.0 * double(stats.untested) / double(stats.keys)
                         : 0.0,
              (unsigned long long)stats.unreproduced);
  std::printf("  probability: %llu sum failures, %llu frequency failures, "
              "mean unseen mass per pair %.4f\n",
              (unsigned long long)stats.mass,
              (unsigned long long)stats.frequency,
              stats.pairs ? stats.missing / double(stats.pairs) : 0.0);
  int calibration = 0;
  // The variance lets a runner pool bins over shards, so every bin with an
  // expectation is printed: leaving out the empty ones would bias the pool.
  std::printf("  calibration of held-out draws (bin: observed / expected, "
              "tail chance; exact expected and variance):\n");
  for (int b = 0; b < Totals::BINS; b++) {
    const double o = stats.observed[b], e = stats.expected[b];
    const double v = stats.variance[b];
    if (o == 0 && e == 0) continue;
    const double p = bin_tail(o, e, v);
    const bool bad = p < BIN_ALPHA;
    calibration += bad;
    if (b == Totals::UNSEEN) {
      std::printf("    unseen        %10.0f / %12.1f  p %8.2g  [%.17g %.17g]%s\n",
                  o, e, p, e, v, bad ? "  FAIL" : "");
    } else {
      std::printf("    2^-%-2d..2^-%-2d %10.0f / %12.1f  p %8.2g  [%.17g %.17g]%s\n",
                  b + 1, b, o, e, p, e, v, bad ? "  FAIL" : "");
    }
  }
  return stats.collisions || stats.mass || stats.frequency || calibration
             ? 1
             : 0;
}
