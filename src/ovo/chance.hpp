// Durations for one Pokémon a side. Actions, Calc and the catalogue are
// hoenn's: the fields that need a bench (switch_target, assist, hits[5]) just
// stay 0.

#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

#include "../chance.hpp"
#include "data.hpp"

namespace hoenn::ovo {

// hoenn::Durations with one sleep count, and no switched().
struct Durations {
  struct Side {
    uint8_t sleep;     // 1-5 (roll 2-5)
    uint8_t confusion; // 1-5 (roll 2-5)
    uint8_t disable;   // 1-6 (roll 3-6, from 1 or 2)
    uint8_t encore;    // 1-6 (roll 3-6)
    uint8_t binding;   // 1-6 (roll 3-6), on the victim's side
    uint8_t attacking; // Uproar 1-5 (roll 2-5) or Thrash 1-2 (roll 2-3)

    friend constexpr bool operator==(const Side&, const Side&) = default;
  };

  Side p1;
  Side p2;

  constexpr Side& get(Player p) { return p == Player::P1 ? p1 : p2; }
  constexpr const Side& get(Player p) const { return p == Player::P1 ? p1 : p2; }

  friend constexpr bool operator==(const Durations&, const Durations&) = default;
};

static_assert(sizeof(Durations) == 12);
static_assert(std::is_trivial_v<Durations> && std::is_standard_layout_v<Durations>);

template <typename R = Rational<double>> using Chance = hoenn::Chance<R, Durations>;

// As hoenn::randomize_hidden. No Set is needed: every counter is in the Battle.
constexpr void randomize_hidden(Battle& battle, const Durations& durations) {
  using hoenn::detail::resample;
  for (const Player p : {Player::P1, Player::P2}) {
    Side& side = battle.side(p);
    const Durations::Side& d = durations.get(p);
    if (d.sleep != 0 && side.status.asleep() && !side.status.self_inflicted()) {
      side.status = Status::slp(resample(battle.rng, d.sleep, Status::MIN_SLEEP, Status::MAX_SLEEP));
    }
    if (battle.turn == 0) continue;
    Volatiles& v = side.volatiles;
    if (d.confusion != 0 && v.confusion != 0) v.confusion = resample(battle.rng, d.confusion, 2, 5);
    if (d.disable != 0 && v.disable_timer != 0) {
      v.disable_timer = resample(battle.rng, d.disable, 3, 6);
    }
    if (d.encore != 0 && v.encore_timer != 0) {
      v.encore_timer = resample(battle.rng, d.encore, 3, 6);
    }
    if (d.binding != 0 && v.wrapped != 0) v.wrapped = resample(battle.rng, d.binding, 3, 6);
    if (d.attacking != 0) {
      if (v.kind == Volatiles::Kind::Uproar) {
        v.lock = resample(battle.rng, d.attacking, 2, 5);
      } else if (v.kind == Volatiles::Kind::Thrash) {
        v.lock = resample(battle.rng, d.attacking, 2, 3);
      }
    }
  }
}

// As hoenn::mask_hidden. Keep this list the same as randomize_hidden's.
constexpr void mask_hidden(Battle& battle) {
  battle.rng = {};
  for (Side& side : battle.sides) {
    if (side.status.asleep() && !side.status.self_inflicted()) side.status = Status::slp(1);
    Volatiles& v = side.volatiles;
    const auto mask = [](uint8_t c) { return uint8_t(c != 0); };
    v.confusion = mask(v.confusion);
    v.wrapped = mask(v.wrapped);
    if (v.kind == Volatiles::Kind::Uproar || v.kind == Volatiles::Kind::Thrash) {
      v.lock = mask(v.lock);
    }
    v.disable_timer = mask(v.disable_timer);
    v.encore_timer = mask(v.encore_timer);
  }
}

} // namespace hoenn::ovo
