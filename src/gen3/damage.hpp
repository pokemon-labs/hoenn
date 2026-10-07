// The damage formula: the deterministic steps of one hit, from the crit
// stage through the damage roll.
//
// It follows Pokémon Showdown 0.11.10's gen3 arithmetic, not the decomp's
// (decided 2026-10-07; src/gen3/README.md, "Damage", has the decomp trace and
// every place the two differ). Showdown applies stat stages first, then
// chains each event's modifiers into one 4096ths fixed-point multiplier
// rounded once, where the decomp floors after every step. Paths:
// battle-actions = sim/battle-actions.ts (getDamage), gen3 =
// data/mods/gen3/scripts.ts (modifyDamage), battle = sim/battle.ts.
//
// Nothing here draws or takes options. The draws (crit, damage roll) are
// roll helpers in mechanics.hpp, which apply Calc overrides and report to
// Chance; they pass the outcome in (Hit::crit, apply_roll's index). So
// calc.hpp's min_ko_roll runs the same path without side effects.
//
// Preconditions: battle.valid(), and both sides have an active Pokémon. The
// functions assert them; update validates input before calling. A zero
// divisor is the one input-dependent failure checked in release: damage()
// returns 0 and the caller returns Result::Error.

#pragma once

#include <cassert>
#include <cstdint>

#include "../common/data.hpp"
#include "data.hpp"
#include "data/tables.hpp"

namespace pkmn::gen3 {

// The dynamic inputs of one hit. The hit backbone's per-action State holds
// one. Power and type are resolved by the caller: the table's, or a base
// power callback's (Hidden Power, Weather Ball, Revenge, Pursuit, Flail...)
// and Weather Ball's or Hidden Power's type. Effects Showdown applies as
// modifiers (Facade, Solar Beam, Charge, the Sports, Gust on Fly and so on)
// are applied here from the battle state.
struct Hit {
  Move move;
  uint16_t power; // Spit Up reaches 300
  Type type;
  bool crit;

  // A hit with the move's table power and type and no crit.
  static constexpr Hit of(Move move) {
    return {move, get(move).bp, get(move).type, false};
  }
};

// gMoveResultFlags as matchup() leaves them (include/constants/battle.h).
// The backbone branches on MISSED and DOESNT_AFFECT_FOE; SUPER and NOT_VERY
// are for the log.
namespace MoveResult {
inline constexpr uint8_t MISSED = 1 << 0;
inline constexpr uint8_t SUPER_EFFECTIVE = 1 << 1;
inline constexpr uint8_t NOT_VERY_EFFECTIVE = 1 << 2;
inline constexpr uint8_t DOESNT_AFFECT_FOE = 1 << 3;
inline constexpr uint8_t FAILED = 1 << 5;
// MOVE_RESULT_NO_EFFECT
inline constexpr uint8_t NO_EFFECT = MISSED | DOESNT_AFFECT_FOE | FAILED;
} // namespace MoveResult

namespace detail {

// Showdown's arithmetic (battle: chain, chainModify, modify). A modifier is
// in 4096ths: trunc(m * 4096), so 1.1 is 4505, not 4506.
namespace mod {
inline constexpr uint32_t ONE = 4096;
inline constexpr uint32_t HALF = 2048;
inline constexpr uint32_t ONE_AND_HALF = 6144;
inline constexpr uint32_t DOUBLE = 8192;
inline constexpr uint32_t TENTH_MORE = 4505;     // 1.1, type-boosting items
inline constexpr uint32_t TWENTIETH_MORE = 4300; // 1.05, Sea Incense
} // namespace mod

// chainModify: fold the next modifier into the event's, rounding half up.
constexpr uint32_t chain(uint32_t previous, uint32_t next) {
  return (previous * next + 2048) >> 12;
}

// modify: apply a modifier, rounding half down. Showdown truncates to u32
// (Dex.trunc); legal values never get there, and here everything saturates.
constexpr uint64_t modify(uint64_t value, uint32_t modifier) {
  return (value * modifier + 2047) / 4096;
}

constexpr uint32_t saturate(uint64_t x) {
  return x > UINT32_MAX ? UINT32_MAX : uint32_t(x);
}

// The hold effect. The Enigma Berry's comes from e-Reader data, which
// Showdown doesn't model; its table entry is None.
constexpr ItemData item_data(Item item) {
  return item == Item::None ? ItemData{HoldEffect::None, 0} : get(item);
}

// The type a type-boosting item boosts by 10% (Sea Incense 5%), or Mystery.
constexpr Type boosted_type(HoldEffect effect) {
  switch (effect) {
  case HoldEffect::BugPower: return Type::Bug;
  case HoldEffect::SteelPower: return Type::Steel;
  case HoldEffect::GroundPower: return Type::Ground;
  case HoldEffect::RockPower: return Type::Rock;
  case HoldEffect::GrassPower: return Type::Grass;
  case HoldEffect::DarkPower: return Type::Dark;
  case HoldEffect::FightingPower: return Type::Fighting;
  case HoldEffect::ElectricPower: return Type::Electric;
  case HoldEffect::WaterPower: return Type::Water;
  case HoldEffect::FlyingPower: return Type::Flying;
  case HoldEffect::PoisonPower: return Type::Poison;
  case HoldEffect::IcePower: return Type::Ice;
  case HoldEffect::GhostPower: return Type::Ghost;
  case HoldEffect::PsychicPower: return Type::Psychic;
  case HoldEffect::FirePower: return Type::Fire;
  case HoldEffect::DragonPower: return Type::Dragon;
  case HoldEffect::NormalPower: return Type::Normal;
  default: return Type::Mystery;
  }
}

// An unfainted active Pokémon has the ability (Showdown's getAllActive and
// suppressingWeather skip fainted ones; the decomp's ABILITY_ON_FIELD2
// doesn't).
constexpr bool on_field(const Battle& battle, Ability ability) {
  for (const Side& side : battle.sides) {
    if (side.active.ability == ability && !side.get(1).fainted()) return true;
  }
  return false;
}

// The weather, unless an unfainted Cloud Nine or Air Lock suppresses it
// (Field.effectiveWeather).
constexpr Weather effective_weather(const Battle& battle) {
  if (on_field(battle, Ability::CloudNine) || on_field(battle, Ability::AirLock)) {
    return Weather::None;
  }
  return battle.field.weather;
}

// Pokemon.calculateStat: floor(stat * (2 + b) / 2) raised, floor(stat * 2 /
// (2 - b)) lowered. The same ratios as the decomp's gStatStageRatios.
constexpr uint32_t apply_stage(uint32_t stat, int8_t stage) {
  assert(stage >= -6 && stage <= 6);
  const Fraction& r = STAT_STAGE_RATIOS[unsigned(stage + 6) % 13];
  return stat * r.num / r.den;
}

// The move counts as typeless: Struggle is "???" in Showdown, so it gets no
// STAB, no type matchup and no type-keyed modifier, and is physical.
constexpr bool typeless(const Hit& hit) { return hit.move == Move::Struggle; }

} // namespace detail

// The crit stage, 0-4: an index into CRITICAL_HIT_CHANCE. Showdown's critRatio
// is this plus 1, capped at 5 (battle-actions getDamage), with the same table.
// The decomp computes the same sum (Cmd_critcalc, bsc:1253). The roll itself
// is a roll helper in mechanics.hpp.
constexpr uint8_t crit_stage(const Battle& battle, Player player, Move move) {
  const Side& side = battle.side(player);
  const ActivePokemon& active = side.active;
  const HoldEffect item = detail::item_data(side.get(1).item).effect;
  const Effect effect = get(move).effect;

  const unsigned stage = 2 * active.volatiles.focus_energy +
                         (effect == Effect::HighCritical) +
                         (effect == Effect::SkyAttack) +
                         (effect == Effect::BlazeKick) +
                         (effect == Effect::PoisonTail) +
                         (item == HoldEffect::ScopeLens) +
                         2 * (item == HoldEffect::LuckyPunch &&
                              active.species == Species::Chansey) +
                         2 * (item == HoldEffect::Stick &&
                              active.species == Species::Farfetchd);
  return uint8_t(stage < 4 ? stage : 4);
}

// Whether the hit can crit at all: not into Battle Armor or Shell Armor.
constexpr bool can_crit(const Battle& battle, Player player) {
  const Ability ability = battle.side(foe(player)).active.ability;
  return ability != Ability::BattleArmor && ability != Ability::ShellArmor;
}

// The type matchup: immunity (Pokemon.runImmunity, with Levitate and
// Foresight), the summed type modifier (Pokemon.runEffectiveness) and Wonder
// Guard (its onTryHit). Returns MoveResult flags: DOESNT_AFFECT_FOE for an
// immunity (with MISSED for Levitate, as the decomp's typecalc), MISSED for
// Wonder Guard, and SUPER or NOT_VERY from the modifier's sign. If any of
// NO_EFFECT is set, the hit does nothing and the caller skips damage() and
// the roll.
//
// The absorbing abilities (Flash Fire, Volt Absorb, Water Absorb) are the
// backbone's, before this.
constexpr uint8_t matchup(const Battle& battle, Player player, const Hit& hit) {
  assert(battle.valid() && hit.move != Move::None);
  if (detail::typeless(hit)) return 0;

  const ActivePokemon& defender = battle.side(foe(player)).active;
  const Type type = hit.type;
  const uint8_t n = defender.types[0] == defender.types[1] ? 1 : 2;

  if (type == Type::Ground && defender.ability == Ability::Levitate) {
    return MoveResult::MISSED | MoveResult::DOESNT_AFFECT_FOE;
  }
  int type_mod = 0;
  for (uint8_t i = 0; i < n; i++) {
    const Type t = defender.types[i];
    const Effectiveness e = effectiveness(type, t);
    if (e == Effectiveness::Immune) {
      // Foresight (its onNegateImmunity) lets Normal and Fighting hit Ghost.
      const bool negated = defender.volatiles.foresight && t == Type::Ghost &&
                           (type == Type::Normal || type == Type::Fighting);
      if (!negated) return MoveResult::DOESNT_AFFECT_FOE;
    }
    type_mod += e == Effectiveness::Super ? 1 : e == Effectiveness::Resisted ? -1 : 0;
  }

  uint8_t flags = type_mod > 0   ? MoveResult::SUPER_EFFECTIVE
                  : type_mod < 0 ? MoveResult::NOT_VERY_EFFECTIVE
                                 : 0;
  if (defender.ability == Ability::WonderGuard && type_mod <= 0) {
    flags |= MoveResult::MISSED;
  }
  return flags;
}

// One hit's damage before the roll (battle-actions getDamage, then gen3
// modifyDamage up to the randomizer). Precondition: matchup() set none of
// NO_EFFECT.
//
// Returns 0 if a divisor is zero (a Def or SpD of 0-3 at -6, or a corrupt
// stat of 0); the caller returns Result::Error.
constexpr uint32_t damage(const Battle& battle, Player player, const Hit& hit) {
  using namespace detail;
  assert(battle.valid());
  assert(hit.move != Move::None && hit.power != 0);
  assert(!(matchup(battle, player, hit) & MoveResult::NO_EFFECT));

  const Side& side = battle.side(player);
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& attacker = side.active;
  const ActivePokemon& defender = foe_side.active;
  const Pokemon& attacker_mon = side.get(1);
  const Pokemon& defender_mon = foe_side.get(1);
  assert(attacker.species != Species::None &&
         defender.species != Species::None);

  const bool typeless = detail::typeless(hit);
  // Mystery matches no type-keyed modifier below.
  const Type type = typeless ? Type::Mystery : hit.type;
  const bool physical = typeless || type < Type::Mystery;
  const HoldEffect attacker_effect = item_data(attacker_mon.item).effect;
  const HoldEffect defender_effect = item_data(defender_mon.item).effect;
  const Weather weather = effective_weather(battle);
  const Effect effect = get(hit.move).effect;

  // BasePower, in handler priority order: Charge 9, the Sports 3, the pinch
  // abilities 2, Thick Fat 1, then Facade, Solar Beam and Bounce 0.
  uint32_t m = mod::ONE;
  if (attacker.volatiles.charged_up && type == Type::Electric) {
    m = chain(m, mod::DOUBLE);
  }
  for (const Side& s : battle.sides) {
    if (s.active.volatiles.mud_sport && type == Type::Electric) m = chain(m, mod::HALF);
    if (s.active.volatiles.water_sport && type == Type::Fire) m = chain(m, mod::HALF);
  }
  if (attacker_mon.hp <= attacker_mon.stats.hp / 3 &&
      ((type == Type::Grass && attacker.ability == Ability::Overgrow) ||
       (type == Type::Fire && attacker.ability == Ability::Blaze) ||
       (type == Type::Water && attacker.ability == Ability::Torrent) ||
       (type == Type::Bug && attacker.ability == Ability::Swarm))) {
    m = chain(m, mod::ONE_AND_HALF);
  }
  if (defender.ability == Ability::ThickFat &&
      (type == Type::Fire || type == Type::Ice)) {
    m = chain(m, mod::HALF);
  }
  if (effect == Effect::Facade && attacker_mon.status.any() &&
      !attacker_mon.status.asleep()) {
    m = chain(m, mod::DOUBLE);
  }
  if (hit.move == Move::SolarBeam &&
      (weather == Weather::Rain || weather == Weather::Sandstorm ||
       weather == Weather::Hail)) {
    m = chain(m, mod::HALF);
  }
  // Bounce's condition doubles Gust and Twister as base power; Fly's
  // doubles them as damage, below.
  if (defender.volatiles.on_air && defender.locked_move == Move::Bounce &&
      (hit.move == Move::Gust || hit.move == Move::Twister)) {
    m = chain(m, mod::DOUBLE);
  }
  uint64_t power = modify(hit.power, m);
  if (power == 0) power = 1;

  // The attacking stat: the stage (a crit ignores a lowered one), then
  // ModifyAtk or ModifySpA. Hustle's handler (priority 5) returns
  // modify(atk, 1.5) directly instead of chaining; the rest chain.
  const int8_t atk_stage = physical ? attacker.boosts.atk : attacker.boosts.spa;
  uint64_t atk = physical ? attacker.stats.atk : attacker.stats.spa;
  atk = apply_stage(uint32_t(atk), hit.crit && atk_stage < 0 ? 0 : atk_stage);
  m = mod::ONE;
  if (physical) {
    if (attacker.ability == Ability::HugePower ||
        attacker.ability == Ability::PurePower) {
      m = chain(m, mod::DOUBLE);
    }
    if (attacker.ability == Ability::Guts && attacker_mon.status.any()) {
      m = chain(m, mod::ONE_AND_HALF);
    }
    if (attacker.ability == Ability::Hustle) atk = modify(atk, mod::ONE_AND_HALF);
    if (attacker_effect == HoldEffect::ChoiceBand) m = chain(m, mod::ONE_AND_HALF);
    if (attacker_effect == HoldEffect::ThickClub &&
        (attacker.species == Species::Cubone || attacker.species == Species::Marowak)) {
      m = chain(m, mod::DOUBLE);
    }
  } else {
    if ((attacker.ability == Ability::Plus && on_field(battle, Ability::Minus)) ||
        (attacker.ability == Ability::Minus && on_field(battle, Ability::Plus))) {
      m = chain(m, mod::ONE_AND_HALF);
    }
    if (attacker_effect == HoldEffect::DeepSeaTooth &&
        attacker.species == Species::Clamperl) {
      m = chain(m, mod::DOUBLE);
    }
    if (attacker_effect == HoldEffect::LightBall &&
        attacker.species == Species::Pikachu) {
      m = chain(m, mod::DOUBLE);
    }
    if (attacker_effect == HoldEffect::SoulDew &&
        (attacker.species == Species::Latias || attacker.species == Species::Latios)) {
      m = chain(m, mod::ONE_AND_HALF);
    }
  }
  if (boosted_type(attacker_effect) == type && type != Type::Mystery) {
    m = chain(m, attacker_mon.item == Item::SeaIncense ? mod::TWENTIETH_MORE
                                                       : mod::TENTH_MORE);
  }
  atk = modify(atk, m);

  // The defending stat: the stage (a crit ignores a raised one), then
  // ModifyDef or ModifySpD.
  const int8_t def_stage = physical ? defender.boosts.def : defender.boosts.spd;
  uint64_t def = physical ? defender.stats.def : defender.stats.spd;
  def = apply_stage(uint32_t(def), hit.crit && def_stage > 0 ? 0 : def_stage);
  m = mod::ONE;
  if (physical) {
    if (defender.ability == Ability::MarvelScale && defender_mon.status.any()) {
      m = chain(m, mod::ONE_AND_HALF);
    }
    if (defender_effect == HoldEffect::MetalPowder &&
        defender.species == Species::Ditto && !defender.volatiles.transformed) {
      m = chain(m, mod::DOUBLE);
    }
  } else {
    if (defender_effect == HoldEffect::SoulDew &&
        (defender.species == Species::Latias || defender.species == Species::Latios)) {
      m = chain(m, mod::ONE_AND_HALF);
    }
    if (defender_effect == HoldEffect::DeepSeaScale &&
        defender.species == Species::Clamperl) {
      m = chain(m, mod::DOUBLE);
    }
  }
  def = modify(def, m);
  // Self-Destruct and Explosion halve Def, at least 1.
  if (effect == Effect::Explosion && physical) def = def / 2 > 0 ? def / 2 : 1;
  if (def == 0) return 0;

  uint64_t d = (2u * attacker_mon.level / 5 + 2) * power * atk / def / 50;

  // gen3 modifyDamage.
  if (physical && attacker_mon.status.burned() &&
      attacker.ability != Ability::Guts) {
    d = modify(d, mod::HALF);
  }
  // ModifyDamagePhase1: the screens unless crit, and Flash Fire, chained.
  m = mod::ONE;
  if (!hit.crit && (physical ? foe_side.conditions.reflect
                             : foe_side.conditions.light_screen) != 0) {
    m = chain(m, mod::HALF);
  }
  if (attacker.volatiles.flash_fire && type == Type::Fire) {
    m = chain(m, mod::ONE_AND_HALF);
  }
  d = modify(d, m);
  if (weather == Weather::Rain) {
    if (type == Type::Water) d = modify(d, mod::ONE_AND_HALF);
    if (type == Type::Fire) d = modify(d, mod::HALF);
  } else if (weather == Weather::Sun) {
    if (type == Type::Fire) d = modify(d, mod::ONE_AND_HALF);
    if (type == Type::Water) d = modify(d, mod::HALF);
  }
  if (physical && d == 0) d = 1;
  d += 2;
  if (hit.crit) d = modify(d, mod::DOUBLE);

  if (!typeless) {
    if (attacker.types[0] == type || attacker.types[1] == type) {
      d = modify(d, mod::ONE_AND_HALF);
    }
    // All the doublings, then all the halvings.
    const uint8_t n = defender.types[0] == defender.types[1] ? 1 : 2;
    int type_mod = 0;
    for (uint8_t i = 0; i < n; i++) {
      const Effectiveness e = effectiveness(type, defender.types[i]);
      type_mod += e == Effectiveness::Super ? 1 : e == Effectiveness::Resisted ? -1 : 0;
    }
    for (; type_mod > 0; type_mod--) d *= 2;
    for (; type_mod < 0; type_mod++) d /= 2;
  }

  // ModifyDamage: Dig, Dive, Fly and Minimize's conditions on the defender.
  const Move move = hit.move;
  const Volatiles& v = defender.volatiles;
  if ((v.underground && (move == Move::Earthquake || move == Move::Magnitude)) ||
      (v.underwater && (move == Move::Surf || move == Move::Whirlpool)) ||
      (v.on_air && defender.locked_move != Move::Bounce &&
       (move == Move::Gust || move == Move::Twister)) ||
      (v.minimized && move == Move::Stomp)) {
    d = modify(d, mod::DOUBLE);
  }
  return saturate(d);
}

// The randomizer (battle.randomizer, then gen3 modifyDamage's floor): `index`
// 0-15 is the percentage minus 85, so index = 15 - random(16). The result is
// at least 1. The decomp's ApplyRandomDmgMultiplier (bsc:1641) is the same
// with Random() % 16.
constexpr uint32_t apply_roll(uint32_t damage, uint8_t index) {
  assert(index <= 15);
  const uint32_t d = uint32_t(uint64_t(damage) * (85u + (index & 15)) / 100);
  return d == 0 ? 1 : d;
}

static_assert(detail::chain(detail::mod::ONE_AND_HALF, detail::mod::ONE_AND_HALF) == 9216);
static_assert(detail::chain(detail::mod::HALF, detail::mod::ONE_AND_HALF) == 3072);
static_assert(detail::modify(95, detail::mod::HALF) == 47);
static_assert(detail::modify(349, 9216) == 785); // Choice Band + Guts
static_assert(apply_roll(100, 0) == 85 && apply_roll(100, 15) == 100);
static_assert(apply_roll(1, 0) == 1 && apply_roll(0, 15) == 1);
static_assert(apply_roll(UINT32_MAX, 15) == UINT32_MAX);

} // namespace pkmn::gen3
