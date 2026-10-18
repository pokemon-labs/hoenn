// Calc, as libpkmn's calc.zig: overrides that force chance branches, and (to
// come) min_ko_roll.
//
// Overrides are an Actions: a nonzero field forces that branch, 0 rolls
// normally. The roll helpers in mechanics.hpp read them directly behind
// `if constexpr (enabled)` (CLAUDE.md, "Chance", Hooks). Hidden rolls are in
// neither; to fix a sleep length, set the counter in the battle. Summaries
// are on hold.
//
// min_ko_roll(battle, player, slot, crit) -> 0..15, or 16 (deferred): the
// smallest damage-roll index (0 = 85%, 15 = 100%) at which one hit from that
// move slot KOs the foe's active Pokémon, via the normal damage path without
// side effects. Ignores multi-hit; fixed-damage moves return 0 or 16.

#pragma once

#include <type_traits>

#include "chance.hpp"

namespace hoenn {

struct Calc {
  static constexpr bool enabled = true;

  Actions overrides;
};

// Search's default: every draw rolls.
struct NullCalc {
  static constexpr bool enabled = false;
};

static_assert(std::is_empty_v<NullCalc>);

} // namespace hoenn
