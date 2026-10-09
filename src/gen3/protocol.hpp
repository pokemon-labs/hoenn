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
//   Activate     ident, Activate, then: Ability: ability; Magnitude: n;
//                activate_move: move u16; activate_of: of (move first)
//   Start        ident, Start, then: Curse..AttractCuteCharm: of;
//                Disable: move u16; TypeChange, ColorChange: type;
//                Stockpile, Perish: n
//   End          ident, End [, move u16 for end_move]
//   SingleTurn   ident, SingleTurn
//   SingleMove   ident, SingleMove
//   FieldActivate FieldActivate
//   SetHP        ident, HP, silent ([from] move: Pain Split)
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
//   Boost        ident, stat | BOOST_DOWN | BOOST_ITEM, stages [, item]
//   SetBoost     ident (Belly Drum's Attack to 6)
//   CopyBoost    source, target ([from] move: Psych Up)
//   ClearAllBoost       -
//   ClearNegativeBoost  ident ([silent]: White Herb)
//   Crit, SuperEffective, Resisted       ident
//   HitCount     ident, hits
//   Prepare      ident, move u16
//   Anim         source, move u16, target
//   SideStart    player, SideCondition
//   SideEnd      player, SideCondition [, of for SpikesRapidSpin]
//   Weather      weather (gen3::Weather), WeatherFrom [, ability, of for
//                Ability]
//   Item         ident, item, ItemFrom [, of for Thief and Covet]
//   EndItem      ident, item, EndItemFrom [, of for KnockOff and Thief]
//   Ability      ident, ability, AbilityFrom [, old ability, of for Trace
//                and RolePlay]
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

// -activate's effect: the gen3 inventory (README, "Protocol"). Those marked
// + carry more bytes after the effect.
enum class Activate : uint8_t {
  Protect,         // |Protect
  Safeguard,       // |move: Safeguard
  Mist,            // |move: Mist
  SuctionCups,     // |ability: Suction Cups
  Ingrain,         // |move: Ingrain
  HealBell,        // |move: Heal Bell
  SubstituteDamage,// |Substitute|[damage]
  Confusion,       // |confusion
  Trapped,         // |trapped
  FocusBand,       // |item: Focus Band
  DestinyBond,     // |move: Destiny Bond
  Endure,          // |move: Endure
  Pursuit,         // |move: Pursuit
  Bide,            // |move: Bide
  // + an ability: |ability: A (Sticky Hold, Shed Skin, the status cures)
  Ability,
  // + a move.
  Mimic,  // |move: Mimic|M
  Grudge, // |move: Grudge|M
  Leppa,  // |item: Leppa Berry|M|[consumed]
  // + of.
  Attract,    // |move: Attract|[of] P
  LockOn,     // |move: Lock-On|[of] P
  MindReader, // |move: Mind Reader|[of] P
  Snatch,     // |move: Snatch|[of] P
  Trick,      // |move: Trick|[of] P
  // + a move, then of: |move: M|[of] P (a binding move's start)
  Bound,
  // + a number: |move: Magnitude|N
  Magnitude,
};

constexpr bool activate_move(Activate a) {
  return a == Activate::Mimic || a == Activate::Grudge || a == Activate::Leppa ||
         a == Activate::Bound;
}
constexpr bool activate_of(Activate a) {
  return (a >= Activate::Attract && a <= Activate::Trick) || a == Activate::Bound;
}

// -start's effect. Those marked + carry more bytes after the effect.
enum class Start : uint8_t {
  Confusion,        // |confusion
  ConfusionFatigue, // |confusion|[fatigue]
  Substitute,       // |Substitute
  LeechSeed,        // |move: Leech Seed
  Nightmare,        // |Nightmare
  Encore,           // |Encore
  Taunt,            // |move: Taunt
  Torment,          // |Torment
  Foresight,        // |Foresight
  Imprison,         // |move: Imprison
  Ingrain,          // |move: Ingrain
  MudSport,         // |Mud Sport
  WaterSport,       // |move: Water Sport
  Charge,           // |Charge
  Uproar,           // |Uproar
  UproarUpkeep,     // |Uproar|[upkeep]
  Bide,             // |move: Bide
  FlashFire,        // |ability: Flash Fire
  FocusEnergy,      // |move: Focus Energy
  FocusEnergySilent,// |move: Focus Energy|[silent] (Psych Up)
  FutureSight,      // |Future Sight
  DoomDesire,       // |Doom Desire
  PerishSilent,     // |perish3|[silent] (Perish Song itself)
  // + of.
  Curse,            // |Curse|[of] P
  Yawn,             // |move: Yawn|[of] P
  AttractCuteCharm, // |Attract|[from] ability: Cute Charm|[of] P
  // + a move.
  Disable, // |Disable|M
  // + a type.
  TypeChange,  // |typechange|T
  ColorChange, // |typechange|T|[from] ability: Color Change
  // + a number.
  Stockpile, // |stockpileN
  Perish,    // |perishN
};

// -end's effect. Those marked + carry more bytes after the effect.
enum class End : uint8_t {
  Confusion,          // |confusion
  Substitute,         // |Substitute
  LeechSeedRapidSpin, // |Leech Seed|[from] move: Rapid Spin|[of] P (itself)
  Nightmare,          // |Nightmare|[silent]
  Encore,             // |Encore
  Disable,            // |Disable
  Taunt,              // |move: Taunt|[silent] (gen3's)
  Torment,            // |Torment
  Yawn,               // |move: Yawn|[silent]
  Attract,            // |Attract|[silent]
  AttractOblivious,   // |move: Attract|[from] ability: Oblivious
  AttractMentalHerb,  // |move: Attract|[from] item: Mental Herb
  Uproar,             // |Uproar
  Bide,               // |move: Bide
  Stockpile,          // |Stockpile
  Charge,             // |Charge|[silent]
  FlashFire,          // |ability: Flash Fire|[silent] (the ability's End)
  // + a move.
  Bound,       // |M|[partiallytrapped]
  BoundSilent, // |M|[partiallytrapped]|[silent] (Substitute frees it)
  FutureMove,  // |move: M (Future Sight or Doom Desire hits)
};

constexpr bool end_move(End e) { return e >= End::Bound; }

// -singleturn's effect.
enum class SingleTurn : uint8_t {
  Protect,    // |Protect (Protect and Detect)
  Endure,     // |move: Endure
  FocusPunch, // |move: Focus Punch
  Snatch,     // |Snatch
  MagicCoat,  // |move: Magic Coat
};

// -singlemove's effect.
enum class SingleMove : uint8_t {
  DestinyBond, // |Destiny Bond
  Grudge,      // |Grudge
  Rage,        // |Rage
};

// -fieldactivate's effect.
enum class FieldActivate : uint8_t {
  PerishSong, // |move: Perish Song
  PayDay,     // |move: Pay Day
};

// -sidestart's and -sideend's condition (gen3's: no Persistent, so no
// Safeguard [persistent]).
enum class SideCondition : uint8_t {
  Reflect,     // |Reflect
  LightScreen, // |move: Light Screen
  Mist,        // |Mist
  Safeguard,   // |Safeguard
  Spikes,      // |Spikes
  // -sideend only, + of: |Spikes|[from] move: Rapid Spin|[of] P
  SpikesRapidSpin,
};

// -weather's form.
enum class WeatherFrom : uint8_t {
  None,    // a move's weather, or none when it ends
  Upkeep,  // |[upkeep]
  Ability, // |[from] ability: A|[of] P, + ability, of
};

// -item's form: the item gained.
enum class ItemFrom : uint8_t {
  Thief, // |[from] move: Thief|[of] P, + of
  Covet, // |[from] move: Covet|[of] P, + of
  Trick, // |[from] move: Trick
};

// -enditem's form: the item lost.
enum class EndItemFrom : uint8_t {
  None,     // useItem: White Herb, Mental Herb, Berry Juice
  Eat,      // |[eat]: eatItem, every berry
  KnockOff, // |[from] move: Knock Off|[of] P, + of
  Thief,    // |[silent]|[from] move: Thief|[of] P, + of (Covet prints none)
  Trick,    // |[silent]|[from] move: Trick
};

// -ability's form.
enum class AbilityFrom : uint8_t {
  Boost,    // |boost: Intimidate, and Speed Boost before its -boost
  Pressure, // |[silent], in a |split| triplet with an empty public line
  Trace,    // NEW|OLD|[from] ability: Trace|[of] P, + old ability, of
  RolePlay, // NEW|OLD|[from] move: Role Play|[of] P, + old ability, of
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

// Queues the Faint of a Pokémon that just reached 0 HP (FaintQueue). After
// its faint line, Showdown's faintMessages runs the Faint event, then the
// ability's End: with `bond` its Destiny Bond takes the foe (-activate|P|
// move: Destiny Bond), and with `flash_fire` the boost ends (-end|P|ability:
// Flash Fire|[silent]).
template <typename L>
void faint(L& log, ID id, bool bond = false, bool flash_fire = false) {
  if constexpr (L::enabled) {
    auto& q = log.faints;
    if (q.written & (1u << id.bits)) return;
    if (bond) q.bond = uint16_t(q.bond | (1u << id.bits));
    if (flash_fire) q.flash_fire = uint16_t(q.flash_fire | (1u << id.bits));
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
      if (q.bond & (1u << q.ids[i].bits)) {
        log.write(ArgType::Activate);
        log.write_byte(q.ids[i].bits);
        log.write_byte(uint8_t(Activate::DestinyBond));
      }
      if (q.flash_fire & (1u << q.ids[i].bits)) {
        log.write(ArgType::End);
        log.write_byte(q.ids[i].bits);
        log.write_byte(uint8_t(End::FlashFire));
      }
    }
    q.n = 0;
    q.bond = 0;
    q.flash_fire = 0;
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
    assert(effect < Activate::Ability);
    log.write(ArgType::Activate);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
  }
}

// -activate|P|ability: A.
template <typename L> void activate_ability(L& log, ID id, Ability a) {
  if constexpr (L::enabled) {
    log.write(ArgType::Activate);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(Activate::Ability));
    log.write_byte(uint8_t(a));
  }
}

// -activate with a move, an of, or both (activate_move, activate_of).
template <typename L>
void activate_with(L& log, ID id, Activate effect, Move m, ID of = NO_ID) {
  if constexpr (L::enabled) {
    assert(activate_move(effect) || activate_of(effect));
    log.write(ArgType::Activate);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
    if (activate_move(effect)) detail::move_id(log, m);
    if (activate_of(effect)) log.write_byte(of.bits);
  }
}

// -start: the plain effects, or with `of` for Curse, Yawn and Cute Charm's
// Attract.
template <typename L> void start(L& log, ID id, Start effect, ID of = NO_ID) {
  if constexpr (L::enabled) {
    assert(effect < Start::Disable);
    assert((effect >= Start::Curse) == (of != NO_ID));
    log.write(ArgType::Start);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
    if (effect >= Start::Curse) log.write_byte(of.bits);
  }
}

// -start|P|Disable|M.
template <typename L> void start_disable(L& log, ID id, Move m) {
  if constexpr (L::enabled) {
    log.write(ArgType::Start);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(Start::Disable));
    detail::move_id(log, m);
  }
}

// -start|P|typechange|T, plain or from Color Change.
template <typename L> void start_type(L& log, ID id, gen3::Type t, bool color_change) {
  if constexpr (L::enabled) {
    log.write(ArgType::Start);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(color_change ? Start::ColorChange : Start::TypeChange));
    log.write_byte(uint8_t(t));
  }
}

// -formechange|P|FORME|[msg]|[from] ability: Forecast: Castform's form,
// written as the type it takes (Normal for Castform, Fire, Water or Ice for
// Castform-Sunny, -Rainy, -Snowy).
template <typename L> void forme_change(L& log, ID id, gen3::Type t) {
  if constexpr (L::enabled) {
    assert(t == gen3::Type::Normal || t == gen3::Type::Fire || t == gen3::Type::Water ||
           t == gen3::Type::Ice);
    log.write(ArgType::FormeChange);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(t));
  }
}

// -start|P|perishN (n 0-3) or -start|P|stockpileN (n 1-3).
template <typename L> void start_count(L& log, ID id, Start effect, uint8_t n) {
  if constexpr (L::enabled) {
    assert(effect == Start::Perish || effect == Start::Stockpile);
    assert(n <= 3);
    log.write(ArgType::Start);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
    log.write_byte(n);
  }
}

// -end, with the move for the binding ends and a future move's hit.
template <typename L>
void end_effect(L& log, ID id, End effect, Move m = Move::None) {
  if constexpr (L::enabled) {
    log.write(ArgType::End);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
    if (end_move(effect)) detail::move_id(log, m);
  }
}

template <typename L> void single_turn(L& log, ID id, SingleTurn effect) {
  if constexpr (L::enabled) {
    log.write(ArgType::SingleTurn);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
  }
}

template <typename L> void single_move(L& log, ID id, SingleMove effect) {
  if constexpr (L::enabled) {
    log.write(ArgType::SingleMove);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(effect));
  }
}

template <typename L> void field_activate(L& log, FieldActivate effect) {
  if constexpr (L::enabled) {
    log.write(ArgType::FieldActivate);
    log.write_byte(uint8_t(effect));
  }
}

// -sethp|P|HP|[from] move: Pain Split, with [silent] for the target's.
template <typename L> void set_hp(L& log, ID id, const Pokemon& mon, bool silent) {
  if constexpr (L::enabled) {
    log.write(ArgType::SetHP);
    log.write_byte(id.bits);
    detail::hp(log, mon);
    log.write_byte(silent);
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

// Boost's stat byte: the stage index (mechanics.hpp's stage(): 0 Atk, 1 Def,
// 2 Spe, 3 SpA, 4 SpD, 5 accuracy, 6 evasion), with these flags.
inline constexpr uint8_t BOOST_DOWN = 1 << 6; // -unboost
inline constexpr uint8_t BOOST_ITEM = 1 << 7; // [from] item: I, + item

// -boost|P|STAT|N or -unboost (Battle.boost, sim/battle.ts:2024): `n` is
// the stages the stat moved, 0 at the cap. Showdown prints -unboost for a
// drop and for any change that leaves the stage at -6, which only a drop
// of 0 can do, so `down` is the direction asked for. A stat berry adds
// [from] item (`item`), but a 0 is printed plain (the boostBy == 0 branch).
template <typename L>
void boost(L& log, ID id, uint8_t stat, int8_t n, bool down, Item item = Item::None) {
  if constexpr (L::enabled) {
    assert(stat < 7 && n >= 0 && n <= 12);
    if (n == 0) item = Item::None;
    log.write(ArgType::Boost);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(stat | (down ? BOOST_DOWN : 0) |
                           (item != Item::None ? BOOST_ITEM : 0)));
    log.write_byte(uint8_t(n));
    if (item != Item::None) log.write_byte(uint8_t(item));
  }
}

// -setboost|P|atk|6|[from] move: Belly Drum: gen3's only -setboost.
template <typename L> void set_boost(L& log, ID id) {
  if constexpr (L::enabled) {
    log.write(ArgType::SetBoost);
    log.write_byte(id.bits);
  }
}

// -copyboost|SOURCE|TARGET|[from] move: Psych Up.
template <typename L> void copy_boost(L& log, ID source, ID target) {
  if constexpr (L::enabled) {
    log.write(ArgType::CopyBoost);
    log.write_byte(source.bits);
    log.write_byte(target.bits);
  }
}

// -clearallboost: Haze.
template <typename L> void clear_all_boost(L& log) { log.write(ArgType::ClearAllBoost); }

// -clearnegativeboost|P|[silent]: White Herb.
template <typename L> void clear_negative_boost(L& log, ID id) {
  if constexpr (L::enabled) {
    log.write(ArgType::ClearNegativeBoost);
    log.write_byte(id.bits);
  }
}

template <typename L> void side_start(L& log, Player p, SideCondition c) {
  if constexpr (L::enabled) {
    assert(c != SideCondition::SpikesRapidSpin);
    log.write(ArgType::SideStart);
    log.write_byte(uint8_t(p));
    log.write_byte(uint8_t(c));
  }
}

// -sideend, with the Rapid Spin user `of` for SpikesRapidSpin.
template <typename L>
void side_end(L& log, Player p, SideCondition c, ID of = NO_ID) {
  if constexpr (L::enabled) {
    assert((c == SideCondition::SpikesRapidSpin) == (of != NO_ID));
    log.write(ArgType::SideEnd);
    log.write_byte(uint8_t(p));
    log.write_byte(uint8_t(c));
    if (c == SideCondition::SpikesRapidSpin) log.write_byte(of.bits);
  }
}

// -weather: a move's (plain), the end of the weather (Weather::None), or
// its residual (Upkeep).
template <typename L>
void weather(L& log, gen3::Weather w, WeatherFrom from = WeatherFrom::None) {
  if constexpr (L::enabled) {
    assert(from != WeatherFrom::Ability);
    assert((w == gen3::Weather::None) <= (from == WeatherFrom::None));
    log.write(ArgType::Weather);
    log.write_byte(uint8_t(w));
    log.write_byte(uint8_t(from));
  }
}

// -weather|W|[from] ability: A|[of] P: Drizzle, Drought, Sand Stream.
template <typename L> void weather_ability(L& log, gen3::Weather w, Ability a, ID of) {
  if constexpr (L::enabled) {
    log.write(ArgType::Weather);
    log.write_byte(uint8_t(w));
    log.write_byte(uint8_t(WeatherFrom::Ability));
    log.write_byte(uint8_t(a));
    log.write_byte(of.bits);
  }
}

// -item: Thief's and Covet's `of` is the Pokémon robbed; Trick has none.
template <typename L> void item(L& log, ID id, Item i, ItemFrom from, ID of = NO_ID) {
  if constexpr (L::enabled) {
    assert((from != ItemFrom::Trick) == (of != NO_ID));
    log.write(ArgType::Item);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(i));
    log.write_byte(uint8_t(from));
    if (from != ItemFrom::Trick) log.write_byte(of.bits);
  }
}

// -enditem: Knock Off's and Thief's `of` is the user.
template <typename L>
void end_item(L& log, ID id, Item i, EndItemFrom from, ID of = NO_ID) {
  if constexpr (L::enabled) {
    const bool has_of = from == EndItemFrom::KnockOff || from == EndItemFrom::Thief;
    assert(has_of == (of != NO_ID));
    log.write(ArgType::EndItem);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(i));
    log.write_byte(uint8_t(from));
    if (has_of) log.write_byte(of.bits);
  }
}

// -ability|P|A|boost (Intimidate, Speed Boost) or Pressure's split line.
template <typename L> void ability(L& log, ID id, Ability a, AbilityFrom from) {
  if constexpr (L::enabled) {
    assert(from == AbilityFrom::Boost || from == AbilityFrom::Pressure);
    log.write(ArgType::Ability);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(a));
    log.write_byte(uint8_t(from));
  }
}

// -ability|P|NEW|OLD|[from] ...|[of] Q: Trace and Role Play (setAbility).
template <typename L>
void ability_copy(L& log, ID id, Ability a, Ability old, AbilityFrom from, ID of) {
  if constexpr (L::enabled) {
    assert(from == AbilityFrom::Trace || from == AbilityFrom::RolePlay);
    log.write(ArgType::Ability);
    log.write_byte(id.bits);
    log.write_byte(uint8_t(a));
    log.write_byte(uint8_t(from));
    log.write_byte(uint8_t(old));
    log.write_byte(of.bits);
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
    assert(log.faints.n == 0 && log.faints.bond == 0 && log.faints.flash_fire == 0);
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
