// The turn frame: update() and choices(), as libpkmn's gen*/mechanics.zig.
//
// Plan step 2 (CLAUDE.md): the frame, the order of actions, switching and the
// roll helpers it needs, with real and null Chance and Calc. Not here yet,
// and marked TODO where they go: the hit backbone (a move does nothing),
// switch-in effects (Spikes, abilities, items), the residuals, Future Sight,
// Perish Song and Baton Pass.
//
// The frame is one switch on request.phase whose cases fall through in turn
// order (CLAUDE.md, "Turn phases"). A case resumed after a request first
// does its replacement. Internal steps return a Flow: empty to continue, or
// the Result to return (a request, or the end of the battle).
//
// The caller passes only valid battles and choices from choices(). Debug
// asserts both; release checks neither (CLAUDE.md, "Safety").
//
// Paths: bsc = src/battle_script_commands.c, util = src/battle_util.c,
// main = src/battle_main.c, bs1 = data/battle_scripts_1.s.

#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "../common/data.hpp"
#include "../common/options.hpp"
#include "calc.hpp"
#include "chance.hpp"
#include "damage.hpp"
#include "data.hpp"

namespace pkmn::gen3 {

// The most choices a player can have: four moves and five switches.
inline constexpr uint8_t MAX_CHOICES = 9;

namespace detail {

template <typename O>
inline constexpr bool CHANCE = std::remove_cvref_t<O>::ChanceType::enabled;
template <typename O>
inline constexpr bool CALC = std::remove_cvref_t<O>::CalcType::enabled;

using Flow = std::optional<Result>;

constexpr uint8_t index(Player p) { return uint8_t(p); }

constexpr int8_t priority(Move move) {
  return move == Move::None ? 0 : get(move).priority;
}

constexpr bool holds(const Side& side, HoldEffect effect) {
  return item_data(side.get(1).item).effect == effect;
}

constexpr bool has_type(const ActivePokemon& active, Type type) {
  return active.types[0] == type || active.types[1] == type;
}

// Charging, rampage, Bide, Rollout, Uproar and recharge: the only choice is
// Move(0), and the action is the locked move (main:4160, util:107).
constexpr bool forced(const ActivePokemon& active) {
  return active.volatiles.multiple_turns || active.volatiles.recharge;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Roll helpers, as libpkmn's Rolls: one per catalogue row in chance.hpp. Each
// takes the Calc override if one is set, else draws, then records the
// outcome and its probability in Chance. A helper is called only where its
// outcome can matter (CLAUDE.md, "Chance").
// ---------------------------------------------------------------------------

namespace rolls {

// Sets an Actions field. A field is set at most once per update; a second
// write means the catalogue's multiplicity is wrong.
template <typename T> constexpr void record(T& field, T value) {
  assert(field == T{} && "Actions field set twice in one update");
  field = value;
}

template <typename O>
constexpr void observe(O& options, uint64_t num, uint64_t den) {
  if constexpr (detail::CHANCE<O>) {
    assert(num >= 1 && num <= den);
    const bool ok = options.chance.probability.update(num, den);
    assert(ok);
    (void)ok;
  }
}

// 1/5, as Showdown (one randomChance(1, 5) per turn, sim/battle.ts:1802).
// The decomp's gRandomTurnNumber < 0xFFFF * 20 / 100 (main:4653) is
// 13107/65536, just under.

// The turn's Quick Claw draw: one u16 serves both holders and both orders of
// the turn (main:4013). Drawn once, kept in the request.
template <typename O> constexpr bool quick_claw(Battle& battle, O& options) {
  Optional& drawn = battle.request.quick_claw;
  if (drawn != Optional::None) return drawn == Optional::True;

  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.turn.quick_claw;
  assert(forced <= Optional::False);
  const bool proc = forced != Optional::None
                        ? forced == Optional::True
                        : battle.rng.chance(1, 5);

  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.turn.quick_claw, optional(proc));
    observe(options, proc ? 1 : 4, 5);
  }
  drawn = optional(proc);
  return proc;
}

// A speed tie: `Random() & 1` in GetWhoStrikesFirst (main:4728, 4745), for
// the move order (or two switches) or, with `residual`, ENDTURN_ORDER's
// independent re-roll (util:1209). Returns who goes first.
template <typename O>
constexpr Player speed_tie(Battle& battle, bool residual, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) {
    const Turn& t = options.calc.overrides.turn;
    forced = residual ? t.residual_tie : t.speed_tie;
  }
  assert(forced <= 2);
  const bool p1 = forced != 0 ? forced == 1 : battle.rng.chance(1, 2);

  if constexpr (detail::CHANCE<O>) {
    Turn& t = options.chance.actions.turn;
    record(residual ? t.residual_tie : t.speed_tie, uint8_t(p1 ? 1 : 2));
    observe(options, 1, 2);
  }
  return p1 ? Player::P1 : Player::P2;
}

// A yes/no draw with probability num / den, for player p's action. `forced`
// is the Calc override (None rolls) and `field` the Actions field to record.
template <typename O>
constexpr bool coin(Battle& battle, Optional forced, Optional* field,
                    uint32_t num, uint32_t den, O& options) {
  assert(num >= 1 && num < den);
  assert(forced <= Optional::False);
  const bool yes = forced != Optional::None ? forced == Optional::True
                                            : battle.rng.chance(num, den);
  if constexpr (detail::CHANCE<O>) {
    record(*field, optional(yes));
    observe(options, yes ? num : den - num, den);
  }
  (void)field;
  return yes;
}

// coin() on p's Action field `m`: its Calc override and its record.
template <typename O>
constexpr bool coin(Battle& battle, Player p, Optional Action::*m,
                    uint32_t num, uint32_t den, O& options) {
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).*m;
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) field = &(options.chance.actions.get(p).*m);
  return coin(battle, forced, field, num, den, options);
}

// Thawing at a move attempt: Random() % 5 == 0 (CANCELER_FROZEN, util:2048).
template <typename O>
constexpr bool thaw(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::thaw, 1, 5, options);
}

// Full paralysis: Random() % 4 == 0 (CANCELER_PARALYZED, util:2175).
template <typename O>
constexpr bool paralysis(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::paralysis, 1, 4, options);
}

// A confusion self-hit: !(Random() & 1) (CANCELER_CONFUSED, util:2148).
template <typename O>
constexpr bool confusion(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::confusion, 1, 2, options);
}

// Immobilized by love: !(Random() & 1) (CANCELER_IN_LOVE, util:2190).
template <typename O>
constexpr bool infatuation(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::infatuation, 1, 2, options);
}

// The accuracy check: a hit on `calc` of 100 outcomes, from accuracy()
// (Showdown's randomChance; the decomp's draw is at bsc:1176). Called only
// for 0 < calc < 100; outside that the outcome is certain.
template <typename O>
constexpr bool hit(Battle& battle, Player p, uint32_t calc, O& options) {
  assert(calc > 0 && calc < 100);
  return coin(battle, p, &Action::hit, calc, 100, options);
}

// A move's secondary effect: Random() % 100 < chance (seteffectwithchance,
// bsc:2923). Called only for 0 < chance < 100 and when the effect could take
// hold.
template <typename O>
constexpr bool secondary(Battle& battle, Player p, uint32_t chance,
                         O& options) {
  assert(chance > 0 && chance < 100);
  return coin(battle, p, &Action::secondary, chance, 100, options);
}

// Tri Attack's status: Random() % 3, 1 burn, 2 freeze, 3 paralysis
// (bsc:2601).
template <typename O>
constexpr uint8_t tri_attack(Battle& battle, Player p, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).tri_attack;
  assert(forced <= 3);
  const uint8_t which = forced != 0 ? forced : uint8_t(1 + battle.rng.range(3));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).tri_attack, which);
    observe(options, 1, 3);
  }
  return which;
}

// A crit on hit `i`: !(Random() % n) with n from the crit stage (bsc:1282).
template <typename O>
constexpr bool crit(Battle& battle, Player p, uint8_t i, uint8_t stage,
                    O& options) {
  assert(i < 5 && stage < 5);
  const uint8_t n = CRITICAL_HIT_CHANCE[stage];
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].crit;
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) field = &options.chance.actions.get(p).hits[i].crit;
  return coin(battle, forced, field, 1, n, options);
}

// The defender's Focus Band on hit `i`: Random() % 100 < param (bsc:1677).
// Called only when the hit would be lethal.
template <typename O>
constexpr bool focus_band(Battle& battle, Player p, uint8_t i, uint8_t param,
                          O& options) {
  assert(i < 5);
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) {
    forced = options.calc.overrides.get(p).hits[i].focus_band;
  }
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) {
    field = &options.chance.actions.get(p).hits[i].focus_band;
  }
  return coin(battle, forced, field, param, 100, options);
}

// The damage roll on hit `i`: 100 - Random() % 16 percent
// (ApplyRandomDmgMultiplier, bsc:1641). Returns apply_roll's index, 0-15
// (85-100%). Actions holds the percentage.
template <typename O>
constexpr uint8_t damage(Battle& battle, Player p, uint8_t i, O& options) {
  assert(i < 5);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].damage;
  assert(forced == 0 || (forced >= 85 && forced <= 100));
  const uint8_t index =
      forced != 0 ? uint8_t(forced - 85) : uint8_t(battle.rng.range(16));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).hits[i].damage, uint8_t(85 + index));
    observe(options, 1, 16);
  }
  return index;
}

// A hidden counter's tick, observed: whether the effect ends now. The battle
// holds the true counter, so nothing is drawn; this records the outcome, its
// probability given the public count, and the new count. Callers guard it
// with `if constexpr (detail::CHANCE<O>)`. `field` is the Action field,
// `count` the Durations count (0 is untracked: no probability, and it stays
// 0), `lo`-`hi` the roll's range and `by` the step (2 for Early Bird). A
// count of c means c - 1 steps were taken, so the roll N is uniform on
// [max(lo, c), hi] and the effect ends now if N <= c - 1 + by.
template <typename O>
constexpr void tick(Optional& field, uint8_t& count, bool ended, uint8_t lo,
                    uint8_t hi, uint8_t by, O& options) {
  static_assert(detail::CHANCE<O>);
  record(field, optional(ended));
  const uint8_t c = count;
  if (c == 0) return;
  const unsigned min = c > lo ? c : lo;
  assert(min <= hi);
  const unsigned last = unsigned(c - 1 + by) < hi ? unsigned(c - 1 + by) : hi;
  const unsigned ends = last >= min ? last - min + 1 : 0;
  const unsigned total = hi - min + 1;
  observe(options, ended ? ends : total - ends, total);
  count = ended ? 0 : uint8_t(c + by);
}

} // namespace rolls

namespace detail {

// ---------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------

// The speed GetWhoStrikesFirst compares (main:4595), before Quick Claw:
// Swift Swim and Chlorophyll in effective weather, the stage, Macho Brace,
// paralysis. The badge boost is off in link battles.
constexpr uint32_t speed(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  const ActivePokemon& active = side.active;
  const Weather weather = effective_weather(battle);
  const uint32_t weather_mul =
      (active.ability == Ability::SwiftSwim && weather == Weather::Rain) ||
              (active.ability == Ability::Chlorophyll &&
               weather == Weather::Sun)
          ? 2
          : 1;
  const Fraction& stage = STAT_STAGE_RATIOS[unsigned(active.boosts.spe + 6) % 13];
  uint32_t spe = active.stats.spe * weather_mul * stage.num / stage.den;
  if (holds(side, HoldEffect::MachoBrace)) spe /= 2;
  if (side.get(1).status.paralyzed()) spe /= 4;
  return spe;
}

// What GetWhoStrikesFirst reads, taken at one moment so a lazy draw later
// sees the same inputs.
struct Speeds {
  uint32_t speed[2];
  int8_t priority[2];
  bool quick_claw[2];
};

// `moves` are the players' chosen moves (Move::None for a switch, so
// priority 0). Quick Claw applies unless `quick_claw` is false (two
// switches: Showdown sorts them by speed alone).
constexpr Speeds speeds(const Battle& battle, Move m1, Move m2,
                        bool quick_claw) {
  Speeds s{};
  const Move moves[2] = {m1, m2};
  for (const Player p : {Player::P1, Player::P2}) {
    const uint8_t i = index(p);
    s.speed[i] = speed(battle, p);
    s.priority[i] = priority(moves[i]);
    s.quick_claw[i] =
        quick_claw && holds(battle.side(p), HoldEffect::QuickClaw);
  }
  return s;
}

// GetWhoStrikesFirst (main:4595): priority, then speed with Quick Claw as
// the maximum speed, then a tie draw. The Quick Claw draw is made only if it
// would change the comparison (both holders proccing is a tie, as on the
// cartridge).
template <typename O>
constexpr Player first(Battle& battle, const Speeds& s, bool residual,
                       O& options) {
  if (s.priority[0] != s.priority[1]) {
    return s.priority[0] > s.priority[1] ? Player::P1 : Player::P2;
  }
  uint32_t a = s.speed[0];
  uint32_t b = s.speed[1];
  if (s.quick_claw[0] || s.quick_claw[1]) {
    const auto cmp = [](uint32_t x, uint32_t y) { return (x > y) - (x < y); };
    const uint32_t qa = s.quick_claw[0] ? UINT32_MAX : a;
    const uint32_t qb = s.quick_claw[1] ? UINT32_MAX : b;
    if (cmp(a, b) != cmp(qa, qb) && rolls::quick_claw(battle, options)) {
      a = qa;
      b = qb;
    }
  }
  if (a != b) return a > b ? Player::P1 : Player::P2;
  return rolls::speed_tie(battle, residual, options);
}

// SetActionsAndBattlersTurnOrder (main:4756): a switch goes before a move;
// two moves go by GetWhoStrikesFirst; two switches by speed, as Showdown
// (decided; the decomp takes them in battler-ID order, see the README).
template <typename O> constexpr void order(Battle& battle, O& options) {
  Request& r = battle.request;
  const bool s1 = r.choice[0].type() == Choice::Switch;
  const bool s2 = r.choice[1].type() == Choice::Switch;
  if (s1 != s2) {
    r.first = s1 ? Player::P1 : Player::P2;
    return;
  }
  r.first = first(battle, speeds(battle, r.move[0], r.move[1], !s1), false,
                  options);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

// The move slots p can select, as a 4-bit mask: CheckMoveLimitations
// (util:1082) with MOVE_LIMITATIONS_ALL. Choice Band ignores
// MOVE_UNAVAILABLE for now (CLAUDE.md, "Open").
constexpr uint8_t usable_moves(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  const ActivePokemon& a = side.active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  const bool choice_band = holds(side, HoldEffect::ChoiceBand);
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const Move m = a.moves[i].id;
    if (m == Move::None || a.moves[i].pp == 0) continue;
    if (m == a.disabled_move) continue;
    if (a.volatiles.torment && m == a.last_move) continue;
    if (a.taunt != 0 && get(m).bp == 0) continue;
    if (f.volatiles.imprisoned_others) {
      // GetImprisonedMovesCount: the foe has Imprisoned and knows the move.
      bool imprisoned = false;
      for (const MoveSlot& fm : f.moves) imprisoned |= fm.id == m;
      if (imprisoned) continue;
    }
    if (a.encore_timer != 0 && a.encored_move != m) continue;
    if (choice_band && a.choice_move != Move::None && a.choice_move != m) {
      continue;
    }
    mask |= uint8_t(1 << i);
  }
  return mask;
}

// Whether p's active Pokémon can't switch out (main:4232-4247): binding,
// Mean Look and the like, Ingrain, the foe's Shadow Tag, Arena Trap (not on
// Flying types or Levitate) or Magnet Pull (Steel types). Shadow Tag traps
// Shadow Tag here, as on the cartridge *(verify against Showdown)*.
constexpr bool trapped(const Battle& battle, Player player) {
  const ActivePokemon& a = battle.side(player).active;
  const Ability foe_ability = battle.side(foe(player)).active.ability;
  if (a.volatiles.trapped() || a.volatiles.rooted) return true;
  if (foe_ability == Ability::ShadowTag) return true;
  if (foe_ability == Ability::ArenaTrap && !has_type(a, Type::Flying) &&
      a.ability != Ability::Levitate) {
    return true;
  }
  return foe_ability == Ability::MagnetPull && has_type(a, Type::Steel);
}

// Records p's choice in the request, resolved as gChosenMoveByBattler
// (main:4129, util:78): a forced move is the locked move, Move(0) is
// otherwise Struggle, and under Encore only the encored slot is offered.
constexpr void select(Battle& battle, Player player, Choice choice) {
  Request& r = battle.request;
  const ActivePokemon& a = battle.side(player).active;
  const uint8_t i = index(player);
  r.choice[i] = choice;
  if (choice.type() == Choice::Switch) {
    r.move[i] = Move::None;
  } else if (forced(a)) {
    assert(choice.data() == 0);
    r.move[i] = a.locked_move;
  } else if (choice.data() == 0) {
    r.move[i] = Move::Struggle;
  } else {
    assert(a.encored_move == Move::None ||
           choice.data() - 1 == a.encored_slot);
    r.move[i] = a.moves[choice.data() - 1].id;
  }
}

// ---------------------------------------------------------------------------
// Switching
// ---------------------------------------------------------------------------

// TurnValuesCleanUp(FALSE) (main:4866), minus the per-turn struct, which
// isn't in Battle (README decision 2). It runs before turn 1 too (main:3909).
constexpr void turn_cleanup(Battle& battle) {
  for (Side& side : battle.sides) {
    ActivePokemon& a = side.active;
    if (a.first_turn != 0) a.first_turn--;
    if (a.recharge_timer != 0 && --a.recharge_timer == 0) {
      a.volatiles.recharge = false;
    }
    if (a.substitute_hp == 0) a.volatiles.substitute = false;
  }
}

// p's active Pokémon has just reached 0 HP: cleareffectsonfaint (bsc), which
// clears its status, then FaintClearSetData (main:3264). The Pokémon stays
// in the active slot until its replacement switches in, so it keeps its
// species, stats, moves and ability; its stages, volatiles, disable struct
// and last-move fields are cleared, its types revert to its species', and
// the foe loses the state this Pokémon was the source of. The decomp leaves
// gLockedMoves and gBideDmg, and so does this.
template <typename O>
constexpr void faint(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  assert(mon.fainted());
  mon.status = Status::none();

  ActivePokemon& a = side.active;
  const ActivePokemon kept = a;
  a = ActivePokemon{};
  a.bide_damage = kept.bide_damage;
  a.stats = kept.stats;
  for (uint8_t i = 0; i < 4; i++) a.moves[i] = kept.moves[i];
  a.species = kept.species;
  a.locked_move = kept.locked_move;
  a.types[0] = get(a.species).types[0];
  a.types[1] = get(a.species).types[1];
  a.ability = kept.ability;
  a.first_turn = 2;

  ActivePokemon& f = battle.side(foe(player)).active;
  f.volatiles.escape_prevention = false;
  f.volatiles.infatuated = false;
  f.volatiles.wrapped = 0;
  f.wrapped_move = Move::None;
  f.last_taken_move = Move::None;

  if constexpr (CHANCE<O>) {
    Durations::Side& d = options.chance.durations.get(player);
    d.sleep[0] = 0;
    d.confusion = 0;
    d.disable = 0;
    d.encore = 0;
    d.binding = 0;
    d.attacking = 0;
    options.chance.durations.get(foe(player)).binding = 0;
  }
}

// Spikes on a switch-in (Cmd_switchineffects, bsc:5180): max HP /
// ((5 - layers) * 2), at least 1, so 1/8, 1/6 or 1/4. Flying types and
// Levitate are immune. Returns whether the Pokémon fainted, in which case
// tryfaintmon_spikes (bs1:3394) skips its switch-in abilities and items.
template <typename O>
constexpr bool spikes(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  const uint8_t layers = side.conditions.spikes;
  if (layers == 0 || has_type(a, Type::Flying) ||
      a.ability == Ability::Levitate) {
    return false;
  }
  assert(layers <= 3);
  Pokemon& mon = side.get(1);
  a.volatiles.destiny_bond = false;
  uint16_t damage = uint16_t(mon.stats.hp / ((5 - layers) * 2));
  if (damage == 0) damage = 1;
  mon.hp = mon.hp > damage ? uint16_t(mon.hp - damage) : 0;
  if (!mon.fainted()) return false;
  faint(battle, player, options);
  return true;
}

// The Pokémon in SLOT 1 becomes active: Cmd_switchindataupdate (bsc:4621)
// and SwitchInClearSetData (main:3152), as README decision 4. Then
// switchineffects (bsc:5180).
template <typename O>
constexpr void switch_in(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  assert(mon.species != Species::None && !mon.fainted());

  ActivePokemon& a = side.active;
  a = ActivePokemon{};
  a.stats = mon.stats;
  for (uint8_t i = 0; i < 4; i++) a.moves[i] = mon.moves[i];
  a.species = mon.species;
  a.types[0] = mon.types[0];
  a.types[1] = mon.types[1];
  a.ability = mon.ability;
  a.first_turn = 2; // main:3220, decision 14

  if (spikes(battle, player, options)) return;
  if (a.ability == Ability::Truant) a.truant = true; // bsc:5258
  // TODO(plan step 4): the switch-in abilities and items
  // (ABILITYEFFECT_ON_SWITCHIN, ITEMEFFECT_ON_SWITCH_IN).
}

// p's active Pokémon leaves for SLOT `slot`: a switch action or a faint
// replacement. Switch-out clears the foe's state that p's Pokémon was the
// source of (SwitchInClearSetData and FaintClearSetData, main:3158-3199;
// README decision 9). Baton Pass will need its own version.
template <typename O>
constexpr void switch_to(Battle& battle, Player player, uint8_t slot,
                         O& options) {
  Side& side = battle.side(player);
  assert(slot >= 2 && slot <= 6);
  assert(side.get(slot).species != Species::None && !side.get(slot).fainted());

  // TODO(plan step 4): Pursuit on the switching target (bs1:3088), and
  // switchoutabilities (Natural Cure).
  ActivePokemon& f = battle.side(foe(player)).active;
  f.volatiles.infatuated = false;
  f.volatiles.wrapped = 0;
  f.wrapped_move = Move::None;
  f.volatiles.escape_prevention = false;
  f.volatiles.lock_on = 0;

  std::swap(side.order[0], side.order[slot - 1]);
  if constexpr (CHANCE<O>) options.chance.durations.switched(player, slot);
  switch_in(battle, player, options);
}

// ---------------------------------------------------------------------------
// Faints and the end of the battle
// ---------------------------------------------------------------------------

constexpr bool lost(const Side& side) {
  for (const Pokemon& p : side.pokemon) {
    if (p.species != Species::None && !p.fainted()) return false;
  }
  return true;
}

// Cmd_checkteamslost (bsc:3534): both sides out is a draw (B_OUTCOME_DREW
// is WON | LOST), whoever fainted first, Explosion included. Showdown agrees
// for Gen III (CLAUDE.md, plan step 3).
constexpr Flow outcome(const Battle& battle) {
  const bool l1 = lost(battle.sides[0]);
  const bool l2 = lost(battle.sides[1]);
  if (!l1 && !l2) return std::nullopt;
  const Result::Type type = l1 && l2 ? Result::Tie
                            : l1     ? Result::Lose
                                     : Result::Win;
  return Result::init(type, Choice::Pass, Choice::Pass);
}

// HandleFaintedMonActions (util:1880) at the point `phase` names: the end of
// the battle, or a replacement request for each side whose active Pokémon
// fainted. Fainting itself (faint()) happened where the HP reached 0.
constexpr Flow faints(Battle& battle, Phase phase) {
  if (Flow over = outcome(battle)) return over;
  const bool f1 = battle.sides[0].get(1).fainted();
  const bool f2 = battle.sides[1].get(1).fainted();
  if (!f1 && !f2) return std::nullopt;
  battle.request.phase = phase;
  battle.request.next = 0;
  return Result::init(Result::None, f1 ? Choice::Switch : Choice::Pass,
                      f2 ? Choice::Switch : Choice::Pass);
}

// The replacements a faint request asked for, P1 first: HandleFaintedMonActions
// goes in battler-ID order *(verify against Showdown)*.
template <typename O>
constexpr void replace(Battle& battle, Choice c1, Choice c2, O& options) {
  const Choice choices[2] = {c1, c2};
  for (const Player p : {Player::P1, Player::P2}) {
    const Choice c = choices[index(p)];
    assert((c.type() == Choice::Switch) == battle.side(p).get(1).fainted());
    if (c.type() == Choice::Switch) switch_to(battle, p, c.data(), options);
  }
  battle.request.phase = Phase::None;
}

// ---------------------------------------------------------------------------
// The stages
// ---------------------------------------------------------------------------

// Turn 0, as libpkmn's start: both leads come in and the battle moves to
// turn 1.
//
// Then TryDoEventsBeforeFirstTurn (main:3841): it orders the battlers by
// GetWhoStrikesFirst ignoring chosen moves (a speed tie draws), then runs the
// switch-in weather (none in link battles), the switch-in abilities in that
// order, Intimidate, Trace and the switch-in items in that order, then
// TurnValuesCleanUp and the turn's Quick Claw draw. Only the abilities and
// items depend on the order, so the tie is drawn with them, in step 4.
template <typename O> constexpr Result start(Battle& battle, O& options) {
  switch_in(battle, Player::P1, options);
  switch_in(battle, Player::P2, options);
  // TODO(plan step 4): the switch-in abilities and items, in speed order.
  turn_cleanup(battle);
  battle.turn = 1;
  return Result::start();
}

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------

// The decomp's MOVE_UNAVAILABLE (0xFFFF), which gLastMoves and
// gLastLandedMoves record for a move that was stopped. TODO: a sentinel in
// the generator (CLAUDE.md, "Open"); until then None stands in, which only
// matters to Encore, Disable, Mimic and the like (plan step 4).
inline constexpr Move UNAVAILABLE = Move::None;

// What one move execution tracks: the decomp's per-action globals
// (gCurrentMove, gCurrMovePos, gMoveResultFlags, gHpDealt) and the
// gHitMarker bits that this code reads. On the stack, as libpkmn gen2's
// State; HandleAction_ActionFinished (util:661) clears the hit markers
// between actions.
struct MoveState {
  Move move;         // gCurrentMove, which is also gChosenMove here
  uint8_t slot;      // gCurrMovePos, 0-3, or NO_SLOT
  uint8_t result;    // gMoveResultFlags (MoveResult)
  uint16_t hp_dealt; // gHpDealt: HP the target or its Substitute lost
  bool self;         // the target is the user (MOVE_TARGET_USER)
  bool no_pp;        // HITMARKER_NO_PPDEDUCT (Struggle)
  bool obeys;        // HITMARKER_OBEYS: passed the attack canceler
  bool attackstring; // HITMARKER_ATTACKSTRING_PRINTED
  bool unable;       // HITMARKER_UNABLE_TO_USE_MOVE
  bool damaged;      // TARGET_TURN_DAMAGED: the target's own HP took damage
  bool first;        // the target acts after this, so a flinch can stop it
  bool ignore_on_air;      // HITMARKER_IGNORE_ON_AIR (Thunder, Sky Uppercut)
  bool ignore_underground; // HITMARKER_IGNORE_UNDERGROUND (Earthquake)

  static constexpr uint8_t NO_SLOT = 4;
};

constexpr bool physical(Type type) { return type < Type::Mystery; }

// AtkCanceler_UnableToUseMove (util:1987), then the rest of
// Cmd_attackcanceler (bsc:915). Returns whether the move goes ahead; if not,
// the caller goes to moveend, as every canceling script does. The cases run
// in the decomp's order, and the first that stops the move ends the check.
template <typename O>
constexpr bool cancel(Battle& battle, Player player, MoveState& m,
                      O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  Volatiles& v = a.volatiles;
  // A faint ends the actions, so the attacker is never fainted here.
  assert(!mon.fainted());

  // CANCELER_FLAGS
  v.destiny_bond = false;
  v.grudge = false;

  // CANCELER_ASLEEP. Uproar wakes it outright (UproarWakeUpCheck, bsc:6801),
  // unless it has Soundproof; that's public, so Durations just resets.
  // Otherwise the counter ticks, and it still can't move unless it woke up
  // or uses Snore or Sleep Talk.
  if (mon.status.asleep()) {
    const bool uproar = (battle.sides[0].active.volatiles.uproar != 0 ||
                         battle.sides[1].active.volatiles.uproar != 0) &&
                        a.ability != Ability::Soundproof;
    if (uproar) {
      mon.status = Status::none();
      v.nightmare = false;
      if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 0;
    } else {
      const uint8_t by = a.ability == Ability::EarlyBird ? 2 : 1;
      const bool rest = mon.status.self_inflicted();
      const bool woke = !mon.status.tick_sleep(by);
      if constexpr (CHANCE<O>) {
        // Rest's counter is always 3 (Status::REST_SLEEP).
        rolls::tick(options.chance.actions.get(player).sleep,
                    options.chance.durations.get(player).sleep[0], woke,
                    rest ? Status::REST_SLEEP : Status::MIN_SLEEP,
                    rest ? Status::REST_SLEEP : Status::MAX_SLEEP, by,
                    options);
      }
      if (woke) {
        v.nightmare = false;
      } else if (m.move != Move::Snore && m.move != Move::SleepTalk) {
        m.unable = true;
        return false;
      }
    }
  }

  // CANCELER_FROZEN: it thaws with 1/5. A thawing move (EFFECT_THAW_HIT)
  // that stays frozen goes on, and CANCELER_THAW thaws it below if nothing
  // else stops the move first. A frozen Pokémon that can't move prints no
  // attack string (HITMARKER_NO_ATTACKSTRING).
  const bool thaw_hit = get(m.move).effect == Effect::ThawHit;
  if (mon.status.frozen()) {
    if (rolls::thaw(battle, player, options)) {
      mon.status = Status::none();
    } else if (!thaw_hit) {
      return false;
    }
  }

  // CANCELER_TRUANT. The counter toggles at the end of each turn
  // (ABILITYEFFECT_ENDTURN, util:2640). TODO(plan step 4): that toggle.
  if (a.ability == Ability::Truant && a.truant) {
    a.cancel_multi_turn_moves();
    m.unable = true;
    m.result |= MoveResult::MISSED;
    return false;
  }

  // CANCELER_RECHARGE
  if (v.recharge) {
    v.recharge = false;
    a.recharge_timer = 0;
    a.cancel_multi_turn_moves();
    m.unable = true;
    return false;
  }

  // CANCELER_FLINCH
  if (v.flinched) {
    v.flinched = false;
    a.cancel_multi_turn_moves();
    m.unable = true;
    return false;
  }

  // CANCELER_DISABLED
  if (a.disabled_move != Move::None && a.disabled_move == m.move) {
    a.cancel_multi_turn_moves();
    m.unable = true;
    return false;
  }

  // CANCELER_TAUNTED
  if (a.taunt != 0 && get(m.move).bp == 0) {
    a.cancel_multi_turn_moves();
    m.unable = true;
    return false;
  }

  // CANCELER_IMPRISONED: GetImprisonedMovesCount, the foe Imprisoned and
  // knows the move.
  if (f.volatiles.imprisoned_others) {
    bool imprisoned = false;
    for (const MoveSlot& fm : f.moves) imprisoned |= fm.id == m.move;
    if (imprisoned) {
      a.cancel_multi_turn_moves();
      m.unable = true;
      return false;
    }
  }

  // CANCELER_CONFUSED. The hidden counter ticks; at 0 the Pokémon snaps out
  // and moves this turn. Otherwise it hits itself with 1/2, which replaces
  // the move: BattleScript_MoveUsedIsConfused (bs1:3796) with the target set
  // to the user (util:2158, so moveend's target cases see the user), then
  // tryfaintmon and moveend. The damage is Showdown's (decided 2026-10-07):
  // Hit::confusion(), the roll, then Focus Band, which Showdown applies
  // behind a Substitute too (the decomp doesn't, bsc:1725). The self-hit
  // ignores the Substitute (HITMARKER_IGNORE_SUBSTITUTE) and counts toward
  // Bide (datahpupdate, bsc:1905) but not toward Counter
  // (HITMARKER_PASSIVE_HP_UPDATE). Its draws go in hits[0] (chance.hpp).
  if (v.confusion != 0) {
    v.confusion--;
    const bool ended = v.confusion == 0;
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).confusion_end,
                  options.chance.durations.get(player).confusion, ended, 2, 5,
                  1, options);
    }
    if (!ended && rolls::confusion(battle, player, options)) {
      m.self = true;
      m.unable = true;
      a.cancel_multi_turn_moves();
      uint32_t d = apply_roll(damage(battle, player, player, Hit::confusion()),
                              rolls::damage(battle, player, 0, options));
      const ItemData item = item_data(mon.item);
      if (mon.hp <= d && item.effect == HoldEffect::FocusBand &&
          rolls::focus_band(battle, player, 0, item.param, options)) {
        d = mon.hp - 1u;
      }
      a.bide_damage =
          a.bide_damage > UINT32_MAX - d ? UINT32_MAX : a.bide_damage + d;
      mon.hp = mon.hp > d ? uint16_t(mon.hp - d) : 0;
      if (mon.fainted()) faint(battle, player, options);
      return false;
    }
  }

  // CANCELER_PARALYZED. The decomp's own CancelMultiTurnMoves is commented
  // out, but the script it jumps to runs cancelmultiturnmoves (bs1:3773).
  if (mon.status.paralyzed() && rolls::paralysis(battle, player, options)) {
    a.cancel_multi_turn_moves();
    m.unable = true;
    return false;
  }

  // CANCELER_IN_LOVE
  if (v.infatuated && rolls::infatuation(battle, player, options)) {
    a.cancel_multi_turn_moves();
    m.unable = true;
    return false;
  }

  // CANCELER_BIDE. TODO(plan step 4). Nothing in step 3 starts Bide.
  assert(v.bide == 0 && "Bide isn't implemented");

  // CANCELER_THAW
  if (mon.status.frozen()) {
    assert(thaw_hit);
    mon.status = Status::none();
  }

  // TODO(plan step 4): ABILITYEFFECT_MOVES_BLOCK (Soundproof).

  // No PP left (BattleScript_NoPPForMove, bs1:3556): only if the PP went
  // to 0 after selection, by Spite or Pressure from a faster foe.
  if (m.slot != MoveState::NO_SLOT && a.moves[m.slot].pp == 0 &&
      m.move != Move::Struggle && !v.multiple_turns) {
    m.attackstring = true;
    m.result |= MoveResult::MISSED;
    return false;
  }

  m.obeys = true;
  // TODO(plan step 4): Magic Coat, Snatch and Protect, which need the
  // per-turn ProtectStruct (README decision 2).
  return true;
}

// Showdown's arithmetic on JavaScript numbers, which are doubles; C++'s
// IEEE doubles give the same results for the same operations. `trunc` is
// Dex.trunc (`num >>> 0`, sim/dex.ts:365) for the small positive values
// here.
constexpr double js_trunc(double x) { return double(uint32_t(x)); }

// Battle.modify (sim/battle.ts:2336).
constexpr double js_modify(double value, double modifier) {
  const double m = js_trunc(modifier * 4096);
  return js_trunc((js_trunc(value * m) + 2048 - 1) / 4096);
}

// Battle.chainModify (sim/battle.ts:2325): folds `next` into `modifier`.
constexpr double js_chain(double modifier, double next) {
  const uint32_t prev = uint32_t(modifier * 4096);
  const uint32_t n = uint32_t(next * 4096);
  return double((prev * n + 2048) >> 12) / 4096;
}

// boostTable in Showdown's gen3 tryMoveHit (data/mods/gen3/scripts.ts:319).
inline constexpr double ACCURACY_BOOSTS[7] = {1,         4.0 / 3, 5.0 / 3, 2,
                                              7.0 / 3,   8.0 / 3, 3};

// The accuracy check without its draw: 0 for a certain miss, 100 or more
// for a certain hit, otherwise the number of the 100 outcomes that hit.
// The certain cases are the decomp's (Cmd_accuracycheck, bsc:1099, and
// AccuracyCalcHelper, bsc:1054); the arithmetic is Showdown's (decided,
// 2026-10-07), from its gen3 tryMoveHit (data/mods/gen3/scripts.ts:319-363):
// - The accuracy stage, then the evasion stage, each as an exact fraction in
//   floating point. The decomp combines them into one clamped stage with
//   integer ratios (sAccuracyStageRatios, bsc:588).
// - Foresight ignores only a positive evasion stage (its onModifyBoost,
//   data/moves.ts:6114); the decomp ignores evasion entirely.
// - ModifyAccuracy in priority order (data/mods/gen4/abilities.ts,
//   items.ts): Compound Eyes ×1.3, Sand Veil ×0.8 and Hustle ×3277/4096 are
//   chained; BrightPowder (×0.9) and Lax Incense (×0.95) multiply the value
//   directly. runEvent applies the chained modifier only if the value is
//   then a whole number (sim/battle.ts:933), so after a stage or a
//   BrightPowder that leaves a fraction the chained modifiers are dropped.
// - randomChance(acc, 100) is `random(100) < acc` (sim/prng.ts:115), so a
//   fractional accuracy hits on ceil(acc) outcomes of 100.
constexpr uint32_t accuracy(const Battle& battle, Player player,
                            const MoveState& m) {
  const Side& side = battle.side(player);
  const ActivePokemon& a = side.active;
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& f = foe_side.active;
  const MoveData& data = get(m.move);
  const Weather weather = effective_weather(battle);

  // Lock-On and Mind Reader: the only possible locker in singles is the
  // attacker.
  if (f.volatiles.lock_on != 0) return 100;
  // Semi-invulnerable targets, unless the move reaches them: Surf hits Dive
  // (HITMARKER_IGNORE_UNDERWATER, bs1:239), and the moves that set
  // ignore_on_air or ignore_underground. TODO(plan step 4): Gust, Twister
  // and Magnitude.
  if (f.volatiles.on_air && !m.ignore_on_air) return 0;
  if (f.volatiles.underground && !m.ignore_underground) return 0;
  if (f.volatiles.underwater && m.move != Move::Surf) return 0;
  if ((weather == Weather::Rain && data.effect == Effect::Thunder) ||
      data.effect == Effect::AlwaysHit || data.effect == Effect::VitalThrow) {
    return 100;
  }

  double acc = data.accuracy;
  if (weather == Weather::Sun && data.effect == Effect::Thunder) acc = 50;
  const int8_t boost = a.boosts.acc;
  if (boost > 0) {
    acc *= ACCURACY_BOOSTS[boost];
  } else {
    acc /= ACCURACY_BOOSTS[-boost];
  }
  int8_t evasion = f.boosts.eva;
  if (f.volatiles.foresight && evasion > 0) evasion = 0;
  if (evasion > 0) {
    acc /= ACCURACY_BOOSTS[evasion];
  } else if (evasion < 0) {
    acc *= ACCURACY_BOOSTS[-evasion];
  }

  double modifier = 1;
  if (a.ability == Ability::CompoundEyes) modifier = js_chain(modifier, 1.3);
  if (f.ability == Ability::SandVeil && weather == Weather::Sandstorm) {
    modifier = js_chain(modifier, 0.8);
  }
  if (a.ability == Ability::Hustle && physical(data.type)) {
    modifier = js_chain(modifier, 3277.0 / 4096);
  }
  const ItemData item = item_data(foe_side.get(1).item);
  if (item.effect == HoldEffect::EvasionUp) {
    assert(item.param == 10 || item.param == 5);
    acc *= item.param == 10 ? 0.9 : 0.95;
  }
  if (acc == double(uint32_t(acc))) acc = js_modify(acc, modifier);

  // ceil(acc), from below: every integer r in 0-99 with r < acc.
  const uint32_t floor = uint32_t(acc);
  return double(floor) == acc ? floor : floor + 1;
}

// Cmd_ppreduce (bsc:1205): one PP, or two into Pressure, unless the move
// uses none (Struggle). A move that isn't Mimic's or Transform's
// (MOVE_IS_PERMANENT) writes the PP back to the party.
constexpr void pp_reduce(Battle& battle, Player player, const MoveState& m) {
  if (m.no_pp || m.slot == MoveState::NO_SLOT) return;
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  MoveSlot& ms = a.moves[m.slot];
  if (ms.pp == 0) return;
  const uint8_t deduct =
      1 + (!m.self && battle.side(foe(player)).active.ability == Ability::Pressure);
  ms.pp = ms.pp > deduct ? uint8_t(ms.pp - deduct) : 0;
  if (!a.volatiles.transformed && !(a.mimicked & (1 << m.slot))) {
    side.get(1).moves[m.slot].pp = ms.pp;
  }
}

// Cmd_datahpupdate (bsc:1844) on p's target, for a move's damage: a
// Substitute takes it if there is one, otherwise the Pokémon does, and the
// damage counts toward its Bide. Returns gHpDealt.
//
// TODO(plan step 4): the per-turn physical and special damage that Counter
// and Mirror Coat read (ProtectStruct, README decision 2).
constexpr uint16_t hp_update(Battle& battle, Player target, uint32_t damage,
                             MoveState& m) {
  Side& side = battle.side(target);
  ActivePokemon& a = side.active;
  if (a.volatiles.substitute && a.substitute_hp != 0) {
    const uint8_t dealt =
        a.substitute_hp >= damage ? uint8_t(damage) : a.substitute_hp;
    a.substitute_hp = uint8_t(a.substitute_hp - dealt);
    return dealt;
  }
  a.bide_damage = a.bide_damage > UINT32_MAX - damage ? UINT32_MAX
                                                      : a.bide_damage + damage;
  Pokemon& mon = side.get(1);
  const uint16_t dealt = mon.hp > damage ? uint16_t(damage) : mon.hp;
  mon.hp = uint16_t(mon.hp - dealt);
  m.damaged = true;
  return dealt;
}

// Cmd_moveend (bsc:4210) with moveendall: the cases in the decomp's order.
template <typename O>
constexpr void moveend(Battle& battle, Player player, const MoveState& m,
                       O&) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Side& target_side = m.self ? side : battle.side(foe(player));
  ActivePokemon& t = target_side.active;
  const Pokemon& target = target_side.get(1);
  const MoveData& data = get(m.move);
  const bool no_effect = m.result & MoveResult::NO_EFFECT;
  // TODO(plan step 4): the dynamic type (Hidden Power, Weather Ball).
  const Type type = data.type;

  // MOVEEND_RAGE
  if (!m.self && t.volatiles.rage && !target.fainted() && !no_effect &&
      m.damaged && data.bp != 0 && t.boosts.atk < 6) {
    t.boosts.atk++;
  }

  // MOVEEND_DEFROST: a Fire move's special damage thaws the target. Hidden
  // Power's damage is recorded under its base type, Normal, so Hidden Power
  // Fire doesn't thaw (datahpupdate, bsc:1852).
  if (!m.self && target.status.frozen() && !target.fainted() && m.damaged &&
      !physical(type) && !no_effect && type == Type::Fire &&
      data.effect != Effect::HiddenPower) {
    target_side.get(1).status = Status::none();
  }

  // TODO(plan step 4): MOVEEND_SYNCHRONIZE_TARGET, ON_DAMAGE_ABILITIES,
  // IMMUNITY_ABILITIES and SYNCHRONIZE_ATTACKER (AbilityBattleEffects).

  // MOVEEND_CHOICE_MOVE. A Baton Pass that worked doesn't lock, and skips
  // the check that the locked move is still known.
  bool skip = false;
  if (m.obeys && holds(side, HoldEffect::ChoiceBand) &&
      m.move != Move::Struggle && a.choice_move == Move::None) {
    skip = m.move == Move::BatonPass && !(m.result & MoveResult::FAILED);
    if (!skip) a.choice_move = m.move;
  }
  if (!skip) {
    bool known = false;
    for (const MoveSlot& ms : a.moves) known |= ms.id == a.choice_move;
    if (!known) a.choice_move = Move::None;
  }

  // TODO(plan step 4): MOVEEND_CHANGED_ITEMS (Trick, Thief, Covet).

  // MOVEEND_ATTACKER_VISIBLE: a move that failed or was stopped ends the
  // attacker's semi-invulnerable turn.
  if (no_effect || m.unable) {
    a.volatiles.on_air = false;
    a.volatiles.underground = false;
    a.volatiles.underwater = false;
  }

  // TODO(plan step 4): MOVEEND_ITEM_EFFECTS_ALL and KINGSROCK_SHELLBELL
  // (ItemBattleEffects).

  // MOVEEND_SUBSTITUTE
  for (Side& s : battle.sides) {
    if (s.active.substitute_hp == 0) s.active.volatiles.substitute = false;
  }

  // MOVEEND_UPDATE_LAST_MOVES
  if (m.attackstring) a.last_printed_move = m.move;
  if (data.effect != Effect::BatonPass) {
    a.last_move = m.obeys ? m.move : UNAVAILABLE;
    a.last_resulting_move = m.obeys ? m.move : UNAVAILABLE;
    if (m.obeys && !no_effect) {
      t.last_landed_move = m.move;
      t.last_hit_type = type;
    } else {
      t.last_landed_move = UNAVAILABLE;
    }
  }

  // MOVEEND_MIRROR_MOVE
  if ((data.flags & Flag::MirrorMoveAffected) && m.obeys && !m.self &&
      !target.fainted() && !no_effect) {
    t.last_taken_move = m.move;
  }
}

// The effects whose script is BattleScript_EffectHit, maybe after setting a
// move effect or a hit marker: the plain hits (Quick Attack and Vital Throw
// differ only in the table; Pursuit only when the target switches,
// TODO(plan step 4)), Effect::Recoil (MOVE_EFFECT_RECOIL_25, bs1:897), the
// status and flinch secondaries (secondary_of), Hidden Power
// (hiddenpowercalc, bs1:1740), Thunder and Sky Uppercut (IGNORE_ON_AIR) and
// Earthquake (bs1:1834: PP before accuracy, which changes nothing in
// singles, and IGNORE_UNDERGROUND; damage.hpp doubles it into Dig).
// Double-Edge is MOVE_EFFECT_RECOIL_33 (bs1:2567). Explosion (bs1:374) and
// Seismic Toss and Night Shade (Effect::LevelDamage, bs1:1195) are
// variations on the same path, handled in hit(), as are Rapid Spin
// (MOVE_EFFECT_RAPIDSPIN, bs1:1716) and Brick Break (bs1:2418).
constexpr bool plain_hit(Effect effect) {
  switch (effect) {
  case Effect::Hit:
  case Effect::AlwaysHit:
  case Effect::HighCritical:
  case Effect::VitalThrow:
  case Effect::FalseSwipe:
  case Effect::QuickAttack:
  case Effect::Pursuit:
  case Effect::Recoil:
  case Effect::DoubleEdge:
  case Effect::Explosion:
  case Effect::LevelDamage:
  case Effect::HiddenPower:
  case Effect::Thunder:
  case Effect::SkyUppercut:
  case Effect::Earthquake:
  case Effect::PoisonHit:
  case Effect::PoisonTail:
  case Effect::PoisonFang:
  case Effect::BurnHit:
  case Effect::BlazeKick:
  case Effect::ThawHit:
  case Effect::FreezeHit:
  case Effect::ParalyzeHit:
  case Effect::FlinchHit:
  case Effect::ConfuseHit:
  case Effect::TriAttack:
  case Effect::AttackDownHit:
  case Effect::DefenseDownHit:
  case Effect::SpeedDownHit:
  case Effect::SpecialAttackDownHit:
  case Effect::SpecialDefenseDownHit:
  case Effect::AccuracyDownHit:
  case Effect::AttackUpHit:
  case Effect::DefenseUpHit:
  case Effect::RapidSpin:
  case Effect::BrickBreak:
    return true;
  default:
    return false;
  }
}

// MOVE_EFFECT_* bytes this code applies, in the decomp's numbering order.
enum class Secondary : uint8_t {
  None, Poison, Burn, Freeze, Paralysis, Toxic, Confusion, Flinch, TriAttack,
  // MOVE_EFFECT_*_MINUS_1 on the target, in Boosts order (atk-acc), then
  // MOVE_EFFECT_ATK_PLUS_1 and DEF_PLUS_1 with MOVE_EFFECT_AFFECTS_USER.
  AtkDown, DefDown, SpeDown, SpaDown, SpdDown, AccDown, AtkUp, DefUp,
};

// The stage a stat change moves: 0 Atk, 1 Def, 2 Spe, 3 SpA, 4 SpD, 5 Acc.
constexpr int8_t& stage(Boosts& b, uint8_t stat) {
  switch (stat) {
  case 0: return b.atk;
  case 1: return b.def;
  case 2: return b.spe;
  case 3: return b.spa;
  case 4: return b.spd;
  default: assert(stat == 5); return b.acc;
  }
}

// setmoveeffect in each effect's script (bs1). Blaze Kick and Poison Tail
// share Burn Hit's and Poison Hit's scripts; Thunder sets PARALYSIS.
constexpr Secondary secondary_of(Effect effect) {
  switch (effect) {
  case Effect::PoisonHit:
  case Effect::PoisonTail: return Secondary::Poison;
  case Effect::PoisonFang: return Secondary::Toxic;
  case Effect::BurnHit:
  case Effect::BlazeKick:
  case Effect::ThawHit: return Secondary::Burn;
  case Effect::FreezeHit: return Secondary::Freeze;
  case Effect::ParalyzeHit:
  case Effect::Thunder: return Secondary::Paralysis;
  case Effect::ConfuseHit: return Secondary::Confusion;
  case Effect::FlinchHit: return Secondary::Flinch;
  case Effect::TriAttack: return Secondary::TriAttack;
  case Effect::AttackDownHit: return Secondary::AtkDown;
  case Effect::DefenseDownHit: return Secondary::DefDown;
  case Effect::SpeedDownHit: return Secondary::SpeDown;
  case Effect::SpecialAttackDownHit: return Secondary::SpaDown;
  case Effect::SpecialDefenseDownHit: return Secondary::SpdDown;
  case Effect::AccuracyDownHit: return Secondary::AccDown;
  case Effect::AttackUpHit: return Secondary::AtkUp;
  case Effect::DefenseUpHit: return Secondary::DefUp;
  default: return Secondary::None;
  }
}

// Whether a major status `s` would take hold on p's foe now: SetMoveEffect's
// checks for a secondary (not primary, not certain; bsc:2408-2560) after
// its preamble. Safeguard, then each status's type and ability immunities,
// and an existing status. Freeze Clause (Smogon, always on, as Showdown's
// freezeclausemod: no freeze while a Pokémon on that side is frozen) and
// sun stop a freeze.
constexpr bool status_takes(const Battle& battle, Player player, Secondary s) {
  const Side& side = battle.side(foe(player));
  const ActivePokemon& t = side.active;
  if (side.conditions.safeguard != 0) return false;
  if (side.get(1).status.any()) return false;
  switch (s) {
  case Secondary::Poison:
  case Secondary::Toxic:
    return !has_type(t, Type::Poison) && !has_type(t, Type::Steel) &&
           t.ability != Ability::Immunity;
  case Secondary::Burn:
    return !has_type(t, Type::Fire) && t.ability != Ability::WaterVeil;
  case Secondary::Freeze: {
    if (has_type(t, Type::Ice) || t.ability == Ability::MagmaArmor) return false;
    if (effective_weather(battle) == Weather::Sun) return false;
    for (const Pokemon& p : side.pokemon) {
      if (p.status.frozen()) return false;
    }
    return true;
  }
  case Secondary::Paralysis:
    return t.ability != Ability::Limber;
  default:
    assert(false && "not a major status");
    return false;
  }
}

// Sets a major status on p's foe, as SetMoveEffect does once statusChanged
// (bsc:2565): freezing also cancels its multi-turn move. Changing the status
// restarts the toxic counter (data.hpp, Status). TODO(plan step 4):
// Synchronize (HITMARKER_SYNCHRONIZE_EFFECT).
constexpr void set_status(Battle& battle, Player player, Secondary s) {
  Side& side = battle.side(foe(player));
  Status& status = side.get(1).status;
  switch (s) {
  case Secondary::Poison: status = Status::psn(); break;
  case Secondary::Toxic: status = Status::tox(); break;
  case Secondary::Burn: status = Status::brn(); break;
  case Secondary::Freeze:
    status = Status::frz();
    side.active.cancel_multi_turn_moves();
    break;
  case Secondary::Paralysis: status = Status::par(); break;
  default: assert(false && "not a major status");
  }
  side.active.toxic = 0;
}

// MOVE_EFFECT_CONFUSION taking hold on `target` (bsc:2541): the counter is
// Random() % 4 + 2, hidden, so it's drawn without Chance (CLAUDE.md,
// "Chance"), and the public count starts at 1.
template <typename O>
constexpr void confuse(Battle& battle, Player target, O& options) {
  ActivePokemon& t = battle.side(target).active;
  assert(t.volatiles.confusion == 0);
  t.volatiles.confusion = uint8_t(2 + battle.rng.range(4));
  if constexpr (CHANCE<O>) options.chance.durations.get(target).confusion = 1;
}

// seteffectwithchance (bsc:2923) and SetMoveEffect (bsc:2380) for a move's
// secondary effect on its target, after the damage. The chance is the
// table's, doubled by Serene Grace; it's drawn only when the effect could
// take hold (or, for Poison Fang, change the result flags), since otherwise
// no outcome changes anything.
template <typename O>
constexpr void secondary(Battle& battle, Player player, MoveState& m,
                         O& options) {
  const MoveData& data = get(m.move);
  const Secondary s = secondary_of(data.effect);
  if (s == Secondary::None || (m.result & MoveResult::NO_EFFECT)) return;
  const uint32_t chance =
      data.chance *
      (battle.side(player).active.ability == Ability::SereneGrace ? 2u : 1u);
  if (chance == 0) return;

  // A stat raise on the user (MOVE_EFFECT_AFFECTS_USER): SetMoveEffect's
  // preamble checks the user, which is alive, and ChangeStatBuffs only fails
  // at +6 (bsc:6937).
  if (s == Secondary::AtkUp || s == Secondary::DefUp) {
    int8_t& b = stage(battle.side(player).active.boosts,
                      s == Secondary::AtkUp ? 0 : 1);
    if (b >= 6) return;
    if (chance < 100 && !rolls::secondary(battle, player, chance, options)) {
      return;
    }
    b++;
    return;
  }

  // SetMoveEffect's preamble: not into a fainted target, a Substitute or
  // Shield Dust (for the statuses, flinch and Tri Attack; ChangeStatBuffs
  // has its own Shield Dust check for secondary stat drops).
  Side& side = battle.side(foe(player));
  ActivePokemon& t = side.active;
  const Pokemon& target = side.get(1);
  if (target.fainted() || t.volatiles.substitute ||
      t.ability == Ability::ShieldDust) {
    return;
  }

  bool matters = false;
  switch (s) {
  case Secondary::AtkDown:
  case Secondary::DefDown:
  case Secondary::SpeDown:
  case Secondary::SpaDown:
  case Secondary::SpdDown:
  case Secondary::AccDown: {
    // ChangeStatBuffs (bsc:6937) for a secondary drop: Mist, Clear Body and
    // White Smoke, Hyper Cutter for Attack, Keen Eye for accuracy, and a
    // stage already at -6 stop it. TODO(plan step 4): Protect's check.
    const uint8_t stat = uint8_t(s) - uint8_t(Secondary::AtkDown);
    matters = side.conditions.mist == 0 &&
              t.ability != Ability::ClearBody &&
              t.ability != Ability::WhiteSmoke &&
              !(t.ability == Ability::HyperCutter && stat == 0) &&
              !(t.ability == Ability::KeenEye && stat == 5) &&
              stage(t.boosts, stat) > -6;
    break;
  }
  case Secondary::Confusion:
    // Safeguard stops it as a secondary (bsc:2257, MOVE_EFFECT_CONFUSION is
    // 7), then Own Tempo or an existing confusion (bsc:2534).
    matters = side.conditions.safeguard == 0 &&
              t.ability != Ability::OwnTempo && t.volatiles.confusion == 0;
    break;
  case Secondary::Flinch:
    // Set only if the target hasn't moved yet this turn (bsc:2685).
    matters = m.first && !t.volatiles.flinched &&
              t.ability != Ability::InnerFocus;
    break;
  case Secondary::TriAttack:
    // Any status at all stops it before the 1/3 draw; Safeguard and the
    // immunities apply to the status it picks.
    matters = !target.status.any();
    break;
  case Secondary::Toxic:
    // Into a Poison or Steel type with no status, it marks the move as not
    // affecting the foe (bsc:2552), which moveend reads.
    matters = status_takes(battle, player, s) ||
              (!target.status.any() && side.conditions.safeguard == 0 &&
               (has_type(t, Type::Poison) || has_type(t, Type::Steel)));
    break;
  default:
    matters = status_takes(battle, player, s);
  }
  if (!matters) return;
  if (chance < 100 && !rolls::secondary(battle, player, chance, options)) {
    return;
  }

  switch (s) {
  case Secondary::AtkDown:
  case Secondary::DefDown:
  case Secondary::SpeDown:
  case Secondary::SpaDown:
  case Secondary::SpdDown:
  case Secondary::AccDown:
    stage(t.boosts, uint8_t(uint8_t(s) - uint8_t(Secondary::AtkDown)))--;
    break;
  case Secondary::Confusion:
    confuse(battle, foe(player), options);
    break;
  case Secondary::Flinch:
    t.volatiles.flinched = true;
    break;
  case Secondary::TriAttack: {
    constexpr Secondary TRI[3] = {Secondary::Burn, Secondary::Freeze,
                                  Secondary::Paralysis};
    const Secondary which = TRI[rolls::tri_attack(battle, player, options) - 1];
    if (status_takes(battle, player, which)) set_status(battle, player, which);
    break;
  }
  case Secondary::Toxic:
    if (status_takes(battle, player, s)) {
      set_status(battle, player, s);
    } else {
      m.result |= MoveResult::DOESNT_AFFECT_FOE;
    }
    break;
  default:
    set_status(battle, player, s);
  }
}

// Every move with one of those effects has a power to hit with.
static_assert([] {
  for (uint16_t i = 1; i < MOVES; i++) {
    const MoveData& data = get(Move(i));
    if (plain_hit(data.effect) && data.bp == 0) return false;
  }
  return true;
}());

// BattleScript_EffectHit (bs1:236), from the attack canceler on.
template <typename O>
constexpr void hit(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  const Effect effect = get(m.move).effect;
  Pokemon& user = battle.side(player).get(1);

  // Explosion (bs1:374): attackstring and ppreduce come first, which
  // changes nothing; tryexplosion fails if any active Pokémon has Damp
  // (BattleScript_DampStopsExplosion goes to moveend); otherwise the user's
  // HP drops to 0 before the hit (setatkhptozero, bsc:6577) and it faints
  // after moveend, whether or not the hit lands.
  if (effect == Effect::Explosion) {
    if (battle.sides[0].active.ability == Ability::Damp ||
        battle.sides[1].active.ability == Ability::Damp) {
      m.attackstring = true;
      pp_reduce(battle, player, m);
      return moveend(battle, player, m, options);
    }
    user.hp = 0;
  }

  Side& side = battle.side(player);
  const Player defender = foe(player);
  Side& foe_side = battle.side(defender);
  Pokemon& target = foe_side.get(1);
  const MoveData& data = get(m.move);
  Hit h = Hit::of(m.move);
  // hiddenpowercalc (bsc:8889): the type is in the move ID, the power on the
  // Pokémon.
  if (data.effect == Effect::HiddenPower) {
    assert(m.move != Move::HiddenPower && side.get(1).hidden_power != 0);
    h.power = side.get(1).hidden_power;
  }

  // typecalc's immunities first, as Showdown (decided; README "Damage"). A
  // move with no effect draws neither accuracy nor crit: the decomp draws
  // them, but no outcome changes anything.
  m.result |= matchup(battle, player, h);
  const bool affects = !(m.result & MoveResult::NO_EFFECT);
  // Brick Break's accuracy check comes before typecalc and its screen
  // breaking after, so it breaks screens even into a Ghost type: its
  // accuracy matters then too. Showdown agrees (gen3 tryMoveHit runs TryHit
  // before it reports the immunity, data/mods/gen3/scripts.ts:367).
  SideConditions& foe_conditions = battle.side(foe(player)).conditions;
  const bool breaks = effect == Effect::BrickBreak &&
                      (foe_conditions.reflect != 0 || foe_conditions.light_screen != 0);
  if (affects || breaks) {
    const uint32_t calc = accuracy(battle, player, m);
    const bool hits =
        calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options));
    if (!hits) {
      // BattleScript_PrintMoveMissed (bs1:273)
      m.result |= MoveResult::MISSED;
      m.attackstring = true;
      pp_reduce(battle, player, m);
      moveend(battle, player, m, options);
      if (effect == Effect::Explosion) faint(battle, player, options);
      return;
    }
  }
  // TODO(plan step 4): what JumpIfMoveFailed does after a hit (bsc:1009):
  // Destiny Bond's setup and the absorbing abilities (Volt Absorb, Water
  // Absorb, Flash Fire).

  m.attackstring = true;
  pp_reduce(battle, player, m);
  // removelightscreenreflect (bsc:9883), before the damage.
  if (breaks) {
    foe_conditions.reflect = 0;
    foe_conditions.light_screen = 0;
  }

  if (affects) {
    // critcalc, damagecalc, typecalc, then adjustnormaldamage: the roll,
    // then whatever lets the target survive at 1 HP (not behind a
    // Substitute). Focus Band is drawn only when the hit would be lethal,
    // and not for False Swipe, which spares the target anyway.
    // Seismic Toss and Night Shade: the user's level, with no crit or roll
    // (dmgtolevel, then adjustsetdamage, bsc:5878, which has the same
    // survival checks).
    uint32_t d = user.level;
    if (effect != Effect::LevelDamage) {
      if (can_crit(battle, player)) {
        h.crit = rolls::crit(battle, player, 0,
                             crit_stage(battle, player, m.move), options);
      }
      d = apply_roll(damage(battle, player, h),
                     rolls::damage(battle, player, 0, options));
    }
    if (!foe_side.active.volatiles.substitute && target.hp <= d) {
      const ItemData item = item_data(target.item);
      bool survives = data.effect == Effect::FalseSwipe;
      if (!survives && item.effect == HoldEffect::FocusBand) {
        survives = rolls::focus_band(battle, player, 0, item.param, options);
      }
      // TODO(plan step 4): Endure (ProtectStruct).
      if (survives) d = target.hp - 1u;
    }
    // datahpupdate
    m.hp_dealt = hp_update(battle, defender, d, m);
  }

  secondary(battle, player, m, options);

  // Rapid Spin's certain MOVE_EFFECT_RAPIDSPIN on the user, if the move
  // affected the target: Cmd_rapidspinfree (bsc:8821) runs once per release, so
  // it frees the user from binding and Leech Seed and clears its side's
  // Spikes, all three.
  if (effect == Effect::RapidSpin && affects && !user.fainted()) {
    ActivePokemon& a = side.active;
    a.volatiles.wrapped = 0;
    a.wrapped_move = Move::None;
    a.volatiles.leech_seed = false;
    side.conditions.spikes = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).binding = 0;
  }

  // seteffectwithchance: the certain recoil of Effect::Recoil
  // (MOVE_EFFECT_RECOIL_25, a quarter of the HP dealt, bsc:2636) and
  // Double-Edge (MOVE_EFFECT_RECOIL_33, a third, bsc:2843), at least 1.
  // Rock Head prevents it except for Struggle (BattleScript_MoveEffectRecoil,
  // bs1:3938). The recoil script faints the user before the target's
  // tryfaintmon below.
  if ((effect == Effect::Recoil || effect == Effect::DoubleEdge) && affects &&
      (m.move == Move::Struggle || side.active.ability != Ability::RockHead)) {
    uint16_t recoil =
        uint16_t(m.hp_dealt / (effect == Effect::DoubleEdge ? 3 : 4));
    if (recoil == 0) recoil = 1;
    user.hp = user.hp > recoil ? uint16_t(user.hp - recoil) : 0;
    if (user.fainted()) faint(battle, player, options);
  }

  // tryfaintmon
  if (target.fainted()) faint(battle, defender, options);
  moveend(battle, player, m, options);
  if (effect == Effect::Explosion) faint(battle, player, options);
}

// BattleScript_EffectConfuse (bs1:903): Confuse Ray, Supersonic and Sweet
// Kiss. After the canceler, attackstring and ppreduce, it stops at Own
// Tempo, a Substitute or an existing confusion, then checks accuracy, then
// Safeguard. On the cartridge Safeguard's script ends with end2 (bs1:3257),
// skipping moveend, so the move isn't recorded as used; Showdown fails the
// move normally, and this follows it.
template <typename O>
constexpr void confuse_move(Battle& battle, Player player, MoveState& m,
                            O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& t = foe_side.active;
  // BattleScript_OwnTempoPrevents and BattleScript_AlreadyConfused set no
  // result flag.
  if (t.ability == Ability::OwnTempo) return moveend(battle, player, m, options);
  if (t.volatiles.substitute) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (t.volatiles.confusion != 0) return moveend(battle, player, m, options);
  const uint32_t calc = accuracy(battle, player, m);
  if (!(calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options)))) {
    m.result |= MoveResult::MISSED | MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (foe_side.conditions.safeguard != 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  confuse(battle, foe(player), options);
  moveend(battle, player, m, options);
}

// HandleAction_UseMove (util:78): which move and slot the action uses, then
// its script. Struggle uses no PP. A forced move is the locked move. Encore
// applies even when it began after selection: the encored move replaces the
// chosen one, unless it was overwritten, which ends the Encore.
template <typename O>
constexpr void use_move(Battle& battle, Player player, bool first,
                        O& options) {
  const Request& r = battle.request;
  const uint8_t i = index(player);
  ActivePokemon& a = battle.side(player).active;
  const uint8_t data = r.choice[i].data();

  MoveState m{};
  m.slot = MoveState::NO_SLOT;
  if (data == 0 && r.move[i] == Move::Struggle) {
    m.move = Move::Struggle;
    m.no_pp = true;
  } else if (forced(a)) {
    // gCurrMovePos keeps the slot chosen when the lock began *(verify)*.
    m.move = a.locked_move;
    for (uint8_t s = 0; s < 4; s++) {
      if (a.moves[s].id == m.move) {
        m.slot = s;
        break;
      }
    }
  } else if (a.encored_move != Move::None &&
             a.moves[a.encored_slot].id == a.encored_move) {
    m.move = a.encored_move;
    m.slot = a.encored_slot;
  } else if (a.encored_move != Move::None) {
    m.slot = a.encored_slot;
    m.move = a.moves[m.slot].id;
    a.encored_move = Move::None;
    a.encored_slot = 0;
    a.encore_timer = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).encore = 0;
  } else {
    assert(data >= 1 && data <= 4);
    m.slot = uint8_t(data - 1);
    m.move = a.moves[m.slot].id;
  }
  assert(m.move != Move::None);
  m.self = get(m.move).target == Target::User;
  m.first = first;
  const Effect effect = get(m.move).effect;
  m.ignore_on_air = effect == Effect::Thunder || effect == Effect::SkyUppercut;
  m.ignore_underground = effect == Effect::Earthquake;

  if (plain_hit(get(m.move).effect)) return hit(battle, player, m, options);
  if (effect == Effect::Confuse) return confuse_move(battle, player, m, options);

  // TODO(plan step 4): every other effect. Until then the move only uses
  // its PP (decided): the attack canceler, attackstring and ppreduce, then
  // moveend.
  if (cancel(battle, player, m, options)) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
  }
  moveend(battle, player, m, options);
}

// RunTurnActionsFunctions (main:4937) from action `from` (0, or after a
// Baton Pass the next one). Each action ends in HandleAction_TryFinish
// (util:638): a faint ends the actions (cancelallactions, bsc:5850).
template <typename O>
constexpr Flow actions(Battle& battle, uint8_t from, O& options) {
  for (uint8_t i = from; i < Request::END_OF_ACTIONS; i++) {
    const Player p = i == 0 ? battle.request.first : foe(battle.request.first);
    const Choice c = battle.request.choice[index(p)];
    if (c.type() == Choice::Switch) {
      switch_to(battle, p, c.data(), options);
    } else {
      use_move(battle, p, i == 0, options);
    }
    if (Flow f = faints(battle, Phase::FaintAfterAction)) return f;
  }
  return std::nullopt;
}

// HandleEndTurn_ContinueBattle (main:3932), then BattleTurnPassed's residuals
// (main:3956) and HandleFaintedMonActions.
template <typename O> constexpr Flow end_of_turn(Battle& battle, O&) {
  for (Side& side : battle.sides) {
    ActivePokemon& a = side.active;
    a.volatiles.flinched = false;
    if (side.get(1).status.asleep() && a.volatiles.multiple_turns) {
      a.cancel_multi_turn_moves();
    }
  }
  // TODO(plan step 4): DoFieldEndTurnEffects and DoBattlerEndTurnEffects.
  // ENDTURN_ORDER (util:1209) takes the order's inputs here, with
  // speeds(battle, request.move[0], request.move[1], true), and resolves it
  // with first(..., true, options) at the first residual whose order
  // matters, so the tie isn't drawn when nothing depends on it.
  return faints(battle, Phase::FaintAfterResiduals);
}

// HandleWishPerishSongOnTurnEnd (util:1781), Future Sight and Doom Desire.
// TODO(plan step 4).
template <typename O> constexpr Flow future_sight(Battle&, O&) {
  return std::nullopt;
}

// HandleWishPerishSongOnTurnEnd, Perish Song. TODO(plan step 4).
template <typename O> constexpr Flow perish_song(Battle&, O&) {
  return std::nullopt;
}

// The rest of BattleTurnPassed (main:3974-4013): the next turn, or a tie at
// the turn cap.
constexpr Result next_turn(Battle& battle) {
  turn_cleanup(battle);
  battle.request = Request{};
  battle.turn++;
  if (battle.turn >= Battle::MAX_TURNS) {
    return Result::init(Result::Tie, Choice::Pass, Choice::Pass);
  }
  return Result::start();
}

// The type of choice p must make now, from the request.
constexpr Choice::Type requested(const Battle& battle, Player player) {
  const Request& r = battle.request;
  switch (r.phase) {
  case Phase::None:
    return battle.turn == 0 ? Choice::Pass : Choice::Move;
  case Phase::BatonPass: {
    const Player passer = r.next == 1 ? r.first : foe(r.first);
    return passer == player ? Choice::Switch : Choice::Pass;
  }
  default:
    return battle.side(player).get(1).fainted() ? Choice::Switch
                                                : Choice::Pass;
  }
}

constexpr Result checked(const Battle& battle, Result result) {
  assert(battle.valid());
  (void)battle;
  return result;
}

} // namespace detail

// The legal choices for `player` given the request type its last Result
// asked for, written to `out`; returns how many (at least 1).
constexpr uint8_t choices(const Battle& battle, Player player,
                          Choice::Type request,
                          std::span<Choice, MAX_CHOICES> out) {
  assert(battle.valid());
  const Side& side = battle.side(player);
  uint8_t n = 0;
  const auto switches = [&] {
    for (uint8_t s = 2; s <= 6; s++) {
      const Pokemon& p = side.get(s);
      if (p.species != Species::None && !p.fainted()) out[n++] = Choice::swap(s);
    }
  };

  switch (request) {
  case Choice::Pass:
    out[n++] = Choice::pass();
    break;
  case Choice::Switch:
    switches();
    assert(n > 0);
    break;
  case Choice::Move: {
    assert(battle.turn != 0);
    if (detail::forced(side.active)) {
      out[n++] = Choice::move(0);
      break;
    }
    const uint8_t usable = detail::usable_moves(battle, player);
    if (usable == 0) out[n++] = Choice::move(0); // Struggle
    for (uint8_t i = 0; i < 4; i++) {
      if (usable & (1 << i)) out[n++] = Choice::move(uint8_t(i + 1));
    }
    if (!detail::trapped(battle, player)) switches();
    break;
  }
  }
  assert(n >= 1 && n <= MAX_CHOICES);
  return n;
}

namespace detail {
// Whether `choice` is one of choices(): the caller's contract, asserted.
constexpr bool legal(const Battle& battle, Player player, Choice choice) {
  Choice buffer[MAX_CHOICES] = {};
  const uint8_t n = choices(battle, player, requested(battle, player), buffer);
  for (uint8_t i = 0; i < n; i++) {
    if (buffer[i] == choice) return true;
  }
  return false;
}
} // namespace detail

// Runs one turn, or the rest of one after a mid-turn request (libpkmn's
// update). At turn 0 both players pass and the battle starts.
template <typename O>
constexpr Result update(Battle& battle, Choice c1, Choice c2, O&& options) {
  using namespace detail;
  assert(battle.valid());
  assert(legal(battle, Player::P1, c1) && legal(battle, Player::P2, c2));
  if constexpr (CHANCE<O>) options.chance.reset();

  const Phase phase = battle.request.phase;
  Flow f;
  switch (phase) {
  case Phase::None:
  case Phase::BatonPass: {
    uint8_t from = 0;
    if (phase == Phase::None) {
      if (battle.turn == 0) return checked(battle, start(battle, options));
      select(battle, Player::P1, c1);
      select(battle, Player::P2, c2);
      order(battle, options);
      // CheckFocusPunch_ClearVarsBeforeTurnStarts (main:4906): Focus Punch's
      // setup is only a message (bs1:3709). Then TryClearRageStatuses.
      for (Side& side : battle.sides) {
        const Move chosen = battle.request.move[&side - battle.sides];
        if (chosen != Move::Rage) side.active.volatiles.rage = false;
      }
    } else {
      // TODO(plan step 4): finish Baton Pass's switch-in (README decision 5).
      assert(false && "Baton Pass isn't implemented");
      from = battle.request.next;
      battle.request.phase = Phase::None;
    }
    if ((f = actions(battle, from, options))) return checked(battle, *f);
    [[fallthrough]];
  }
  case Phase::FaintAfterAction:
    if (phase == Phase::FaintAfterAction) {
      replace(battle, c1, c2, options);
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = faints(battle, phase))) return checked(battle, *f);
    }
    if ((f = end_of_turn(battle, options))) return checked(battle, *f);
    [[fallthrough]];
  case Phase::FaintAfterResiduals:
    if (phase == Phase::FaintAfterResiduals) {
      replace(battle, c1, c2, options);
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = faints(battle, phase))) return checked(battle, *f);
    }
    if ((f = future_sight(battle, options))) return checked(battle, *f);
    [[fallthrough]];
  case Phase::FaintAfterFutureSight:
    if (phase == Phase::FaintAfterFutureSight) {
      replace(battle, c1, c2, options);
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = faints(battle, phase))) return checked(battle, *f);
    }
    if ((f = perish_song(battle, options))) return checked(battle, *f);
    [[fallthrough]];
  case Phase::FaintAfterPerish:
    if (phase == Phase::FaintAfterPerish) {
      replace(battle, c1, c2, options);
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = faints(battle, phase))) return checked(battle, *f);
    }
    return checked(battle, next_turn(battle));
  }
  assert(false && "invalid phase");
  return Result::error();
}

} // namespace pkmn::gen3
