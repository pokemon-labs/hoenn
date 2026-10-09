// The gen3 protocol messages, as the Log methods of libpkmn's
// src/lib/common/protocol.zig: each writes an ArgType, then its fields.
// gen3/format.hpp turns them into Showdown's text; src/gen3/README.md,
// "Protocol", has the decisions and the inventory of Showdown's forms.
//
// Every function returns at once for the null log, so search pays nothing.
// The encodings, after the ArgType byte (IDs are common/data.hpp's ID; an ID
// of 0 means none; u16s are native order; HP is hp, max HP, Status bits):
//
//   Move         source, move u16, target, MoveFrom [, move u16 if From]
//   LastStill, LastMiss, LastNoTarget    (amend the last Move)
//   Miss         source, target
//   Fail         ident, Fail [, ability for Unboost and Ability]
//   Immune       ident, Immune [, ability for Ability and Confusion]
//   Activate     ident, Activate
//   Switch, Drag ident, species u16, gender, level, HP, SwitchFrom
//   Cant         ident, Cant [, move u16 for the reasons that name one]
//   Faint        ident
//   Turn         turn u16
//   Win          player
//   Tie          -
//   Damage       ident, HP, Damage, then: LeechSeed: of;
//                Bound: move u16; Ability: ability, of
//   Heal         ident, HP, Heal, then: Drain: of; Item: item;
//                Ability: ability, of; Wish: wisher's party position
//   Status       ident, status, StatusFrom, then: Move: move u16;
//                Ability: ability, of
//   CureStatus   ident, status, Cure, bench (1 for a Pokémon not active),
//                then: Move: move u16
//   CureTeam     ident ([from] move: Aromatherapy)
//   Crit, SuperEffective, Resisted       ident
//   HitCount     ident, hits
//   Prepare      ident, move u16
//   Anim         source, move u16, target
//   None         ends the update's messages
//
// The species, moves, items and abilities are the engine's enum values.

#pragma once

#include <cstdint>

#include "../common/data.hpp"
#include "../common/protocol.hpp"
#include "data.hpp"

// In libpkmn's one protocol namespace, with the plumbing (the engine is
// gen3 only).
namespace pkmn::protocol {

using gen3::Ability;
using gen3::Battle;
using gen3::Item;
using gen3::Move;
using gen3::Pokemon;
using gen3::Species;

// The most bytes one update writes. A placeholder until the bound is
// derived (decided 2026-10-08): the debug Writer asserts it.
inline constexpr size_t MAX_LOG_SIZE = 2048;

// |move|'s [from].
enum class MoveFrom : uint8_t {
  None,
  From,       // [from] MOVE (Sleep Talk, Pursuit)
  LockedMove, // [from] lockedmove
};

// |switch|'s [from].
enum class SwitchFrom : uint8_t { None, BatonPass };

enum class Cant : uint8_t {
  Sleep,
  Freeze,
  Paralysis,
  Flinch,
  Recharge,
  Attract,
  Truant,
  FocusPunch,
  // These name a move.
  Taunt,
  Disable,
  Imprison,
  PP,
  // The move Sleep Talk picked has no PP: Showdown's gen3 Sleep Talk prints
  // its ID, not its name (data/mods/gen3/moves.ts:574).
  SleepTalkPP,
};

constexpr bool names_move(Cant c) { return c >= Cant::Taunt; }

enum class Damage : uint8_t {
  None,
  Poison,
  Burn,
  Confusion,
  LeechSeed, // + of
  Recoil,
  Spikes,
  Sandstorm,
  Hail,
  Curse,
  Nightmare,
  Bound,   // + the binding move
  Ability, // + ability, of
};

enum class Heal : uint8_t {
  None,
  Silent,
  Drain,   // + of
  Item,    // + item
  Ability, // + ability, of (0 for none)
  Wish,    // + the wisher's party position
  Ingrain,
};

// -fail's reason. Status names the status the target already has.
enum class Fail : uint8_t {
  None,
  Heal,      // |heal
  Sleep,     // |slp
  Poison,    // |psn
  Burn,      // |brn
  Freeze,    // |frz
  Paralysis, // |par
  Toxic,     // |tox
  Substitute,     // |move: Substitute
  SubstituteWeak, // |move: Substitute|[weak]
  // |unboost|[from] ability: A|[of] P, + ability. Hyper Cutter and Keen Eye
  // name their stat (|unboost|atk|...): the formatter adds it.
  Unboost,
  Ability, // |[from] ability: A|[of] P, + ability (Rest and Insomnia)
};

// -immune's reason.
enum class Immune : uint8_t {
  None,
  Ability,   // |[from] ability: A, + ability
  Confusion, // |confusion|[from] ability: A, + ability (Own Tempo)
};

// -status's [from]. Showdown's gen3 conditions (data/conditions.ts and the
// gen3 mod's slp) name a move only for a sleep, and an ability only for the
// other statuses (Effect Spore's sleep is plain).
enum class StatusFrom : uint8_t {
  None,
  Move,    // |[from] move: M, + move
  Ability, // |[from] ability: A|[of] P, + ability, of
};

// Where a status comes from, as inflict()'s callers know it. status()
// keeps only what Showdown prints.
struct StatusSource {
  StatusFrom from;
  Move move;
  Ability ability;
  ID of;

  static constexpr StatusSource none() { return {StatusFrom::None, Move::None, Ability::None, {0}}; }
  static constexpr StatusSource by_move(Move m) { return {StatusFrom::Move, m, Ability::None, {0}}; }
  static constexpr StatusSource by_ability(Ability a, ID of) {
    return {StatusFrom::Ability, Move::None, a, of};
  }
};

// -curestatus's form (Pokemon.cureStatus, sim/pokemon.ts:1674, and its
// callers).
enum class Cure : uint8_t {
  Msg,         // |[msg]: waking, thawing, the berries, Refresh, the abilities
  Silent,      // |[silent]: Heal Bell
  NaturalCure, // |[from] ability: Natural Cure|[silent]
  Move,        // |[from] move: M, + move: a thawing move
};

// -activate's effect. Only the ones that stop a move so far (README,
// "Protocol": the hit results), and Heal Bell's.
enum class Activate : uint8_t {
  Protect,     // |Protect
  Safeguard,   // |move: Safeguard
  Mist,        // |move: Mist
  SuctionCups, // |ability: Suction Cups
  Ingrain,     // |move: Ingrain
  HealBell,    // |move: Heal Bell
};

// The ID of p's active Pokémon: its party position, which never changes.
constexpr ID ident(const Battle& battle, Player p) {
  return ID::init(p, battle.side(p).order[0]);
}

inline constexpr ID NO_ID{0};

namespace detail {

template <typename L> void hp(L& log, const Pokemon& mon) {
  log.template write_int<uint16_t>(mon.hp);
  log.template write_int<uint16_t>(mon.stats.hp);
  log.write_byte(mon.status.bits);
}

template <typename L> void move_id(L& log, Move m) {
  log.template write_int<uint16_t>(uint16_t(m));
}

} // namespace detail

template <typename L>
void move(L& log, ID source, Move m, ID target, MoveFrom from = MoveFrom::None,
          Move by = Move::None) {
  if constexpr (L::enabled) {
    log.write(ArgType::Move);
    log.write_byte(source.bits);
    detail::move_id(log, m);
    log.write_byte(target.bits);
    log.write_byte(uint8_t(from));
    if (from == MoveFrom::From) detail::move_id(log, by);
  }
}

template <typename L> void last_still(L& log) { log.write(ArgType::LastStill); }

// A missed move: [miss] on the move line, then -miss (the order of
// Showdown's tryMoveHit, data/mods/gen3/scripts.ts:383). Perish Song's
// misses have no [miss] (`attr` false).
template <typename L> void miss(L& log, ID source, ID target, bool attr = true) {
  if constexpr (L::enabled) {
    if (attr) log.write(ArgType::LastMiss);
    log.write(ArgType::Miss);
    log.write_byte(source.bits);
    log.write_byte(target.bits);
  }
}

template <typename L>
void switched(L& log, ArgType type, ID id, const Pokemon& mon,
              SwitchFrom from = SwitchFrom::None) {
  if constexpr (L::enabled) {
    log.write(type);
    log.write_byte(id.bits);
    log.template write_int<uint16_t>(uint16_t(mon.species));
    log.write_byte(uint8_t(mon.gender));
    log.write_byte(mon.level);
    detail::hp(log, mon);
    log.write_byte(uint8_t(from));
  }
}

template <typename L> void cant(L& log, ID id, Cant reason, Move m = Move::None) {
  if constexpr (L::enabled) {
    log.write(ArgType::Cant);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(reason));
    if (names_move(reason)) detail::move_id(log, m);
  }
}

// Queues the Faint of a Pokémon that just reached 0 HP (FaintQueue).
template <typename L> void faint(L& log, ID id) {
  if constexpr (L::enabled) {
    auto& q = log.faints;
    if (q.written & (1u << id.bits)) return;
    for (uint8_t i = 0; i < q.n; i++) {
      if (q.ids[i] == id) return;
    }
    assert(q.n < 2);
    q.ids[q.n++] = id;
  }
}

// Writes the queued Faints, in the order the Pokémon reached 0 HP
// (Showdown's faintMessages).
template <typename L> void faints(L& log) {
  if constexpr (L::enabled) {
    auto& q = log.faints;
    for (uint8_t i = 0; i < q.n; i++) {
      log.write(ArgType::Faint);
      log.write_byte(q.ids[i].bits);
      q.written = uint16_t(q.written | (1u << q.ids[i].bits));
    }
    q.n = 0;
  }
}

// -fail. With `still`, the move line gets [still] first, as Showdown's
// tryMoveHit and most onTry failures (attrLastMove('[still]')).
template <typename L>
void fail(L& log, ID id, Fail reason = Fail::None, bool still = false) {
  if constexpr (L::enabled) {
    assert(reason != Fail::Unboost && reason != Fail::Ability);
    if (still) log.write(ArgType::LastStill);
    log.write(ArgType::Fail);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(reason));
  }
}

// The plain -fail of a move that failed, with [still]: the most common form.
template <typename L> void failed(L& log, ID user) { fail(log, user, Fail::None, true); }

// -fail|P|unboost|[from] ability: A|[of] P: Clear Body and the like.
template <typename L> void fail_unboost(L& log, ID id, Ability a) {
  if constexpr (L::enabled) {
    log.write(ArgType::Fail);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(Fail::Unboost));
    log.write_byte(uint8_t(a));
  }
}

// -fail|P|[from] ability: A|[of] P: Rest stopped by the user's own ability.
template <typename L> void fail_ability(L& log, ID id, Ability a) {
  if constexpr (L::enabled) {
    log.write(ArgType::Fail);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(Fail::Ability));
    log.write_byte(uint8_t(a));
  }
}

// -immune, plain or from the target's ability.
template <typename L>
void immune(L& log, ID id, Immune reason = Immune::None, Ability a = Ability::None) {
  if constexpr (L::enabled) {
    log.write(ArgType::Immune);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(reason));
    if (reason != Immune::None) log.write_byte(uint8_t(a));
  }
}

template <typename L> void activate(L& log, ID id, Activate effect) {
  if constexpr (L::enabled) {
    log.write(ArgType::Activate);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
  }
}

// A hit's type effectiveness, as Showdown's gen3 modifyDamage reports it
// (data/mods/gen3/scripts.ts:92): -supereffective or -resisted by the
// summed type modifier's sign, nothing when it is 0.
template <typename L> void effectiveness(L& log, ID id, bool super, bool resisted) {
  if constexpr (L::enabled) {
    if (!super && !resisted) return;
    log.write(super ? ArgType::SuperEffective : ArgType::Resisted);
    log.write_byte(id.bits);
  }
}

template <typename L> void crit(L& log, ID id) {
  if constexpr (L::enabled) {
    log.write(ArgType::Crit);
    log.write_byte(id.bits);
  }
}

// -hitcount|P|N after a multi-hit move's loop.
template <typename L> void hit_count(L& log, ID id, uint8_t hits) {
  if constexpr (L::enabled) {
    log.write(ArgType::HitCount);
    log.write_byte(id.bits);
    log.write_byte(hits);
  }
}

// -prepare|P|MOVE: a two-turn move's first turn.
template <typename L> void prepare(L& log, ID id, Move m) {
  if constexpr (L::enabled) {
    log.write(ArgType::Prepare);
    log.write_byte(id.bits);
    detail::move_id(log, m);
  }
}

// -anim|SOURCE|MOVE|TARGET (Battle.addMove): it becomes the line LastStill
// and LastMiss amend.
template <typename L> void anim(L& log, ID source, Move m, ID target) {
  if constexpr (L::enabled) {
    log.write(ArgType::Anim);
    log.write_byte(source.bits);
    detail::move_id(log, m);
    log.write_byte(target.bits);
  }
}

template <typename L>
void status(L& log, ID id, gen3::Status st, StatusSource src = StatusSource::none()) {
  if constexpr (L::enabled) {
    assert(st.any());
    StatusFrom from = src.from;
    if (from == StatusFrom::Move && !st.asleep()) from = StatusFrom::None;
    if (from == StatusFrom::Ability && st.asleep()) from = StatusFrom::None;
    log.write(ArgType::Status);
    log.write_byte(id.bits);
    log.write_byte(st.bits);
    log.write_byte(uint8_t(from));
    if (from == StatusFrom::Move) detail::move_id(log, src.move);
    if (from == StatusFrom::Ability) {
      log.write_byte(uint8_t(src.ability));
      log.write_byte(src.of.bits);
    }
  }
}

// -curestatus of the status `st` a Pokémon had. `bench` prints Showdown's
// ident of a Pokémon that isn't active (p1: NAME).
template <typename L>
void cure_status(L& log, ID id, gen3::Status st, Cure cure = Cure::Msg,
                 bool bench = false, Move m = Move::None) {
  if constexpr (L::enabled) {
    assert(st.any());
    log.write(ArgType::CureStatus);
    log.write_byte(id.bits);
    log.write_byte(st.bits);
    log.write_byte(uint8_t(cure));
    log.write_byte(bench);
    if (cure == Cure::Move) detail::move_id(log, m);
  }
}

// -cureteam|P|[from] move: Aromatherapy (Showdown's gen4 Aromatherapy).
template <typename L> void cure_team(L& log, ID id) {
  if constexpr (L::enabled) {
    log.write(ArgType::CureTeam);
    log.write_byte(id.bits);
  }
}

template <typename L> void turn(L& log, uint16_t n) {
  if constexpr (L::enabled) {
    log.write(ArgType::Turn);
    log.template write_int<uint16_t>(n);
  }
}

template <typename L> void win(L& log, Player p) {
  if constexpr (L::enabled) {
    log.write(ArgType::Win);
    log.write_byte(uint8_t(p));
  }
}

template <typename L> void tie(L& log) { log.write(ArgType::Tie); }

// Ends the update's messages.
template <typename L> void end(L& log) {
  if constexpr (L::enabled) {
    assert(log.faints.n == 0);
    log.faints.written = 0;
    log.write(ArgType::None);
  }
}

template <typename L>
void damage(L& log, ID id, const Pokemon& mon, Damage reason = Damage::None) {
  if constexpr (L::enabled) {
    assert(reason != Damage::LeechSeed && reason != Damage::Bound &&
           reason != Damage::Ability);
    log.write(ArgType::Damage);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(reason));
  }
}

// Damage [from] Leech Seed, [of] the seeder.
template <typename L>
void damage_seeded(L& log, ID id, const Pokemon& mon, ID of) {
  if constexpr (L::enabled) {
    log.write(ArgType::Damage);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(Damage::LeechSeed));
    log.write_byte(of.bits);
  }
}

template <typename L>
void damage_bound(L& log, ID id, const Pokemon& mon, Move m) {
  if constexpr (L::enabled) {
    log.write(ArgType::Damage);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(Damage::Bound));
    detail::move_id(log, m);
  }
}

template <typename L>
void damage_ability(L& log, ID id, const Pokemon& mon, Ability a, ID of) {
  if constexpr (L::enabled) {
    log.write(ArgType::Damage);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(Damage::Ability));
    log.write_byte(uint8_t(a));
    log.write_byte(of.bits);
  }
}

template <typename L>
void heal(L& log, ID id, const Pokemon& mon, Heal reason = Heal::None) {
  if constexpr (L::enabled) {
    assert(reason == Heal::None || reason == Heal::Silent || reason == Heal::Ingrain);
    log.write(ArgType::Heal);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(reason));
  }
}

template <typename L> void heal_drain(L& log, ID id, const Pokemon& mon, ID of) {
  if constexpr (L::enabled) {
    log.write(ArgType::Heal);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(Heal::Drain));
    log.write_byte(of.bits);
  }
}

template <typename L> void heal_item(L& log, ID id, const Pokemon& mon, Item item) {
  if constexpr (L::enabled) {
    log.write(ArgType::Heal);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(Heal::Item));
    log.write_byte(uint8_t(item));
  }
}

template <typename L>
void heal_ability(L& log, ID id, const Pokemon& mon, Ability a, ID of = NO_ID) {
  if constexpr (L::enabled) {
    log.write(ArgType::Heal);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(Heal::Ability));
    log.write_byte(uint8_t(a));
    log.write_byte(of.bits);
  }
}

template <typename L>
void heal_wish(L& log, ID id, const Pokemon& mon, uint8_t wisher) {
  if constexpr (L::enabled) {
    log.write(ArgType::Heal);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(uint8_t(Heal::Wish));
    log.write_byte(wisher);
  }
}

} // namespace pkmn::protocol
