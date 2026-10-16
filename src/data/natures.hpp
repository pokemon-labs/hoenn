// Hand-written from the decomp, not generated.

#pragma once

#include <cstdint>

namespace hoenn {

// Cartridge order (NATURE_* in include/constants/pokemon.h), which is also
// personality % 25.
enum class Nature : uint8_t {
  Hardy,
  Lonely,
  Brave,
  Adamant,
  Naughty,
  Bold,
  Docile,
  Relaxed,
  Impish,
  Lax,
  Timid,
  Hasty,
  Serious,
  Jolly,
  Naive,
  Modest,
  Mild,
  Quiet,
  Bashful,
  Rash,
  Calm,
  Gentle,
  Sassy,
  Careful,
  Quirky,
};

inline constexpr uint8_t NATURES = 25;

// FLAVOR_* (include/constants/berry.h). Only the confusion berries use it.
enum class Flavor : uint8_t { Spicy, Dry, Sweet, Bitter, Sour };

namespace detail {
// gNatureStatTable (src/pokemon.c), in its column order: Atk, Def, Spe, SpA,
// SpD. That matches BaseStats without HP.
inline constexpr int8_t NATURE_STATS[NATURES][5] = {
    { 0,  0,  0,  0,  0}, // Hardy
    {+1, -1,  0,  0,  0}, // Lonely
    {+1,  0, -1,  0,  0}, // Brave
    {+1,  0,  0, -1,  0}, // Adamant
    {+1,  0,  0,  0, -1}, // Naughty
    {-1, +1,  0,  0,  0}, // Bold
    { 0,  0,  0,  0,  0}, // Docile
    { 0, +1, -1,  0,  0}, // Relaxed
    { 0, +1,  0, -1,  0}, // Impish
    { 0, +1,  0,  0, -1}, // Lax
    {-1,  0, +1,  0,  0}, // Timid
    { 0, -1, +1,  0,  0}, // Hasty
    { 0,  0,  0,  0,  0}, // Serious
    { 0,  0, +1, -1,  0}, // Jolly
    { 0,  0, +1,  0, -1}, // Naive
    {-1,  0,  0, +1,  0}, // Modest
    { 0, -1,  0, +1,  0}, // Mild
    { 0,  0, -1, +1,  0}, // Quiet
    { 0,  0,  0,  0,  0}, // Bashful
    { 0,  0,  0, +1, -1}, // Rash
    {-1,  0,  0,  0, +1}, // Calm
    { 0, -1,  0,  0, +1}, // Gentle
    { 0,  0, -1,  0, +1}, // Sassy
    { 0,  0,  0, -1, +1}, // Careful
    { 0,  0,  0,  0,  0}, // Quirky
};

// The NATURE_STATS column each flavor goes with: Spicy-Atk, Dry-SpA,
// Sweet-Spe, Bitter-SpD, Sour-Def.
inline constexpr uint8_t FLAVOR_STAT[5] = {0, 3, 2, 4, 1};
} // namespace detail

// +1, -1 or 0 for stat `i` in Atk, Def, Spe, SpA, SpD order.
constexpr int8_t nature_modifier(Nature n, uint8_t i) {
  return detail::NATURE_STATS[uint8_t(n)][i];
}

// ModifyStatByNature (src/pokemon.c): x110/100 or x90/100, floored. The
// decomp computes this in a u16, which can't overflow for real species.
constexpr uint16_t apply_nature(Nature n, uint8_t i, uint16_t stat) {
  switch (nature_modifier(n, i)) {
    case 1: return uint16_t(uint32_t(stat) * 110 / 100);
    case -1: return uint16_t(uint32_t(stat) * 90 / 100);
    default: return stat;
  }
}

// gPokeblockFlavorCompatibilityTable (src/pokeblock.c on master) is -1 exactly
// where the flavor's stat is lowered. A confusion berry (TRY_EAT_CONFUSE_BERRY
// in src/battle_util.c) confuses the holder when it dislikes the flavor.
constexpr bool dislikes(Nature n, Flavor f) {
  return nature_modifier(n, detail::FLAVOR_STAT[uint8_t(f)]) < 0;
}

static_assert(dislikes(Nature::Lonely, Flavor::Sour));
static_assert(dislikes(Nature::Brave, Flavor::Sweet));
static_assert(dislikes(Nature::Adamant, Flavor::Dry));
static_assert(dislikes(Nature::Naughty, Flavor::Bitter));
static_assert(dislikes(Nature::Bold, Flavor::Spicy));
static_assert(!dislikes(Nature::Adamant, Flavor::Spicy));
static_assert(!dislikes(Nature::Hardy, Flavor::Spicy));
static_assert(apply_nature(Nature::Adamant, 0, 100) == 110);
static_assert(apply_nature(Nature::Adamant, 3, 101) == 90);

} // namespace hoenn
