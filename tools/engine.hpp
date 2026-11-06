// The two engines behind one face, so soak, check_chance and check_showdown
// are written once and take --ovo. An engine value is one game's setup: the
// turn-0 battle, and for ovo the Sets that update needs. choices,
// randomize_hidden and mask_hidden resolve on the Battle's type.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "helpers.hpp"
#include "mechanics.hpp"
#include "ovo/mechanics.hpp"

namespace hoenn::tools {

using Team = std::span<const helpers::Pokemon>;

struct Full {
  using Battle = hoenn::Battle;
  using Durations = hoenn::Durations;
  using Chance = hoenn::Chance<>;
  static constexpr uint8_t MAX_CHOICES = hoenn::MAX_CHOICES;
  static constexpr bool OVO = false;

  Battle start;

  static std::optional<Full> init(uint64_t seed, Team p1, Team p2) {
    const std::optional<Battle> b = helpers::init_battle(seed, p1, p2);
    if (!b) return std::nullopt;
    return Full{*b};
  }

  template <typename O> Result update(Battle& b, Choice c1, Choice c2, O&& o) const {
    return hoenn::update(b, c1, c2, o);
  }
  bool valid(const Battle& b) const { return b.valid(); }
  const Battle& view(const Battle& b) const { return b; }
  static Phase phase(const Battle& b) { return b.request.phase; }
};

struct Ovo {
  using Battle = ovo::Battle;
  using Durations = ovo::Durations;
  using Chance = ovo::Chance<>;
  static constexpr uint8_t MAX_CHOICES = ovo::MAX_CHOICES;
  static constexpr bool OVO = true;

  ovo::Set set[2];
  Battle start;

  static std::optional<Ovo> init(uint64_t seed, Team p1, Team p2) {
    if (p1.size() != 1 || p2.size() != 1) return std::nullopt;
    const std::optional<hoenn::Battle> b = helpers::init_battle(seed, p1, p2);
    if (!b) return std::nullopt;
    Ovo e{};
    for (uint8_t i = 0; i < 2; i++) {
      const Pokemon& p = b->sides[i].pokemon[0];
      e.set[i] = ovo::set_of(p);
      e.start.sides[i] = ovo::side_of(p);
    }
    e.start.rng = b->rng;
    return e;
  }

  ovo::Sets sets() const { return {set[0], set[1]}; }

  template <typename O> Result update(Battle& b, Choice c1, Choice c2, O&& o) const {
    return ovo::update(b, sets(), c1, c2, o);
  }
  bool valid(const Battle& b) const { return b.valid(sets()); }
  ovo::View<const Battle> view(const Battle& b) const { return ovo::view(b, sets()); }
  static Phase phase(const Battle&) { return Phase::None; }
};

// Every Pokémon of every team as a team of one, for --ovo.
inline std::vector<std::vector<helpers::Pokemon>>
singles(const std::vector<std::vector<helpers::Pokemon>>& teams) {
  std::vector<std::vector<helpers::Pokemon>> out;
  for (const auto& team : teams) {
    for (const helpers::Pokemon& p : team) out.push_back({p});
  }
  return out;
}

} // namespace hoenn::tools
