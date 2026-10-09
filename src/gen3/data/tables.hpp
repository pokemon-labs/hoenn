// Hand-written from the decomp, not generated. Small lookup tables that the
// decomp keeps inside battle code rather than in move, species or item data.
// Paths: bsc = src/battle_script_commands.c, util = src/battle_util.c.

#pragma once

#include <cstdint>

#include "items.hpp"
#include "moves.hpp"
#include "types.hpp"

namespace pkmn::gen3 {

// A ratio applied as x * num / den, floored.
struct Fraction {
  uint8_t num;
  uint8_t den;
};

// gStatStageRatios (src/pokemon.c), indexed by stage + 6. Used for Atk, Def,
// Spe, SpA and SpD.
inline constexpr Fraction STAT_STAGE_RATIOS[13] = {
    {10, 40}, {10, 35}, {10, 30}, {10, 25}, {10, 20}, {10, 15}, {10, 10},
    {15, 10}, {20, 10}, {25, 10}, {30, 10}, {35, 10}, {40, 10},
};

// sAccuracyStageRatios (bsc:588), indexed by stage + 6. Cmd_accuracycheck
// computes the stage as accuracy - evasion + 6, clamped to 0-12, and applies
// it to the move's accuracy.
inline constexpr Fraction ACCURACY_STAGE_RATIOS[13] = {
    {33, 100}, {36, 100}, {43, 100}, {50, 100}, {60, 100}, {75, 100}, {1, 1},
    {133, 100}, {166, 100}, {2, 1}, {233, 100}, {133, 50}, {3, 1},
};

// sCriticalHitChance (bsc:606): a crit is !(Random() % n) at each crit stage.
inline constexpr uint8_t CRITICAL_HIT_CHANCE[5] = {16, 8, 4, 3, 2};

// sProtectSuccessRates (bsc:719): Protect, Detect and Endure succeed if
// rate >= Random() (a u16), indexed by consecutive successes. They also fail
// if the user moves last in the turn.
//
// The decomp never caps protectUses, so a fifth consecutive success reads past
// the array into sMovesForbiddenToCopy. We clamp to the last entry instead,
// for safety; it takes four straight 1/8 successes to get there.
inline constexpr uint16_t PROTECT_SUCCESS_RATES[4] = {0xFFFF, 0xFFFF / 2,
                                                      0xFFFF / 4, 0xFFFF / 8};

// sFlailHpScaleToPowerTable (bsc:749), for Flail and Reversal: the first entry
// with GetScaledHPFraction(hp, maxHP, 48) <= threshold gives the power. That
// fraction is hp * 48 / maxHP, floored, but 1 if it floors to 0 with hp > 0
// (src/battle_interface.c on master).
struct FlailEntry {
  uint8_t threshold;
  uint8_t power;
};
inline constexpr FlailEntry FLAIL_POWER[6] = {
    {1, 200}, {4, 150}, {9, 100}, {16, 80}, {32, 40}, {48, 20},
};

// sWeightToDamageTable (bsc:774), for Low Kick: the first entry whose weight
// (hectograms) is greater than the target's gives the power, else 120.
struct WeightEntry {
  uint16_t weight;
  uint8_t power;
};
inline constexpr WeightEntry LOW_KICK_POWER[5] = {
    {100, 20}, {250, 40}, {500, 60}, {1000, 80}, {2000, 100},
};
inline constexpr uint8_t LOW_KICK_MAX_POWER = 120;

// Cmd_magnitudedamagecalculation (bsc:8670): r = Random() % 100, and the first
// entry with r < bound gives the magnitude and power.
struct MagnitudeEntry {
  uint8_t bound;
  uint8_t magnitude;
  uint8_t power;
};
inline constexpr MagnitudeEntry MAGNITUDE[7] = {
    {5, 4, 10},   {15, 5, 30},  {35, 6, 50},   {65, 7, 70},
    {85, 8, 90},  {95, 9, 110}, {100, 10, 150},
};

// Cmd_setmultihitcounter (bsc:7140): two draws give 2, 3, 4 or 5 hits with
// probabilities 3/8, 3/8, 1/8, 1/8. Indexed by hits - 2, in eighths.
inline constexpr uint8_t MULTI_HIT_EIGHTHS[4] = {3, 3, 1, 1};

// sHoldEffectToType (src/pokemon.c): the type a type-boosting item boosts, or
// None if it isn't one. The boost is the item's param (10%).
struct TypeBoost {
  bool boosts;
  Type type;
};
constexpr TypeBoost type_boost(HoldEffect e) {
  switch (e) {
    case HoldEffect::BugPower: return {true, Type::Bug};
    case HoldEffect::SteelPower: return {true, Type::Steel};
    case HoldEffect::GroundPower: return {true, Type::Ground};
    case HoldEffect::RockPower: return {true, Type::Rock};
    case HoldEffect::GrassPower: return {true, Type::Grass};
    case HoldEffect::DarkPower: return {true, Type::Dark};
    case HoldEffect::FightingPower: return {true, Type::Fighting};
    case HoldEffect::ElectricPower: return {true, Type::Electric};
    case HoldEffect::WaterPower: return {true, Type::Water};
    case HoldEffect::FlyingPower: return {true, Type::Flying};
    case HoldEffect::PoisonPower: return {true, Type::Poison};
    case HoldEffect::IcePower: return {true, Type::Ice};
    case HoldEffect::GhostPower: return {true, Type::Ghost};
    case HoldEffect::PsychicPower: return {true, Type::Psychic};
    case HoldEffect::FirePower: return {true, Type::Fire};
    case HoldEffect::DragonPower: return {true, Type::Dragon};
    case HoldEffect::NormalPower: return {true, Type::Normal};
    default: return {false, Type::Normal};
  }
}

// sSoundMovesTable (util:688), for Soundproof. Perish Song and Heal Bell
// aren't on it, so in Gen III Soundproof doesn't block them.
constexpr bool is_sound(Move m) {
  switch (m) {
    case Move::Growl:
    case Move::Roar:
    case Move::Sing:
    case Move::Supersonic:
    case Move::Screech:
    case Move::Snore:
    case Move::Uproar:
    case Move::MetalSound:
    case Move::GrassWhistle:
    case Move::HyperVoice:
      return true;
    default:
      return false;
  }
}

// Link battles take place in the Cable Club, an indoor map, so
// BattleSetup_GetEnvironmentId gives BATTLE_ENVIRONMENT_BUILDING (recorded
// battles force it too). The environment-dependent moves are then constants: Nature Power calls Swift (sNaturePowerMoves, bsc:759),
// Secret Power's secondary effect is paralysis (Cmd_getsecretpowereffect's
// default) and Camouflage gives Normal
// (sEnvironmentToType, bsc:826).
inline constexpr Move NATURE_POWER_MOVE = Move::Swift;
inline constexpr Type CAMOUFLAGE_TYPE = Type::Normal;

static_assert(sizeof(Fraction) == 2);
static_assert(type_boost(HoldEffect::FirePower).type == Type::Fire);
static_assert(!type_boost(HoldEffect::Leftovers).boosts);
static_assert(is_sound(Move::HyperVoice) && !is_sound(Move::PerishSong));

} // namespace pkmn::gen3
