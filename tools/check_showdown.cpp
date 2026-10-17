// The realizability test (TESTING.md): replays Showdown games
// from tools/showdown_traces.cjs and checks that some branch of ours
// produces each step.
//
// Per game: build the battle from both teams' export text, taking
// Showdown's genders (random where a set names none) and checking its
// abilities and stats against ours. Then, per step:
//
// - our choices() must equal Showdown's legal set;
// - rerun `update` on the played pair, each time from a fresh RNG seed and
//   after randomize_hidden, until a branch matches, up to --tries; continue
//   from the branch that matched.
//
// A branch matches when its Result agrees with Showdown's (the outcome, or
// the kind of choice each side makes next), its log agrees with Showdown's
// (below) and, as a temporary guard until the log is complete, each side's Pokémon by SLOT agree in species, HP, max
// HP, status, PP and item, the active Pokémon in boosts, Substitute HP and the
// volatiles in VOLATILES, each side's Spikes and screens, the weather, and a replacement request comes before or after
// the residuals as Showdown's (its |upkeep). Without the log, state the guard leaves out can drift: a
// failure can come from an earlier step that matched the wrong branch. The log comparison replaces the guard as the messages are
// written (TESTING.md). A game stops at its first failure, since
// we can't stay in step with Showdown after it.
//
// The log: our binary log through format.hpp against the step's raw
// Showdown lines, |split| triplets included, after dropping what
// Showdown's FILTER drops. Messages are written in groups (README,
// "Protocol"), so only the message types in LOGGED are compared, on both
// sides; --no-log turns the comparison off.
//
// A failure to match within the cap can be a bug or bad luck: rerun it with
// a higher --tries.
//
// Usage: check_showdown [--tries N] [--seed N] [--game G] [--quiet] [--why]
//                       [--no-log] traces.jsonl...
//
// A failing step reports the longest log prefix among the tries whose state
// matched. --why also tallies every try's mismatch and reports the longest
// log prefix of any try, to tell a branch we never produce from a rare one.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "options.hpp"
#include "log.hpp"
#include "data/names.hpp"
#include "format.hpp"
#include "helpers.hpp"
#include "mechanics.hpp"
#include "showdown.hpp"
#include "json.hpp"

using namespace hoenn;
using namespace hoenn::names;

namespace {

using Observed = Options<protocol::FixedLog, Chance<>, Calc>;

// Metronome's pick, read from Showdown's move line ([from] Metronome) and
// forced through Calc: a random search can't find two picks (1/336 each)
// plus their rolls within the try cap. Only the pick is forced.
void force_metronome(const json::Value& log, Calc& calc) {
  for (const json::Value& l : log.array) {
    const std::string& line = l.string;
    if (!line.starts_with("|move|p") || line.find("|[from] Metronome") == std::string::npos) {
      continue;
    }
    const Player p = line[7] == '1' ? Player::P1 : Player::P2;
    const size_t a = line.find('|', 6) + 1, b = line.find('|', a);
    const std::string_view name(line.data() + a, b - a);
    for (uint16_t i = 1; i < CARTRIDGE_MOVES; i++) {
      if (display::MOVE_NAMES[i] == name && detail::metronome_can_call(Move(i))) {
        calc.overrides.get(p).metronome.set(Move(i));
        break;
      }
    }
  }
}

// Whether Showdown's log of a step uses a multi-hit move, called ones
// (Metronome, Sleep Talk, Mirror Move, Assist) included: each gets its own
// |move| line. A game whose first failure is such a step leaves the count
// (TESTING.md, "Pass criteria"): its rolls and crits are too many for the
// random search.
bool multi_hit(const json::Value& log) {
  for (const json::Value& l : log.array) {
    const std::string& line = l.string;
    if (!line.starts_with("|move|p")) continue;
    const size_t a = line.find('|', 6) + 1, b = line.find('|', a);
    const std::string_view name(line.data() + a, b - a);
    for (uint16_t i = 1; i < CARTRIDGE_MOVES; i++) {
      if (display::MOVE_NAMES[i] != name) continue;
      switch (get(Move(i)).effect) {
      case Effect::MultiHit:
      case Effect::DoubleHit:
      case Effect::Twineedle:
      case Effect::TripleKick:
      case Effect::BeatUp: return true;
      default: break;
      }
    }
  }
  return false;
}

// A log unit: one line, or a |split| triplet as one.
using Unit = std::vector<std::string>;

// The message type of a line: "|-damage|..." is "-damage".
std::string kind(const std::string& line) {
  if (line.size() < 2 || line[0] != '|') return "";
  const size_t bar = line.find('|', 1);
  return line.substr(1, bar == std::string::npos ? std::string::npos : bar - 1);
}

// The message types written so far, compared on both sides: group 1, the
// frame, group 2, the hit results, groups 3 to 5 (src/README.md, "Protocol").
const std::set<std::string> LOGGED = {
    "move", "switch", "drag", "cant", "faint", "turn", "win", "tie",
    "-damage", "-heal", "-fail", "-miss", "-immune", "-activate",
    // Group 3a: the rest of the hit results.
    "-crit", "-supereffective", "-resisted", "-hitcount", "-prepare", "-anim",
    // Group 3b: major status.
    "-status", "-curestatus", "-cureteam",
    // Group 3c: boosts.
    "-boost", "-unboost", "-setboost", "-copyboost", "-clearallboost", "-clearnegativeboost",
    // Group 3d: volatiles.
    "-start", "-end", "-singleturn", "-singlemove", "-sethp", "-fieldactivate",
    // Group 4: side conditions and weather.
    "-sidestart", "-sideend", "-weather",
    // Group 5: items and abilities.
    "-item", "-enditem", "-ability",
    // Forecast, with the step 5 group 1 moves (Weather Ball).
    "-formechange",
    // Step 5 group 2: OHKO moves.
    "-ohko",
    // Step 5 group 3: recharge.
    "-mustrecharge",
};

// Whether a line of a LOGGED type is compared.
bool logged(const std::string& line) { return LOGGED.count(kind(line)) != 0; }

// The units of `lines` whose type is in LOGGED.
std::vector<Unit> units(const std::vector<std::string>& lines) {
  std::vector<Unit> out;
  for (size_t i = 0; i < lines.size(); i++) {
    Unit u{lines[i]};
    if (kind(lines[i]) == "split" && i + 2 < lines.size()) {
      u = {lines[i], lines[i + 1], lines[i + 2]};
      i += 2;
    }
    if (logged(u.back() == "" ? u[1] : u.back())) out.push_back(u);
  }
  return out;
}

std::string show(const Unit& u) {
  std::string s;
  for (const std::string& l : u) s += (s.empty() ? "" : " / ") + l;
  return s;
}

// Whether the move line's [still] and [miss] are compared (since group 2).
// If not, both are dropped, and a target either side blanked for [still]
// is blanked on both.
constexpr bool MOVE_ATTRIBUTES = true;

Item item_of(std::string_view id);

// Showdown's toID: lowercase letters and digits.
std::string to_id(std::string_view name) {
  std::string id;
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      id += char(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return id;
}

// Field `n` of a protocol line ("|-item|p2a: X|Name|..." has the item at 3),
// as [start, end), or npos if it has fewer fields.
std::pair<size_t, size_t> field(const std::string& line, int n) {
  size_t at = 0;
  for (int i = 0; i < n; i++) {
    at = line.find('|', at);
    if (at == std::string::npos) return {at, at};
    at++;
  }
  const size_t end = line.find('|', at);
  return {at, end == std::string::npos ? line.size() : end};
}

// The engine's items without an effect are one Other (and one Mail), which
// the log can't name: our -item and -enditem lines leave the name empty.
// Those match any Showdown item the engine takes as the same, as the state
// compares items (item_of).
void normalize_items(Unit& a, const Unit& b) {
  if (a.size() != b.size()) return;
  for (size_t i = 0; i < a.size(); i++) {
    const std::string k = kind(a[i]);
    if ((k != "-item" && k != "-enditem") || kind(b[i]) != k) continue;
    const auto [x0, x1] = field(a[i], 3);
    const auto [y0, y1] = field(b[i], 3);
    if (x0 == std::string::npos || y0 == std::string::npos || x1 != x0) continue;
    const Item theirs = item_of(to_id(std::string_view(b[i]).substr(y0, y1 - y0)));
    if (theirs != Item::Other && theirs != Item::Mail) continue;
    a[i].insert(x0, b[i], y0, y1 - y0);
  }
}

// The move lines of two units, normalized as MOVE_ATTRIBUTES says, and the
// unnamed items (normalize_items).
void normalize(Unit& a, Unit& b) {
  normalize_items(a, b);
  if (MOVE_ATTRIBUTES || a.size() != 1 || b.size() != 1 || kind(a[0]) != "move" ||
      kind(b[0]) != "move") {
    return;
  }
  bool still = false;
  for (std::string* s : {&a[0], &b[0]}) {
    for (const char* attr : {"|[still]", "|[miss]"}) {
      if (const size_t at = s->find(attr); at != std::string::npos) {
        still |= std::string_view(attr) == "|[still]";
        s->erase(at, std::strlen(attr));
      }
    }
  }
  if (!still) return;
  for (std::string* s : {&a[0], &b[0]}) {
    size_t bar = 0;
    for (int i = 0; i < 3; i++) bar = s->find('|', bar + 1);
    const size_t next = s->find('|', bar + 1);
    s->erase(bar + 1, next == std::string::npos ? std::string::npos : next - bar - 1);
  }
}

struct Config {
  uint64_t tries = 1000000;
  uint64_t seed = 1;
  int64_t game = -1;
  bool quiet = false;
  bool why = false; // tally every try's mismatch at a failing step
  bool log = true;  // compare the log
};

struct Counts {
  uint64_t games = 0;
  uint64_t completed = 0;
  uint64_t steps = 0;
  uint64_t tries = 0;
  uint64_t max_tries = 0;
  uint64_t setup = 0;
  uint64_t choices = 0;
  uint64_t unmatched = 0;
  uint64_t log_only = 0; // unmatched steps where some try matched the state
  uint64_t multi_hit = 0; // unmatched at a multi-hit step: neither matched
                          // nor failed, and not in `unmatched`
};

Counts stats;

std::string_view species_id(Species s) { return SPECIES_IDS[uint16_t(s)]; }

// The item as Showdown's Pokemon.item: '' for none or knocked off.
std::string_view item_id(Item item) {
  if (item == Item::None || item == Item::KnockedOff) return "";
  for (const ItemName& n : ITEM_IDS) {
    if (n.item == item) return n.id;
  }
  return "?";
}

// Showdown's item id as ours: the items without an effect are all Other (or
// Mail), so two of them compare equal.
Item item_of(std::string_view id) {
  if (id.empty()) return Item::None;
  for (const ItemName& n : ITEM_IDS) {
    if (n.id == id) return n.item;
  }
  return Item::KnockedOff; // no Showdown item maps here, so it never matches
}

// As Showdown's Pokemon.status, which is "fnt" once fainted.
const char* status_id(const Pokemon& p) {
  const Status s = p.status;
  if (p.hp == 0) return "fnt";
  if (s.sleep()) return "slp";
  if (s.bits == Status::TOX) return "tox";
  if (s.bits & Status::PSN) return "psn";
  if (s.bits & Status::BRN) return "brn";
  if (s.bits & Status::FRZ) return "frz";
  if (s.bits & Status::PAR) return "par";
  return "";
}

// Our choice as Showdown writes it, with "forced" for a locked move or
// Struggle (Choice data 0), which Showdown offers as "move 1".
std::string choice_string(Choice c) {
  switch (c.type()) {
  case Choice::Pass: return "pass";
  case Choice::Move:
    return c.data() == 0 ? "forced" : "move " + std::to_string(c.data());
  case Choice::Switch: return "switch " + std::to_string(c.data());
  }
  return "?";
}

// Whether every move of the side's active Pokémon is disabled or out of PP
// in `prev`'s state. Imprison disables moves as 'hidden': Showdown's request
// still offers them, and any of them becomes Struggle (side.ts chooseMove).
bool hidden_struggle(const json::Value* prev, const char* side) {
  if (!prev) return false;
  const json::Value& moves = (*prev)["state"][std::string(side) + "active"]["moves"];
  if (moves.array.empty()) return false;
  for (const json::Value& m : moves.array) {
    if (!m[2].boolean && m[1].integer() > 0) return false;
  }
  return true;
}

bool showdown_forced(const json::Value& step, const json::Value* prev, const char* side) {
  return step["forced"][side].boolean || hidden_struggle(prev, side);
}

std::vector<std::string> showdown_choices(const json::Value& step, const json::Value* prev,
                                          const char* side) {
  std::vector<std::string> out;
  const bool forced = showdown_forced(step, prev, side);
  for (const json::Value& c : step["choices"][side].array) {
    if (forced && c.string.rfind("move ", 0) == 0) {
      if (c.string == "move 1") out.push_back("forced");
    } else {
      out.push_back(c.string);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

Choice to_choice(const std::string& s, bool forced) {
  if (s == "pass") return Choice::pass();
  const uint8_t n = uint8_t(std::atoi(s.c_str() + s.find(' ') + 1));
  if (s.rfind("switch ", 0) == 0) return Choice::swap(n);
  return Choice::move(forced ? 0 : n);
}

// The kind of choice a legal set asks for.
Choice::Type request_kind(const json::Value& choices) {
  for (const json::Value& c : choices.array) {
    if (c.string == "pass") return Choice::Pass;
    if (c.string.rfind("move ", 0) == 0) return Choice::Move;
  }
  return Choice::Switch;
}

void print_log(const json::Value& step) {
  for (const json::Value& l : step["log"].array) {
    std::printf("    %s\n", l.string.c_str());
  }
}

// Our active Pokémon's moves beside Showdown's, after the previous step.
void print_active(const Battle& battle, Player p, const json::Value* prev) {
  const char* side = p == Player::P1 ? "p1" : "p2";
  const ActivePokemon& a = battle.side(p).active;
  std::printf("  ours:");
  for (const MoveSlot& m : a.moves) {
    const std::string_view id = MOVE_IDS[uint16_t(m.id)];
    std::printf(" %.*s(%u)", int(id.size()), id.data(), m.pp);
  }
  std::printf("\n");
  if (!prev) return;
  const json::Value& sa = (*prev)["state"][std::string(side) + "active"];
  std::printf("  Showdown:");
  for (const json::Value& m : sa["moves"].array) {
    std::printf(" %s(%lld%s)", m[0].string.c_str(), (long long)m[1].integer(),
                m[2].boolean ? ", disabled" : "");
  }
  std::printf("\n  Showdown's volatiles:");
  for (const json::Value& v : sa["volatiles"].array) {
    std::printf(" %s", v.string.c_str());
  }
  std::printf("\n  Showdown's log:\n");
  print_log(*prev);
}

// The volatiles the guard compares, by Showdown's name, and whether our
// active Pokémon has each.
struct VolatileName {
  const char* name;
  bool (*has)(const ActivePokemon&);
};

constexpr VolatileName VOLATILES[] = {
    {"confusion", [](const ActivePokemon& a) { return a.volatiles.confusion != 0; }},
    {"substitute", [](const ActivePokemon& a) { return a.volatiles.substitute; }},
    {"leechseed", [](const ActivePokemon& a) { return a.volatiles.leech_seed; }},
    {"encore", [](const ActivePokemon& a) { return a.encored_move != Move::None; }},
    {"taunt", [](const ActivePokemon& a) { return a.taunt != 0; }},
    {"disable", [](const ActivePokemon& a) { return a.disabled_move != Move::None; }},
    {"focusenergy", [](const ActivePokemon& a) { return a.volatiles.focus_energy; }},
    {"ingrain", [](const ActivePokemon& a) { return a.volatiles.rooted; }},
    {"perishsong", [](const ActivePokemon& a) { return a.volatiles.perish_song; }},
    {"curse", [](const ActivePokemon& a) { return a.volatiles.cursed; }},
    {"nightmare", [](const ActivePokemon& a) { return a.volatiles.nightmare; }},
    {"yawn", [](const ActivePokemon& a) { return a.volatiles.yawn != 0; }},
    {"attract", [](const ActivePokemon& a) { return a.volatiles.infatuated; }},
    {"trapped", [](const ActivePokemon& a) { return a.volatiles.escape_prevention; }},
    {"partiallytrapped", [](const ActivePokemon& a) { return a.volatiles.wrapped != 0; }},
    {"flashfire", [](const ActivePokemon& a) { return a.volatiles.flash_fire; }},
    {"choicelock", [](const ActivePokemon& a) { return a.choice_move != Move::None; }},
    {"foresight", [](const ActivePokemon& a) { return a.volatiles.foresight; }},
    {"torment", [](const ActivePokemon& a) { return a.volatiles.torment; }},
    {"charge", [](const ActivePokemon& a) { return a.volatiles.charged_up; }},
    {"grudge", [](const ActivePokemon& a) { return a.volatiles.grudge; }},
    {"mustrecharge", [](const ActivePokemon& a) { return a.volatiles.recharge; }},
    // Step 5 group 4: the locked moves and Rage.
    {"lockedmove", [](const ActivePokemon& a) {
       return a.volatiles.multiple_turns && get(a.locked_move).effect == Effect::Rampage;
     }},
    {"uproar", [](const ActivePokemon& a) { return a.volatiles.uproar != 0; }},
    {"bide", [](const ActivePokemon& a) { return a.volatiles.bide != 0; }},
    {"rollout", [](const ActivePokemon& a) { return a.rollout != 0 && a.locked_move == Move::Rollout; }},
    {"iceball", [](const ActivePokemon& a) { return a.rollout != 0 && a.locked_move == Move::IceBall; }},
    {"furycutter", [](const ActivePokemon& a) { return a.fury_cutter != 0; }},
    {"rage", [](const ActivePokemon& a) { return a.volatiles.rage; }},
    // The semi-invulnerable turn, by the move's own volatile. Not
    // twoturnmove: Showdown keeps it until the residuals after the second
    // turn, so a request in between would differ.
    {"fly", [](const ActivePokemon& a) { return a.volatiles.on_air && a.locked_move == Move::Fly; }},
    {"bounce", [](const ActivePokemon& a) { return a.volatiles.on_air && a.locked_move == Move::Bounce; }},
    {"dig", [](const ActivePokemon& a) { return a.volatiles.underground; }},
    {"dive", [](const ActivePokemon& a) { return a.volatiles.underwater; }},
};

// The active Pokémon's boosts and guarded volatiles against Showdown's.
std::string active_mismatch(const Battle& battle, Player p,
                            const json::Value& sa, bool over) {
  const ActivePokemon& a = battle.side(p).active;
  const Pokemon& mon = battle.side(p).get(1);
  if (sa.is_null() || mon.hp == 0) return "";
  const char* side = p == Player::P1 ? "p1" : "p2";
  std::string why;
  const Boosts& b = a.boosts;
  const std::pair<const char*, int8_t> boosts[] = {
      {"atk", b.atk}, {"def", b.def}, {"spa", b.spa}, {"spd", b.spd},
      {"spe", b.spe}, {"accuracy", b.acc}, {"evasion", b.eva}};
  for (const auto& [name, ours] : boosts) {
    const int64_t theirs = sa["boosts"][name].integer();
    if (ours != theirs) {
      why += std::string(" ") + side + " " + name + " " + std::to_string(ours) +
             ", Showdown " + std::to_string(theirs) + ";";
    }
  }
  // Showdown's stall duration, which our `stall` mirrors (protect()): a
  // Protect whose outcome changes nothing visible (the foe's move fails
  // anyway) can otherwise match either branch and drift the count. Not
  // once the battle is over: Showdown's residual loop flushes a faint that
  // ends it only at the next handler, so its count can stop part way
  // (decided with the user, 2026-10-09).
  if (!over && a.stall != sa["stall"].integer()) {
    why += std::string(" ") + side + " stall " + std::to_string(a.stall) +
           ", Showdown " + std::to_string(sa["stall"].integer()) + ";";
  }
  if (a.volatiles.substitute && a.substitute_hp != sa["subhp"].integer()) {
    why += std::string(" ") + side + " Substitute HP " +
           std::to_string(a.substitute_hp) + ", Showdown " +
           std::to_string(sa["subhp"].integer()) + ";";
  }
  for (const VolatileName& v : VOLATILES) {
    // Showdown locks a Baton Passer with a Choice Band; the decomp doesn't.
    // It leaves at once, so the lock is never seen.
    if (battle.request.phase == Phase::BatonPass &&
        std::string_view(v.name) == "choicelock") {
      continue;
    }
    // A lock whose band was traded or knocked away stays in Showdown until
    // the holder's next DisableMove, inert.
    if (std::string_view(v.name) == "choicelock" && a.choice_move == Move::None &&
        !detail::holds(battle.side(p), HoldEffect::ChoiceBand)) {
      continue;
    }
    bool theirs = false;
    for (const json::Value& n : sa["volatiles"].array) theirs |= n.string == v.name;
    if (v.has(a) != theirs) {
      why += std::string(" ") + side + " " + v.name + (theirs ? " missing;" : " extra;");
    }
  }
  return why;
}

// Each side's conditions (Spikes layers, and whether each screen is up) and
// the weather, against Showdown's.
std::string field_mismatch(const Battle& battle, const json::Value& state) {
  std::string why;
  for (int s = 0; s < 2; s++) {
    const std::string side = s == 0 ? "p1" : "p2";
    const SideConditions& c = battle.sides[s].conditions;
    const json::Value& sc = state[side + "side"];
    if (c.spikes != sc["spikes"].integer()) {
      why += " " + side + " Spikes " + std::to_string(c.spikes) + ", Showdown " +
             std::to_string(sc["spikes"].integer()) + ";";
    }
    // The future attack's turns left and stored damage (absent from old
    // traces).
    if (const json::Value& f = sc["future"]; !f.array.empty()) {
      const Side& side_ = battle.sides[s];
      const int64_t turns = c.future_sight, damage = side_.future_sight_damage;
      if (turns != f.array[0].integer() || damage != f.array[1].integer()) {
        why += " " + side + " future attack " + std::to_string(turns) + "/" +
               std::to_string(damage) + ", Showdown " +
               std::to_string(f.array[0].integer()) + "/" +
               std::to_string(f.array[1].integer()) + ";";
      }
    }
    const std::pair<const char*, bool> screens[] = {
        {"reflect", c.reflect != 0}, {"lightscreen", c.light_screen != 0},
        {"safeguard", c.safeguard != 0}, {"mist", c.mist != 0}};
    for (const auto& [name, ours] : screens) {
      if (ours != sc[name].boolean) {
        why += " " + side + " " + name + (ours ? " extra;" : " missing;");
      }
    }
  }
  static constexpr const char* WEATHERS[] = {"", "raindance", "sunnyday",
                                             "sandstorm", "hail"};
  const char* ours = WEATHERS[uint8_t(battle.field.weather)];
  if (state["weather"].string != ours) {
    why += std::string(" weather ") + ours + ", Showdown " +
           state["weather"].string + ";";
  }
  return why;
}

// Why `result` and `battle` don't match the step, or "" if they do. `next`
// is the following step, or null after the last.
std::string mismatch(const Battle& battle, Result result, Phase before,
                     const json::Value& step, const json::Value* next) {
  const json::Value& want = step["result"];
  if (!want.is_null()) {
    const Result::Type t = want.string == "p1"   ? Result::Win
                           : want.string == "p2" ? Result::Lose
                                                 : Result::Tie;
    if (result.type() != t) {
      return "result " + std::to_string(result.type()) + ", Showdown " +
             want.string + ";";
    }
  } else if (result.over()) {
    return "result " + std::to_string(result.type()) +
           ", Showdown's battle goes on;";
  } else if (next) {
    const Choice::Type k1 = request_kind((*next)["choices"]["p1"]);
    const Choice::Type k2 = request_kind((*next)["choices"]["p2"]);
    if (result.p1() != k1 || result.p2() != k2) {
      return "requests " + std::to_string(result.p1()) + "/" +
             std::to_string(result.p2()) + ", Showdown " + std::to_string(k1) +
             "/" + std::to_string(k2) + ";";
    }
  }
  std::string why;
  // Whether this step's residuals ran before its replacement request:
  // Showdown writes |upkeep after them, and our Phase says which request
  // this is. Without it, an attack's KO and a residual KO of the same
  // Pokémon look alike. Only a step that can reach the residuals counts.
  const bool reaches_residuals = before == Phase::None ||
                                 before == Phase::BatonPass ||
                                 before == Phase::FaintAfterAction;
  if (reaches_residuals && !result.over() &&
      (result.p1() == Choice::Switch || result.p2() == Choice::Switch) &&
      battle.request.phase != Phase::BatonPass) {
    bool upkeep = false;
    for (const json::Value& l : step["log"].array) upkeep |= l.string == "|upkeep";
    const bool residuals = battle.request.phase == Phase::FaintAfterResiduals;
    if (upkeep != residuals) {
      why += residuals ? " the request comes after the residuals, Showdown's before;"
                       : " the request comes before the residuals, Showdown's after;";
    }
  }
  why += active_mismatch(battle, Player::P1, step["state"]["p1active"], result.over());
  why += active_mismatch(battle, Player::P2, step["state"]["p2active"], result.over());
  why += field_mismatch(battle, step["state"]);
  for (int s = 0; s < 2; s++) {
    const char* side = s == 0 ? "p1" : "p2";
    const json::Value& mons = step["state"][side];
    for (uint8_t slot = 1; slot <= mons.size(); slot++) {
      const json::Value& m = mons[slot - 1];
      const Pokemon& p = battle.sides[s].get(slot);
      // Showdown's species is the active one, which Transform changes.
      const bool active = slot == 1 && !p.fainted() && battle.turn > 0;
      const std::string_view id =
          species_id(active ? battle.sides[s].active.species : p.species);
      bool pp = true;
      for (size_t i = 0; i < m[4].size(); i++) {
        pp = pp && p.moves[i].pp == m[4][i].integer();
      }
      if (!pp) {
        why += std::string(" ") + side + " slot " + std::to_string(slot) +
               ": PP ours";
        for (const MoveSlot& ms : p.moves) why += " " + std::to_string(ms.pp);
        why += ", Showdown";
        for (const json::Value& x : m[4].array) why += " " + std::to_string(x.integer());
        why += ";";
      }
      if ((p.item == Item::KnockedOff ? Item::None : p.item) != item_of(m[5].string)) {
        why += std::string(" ") + side + " slot " + std::to_string(slot) +
               ": item " + std::string(item_id(p.item)) + ", Showdown " +
               m[5].string + ";";
      }
      // Castform's forme is our active type (Forecast, mechanics.hpp); with
      // another ability, its type (Color Change's) says nothing of it.
      std::string sd_id = m[0].string;
      if (sd_id.starts_with("castform")) {
        if (slot == 1 && !p.fainted() &&
            battle.sides[s].active.ability == Ability::Forecast) {
          const Type t = battle.sides[s].active.types[0];
          const char* forme = t == Type::Fire    ? "castformsunny"
                              : t == Type::Water ? "castformrainy"
                              : t == Type::Ice   ? "castformsnowy"
                                                 : "castform";
          if (sd_id != forme) {
            why += std::string(" ") + side + ": forme " + forme + ", Showdown " + sd_id + ";";
          }
        }
        sd_id = "castform";
      }
      if (id != sd_id || p.hp != m[1].integer() ||
          p.stats.hp != m[2].integer() || status_id(p) != m[3].string) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      " %s slot %u: ours %.*s %u/%u %s, Showdown %s %lld/%lld %s;",
                      side, slot, int(id.size()), id.data(), p.hp, p.stats.hp,
                      status_id(p), m[0].string.c_str(),
                      (long long)m[1].integer(), (long long)m[2].integer(),
                      m[3].string.c_str());
        why += buf;
      }
    }
  }
  return why;
}

// Builds one side's team from the trace, or prints why it can't.
bool build_team(const json::Value& header, const char* side, RNG& rng,
                std::vector<helpers::Pokemon>& out) {
  const json::Value& t = header[side];
  showdown::Parsed<showdown::Team> parsed = showdown::parse_team(t["text"].string, rng);
  if (!parsed.value) {
    std::printf("game %lld: %s's team %lld doesn't parse\n",
                (long long)header["game"].integer(), side,
                (long long)t["team"].integer());
    return false;
  }
  out = parsed.value->pokemon;
  const json::Value& sets = t["sets"];
  if (out.size() != sets.size()) {
    std::printf("game %lld: %s has %zu Pokémon, Showdown %zu\n",
                (long long)header["game"].integer(), side, out.size(),
                sets.size());
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < out.size(); i++) {
    const json::Value& set = sets[i];
    helpers::Pokemon& p = out[i];
    const std::string& g = set["gender"].string;
    p.gender = g == "M" ? Gender::Male : g == "F" ? Gender::Female : Gender::None;
    const std::string_view ability = ABILITY_IDS[uint8_t(p.ability)];
    if (species_id(p.species) != set["species"].string ||
        ability != set["ability"].string) {
      std::printf("game %lld: %s's set %zu is %.*s with %.*s, Showdown %s "
                  "with %s\n",
                  (long long)header["game"].integer(), side, i + 1,
                  int(species_id(p.species).size()), species_id(p.species).data(),
                  int(ability.size()), ability.data(),
                  set["species"].string.c_str(), set["ability"].string.c_str());
      ok = false;
    }
  }
  return ok;
}

// Our stats against Showdown's, in its order (hp, atk, def, spa, spd, spe).
bool check_stats(const Battle& battle, const json::Value& header) {
  bool ok = true;
  for (int s = 0; s < 2; s++) {
    const char* side = s == 0 ? "p1" : "p2";
    const json::Value& sets = header[side]["sets"];
    for (size_t i = 0; i < sets.size(); i++) {
      const Stats& x = battle.sides[s].pokemon[i].stats;
      const uint16_t ours[6] = {x.hp, x.atk, x.def, x.spa, x.spd, x.spe};
      const json::Value& theirs = sets[i]["stats"];
      for (int k = 0; k < 6; k++) {
        if (ours[k] != theirs[size_t(k)].integer()) {
          std::printf("game %lld: %s's %s stat %d is %u, Showdown %lld\n",
                      (long long)header["game"].integer(), side,
                      sets[i]["species"].string.c_str(), k, ours[k],
                      (long long)theirs[size_t(k)].integer());
          ok = false;
        }
      }
    }
  }
  return ok;
}

void play(const json::Value& header, const std::vector<json::Value>& steps,
          const Config& config) {
  const long long game = header["game"].integer();
  stats.games++;
  RNG rng{config.seed ^ uint64_t(game) * 0x9E3779B97F4A7C15ull};
  std::vector<helpers::Pokemon> p1, p2;
  const bool built = build_team(header, "p1", rng, p1) & build_team(header, "p2", rng, p2);
  if (!built) {
    stats.setup++;
    return;
  }
  std::optional<Battle> init = helpers::init_battle(rng.next(), p1, p2);
  if (!init) {
    std::printf("game %lld: init_battle failed\n", game);
    stats.setup++;
    return;
  }
  if (!check_stats(*init, header)) {
    stats.setup++;
    return;
  }

  format::Names names;
  for (int s = 0; s < 2; s++) {
    const json::Value& sets = header[s == 0 ? "p1" : "p2"]["sets"];
    for (size_t i = 0; i < sets.size() && i < 6; i++) {
      names.nickname[size_t(s)][i] = sets[i]["name"].string;
      names.shiny[size_t(s)][i] = sets[i]["shiny"].boolean;
    }
  }
  // Custom Game is a debug format: Showdown shows exact HP to everyone.
  bool exact_hp = false;
  for (const json::Value& line : steps[0]["log"].array) {
    exact_hp |= line.string == "|tier|[Gen 3] Custom Game";
  }
  format::Formatter formatter(names, exact_hp);
  std::vector<uint8_t> buffer(protocol::MAX_LOG_SIZE);

  Battle battle = *init;
  Durations durations{};
  Result r{};
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  for (size_t k = 0; k < steps.size(); k++) {
    const json::Value& step = steps[k];
    const json::Value* next = k + 1 < steps.size() ? &steps[k + 1] : nullptr;

    // Turn 0 has no choices() to compare: both sides pass.
    if (k > 0) {
      for (const Player p : {Player::P1, Player::P2}) {
        const char* side = p == Player::P1 ? "p1" : "p2";
        auto& out = p == Player::P1 ? c1 : c2;
        const uint8_t n = choices(battle, p, r.request(p), out);
        std::vector<std::string> ours;
        for (uint8_t i = 0; i < n; i++) ours.push_back(choice_string(out[i]));
        std::sort(ours.begin(), ours.end());
        const std::vector<std::string> theirs =
            showdown_choices(step, k > 0 ? &steps[k - 1] : nullptr, side);
        if (ours != theirs) {
          std::printf("game %lld, step %zu (turn %u): %s's choices differ\n  ours:",
                      game, k, battle.turn, side);
          for (const auto& c : ours) std::printf(" [%s]", c.c_str());
          std::printf("\n  Showdown:");
          for (const auto& c : theirs) std::printf(" [%s]", c.c_str());
          std::printf("\n");
          print_active(battle, p, k > 0 ? &steps[k - 1] : nullptr);
          stats.choices++;
          return;
        }
      }
    }

    const json::Value* prev = k > 0 ? &steps[k - 1] : nullptr;
    const Choice x = to_choice(step["chosen"]["p1"].string, showdown_forced(step, prev, "p1"));
    const Choice y = to_choice(step["chosen"]["p2"].string, showdown_forced(step, prev, "p2"));
    std::string why;
    std::map<std::string, uint64_t> tally;
    bool matched = false;
    bool state_matched = false;
    std::string log_why; // the log mismatch of the state-matching try that
                         // matched the longest prefix
    size_t log_best = 0;
    std::string any_why; // --why: the longest log prefix of any try
    size_t any_best = 0;
    const std::vector<Unit> theirs = units([&] {
      std::vector<std::string> v;
      for (const json::Value& l : step["log"].array) v.push_back(l.string);
      return v;
    }());
    uint64_t t = 0;
    for (; t < config.tries && !matched; t++) {
      Battle copy = battle;
      copy.rng.seed = rng.next();
      randomize_hidden(copy, durations);
      protocol::Writer writer{buffer.data(), buffer.size(), 0};
      Observed o{protocol::fixed_log(writer), {}, {}};
      o.chance.durations = durations;
      force_metronome(step["log"], o.calc);
      const Result result = update(copy, x, y, o);
      std::string w = mismatch(copy, result, battle.request.phase, step, next);
      // The log lines this try matches before its first difference, and
      // that difference.
      const auto compare = [&](std::string& diff) {
        const std::vector<Unit> ours =
            units(formatter.format(std::span<const uint8_t>(buffer.data(), writer.pos)));
        size_t i = 0;
        while (i < ours.size() && i < theirs.size()) {
          Unit x = ours[i], y = theirs[i];
          normalize(x, y);
          if (x != y) break;
          i++;
        }
        if (i < ours.size() || i < theirs.size()) {
          diff = " log line " + std::to_string(i) + ": ours " +
                 (i < ours.size() ? show(ours[i]) : "(none)") + ", Showdown's " +
                 (i < theirs.size() ? show(theirs[i]) : "(none)") + ";";
        }
        return i;
      };
      // --why: the longest log prefix over every try, whatever its state.
      if (config.why && config.log && !w.empty()) {
        std::string diff;
        const size_t i = compare(diff);
        if (any_why.empty() || i > any_best) {
          any_why = diff + " (" + std::to_string(i) + " lines matched)";
          any_best = i;
        }
      }
      if (w.empty() && config.log) {
        state_matched = true;
        const size_t i = compare(w);
        if (!w.empty()) {
          if (log_why.empty() || i > log_best) {
            log_why = w + " (" + std::to_string(i) + " lines matched)";
            log_best = i;
          }
        }
      }
      if (config.why) tally[w]++;
      // Report the closest try: the fewest differing fields.
      if (t == 0 || std::count(w.begin(), w.end(), ';') <
                        std::count(why.begin(), why.end(), ';')) {
        why = w;
      }
      if (w.empty()) {
        matched = true;
        battle = copy;
        durations = o.chance.durations;
        r = result;
      }
    }
    stats.steps++;
    stats.tries += t;
    stats.max_tries = std::max(stats.max_tries, t);
    if (!matched && state_matched) why = log_why;
    if (!matched) {
      const bool multi = multi_hit(step["log"]);
      std::printf("game %lld, step %zu (turn %u): no match in %llu tries%s, "
                  "choices %s / %s\n  closest try:%s\n  Showdown's log:\n",
                  game, k, battle.turn, (unsigned long long)t,
                  multi ? " (multi-hit, left out)" : "",
                  step["chosen"]["p1"].string.c_str(),
                  step["chosen"]["p2"].string.c_str(), why.c_str());
      print_log(step);
      if (config.why) {
        std::vector<std::pair<uint64_t, std::string>> sorted;
        for (const auto& [w, n] : tally) sorted.emplace_back(n, w);
        std::sort(sorted.rbegin(), sorted.rend());
        if (!any_why.empty()) std::printf("  longest log prefix of any try:%s\n", any_why.c_str());
        std::printf("  mismatches by count:\n");
        for (size_t i = 0; i < sorted.size() && i < 12; i++) {
          std::printf("    %8llu %s\n", (unsigned long long)sorted[i].first,
                      sorted[i].second.c_str());
        }
      }
      if (multi) {
        stats.multi_hit++;
      } else {
        stats.unmatched++;
        if (state_matched) stats.log_only++;
      }
      return;
    }
  }
  stats.completed++;
  if (!config.quiet) {
    std::printf("game %lld: %zu steps match\n", game, steps.size());
  }
}

} // namespace

int main(int argc, char** argv) {
  Config config;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--tries" && i + 1 < argc) {
      config.tries = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--seed" && i + 1 < argc) {
      config.seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--game" && i + 1 < argc) {
      config.game = std::strtoll(argv[++i], nullptr, 10);
    } else if (arg == "--quiet") {
      config.quiet = true;
    } else if (arg == "--why") {
      config.why = true;
    } else if (arg == "--no-log") {
      config.log = false;
    } else {
      files.push_back(arg);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: check_showdown [--tries N] [--seed N] "
                         "[--game G] [--quiet] traces.jsonl...\n");
    return 2;
  }

  for (const std::string& file : files) {
    std::ifstream in(file);
    if (!in) {
      std::fprintf(stderr, "check_showdown: can't read %s\n", file.c_str());
      return 2;
    }
    json::Value header;
    std::vector<json::Value> steps;
    const auto finish = [&] {
      if (!header.is_null() &&
          (config.game < 0 || header["game"].integer() == config.game)) {
        play(header, steps, config);
      }
      steps.clear();
    };
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      json::Value v = json::parse(line);
      if (v["type"].string == "game") {
        finish();
        header = std::move(v);
      } else {
        steps.push_back(std::move(v));
      }
    }
    finish();
  }

  std::printf("check_showdown: %llu games, %llu matched to the end, %llu "
              "left out at a multi-hit step; %llu "
              "steps, %.1f tries per step (max %llu); failures: %llu setup, "
              "%llu choices, %llu unmatched (%llu by the log alone)\n",
              (unsigned long long)stats.games,
              (unsigned long long)stats.completed,
              (unsigned long long)stats.multi_hit,
              (unsigned long long)stats.steps,
              stats.steps ? double(stats.tries) / double(stats.steps) : 0.0,
              (unsigned long long)stats.max_tries,
              (unsigned long long)stats.setup,
              (unsigned long long)stats.choices,
              (unsigned long long)stats.unmatched,
              (unsigned long long)stats.log_only);
  return stats.completed + stats.multi_hit == stats.games ? 0 : 1;
}
