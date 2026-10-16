// Showdown export teams -> helpers::Pokemon. Not for the engine: this allocates
// and handles strings.
//
// Follows Showdown's own import (Teams.parseExportedTeamLine in sim/teams.ts)
// and its defaults (sim/pokemon.ts), except where noted:
//   - EVs default to 0 and IVs to 31.
//   - "Hidden Power [T]" with no IVs line takes T's HPivs spread
//     (data/typechart.ts). Unlike Showdown, a [T] that the IVs don't produce
//     is an error, not ignored.
//   - Happiness: a "Happiness:" line wins. Otherwise 0 if the set has
//     Frustration and not Return, else 255. Showdown gives 0 whenever the set
//     has Frustration, even with Return.
//   - Gender: the "(M)"/"(F)" given, else the species' only gender, else male
//     for Cute Charm, else 50/50 (Showdown: battle.sample(['M', 'F'])) from
//     the caller's RNG.
//   - Nature: none given is Hardy (any neutral nature is equivalent).
//   - Ability: none given is allowed only for a species with one ability.
// Lines Showdown knows but that change nothing in Gen III (Shiny, Pokeball
// and so on) are skipped. Any other line is an error.

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rng.hpp"
#include "data/names.hpp"
#include "helpers.hpp"

namespace hoenn::showdown {

// Showdown's toID: lowercase ASCII letters and digits, everything else dropped
// (so "Farfetch’d" -> "farfetchd").
inline std::string to_id(std::string_view s) {
  std::string id;
  for (const char c : s) {
    if (c >= 'A' && c <= 'Z') {
      id += char(c - 'A' + 'a');
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      id += c;
    }
  }
  return id;
}

namespace detail {

using Alias = std::pair<std::string_view, std::string_view>;

// Showdown ID -> the decomp's ID, where the two name a thing differently.
// Checked against Showdown's data/*.ts: these four moves are the only species,
// move, ability or held item names that differ.
inline constexpr Alias MOVE_ALIASES[] = {
    {"highjumpkick", "hijumpkick"},
    {"feintattack", "faintattack"},
    {"smellingsalts", "smellingsalt"},
    {"visegrip", "vicegrip"},
};

// NATURE_* order (natures.hpp).
inline constexpr std::string_view NATURE_IDS[NATURES] = {
    "hardy", "lonely", "brave",   "adamant", "naughty", "bold",    "docile",
    "relaxed", "impish", "lax",   "timid",   "hasty",   "serious", "jolly",
    "naive", "modest", "mild",    "quiet",   "bashful", "rash",    "calm",
    "gentle", "sassy", "careful", "quirky",
};

// Showdown's HPivs (data/typechart.ts), in Stats order (HP, Atk, Def, Spe,
// SpA, SpD).
struct HPivs {
  Type type;
  Stats ivs;
};
inline constexpr HPivs HP_IVS[] = {
    {Type::Bug, {31, 30, 30, 31, 31, 30}},
    {Type::Dark, {31, 31, 31, 31, 31, 31}},
    {Type::Dragon, {31, 30, 31, 31, 31, 31}},
    {Type::Electric, {31, 31, 31, 31, 30, 31}},
    {Type::Fighting, {31, 31, 30, 30, 30, 30}},
    {Type::Fire, {31, 30, 31, 30, 30, 31}},
    {Type::Flying, {30, 30, 30, 31, 30, 30}},
    {Type::Ghost, {31, 31, 30, 31, 31, 30}},
    {Type::Grass, {31, 30, 31, 31, 30, 31}},
    {Type::Ground, {31, 31, 31, 31, 30, 30}},
    {Type::Ice, {31, 30, 30, 31, 31, 31}},
    {Type::Poison, {31, 31, 30, 31, 30, 30}},
    {Type::Psychic, {31, 30, 31, 30, 31, 31}},
    {Type::Rock, {31, 31, 30, 30, 31, 30}},
    {Type::Steel, {31, 31, 31, 31, 31, 30}},
    {Type::Water, {31, 30, 30, 31, 30, 31}},
};
// Each spread gives its own type under the decomp's formula, at power 70.
static_assert([] {
  for (const HPivs& h : HP_IVS) {
    const HiddenPower hp = hidden_power(h.ivs);
    if (hp.move != hoenn::hidden_power(h.type) || hp.power != 70) return false;
  }
  return true;
}());

template <typename T, size_t N>
std::optional<T> find(const std::string_view (&ids)[N], std::string_view id,
                      std::span<const Alias> aliases = {}) {
  for (const auto& [from, to] : aliases) {
    if (id == from) id = to;
  }
  for (size_t i = 1; i < N; i++) {
    if (ids[i] == id) return T(i);
  }
  return std::nullopt;
}

inline std::string_view trim(std::string_view s) {
  const size_t b = s.find_first_not_of(" \t\r");
  if (b == std::string_view::npos) return {};
  const size_t e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}

inline bool starts_with(std::string_view s, std::string_view p) {
  return s.substr(0, p.size()) == p;
}

inline std::optional<unsigned> number(std::string_view s) {
  s = trim(s);
  unsigned n = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
  return n;
}

// "252 HP / 8 Def / 248 SpD" into `out`. Showdown stat names.
inline bool spread(std::string_view s, Stats& out) {
  while (!s.empty()) {
    const size_t slash = s.find('/');
    const std::string_view part = trim(s.substr(0, slash));
    s = slash == std::string_view::npos ? std::string_view{} : s.substr(slash + 1);
    const size_t space = part.find(' ');
    if (space == std::string_view::npos) return false;
    const std::optional<unsigned> n = number(part.substr(0, space));
    if (!n || *n > 65535) return false;
    const std::string stat = to_id(part.substr(space + 1));
    uint16_t* field = stat == "hp"    ? &out.hp
                      : stat == "atk" ? &out.atk
                      : stat == "def" ? &out.def
                      : stat == "spa" ? &out.spa
                      : stat == "spd" ? &out.spd
                      : stat == "spe" ? &out.spe
                                      : nullptr;
    if (!field) return false;
    *field = uint16_t(*n);
  }
  return true;
}

} // namespace detail

// A parsed set or team, or why it couldn't be parsed.
template <typename T>
struct Parsed {
  std::optional<T> value;
  std::string error;
};

// One set: the lines of one Pokémon, without blank lines. `rng` draws the
// gender when nothing decides it.
inline Parsed<helpers::Pokemon> parse_set(std::string_view text, RNG& rng) {
  using namespace detail;
  const auto fail = [](std::string why) {
    return Parsed<helpers::Pokemon>{std::nullopt, std::move(why)};
  };
  helpers::Pokemon p{};
  bool has_ivs = false, has_happiness = false, has_ability = false;
  std::optional<Type> hp_type;
  size_t moves = 0;
  bool first = true;

  while (!text.empty()) {
    const size_t nl = text.find('\n');
    const std::string_view line = trim(text.substr(0, nl));
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (line.empty()) continue;

    if (first) {
      first = false;
      std::string_view name = line;
      if (const size_t at = name.find(" @ "); at != std::string_view::npos) {
        const std::string item = to_id(name.substr(at + 3));
        name = name.substr(0, at);
        if (!item.empty() && item != "noitem") {
          std::optional<Item> found;
          for (const names::ItemName& n : names::ITEM_IDS) {
            if (n.id == item) found = n.item;
          }
          if (!found) return fail("unknown item: " + item);
          p.item = *found;
        }
      }
      if (name.ends_with(" (M)")) {
        p.gender = Gender::Male;
        name.remove_suffix(4);
      } else if (name.ends_with(" (F)")) {
        p.gender = Gender::Female;
        name.remove_suffix(4);
      }
      // "Nickname (Species)"
      if (name.ends_with(")")) {
        if (const size_t open = name.rfind('('); open != std::string_view::npos) {
          name = name.substr(open + 1, name.size() - open - 2);
        }
      }
      const std::string id = to_id(name);
      const auto species = find<Species>(names::SPECIES_IDS, id);
      if (!species) return fail("unknown species: " + id);
      p.species = *species;
    } else if (starts_with(line, "Ability: ") || starts_with(line, "Trait: ")) {
      const std::string id = to_id(line.substr(line.find(':') + 1));
      const auto ability = find<Ability>(names::ABILITY_IDS, id);
      if (!ability) return fail("unknown ability: " + id);
      p.ability = *ability;
      has_ability = true;
    } else if (starts_with(line, "Level: ")) {
      const auto n = number(line.substr(7));
      if (!n || *n < 1 || *n > 100) return fail("bad level: " + std::string(line));
      p.level = uint8_t(*n);
    } else if (starts_with(line, "Happiness: ")) {
      const auto n = number(line.substr(11));
      if (!n || *n > 255) return fail("bad happiness: " + std::string(line));
      p.happiness = uint8_t(*n);
      has_happiness = true;
    } else if (starts_with(line, "EVs: ")) {
      p.evs = {};
      if (!spread(line.substr(5), p.evs)) return fail("bad EVs: " + std::string(line));
    } else if (starts_with(line, "IVs: ")) {
      p.ivs = {31, 31, 31, 31, 31, 31};
      if (!spread(line.substr(5), p.ivs)) return fail("bad IVs: " + std::string(line));
      has_ivs = true;
    } else if (line.ends_with(" Nature")) {
      const std::string id = to_id(line.substr(0, line.size() - 7));
      bool found = false;
      for (uint8_t i = 0; i < NATURES; i++) {
        if (NATURE_IDS[i] == id) {
          p.nature = Nature(i);
          found = true;
        }
      }
      if (!found) return fail("unknown nature: " + id);
    } else if (line[0] == '-' || line[0] == '~') {
      if (moves == 4) return fail("more than four moves");
      std::string id = to_id(line.substr(1));
      Move move = Move::None;
      if (starts_with(id, "hiddenpower") && id != "hiddenpower") {
        // "Hidden Power [Bug]" or "Hidden Power Bug"
        const auto type = find<Type>(names::TYPE_IDS, id.substr(11));
        if (!type && id.substr(11) != "normal") return fail("unknown Hidden Power type: " + id);
        if (!type || *type == Type::Mystery) return fail("no Hidden Power of type " + id.substr(11));
        hp_type = *type;
        move = Move::HiddenPower;
      } else {
        const auto found = find<Move>(names::MOVE_IDS, id, MOVE_ALIASES);
        if (!found) return fail("unknown move: " + id);
        move = *found;
      }
      p.moves[moves++] = move;
    } else if (starts_with(line, "Shiny: ") || starts_with(line, "Pokeball: ")) {
      // No effect in Gen III battles.
    } else {
      return fail("unknown line: " + std::string(line));
    }
  }
  if (first) return fail("empty set");

  const SpeciesData& s = get(p.species);
  if (!has_ability) {
    if (s.abilities[1] != Ability::None) return fail("no ability given");
    p.ability = s.abilities[0];
  }
  if (hp_type) {
    if (!has_ivs) {
      for (const HPivs& h : HP_IVS) {
        if (h.type == *hp_type) p.ivs = h.ivs;
      }
    }
    if (p.ivs.hp <= 31 && p.ivs.atk <= 31 && p.ivs.def <= 31 &&
        p.ivs.spe <= 31 && p.ivs.spa <= 31 && p.ivs.spd <= 31 &&
        hidden_power(p.ivs).move != hoenn::hidden_power(*hp_type)) {
      return fail("Hidden Power [" + std::string(names::TYPE_IDS[uint8_t(*hp_type)]) +
                  "] doesn't match the IVs");
    }
  }
  if (!has_happiness) {
    bool frustration = false, ret = false;
    for (const Move m : p.moves) {
      frustration |= m == Move::Frustration;
      ret |= m == Move::Return;
    }
    p.happiness = frustration && !ret ? 0 : 255;
  }
  if (p.gender == Gender::None) {
    switch (s.gender_ratio) {
      case 0: p.gender = Gender::Male; break;
      case 254: p.gender = Gender::Female; break;
      case 255: break;
      default:
        p.gender = p.ability == Ability::CuteCharm ? Gender::Male
                   : rng.range(2) == 0             ? Gender::Male
                                                   : Gender::Female;
    }
  }
  if (const char* why = helpers::check_pokemon(p)) return fail(why);
  return {p, {}};
}

struct Team {
  std::string name; // from the "=== [format] name ===" header, if any
  std::vector<helpers::Pokemon> pokemon;
};

// One team: sets separated by blank lines, 1-6 of them. No legality checks,
// not even Species Clause (TESTING.md, "Truly random").
inline Parsed<Team> parse_team(std::string_view text, RNG& rng) {
  using namespace detail;
  Team team;
  std::string error;
  size_t start = 0;
  // Split on blank lines.
  std::string current;
  const auto flush = [&]() -> bool {
    if (trim(current).empty()) return true;
    Parsed<helpers::Pokemon> set = parse_set(current, rng);
    current.clear();
    if (!set.value) {
      error = "set " + std::to_string(team.pokemon.size() + 1) + ": " + set.error;
      return false;
    }
    team.pokemon.push_back(*set.value);
    return true;
  };
  while (start <= text.size()) {
    const size_t nl = std::min(text.find('\n', start), text.size());
    const std::string_view line = text.substr(start, nl - start);
    start = nl + 1;
    if (trim(line).empty()) {
      if (!flush()) return {std::nullopt, error};
    } else {
      current += line;
      current += '\n';
    }
  }
  if (!flush()) return {std::nullopt, error};
  if (const char* why = helpers::check_side(team.pokemon)) return {std::nullopt, why};
  return {std::move(team), {}};
}

// Every team in a file of "=== [format] name ===" headers, each followed by
// one team, in order. Text before the first header is ignored.
inline std::vector<Parsed<Team>> parse_teams(std::string_view text, RNG& rng) {
  std::vector<Parsed<Team>> out;
  std::string name;
  std::string body;
  bool in_team = false;
  const auto flush = [&] {
    if (!in_team) return;
    Parsed<Team> t = parse_team(body, rng);
    if (t.value) t.value->name = name;
    if (!t.value) t.error = name + ": " + t.error;
    out.push_back(std::move(t));
    body.clear();
  };
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = std::min(text.find('\n', start), text.size());
    const std::string_view line = text.substr(start, nl - start);
    start = nl + 1;
    const std::string_view t = detail::trim(line);
    if (t.starts_with("===") && t.ends_with("===") && t.size() >= 6) {
      flush();
      name = std::string(detail::trim(t.substr(3, t.size() - 6)));
      in_team = true;
    } else if (in_team) {
      body += line;
      body += '\n';
    }
  }
  flush();
  return out;
}

} // namespace hoenn::showdown
