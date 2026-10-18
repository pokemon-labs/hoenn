// Random self-play with the Hoenn engine, printing Showdown protocol logs.
//
// Plays games between random team pairs from Showdown export files, with each
// player picking uniformly among its legal choices. Prints the full log of the
// first `show` games, then a summary. Along the way it checks, independently
// of the engine's own asserts:
//
//   - the log agrees with the state: after every update, the HP, faint and
//     active Pokémon that a reader of the log would track (from |switch|,
//     |drag|, |-damage|, |-heal|, |-sethp|, |faint|) match the battle's;
//   - a finished game's result agrees with who has Pokémon left, and the log
//     ends with |win| or |tie| as Showdown's does;
//   - a game replays exactly: the same seed and the same choices give the
//     same log, line for line;
//   - a copy is independent: stepping a copy doesn't touch the original.
//
// A reference, not a build target. From the repo root:
//   g++ -std=c++20 -O2 -Isrc examples/rollout.cpp -o rollout
//   ./rollout [games [seed [show [replay_every [team files...]]]]]
// Defaults: 100 games, seed 1, show 1, replay every 10th game (0 turns it
// off), and every .txt in teams/ou/ (relative to the working directory).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "calc.hpp"
#include "chance.hpp"
#include "data/display.hpp"
#include "format.hpp"
#include "helpers.hpp"
#include "log.hpp"
#include "mechanics.hpp"
#include "options.hpp"
#include "showdown.hpp"

using namespace hoenn;

namespace {

using Logged = Options<protocol::FixedLog, Chance<>, NullCalc>;
using Null = Options<protocol::NullLog, NullChance, NullCalc>;
using Lines = std::vector<std::string>;

struct Team {
  std::string file;
  std::string name;
  std::vector<helpers::Pokemon> pokemon;
};

std::string species_name(Species s) {
  return std::string(display::SPECIES_NAMES[size_t(s)]);
}

// What a reader of the omniscient log knows: HP and who is active.
//
// Showdown's HP lines come as |split|pN triplets: the marker, the line with
// exact HP (the owner's view), then the public line with HP as a percentage.
// The tracker reads the exact line and skips the public one.
struct LogTracker {
  std::map<std::pair<std::string, std::string>, std::pair<int, int>> hp;
  std::map<std::string, std::string> active;
  std::optional<std::string> winner;

  void feed(const Lines& lines) {
    bool private_next = false, skip_public = false;
    for (const std::string& l : lines) {
      if (l.starts_with("|split|")) {
        private_next = true;
        continue;
      }
      if (private_next) {
        private_next = false;
        skip_public = true;
      } else if (skip_public) {
        skip_public = false;
        continue;
      }
      line(l);
    }
  }

  void line(const std::string& l) {
    std::vector<std::string> parts;
    std::stringstream ss(l);
    std::string part;
    std::getline(ss, part, '|'); // before the leading '|'
    while (std::getline(ss, part, '|')) parts.push_back(part);
    if (parts.empty()) return;
    const std::string& kind = parts[0];
    if (kind == "switch" || kind == "drag") {
      const auto id = ident(parts[1]);
      active[id.first] = id.second;
      set_hp(id, parts[3]);
    } else if (kind == "-damage" || kind == "-heal" || kind == "-sethp") {
      set_hp(ident(parts[1]), parts[2]);
    } else if (kind == "faint") {
      auto& seen = hp.at(ident(parts[1]));
      seen.first = 0;
    } else if (kind == "win") {
      winner = parts[1];
    } else if (kind == "tie") {
      winner = "tie";
    }
  }

  void set_hp(const std::pair<std::string, std::string>& id,
              const std::string& health) {
    const std::string cur = health.substr(0, health.find(' '));
    if (cur == "0") {
      hp.at(id).first = 0;
    } else {
      const size_t slash = cur.find('/');
      hp[id] = {std::stoi(cur.substr(0, slash)), std::stoi(cur.substr(slash + 1))};
    }
  }

  // "p1a: Skarmory" -> ("p1", "Skarmory")
  static std::pair<std::string, std::string> ident(const std::string& s) {
    const size_t colon = s.find(": ");
    return {s.substr(0, 2), s.substr(colon + 2)};
  }
};

// The log reader's view against the engine's state.
void check_state(const Battle& battle, const LogTracker& tracker,
                 const std::string& where, Lines& errors) {
  for (const Player player : {Player::P1, Player::P2}) {
    const std::string side = player == Player::P1 ? "p1" : "p2";
    const Side& s = battle.side(player);
    std::vector<const Pokemon*> party; // SLOT order, active first
    for (uint8_t slot = 1; slot <= 6; slot++) {
      if (s.order[slot - 1] == 0) continue;
      const Pokemon& p = s.get(slot);
      if (p.species != Species::None) party.push_back(&p);
    }
    std::set<Species> distinct;
    for (const Pokemon* p : party) distinct.insert(p->species);
    if (distinct.size() != party.size()) continue; // the log can't tell them apart

    const auto active = tracker.active.find(side);
    const std::string seen_active =
        active == tracker.active.end() ? "None" : active->second;
    if (seen_active != species_name(party[0]->species)) {
      errors.push_back(where + ": " + side + " active is " +
                       species_name(party[0]->species) + ", log says " +
                       seen_active);
    }
    for (const Pokemon* p : party) {
      const auto seen = tracker.hp.find({side, species_name(p->species)});
      if (seen == tracker.hp.end()) continue; // never sent out
      if (seen->second != std::pair<int, int>{p->hp, p->stats.hp}) {
        errors.push_back(where + ": " + side + " " + species_name(p->species) +
                         " has " + std::to_string(p->hp) + "/" +
                         std::to_string(p->stats.hp) + ", log says " +
                         std::to_string(seen->second.first) + "/" +
                         std::to_string(seen->second.second));
      }
    }
  }
}

const char* result_name(Result::Type t) {
  switch (t) {
  case Result::None: return "none";
  case Result::Win: return "win";
  case Result::Lose: return "lose";
  case Result::Tie: return "tie";
  case Result::Error: return "error";
  }
  return "?";
}

std::string label(const Battle& battle, Player p, Choice c) {
  switch (c.type()) {
  case Choice::Pass: return "pass";
  case Choice::Move:
    if (c.data() == 0) return "move (forced/Struggle)";
    return "move " + std::string(display::MOVE_NAMES[size_t(
                         battle.side(p).get(1).moves[c.data() - 1].id)]);
  case Choice::Switch:
    return "switch " + species_name(battle.side(p).get(c.data()).species);
  }
  return "?";
}

// A battle with its log: the buffer, the writer into it, the formatter's
// names, and one Chance carried across updates.
struct Game {
  Battle battle;
  format::Names names;
  std::vector<uint8_t> buffer = std::vector<uint8_t>(protocol::MAX_LOG_SIZE);
  protocol::Writer writer{buffer.data(), buffer.size(), 0};
  Logged o{protocol::fixed_log(writer), {}, {}};

  // Showdown's default nickname is the species name; the parser drops
  // nicknames.
  Game(const Team& t1, const Team& t2, uint64_t seed)
      : battle(*helpers::init_battle(seed, t1.pokemon, t2.pokemon)) {
    const Team* teams[2] = {&t1, &t2};
    for (size_t s = 0; s < 2; s++) {
      if (!teams[s]->name.empty()) names.player[s] = teams[s]->name;
      for (size_t i = 0; i < teams[s]->pokemon.size(); i++) {
        names.nickname[s][i] = species_name(teams[s]->pokemon[i].species);
      }
    }
  }
  Game(const Game&) = delete; // the writer and the log point into this

  Result step(Choice c1, Choice c2, Lines& lines) {
    writer.reset();
    const Result r = update(battle, c1, c2, o);
    format::Formatter f(names);
    f.format(std::span<const uint8_t>(buffer.data(), writer.pos), lines);
    return r;
  }

  double probability() const {
    const auto& p = o.chance.probability;
    return double(p.p) / double(p.q);
  }
};

void check_end(const Battle& battle, Result result, const LogTracker& tracker,
               const Team& t1, const Team& t2, Lines& errors) {
  int left[2] = {};
  for (const Player player : {Player::P1, Player::P2}) {
    for (const Pokemon& p : battle.side(player).pokemon) {
      if (p.species != Species::None && p.hp > 0) left[uint8_t(player)]++;
    }
  }
  const std::string counts =
      "[" + std::to_string(left[0]) + ", " + std::to_string(left[1]) + "]";
  const Result::Type t = result.type();
  if (t == Result::Win && !(left[0] > 0 && left[1] == 0)) {
    errors.push_back("P1 won with " + counts + " Pokemon left");
  }
  if (t == Result::Lose && !(left[1] > 0 && left[0] == 0)) {
    errors.push_back("P2 won with " + counts + " Pokemon left");
  }
  if (t == Result::Tie && left[0] > 0 && left[1] > 0 &&
      battle.turn < Battle::MAX_TURNS) {
    errors.push_back("tie before the turn cap with " + counts + " Pokemon left");
  }
  const std::string expected = t == Result::Win    ? (t1.name.empty() ? "p1" : t1.name)
                               : t == Result::Lose ? (t2.name.empty() ? "p2" : t2.name)
                                                   : "tie";
  if (tracker.winner != expected) {
    errors.push_back(std::string("result ") + result_name(t) + ", log's winner " +
                     tracker.winner.value_or("None"));
  }
}

struct Played {
  Result result;
  uint16_t turns;
  Lines log;
  std::vector<std::pair<Choice, Choice>> picks;
  Lines errors;
  std::pair<size_t, size_t> pair;
};

// One game from its seed.
Played play(const std::vector<Team>& teams, uint64_t game_seed, bool show) {
  std::mt19937_64 rng(game_seed);
  std::uniform_int_distribution<size_t> pick_team(0, teams.size() - 1);
  const size_t a = pick_team(rng), b = pick_team(rng);
  Game game(teams[a], teams[b], rng());
  Battle& battle = game.battle;
  LogTracker tracker;
  Played out{};
  out.pair = {a, b};

  if (show) {
    std::printf("=== game %llu: %s  vs  %s\n", (unsigned long long)game_seed,
                teams[a].name.c_str(), teams[b].name.c_str());
    for (const Team* t : {&teams[a], &teams[b]}) {
      std::string species;
      for (const auto& p : t->pokemon) {
        species += (species.empty() ? "" : " / ") + species_name(p.species);
      }
      std::printf("    %s: %s\n", t == &teams[a] ? "p1" : "p2", species.c_str());
    }
  }

  Lines lines;
  Result result = game.step(Choice::pass(), Choice::pass(), lines);
  out.log = lines;
  tracker.feed(lines);
  if (show) {
    for (const auto& l : lines) std::printf("%s\n", l.c_str());
  }
  check_state(battle, tracker, "start", out.errors);

  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  std::uniform_real_distribution<double> unit(0, 1);
  while (!result.over()) {
    const uint8_t n1 = choices(battle, Player::P1, result.p1(), c1);
    const uint8_t n2 = choices(battle, Player::P2, result.p2(), c2);
    const Choice x = c1[std::uniform_int_distribution<uint8_t>(0, n1 - 1)(rng)];
    const Choice y = c2[std::uniform_int_distribution<uint8_t>(0, n2 - 1)(rng)];
    out.picks.push_back({x, y});

    // A copy must be a separate battle.
    if (unit(rng) < 0.05) {
      const Battle before = battle;
      Battle other = battle;
      update(other, x, y, Null{});
      if (std::memcmp(&before, &battle, sizeof(Battle)) != 0) {
        out.errors.push_back("turn " + std::to_string(battle.turn) +
                             ": stepping a copy changed the original");
      }
    }

    const uint16_t turn = battle.turn;
    const std::string where = "turn " + std::to_string(turn);
    const std::string l1 = label(battle, Player::P1, x);
    const std::string l2 = label(battle, Player::P2, y);
    lines.clear();
    result = game.step(x, y, lines);
    out.log.insert(out.log.end(), lines.begin(), lines.end());
    tracker.feed(lines);
    const double prob = game.probability();
    if (show) {
      std::printf("    -- p1 %s | p2 %s  (branch probability %.3g)\n",
                  l1.c_str(), l2.c_str(), prob);
      for (const auto& l : lines) std::printf("%s\n", l.c_str());
    }
    check_state(battle, tracker, where, out.errors);
    if (!(prob > 0 && prob <= 1)) {
      out.errors.push_back(where + ": probability " + std::to_string(prob));
    }
  }

  check_end(battle, result, tracker, teams[a], teams[b], out.errors);
  if (show) {
    std::printf("=== %s after %u turns\n\n", result_name(result.type()),
                unsigned(battle.turn));
  }
  out.result = result;
  out.turns = battle.turn;
  return out;
}

// The same seed and choices again; the log must come out the same.
Lines replay(const std::vector<Team>& teams, uint64_t game_seed,
             const Played& played) {
  std::mt19937_64 rng(game_seed);
  std::uniform_int_distribution<size_t> pick_team(0, teams.size() - 1);
  pick_team(rng), pick_team(rng);
  Game game(teams[played.pair.first], teams[played.pair.second], rng());
  Lines log;
  game.step(Choice::pass(), Choice::pass(), log);
  for (const auto& [x, y] : played.picks) game.step(x, y, log);
  return log;
}

} // namespace

int main(int argc, char** argv) {
  const uint64_t games = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100;
  const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  const uint64_t show = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
  const uint64_t replay_every = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 10;

  std::vector<std::string> files(argv + std::min(argc, 5), argv + argc);
  if (files.empty()) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("teams/ou", ec)) {
      if (e.path().extension() == ".txt") files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
  }

  std::vector<Team> teams;
  for (const std::string& file : files) {
    std::ifstream in(file);
    if (!in) {
      std::fprintf(stderr, "rollout: can't open %s\n", file.c_str());
      return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    RNG parse_rng{0}; // draws a gender when nothing decides it
    for (const auto& t : showdown::parse_teams(ss.str(), parse_rng)) {
      if (!t.value) {
        std::fprintf(stderr, "rollout: %s: %s\n", file.c_str(), t.error.c_str());
        return 2;
      }
      teams.push_back({file, t.value->name, t.value->pokemon});
    }
  }
  if (teams.empty() || games == 0) {
    std::fprintf(stderr,
                 "usage: rollout [games [seed [show [replay_every [team files...]]]]]\n"
                 "(no teams found, or games is 0)\n");
    return 2;
  }
  std::printf("%zu teams from %zu files\n\n", teams.size(), files.size());

  std::mt19937_64 seeds(seed);
  uint64_t outcomes[5] = {};
  uint64_t failed = 0, replays = 0, total_turns = 0;
  uint16_t min_turns = UINT16_MAX, max_turns = 0;
  for (uint64_t i = 0; i < games; i++) {
    const uint64_t game_seed = seeds() >> 1; // 63 bits, as the Python script
    Played p = play(teams, game_seed, i < show);
    outcomes[unsigned(p.result.type()) % 5]++;
    total_turns += p.turns;
    min_turns = std::min(min_turns, p.turns);
    max_turns = std::max(max_turns, p.turns);
    if (replay_every && i % replay_every == 0) {
      replays++;
      if (replay(teams, game_seed, p) != p.log) {
        p.errors.push_back("replay with the same seed and choices differs");
      }
    }
    if (!p.errors.empty()) {
      failed++;
      std::printf("game %llu: %zu problem(s)\n", (unsigned long long)game_seed,
                  p.errors.size());
      for (size_t e = 0; e < p.errors.size() && e < 5; e++) {
        std::printf("    %s\n", p.errors[e].c_str());
      }
    }
  }

  std::printf("%llu games: P1 won %llu, P2 won %llu, ties %llu; turns min %u, "
              "mean %.1f, max %u\n",
              (unsigned long long)games,
              (unsigned long long)outcomes[Result::Win],
              (unsigned long long)outcomes[Result::Lose],
              (unsigned long long)outcomes[Result::Tie], unsigned(min_turns),
              double(total_turns) / double(games), unsigned(max_turns));
  std::printf("%llu games replayed for determinism\n", (unsigned long long)replays);
  std::printf("games with a problem: %llu\n", (unsigned long long)failed);
  return failed ? 1 : 0;
}
