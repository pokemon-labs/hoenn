// One Pokémon a side, nobody switches: Smogon's 1v1 once the picks are made.
// Choice, Result, Status, Stats and the rest are hoenn's; what's here is the
// battle, split in two.
//
// `Set` is what no move can write. `Battle` is everything else, and it's what
// search copies: a Set is shared by every battle that has that Pokémon. Types,
// ability, item and moves are in Battle because moves write them (Conversion,
// Skill Swap, Trick, Mimic), so a hypothesis about a foe's set also patches
// the Battle.
//
// The views have hoenn's names (side.active, side.get(1)), so damage.hpp runs
// on either engine. Transform writes nothing here: it fails on a transformed
// target, so the target's stats and species are always its own Set's, and
// active reads the foe's Set while `transformed` is on.

#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

#include "../data.hpp"

namespace hoenn::ovo {

struct Set {
  Stats stats; // `hp` is max HP
  Species species;
  Ability ability; // its own, whatever it has now: a future attack reads it
  uint8_t level;
  uint8_t happiness;
  Personality personality;
  uint8_t hidden_power;
  Type hidden_power_type; // Fighting..Dark, not Normal or Mystery

  constexpr bool valid() const {
    return species != Species::None && hoenn::detail::valid(species) && level >= 1 &&
           level <= 100 && stats.hp != 0 && personality.valid() && hidden_power >= 30 &&
           hidden_power <= 70 && hoenn::detail::valid(hidden_power_type) &&
           hidden_power_type != Type::Normal && hidden_power_type != Type::Mystery;
  }

  friend constexpr bool operator==(const Set&, const Set&) = default;
};

// The two Sets of a battle. Not stored: update takes it, so one Battle can run
// against any pair.
struct Sets {
  const Set& p1;
  const Set& p2;

  constexpr const Set& get(Player p) const { return p == Player::P1 ? p1 : p2; }
};

// hoenn::Volatiles without what only a switch can reach: move_actions and
// first_turn (`turn` gives both), last_taken_from, lock_on_target,
// wrapper_gone, mimicked. status_counter is the toxic counter alone.
struct Volatiles {
  using Kind = hoenn::Volatiles::Kind;

  uint32_t bide_damage;

  Move choice_move;
  Move encored_move;
  Move disabled_move;
  Move last_move;
  Kind kind;
  uint8_t lock;
  uint8_t taunt;
  uint8_t encore_timer; // Hidden.
  uint8_t wrapped;      // Hidden.
  uint8_t substitute_hp;
  uint8_t disable_timer; // Hidden.
  uint8_t fury_cutter;

  Move locked_move : 9;
  uint8_t perish_song : 3;
  uint8_t stall : 2;
  uint8_t protect_uses : 2;

  Move wrapped_move : 9;
  uint8_t confusion : 3; // Hidden.
  uint8_t lock_on : 2;
  uint8_t stockpile : 2;

  Move last_used_move : 9;
  Type last_used_type : 5;
  uint8_t yawn : 2;

  Move last_taken_move : 9;
  uint8_t encored_slot : 2;
  bool truant : 1;
  uint8_t status_counter : 4;

  bool flinched : 1;
  bool infatuated : 1;
  bool focus_energy : 1;
  bool transformed : 1;
  bool substitute : 1;
  bool escape_prevention : 1; // only Mean Look's failure reads it
  bool nightmare : 1;
  bool cursed : 1;
  bool foresight : 1;
  bool defense_curl : 1;
  bool torment : 1;
  bool leech_seed : 1;
  bool minimized : 1;
  bool charged_up : 1;
  bool rooted : 1;
  bool imprisoned_others : 1;
  bool mud_sport : 1;
  bool water_sport : 1;
  bool flash_fire : 1;
  bool renewed_lock : 1;
  bool renewed_fury_cutter : 1;

  static constexpr uint8_t MAX_TOXIC = hoenn::Volatiles::MAX_TOXIC;

  constexpr bool multiple_turns() const { return kind >= Kind::Charge; }
  constexpr bool charging() const { return kind >= Kind::Charge && kind <= Kind::Underwater; }
  constexpr bool semi_invulnerable() const {
    return kind >= Kind::OnAir && kind <= Kind::Underwater;
  }
  constexpr bool lock_valid() const {
    switch (kind) {
    case Kind::None:
    case Kind::Rage:
    case Kind::DestinyBond:
    case Kind::Grudge: return lock == 0;
    case Kind::Recharge:
    case Kind::Charge:
    case Kind::OnAir:
    case Kind::Underground:
    case Kind::Underwater: return lock >= 1 && lock <= 2;
    case Kind::Thrash:
    case Kind::Bide: return lock >= 1 && lock <= 3;
    case Kind::Uproar:
    case Kind::Rollout: return lock >= 1 && lock <= 5;
    }
    return false;
  }

  constexpr bool valid() const {
    const Move moves[] = {locked_move,   choice_move, wrapped_move,   disabled_move,
                          encored_move,  last_move,   last_used_move, last_taken_move};
    for (Move m : moves) {
      if (!hoenn::detail::valid(m)) return false;
    }
    if (kind >= Kind::Recharge && locked_move == Move::None) return false;
    return lock_valid() && confusion <= 5 && wrapped <= 6 && yawn <= 2 && lock_on <= 2 &&
           hoenn::detail::valid(last_used_type) && disable_timer <= 6 && encore_timer <= 6 &&
           perish_song <= 4 && fury_cutter <= 5 && taunt <= 2 && stall <= 2;
  }

  friend constexpr bool operator==(const Volatiles&, const Volatiles&) = default;
};

// hoenn::SideConditions without the party positions. Spikes never hurts
// anyone here; the layers are for the log (a fourth use fails, Rapid Spin's
// line).
struct SideConditions {
  uint8_t spikes;
  uint8_t reflect;
  uint8_t light_screen;
  uint8_t safeguard;
  uint8_t mist;
  uint8_t wish;
  uint8_t future_sight;

  constexpr bool valid() const {
    return spikes <= 3 && reflect <= 5 && light_screen <= 5 && safeguard <= 5 && mist <= 5 &&
           wish <= 2 && future_sight <= 3;
  }

  friend constexpr bool operator==(const SideConditions&, const SideConditions&) = default;
};

struct Side {
  uint32_t future_sight_damage;
  Volatiles volatiles;
  MoveSlot moves[4];
  uint16_t hp;
  Move future_sight_move;
  Type types[2];
  Ability ability;
  Item item;
  Item recycle_item;
  Status status;
  Boosts boosts;
  SideConditions conditions;

  constexpr bool valid(const Set& set) const {
    for (const MoveSlot& m : moves) {
      if (!hoenn::detail::valid(m.id)) return false;
      if (m.id == Move::None && m.pp != 0) return false;
    }
    if (volatiles.status_counter != 0 && !status.badly_poisoned()) return false;
    return hp <= set.stats.hp && hoenn::detail::valid(types[0]) &&
           hoenn::detail::valid(types[1]) && hoenn::detail::valid(ability) &&
           hoenn::detail::valid(item) && hoenn::detail::valid(recycle_item) &&
           hoenn::detail::valid(future_sight_move) && status.valid() && boosts.valid() &&
           volatiles.valid() && conditions.valid() &&
           (conditions.future_sight == 0) == (future_sight_move == Move::None);
  }

  friend constexpr bool operator==(const Side&, const Side&) = default;
};

struct Battle {
  Side sides[2];
  RNG rng;
  uint16_t turn;
  Field field;

  static constexpr uint16_t MAX_TURNS = hoenn::Battle::MAX_TURNS;

  constexpr Side& side(Player p) { return sides[uint8_t(p)]; }
  constexpr const Side& side(Player p) const { return sides[uint8_t(p)]; }

  constexpr bool valid(Sets sets) const {
    return sets.p1.valid() && sets.p2.valid() && sides[0].valid(sets.p1) &&
           sides[1].valid(sets.p2) && turn <= MAX_TURNS && field.valid();
  }

  friend constexpr bool operator==(const Battle&, const Battle&) = default;
};

// What a Pokémon starts as, from hoenn's. Turn 0 of the battle is these two
// Sides and a seed.
constexpr Set set_of(const hoenn::Pokemon& p) {
  return {p.stats,        p.species,     p.ability,      p.level,
          p.happiness,    p.personality, p.hidden_power, p.hidden_power_type};
}

constexpr Side side_of(const hoenn::Pokemon& p) {
  Side s{};
  for (uint8_t i = 0; i < 4; i++) s.moves[i] = p.moves[i];
  s.hp = p.hp;
  s.types[0] = p.types[0];
  s.types[1] = p.types[1];
  s.ability = p.ability;
  s.item = p.item;
  s.status = p.status;
  return s;
}

template <typename From, typename T>
using Like = std::conditional_t<std::is_const_v<From>, const T, T>;

// hoenn's ActivePokemon: what the Pokémon shows, the foe's Set's under
// Transform. stats.hp is unused, as there.
template <typename S> struct ActiveView {
  const Stats& stats;
  Species species;
  uint8_t hidden_power;
  Type hidden_power_type;
  Like<S, MoveSlot> (&moves)[4];
  Like<S, Type> (&types)[2];
  Like<S, Ability>& ability;
  Like<S, Boosts>& boosts;
  Like<S, Volatiles>& volatiles;
};

// hoenn's Pokemon: its own Set, whatever it shows.
template <typename S> struct MonView {
  const Stats& stats;
  Species species;
  Ability ability;
  uint8_t level;
  uint8_t happiness;
  Personality personality;
  uint8_t hidden_power;
  Type hidden_power_type;
  Like<S, uint16_t>& hp;
  Like<S, Status>& status;
  Like<S, Item>& item;

  constexpr bool fainted() const { return hp == 0; }
};

template <typename S> struct SideView {
  ActiveView<S> active;
  MonView<S> mon;
  Like<S, SideConditions>& conditions;
  Like<S, uint32_t>& future_sight_damage;
  Like<S, Move>& future_sight_move;
  Like<S, Item>& recycle_item;

  // The log's party position.
  static constexpr uint8_t order[1] = {1};

  constexpr const MonView<S>& get(uint8_t slot) const {
    assert(slot == 1);
    (void)slot;
    return mon;
  }
};

// A Battle with its Sets, for the code written against hoenn's names. side()
// reads `transformed` as it's called. B is Battle or const Battle. update's
// Frame keeps its views instead: building one per call cost 60%.
template <typename B> struct View {
  using S = Like<B, Side>;

  B& battle;
  Sets sets;
  Like<B, Field>& field;
  Like<B, RNG>& rng;
  Like<B, uint16_t>& turn;

  constexpr SideView<S> side(Player p) const {
    S& s = battle.side(p);
    const Set& own = sets.get(p);
    const Set& shown = s.volatiles.transformed ? sets.get(foe(p)) : own;
    return {{shown.stats, shown.species, shown.hidden_power, shown.hidden_power_type, s.moves,
             s.types, s.ability, s.boosts, s.volatiles},
            {own.stats, own.species, own.ability, own.level, own.happiness, own.personality,
             own.hidden_power, own.hidden_power_type, s.hp, s.status, s.item},
            s.conditions,
            s.future_sight_damage,
            s.future_sight_move,
            s.recycle_item};
  }

  constexpr bool valid() const { return battle.valid(sets); }
};

template <typename B> constexpr View<B> view(B& battle, Sets sets) {
  return {battle, sets, battle.field, battle.rng, battle.turn};
}

static_assert(sizeof(Set) == 20);
static_assert(sizeof(Volatiles) == 32);
static_assert(sizeof(SideConditions) == 7);
static_assert(sizeof(Side) == 76);
static_assert(sizeof(Battle) == 168);

static_assert(std::is_trivially_copyable_v<Set>);
static_assert(std::is_trivial_v<Battle> && std::is_standard_layout_v<Battle>);

} // namespace hoenn::ovo
