// Building a battle from teams, for tests and tools, as libpkmn's
// gen*/helpers.zig. Not on the hot path: these may use std::optional and
// return error strings.
//
// As in libpkmn, a built battle is at turn 0 with no active Pokémon. The first
// update, with both players passing, switches in the leads and moves to
// turn 1 (libpkmn: gen1/mechanics.zig, update and start).
//
// The Showdown layer (parsing exports, defaulting gender and happiness) sits
// above this and fills in a helpers::Pokemon. Nothing here guesses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "data.hpp"

namespace pkmn::gen3::helpers {

// One Pokémon to build: everything the cartridge keeps on a party Pokémon that
// the battle reads. Personality isn't here; what it decides (nature, gender,
// ability slot) is given directly.
//
// `moves` are packed at the front, 1-4 of them. Pass the generic
// Move::HiddenPower; init_pokemon retypes it from `ivs` and sets its power.
// Every move gets 3 PP Ups (max PP), as Showdown does.
//
// `ivs` and `evs` are in Stats order: HP, Atk, Def, Spe, SpA, SpD.
//
// `ability` and `gender` have no usable default: the ability must be one of
// the species' own and the gender must fit its gender ratio. `hp` defaults to
// full; `hp` and `status` are for scenario tests.
struct Pokemon {
  Species species = Species::None;
  Move moves[4] = {};
  Item item = Item::None;
  Ability ability = Ability::None;
  uint8_t level = 100;
  Nature nature = Nature::Hardy;
  Stats ivs = {31, 31, 31, 31, 31, 31};
  Stats evs = {};
  Gender gender = Gender::None;
  uint8_t happiness = 255;
  std::optional<uint16_t> hp = std::nullopt;
  Status status = Status::none();
};

// Why `p` can't be built, or nullptr if it can.
constexpr const char* check_pokemon(const Pokemon& p) {
  if (p.species == Species::None || !detail::valid(p.species)) {
    return "invalid species";
  }
  if (p.level < 1 || p.level > 100) return "level must be 1-100";
  if (uint8_t(p.nature) >= NATURES) return "invalid nature";
  unsigned total = 0;
  for (uint16_t iv : {p.ivs.hp, p.ivs.atk, p.ivs.def, p.ivs.spe, p.ivs.spa,
                      p.ivs.spd}) {
    if (iv > 31) return "IVs must be 0-31";
  }
  for (uint16_t ev : {p.evs.hp, p.evs.atk, p.evs.def, p.evs.spe, p.evs.spa,
                      p.evs.spd}) {
    if (ev > 255) return "EVs must be 0-255";
    total += ev;
  }
  if (total > 510) return "EVs must total at most 510";

  const SpeciesData& s = get(p.species);
  if (p.ability == Ability::None ||
      (p.ability != s.abilities[0] && p.ability != s.abilities[1])) {
    return "the ability isn't one of the species'";
  }
  if (!detail::valid(p.item)) return "invalid item";

  // genderRatio: 0 always male, 254 always female, 255 genderless.
  switch (s.gender_ratio) {
    case 0:
      if (p.gender != Gender::Male) return "the species is always male";
      break;
    case 254:
      if (p.gender != Gender::Female) return "the species is always female";
      break;
    case 255:
      if (p.gender != Gender::None) return "the species is genderless";
      break;
    default:
      if (p.gender != Gender::Male && p.gender != Gender::Female) {
        return "the species needs a gender";
      }
  }

  if (p.moves[0] == Move::None) return "a Pokémon needs a move";
  for (size_t i = 0; i < 4; i++) {
    const Move m = p.moves[i];
    if (m == Move::None) {
      for (size_t j = i + 1; j < 4; j++) {
        if (p.moves[j] != Move::None) return "moves must be packed at the front";
      }
      break;
    }
    if (!detail::valid(m)) return "invalid move";
    if (is_hidden_power(m)) {
      return "pass the generic HiddenPower; its type comes from the IVs";
    }
    if (m == Move::Struggle) return "Struggle can't be in a moveset";
    for (size_t j = 0; j < i; j++) {
      if (p.moves[j] == m) return "duplicate move";
    }
  }

  if (!p.status.valid()) return "invalid status";
  if (p.hp) {
    if (*p.hp > calc_hp(p.species, p.ivs.hp, p.evs.hp, p.level)) {
      return "HP above max HP";
    }
    if (*p.hp == 0 && p.status.any()) return "a fainted Pokémon has no status";
  }
  return nullptr;
}

// libpkmn: Pokemon.init.
constexpr std::optional<gen3::Pokemon> init_pokemon(const Pokemon& p) {
  if (check_pokemon(p)) return std::nullopt;
  const SpeciesData& s = get(p.species);
  gen3::Pokemon out{};
  out.stats = calc_stats(p.species, p.nature, p.ivs, p.evs, p.level);
  const HiddenPower hp = hidden_power(p.ivs);
  for (size_t i = 0; i < 4 && p.moves[i] != Move::None; i++) {
    Move m = p.moves[i];
    if (m == Move::HiddenPower) m = hp.move;
    out.moves[i] = {m, max_pp(m, 3), 3};
  }
  out.hp = p.hp.value_or(out.stats.hp);
  out.species = p.species;
  out.types[0] = s.types[0];
  out.types[1] = s.types[1];
  out.level = p.level;
  out.status = p.status;
  out.item = p.item;
  out.ability = p.ability;
  out.happiness = p.happiness;
  out.gender = p.gender;
  out.hidden_power = hp.power;
  out.hidden_power_type = get(hp.move).type;
  assert(out.valid());
  return out;
}

// Why `team` can't be built, or nullptr if it can. The lead (SLOT 1) must not
// be fainted: libpkmn's start asserts this in Showdown mode, which is ours.
constexpr const char* check_side(std::span<const Pokemon> team) {
  if (team.empty() || team.size() > 6) return "a side has 1-6 Pokémon";
  for (const Pokemon& p : team) {
    if (const char* why = check_pokemon(p)) return why;
  }
  if (team[0].hp && *team[0].hp == 0) return "the lead is fainted";
  return nullptr;
}

// libpkmn: Side.init. `order` is the identity, and every unused party position
// is an empty Pokemon (Species::None), so `order` stays a permutation of 1-6.
// The active Pokémon and everything else stay zero.
constexpr std::optional<gen3::Side> init_side(std::span<const Pokemon> team) {
  if (check_side(team)) return std::nullopt;
  gen3::Side side{};
  for (uint8_t i = 0; i < 6; i++) side.order[i] = uint8_t(i + 1);
  for (size_t i = 0; i < team.size(); i++) side.pokemon[i] = *init_pokemon(team[i]);
  return side;
}

// libpkmn: Battle.init. The battle's RNG starts at `seed` (any value works).
constexpr std::optional<gen3::Battle> init_battle(
    uint64_t seed, std::span<const Pokemon> p1, std::span<const Pokemon> p2) {
  const std::optional<gen3::Side> s1 = init_side(p1);
  const std::optional<gen3::Side> s2 = init_side(p2);
  if (!s1 || !s2) return std::nullopt;
  gen3::Battle b{};
  b.sides[0] = *s1;
  b.sides[1] = *s2;
  b.rng = RNG{seed};
  assert(b.valid());
  return b;
}

// A real set (teams/adv/sample-teams.txt), checked by hand.
namespace detail {
inline constexpr Pokemon SKARMORY = {
    .species = Species::Skarmory,
    .moves = {Move::Spikes, Move::Protect, Move::Roar, Move::Toxic},
    .item = Item::Leftovers,
    .ability = Ability::KeenEye,
    .nature = Nature::Careful,
    .ivs = {31, 0, 31, 31, 31, 31},
    .evs = {252, 0, 8, 0, 0, 248},
    .gender = Gender::Female,
};
// Hidden Power Bug: Atk, Spe and SpD IVs of 30.
inline constexpr Pokemon TYRANITAR = {
    .species = Species::Tyranitar,
    .moves = {Move::FocusPunch, Move::RockSlide, Move::HiddenPower},
    .item = Item::Leftovers,
    .ability = Ability::SandStream,
    .nature = Nature::Adamant,
    .ivs = {31, 30, 31, 30, 31, 30},
    .evs = {248, 196, 12, 0, 0, 52},
    .gender = Gender::Female,
};
} // namespace detail

static_assert([] {
  const gen3::Pokemon p = *init_pokemon(detail::SKARMORY);
  // HP: (2 * 65 + 31 + 63) + 110; SpD: (2 * 70 + 31 + 62) + 5, then x1.1.
  return p.stats.hp == 334 && p.hp == 334 && p.stats.atk == 165 &&
         p.stats.spd == 261 && p.moves[0].pp == 32 && p.moves[0].pp_ups == 3 &&
         p.moves[3].id == Move::Toxic && p.moves[3].pp == 16 &&
         p.types[0] == Type::Steel && p.types[1] == Type::Flying &&
         p.hidden_power >= 30 && p.happiness == 255;
}());
static_assert([] {
  const gen3::Pokemon p = *init_pokemon(detail::TYRANITAR);
  return p.moves[2].id == Move::HiddenPowerBug && p.moves[2].pp == 24 &&
         p.hidden_power == 70 && p.moves[3].id == Move::None;
}());
static_assert([] {
  const Pokemon team[] = {detail::TYRANITAR, detail::SKARMORY};
  const std::optional<gen3::Battle> b = init_battle(7, team, team);
  return b && b->turn == 0 && b->rng.seed == 7 &&
         b->sides[1].get(2).species == Species::Skarmory &&
         b->sides[0].pokemon[2].species == Species::None &&
         b->sides[0].active.species == Species::None;
}());
static_assert([] {
  Pokemon p = detail::SKARMORY;
  p.ability = Ability::Levitate;
  if (init_pokemon(p)) return false;
  p = detail::SKARMORY;
  p.gender = Gender::None;
  if (init_pokemon(p)) return false;
  p = detail::SKARMORY;
  p.moves[1] = Move::None;
  if (init_pokemon(p)) return false;
  p = detail::SKARMORY;
  p.moves[1] = Move::Spikes;
  if (init_pokemon(p)) return false;
  p = detail::SKARMORY;
  p.moves[1] = Move::HiddenPowerFire;
  if (init_pokemon(p)) return false;
  p = detail::SKARMORY;
  p.evs.atk = 12;  // 512 total
  if (init_pokemon(p)) return false;
  p = detail::SKARMORY;
  p.hp = 0;
  const Pokemon fainted_lead[] = {p, detail::SKARMORY};
  const Pokemon fainted_bench[] = {detail::SKARMORY, p};
  return !init_side(fainted_lead) && init_side(fainted_bench);
}());

} // namespace pkmn::gen3::helpers
