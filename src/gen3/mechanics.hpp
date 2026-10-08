// The turn frame: update() and choices(), as libpkmn's gen*/mechanics.zig.
//
// Plan steps 2-4 (CLAUDE.md): the frame, the order of actions, switching,
// the hit backbone, the residuals and the move effects written so far, with
// the roll helpers, real and null Chance and Calc. Not here yet, and marked
// TODO where they go: the other move effects (they only use PP), Future
// Sight and Perish Song.
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

// Protect, Detect or Endure after n consecutive successes, n 1-3:
// sProtectSuccessRates[n] >= Random() (bsc:6514), exactly 1 / 2^n. The
// decomp reads past the table after the fourth success; Showdown's gen3
// stall stays at 1/8 (counterMax 8, data/mods/gen4/conditions.ts:142), and
// so does this. The first use always works and draws nothing.
template <typename O>
constexpr bool protect(Battle& battle, Player p, uint8_t n, O& options) {
  assert(n >= 1 && n <= 3);
  return coin(battle, p, &Action::protect, 1, 1u << n, options);
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
  assert(i < MAX_HITS && stage < 5);
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
  assert(i < MAX_HITS);
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
  assert(i < MAX_HITS);
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

// Sleep Talk's pick: uniform over the move slots in `mask` (Showdown's
// sample; the decomp redraws Random() % 4 until it hits one, bsc:8266).
// Returns the slot, 0-3; Actions holds it as 1-4.
template <typename O>
constexpr uint8_t sleep_talk(Battle& battle, Player p, uint8_t mask,
                             O& options) {
  assert(mask != 0 && mask < 16);
  const uint8_t n = uint8_t((mask & 1) + (mask >> 1 & 1) + (mask >> 2 & 1) + (mask >> 3 & 1));
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).move_slot;
  assert(forced <= 4 && (forced == 0 || (mask >> (forced - 1) & 1)));
  uint8_t slot = uint8_t(forced - 1);
  if (forced == 0) {
    uint8_t k = uint8_t(battle.rng.range(n));
    for (uint8_t i = 0; i < 4; i++) {
      if ((mask >> i & 1) && k-- == 0) {
        slot = i;
        break;
      }
    }
  }
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).move_slot, uint8_t(slot + 1));
    observe(options, 1, n);
  }
  return slot;
}

// Roar or Whirlwind's pick: uniform over the SLOTs in `mask` (bit s - 1
// for SLOT s, 2-6), the bench Pokémon that can come in (Showdown's
// getRandomSwitchable; the decomp redraws Random() % 6 until it hits one,
// bsc:7346). Returns the SLOT.
template <typename O>
constexpr uint8_t switch_target(Battle& battle, Player p, uint8_t mask,
                                O& options) {
  assert(mask != 0 && (mask & 1) == 0 && mask < 64);
  uint8_t n = 0;
  for (uint8_t s = 2; s <= 6; s++) n = uint8_t(n + (mask >> (s - 1) & 1));
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).switch_target;
  assert(forced == 0 || (forced >= 2 && forced <= 6 && (mask >> (forced - 1) & 1)));
  uint8_t slot = forced;
  if (forced == 0) {
    uint8_t k = uint8_t(battle.rng.range(n));
    for (uint8_t s = 2; s <= 6; s++) {
      if ((mask >> (s - 1) & 1) && k-- == 0) {
        slot = s;
        break;
      }
    }
  }
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).switch_target, slot);
    observe(options, 1, n);
  }
  return slot;
}

// Effect Spore's status: Random() & 3 redrawn while 0, so 1 sleep, 2
// poison, 3 paralysis (MOVE_EFFECT_BURN replaced, util:2776); Showdown
// samples the same three.
template <typename O>
constexpr uint8_t spore(Battle& battle, Player p, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).spore;
  assert(forced <= 3);
  const uint8_t which = forced != 0 ? forced : uint8_t(1 + battle.rng.range(3));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).spore, which);
    observe(options, 1, 3);
  }
  return which;
}

// A contact ability's chance on hit `i` (util:2768-2850): Effect Spore's
// 1/10, or Static, Poison Point, Flame Body and Cute Charm's 1/3. Called
// only when the effect could take hold.
template <typename O>
constexpr bool contact(Battle& battle, Player p, uint8_t i, uint32_t den,
                       O& options) {
  assert(i < MAX_HITS);
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].contact;
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) field = &options.chance.actions.get(p).hits[i].contact;
  return coin(battle, forced, field, 1, den, options);
}

// Starf Berry's stat: uniform over the stats below +6 among Atk, Def, Spe,
// SpA and SpD (the decomp's rejection loop, util:3462; Showdown's onEat
// samples the same set). Returns the stat, 1-5 in that order (STAT_ATK to
// STAT_SPDEF). Called only when one is below +6.
template <typename O>
constexpr uint8_t starf(Battle& battle, Player p, O& options) {
  const Boosts& b = battle.side(p).active.boosts;
  const int8_t stages[5] = {b.atk, b.def, b.spe, b.spa, b.spd};
  uint8_t open = 0;
  for (const int8_t s : stages) open += s < 6;
  assert(open != 0);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).starf;
  assert(forced <= 5 && (forced == 0 || stages[forced - 1] < 6));
  uint8_t stat = forced;
  if (stat == 0) {
    uint8_t k = uint8_t(battle.rng.range(open));
    for (uint8_t i = 0; i < 5; i++) {
      if (stages[i] < 6 && k-- == 0) {
        stat = uint8_t(i + 1);
        break;
      }
    }
  }
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).starf, stat);
    observe(options, 1, open);
  }
  return stat;
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
// priority 0). Quick Claw applies unless `quick_claw` is false: only at the
// battle start, before Showdown makes the first turn's roll (gen3
// getActionSpeed, data/mods/gen3/scripts.ts:18, which every action uses,
// switches included; quickClawRoll, sim/battle.ts:1802).
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
// two moves go by GetWhoStrikesFirst; two switches by speed with Quick
// Claw, as Showdown (decided; the decomp takes them in battler-ID order,
// see the README).
template <typename O> constexpr void order(Battle& battle, O& options) {
  Request& r = battle.request;
  const bool s1 = r.choice[0].type() == Choice::Switch;
  const bool s2 = r.choice[1].type() == Choice::Switch;
  if (s1 != s2) {
    r.first = s1 ? Player::P1 : Player::P2;
    return;
  }
  r.first = first(battle, speeds(battle, r.move[0], r.move[1], true), false,
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

// Cmd_removeitem (bsc:5901): the item is used up, and Recycle remembers it.
constexpr void consume_item(Side& side) {
  side.recycle_item = side.get(1).item;
  side.get(1).item = Item::None;
}

// What ending p's active Pokémon's sleep clears besides the status: the
// Nightmare, the Sleep Talk turns kept for switch-out (sleep_skipped, which
// a later sleep mustn't inherit) and, since the cure is public, the sleep
// count. Callers clear the status.
template <typename O>
constexpr void wake(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  a.volatiles.nightmare = false;
  a.sleep_skipped = 0;
  if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 0;
}

// The cures ITEMEFFECT_NORMAL (util:3485-3606) and ITEMEFFECT_MOVE_END
// (util:3627-3752) share, and White Herb, which both run too. Cured sleep
// and confusion are public, so their counts reset. Returns whether the item
// was used.
template <typename O>
constexpr bool item_cure(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  const auto unconfuse = [&] {
    v.confusion = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).confusion = 0;
  };
  switch (item_data(mon.item).effect) {
  case HoldEffect::CurePar:
    if (!mon.status.paralyzed()) return false;
    mon.status = Status::none();
    break;
  case HoldEffect::CurePsn:
    if (!mon.status.poisoned()) return false;
    mon.status = Status::none();
    a.toxic = 0;
    break;
  case HoldEffect::CureBrn:
    if (!mon.status.burned()) return false;
    mon.status = Status::none();
    break;
  case HoldEffect::CureFrz:
    if (!mon.status.frozen()) return false;
    mon.status = Status::none();
    break;
  case HoldEffect::CureSlp:
    if (!mon.status.asleep()) return false;
    mon.status = Status::none();
    wake(battle, player, options);
    break;
  case HoldEffect::CureConfusion:
    if (v.confusion == 0) return false;
    unconfuse();
    break;
  case HoldEffect::CureStatus:
    if (!mon.status.any() && v.confusion == 0) return false;
    if (mon.status.asleep()) wake(battle, player, options);
    mon.status = Status::none();
    a.toxic = 0;
    unconfuse();
    break;
  case HoldEffect::CureAttract:
    if (!v.infatuated) return false;
    v.infatuated = false;
    break;
  case HoldEffect::RestoreStats: {
    // White Herb: every lowered stage back to 0.
    bool lowered = false;
    const auto reset = [&](int8_t& b) {
      if (b < 0) {
        b = 0;
        lowered = true;
      }
    };
    Boosts& b = a.boosts;
    reset(b.atk), reset(b.def), reset(b.spe), reset(b.spa), reset(b.spd);
    reset(b.acc), reset(b.eva);
    if (!lowered) return false;
    break;
  }
  default:
    return false;
  }
  consume_item(side);
  return true;
}

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

// ---------------------------------------------------------------------------
// Switch-in effects (decided 2026-10-07, following Showdown): each Pokémon's
// Spikes, ability and item run together when it arrives, as gen3
// Showdown's runSwitch (gen4's, data/mods/gen4/scripts.ts:18: hazards,
// SwitchIn, the ability's and item's Start). The cartridge sets flags for
// Intimidate and Trace and runs them after the action (util:1945); for a
// lone switch-in nothing happens in between.
// ---------------------------------------------------------------------------

// The weather a switch-in ability sets, or None.
constexpr Weather ability_weather(Ability ability) {
  switch (ability) {
  case Ability::Drizzle: return Weather::Rain;
  case Ability::Drought: return Weather::Sun;
  case Ability::SandStream: return Weather::Sandstorm;
  default: return Weather::None;
  }
}

// Intimidate (bs1:4024; Showdown's gen3 onStart, data/mods/gen3/abilities.ts:61):
// -1 Attack to the foe unless it has a Substitute, Clear Body, White Smoke
// or Hyper Cutter, its side has Mist (Showdown's onTryBoost), or it is at
// -6. Showdown's White Herb reacts at once (onUpdate).
template <typename O>
constexpr void intimidate(Battle& battle, Player player, O& options) {
  const Player target = foe(player);
  Side& side = battle.side(target);
  ActivePokemon& t = side.active;
  if (side.get(1).fainted() || t.volatiles.substitute ||
      t.ability == Ability::ClearBody || t.ability == Ability::WhiteSmoke ||
      t.ability == Ability::HyperCutter || side.conditions.mist != 0 ||
      t.boosts.atk <= -6) {
    return;
  }
  t.boosts.atk--;
  item_cure(battle, target, options);
}

// ABILITYEFFECT_ON_SWITCHIN (util:2470) with Intimidate and Trace:
// - Drizzle, Drought and Sand Stream set their weather with no timer, unless
//   it is already there from an ability (Showdown's setWeather and its gen <= 5
//   onFieldStart agree).
// - Trace copies the foe's ability if the foe stands; in Gen III the copy
//   doesn't take effect on entry (Showdown's setAbility starts it only for
//   gen > 3, sim/pokemon.ts:1941; the decomp doesn't run it either).
template <typename O>
constexpr void ability_switch_in(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (const Weather w = ability_weather(a.ability); w != Weather::None) {
    Field& field = battle.field;
    if (field.weather != w || !field.permanent) {
      field.weather = w;
      field.weather_turns = 0;
      field.permanent = true;
    }
    return;
  }
  if (a.ability == Ability::Intimidate) return intimidate(battle, player, options);
  if (a.ability == Ability::Trace) {
    const Side& f = battle.side(foe(player));
    if (!f.get(1).fainted() && f.active.ability != Ability::None) {
      a.ability = f.active.ability;
    }
  }
}

// ITEMEFFECT_ON_SWITCH_IN (util:3291): White Herb.
template <typename O>
constexpr void item_switch_in(Battle& battle, Player player, O& options) {
  if (holds(battle.side(player), HoldEffect::RestoreStats)) {
    item_cure(battle, player, options);
  }
}

// The Pokémon in SLOT 1 becomes active: Cmd_switchindataupdate (bsc:4621)
// and SwitchInClearSetData (main:3152), as README decision 4.
constexpr void enter(Battle& battle, Player player) {
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
}

// switchineffects (bsc:5180) for the Pokémon that just entered: Spikes, and
// unless they faint it, Truant's counter and the switch-in ability and item.
template <typename O>
constexpr void arrive(Battle& battle, Player player, O& options) {
  if (spikes(battle, player, options)) return;
  ActivePokemon& a = battle.side(player).active;
  if (a.ability == Ability::Truant) a.truant = true; // bsc:5258
  ability_switch_in(battle, player, options);
  item_switch_in(battle, player, options);
}

// Whether the order of two arrivals can change the result, so a speed tie
// needs its draw: two different weathers (the later one stays), or Spikes,
// whose KO can end the battle before the other arrives. Intimidate, Trace
// and White Herb don't depend on it (see intimidate and ability_switch_in).
constexpr bool arrival_order_matters(const Battle& battle) {
  const Weather w1 = ability_weather(battle.sides[0].active.ability);
  const Weather w2 = ability_weather(battle.sides[1].active.ability);
  return (w1 != Weather::None && w2 != Weather::None && w1 != w2) ||
         battle.sides[0].conditions.spikes != 0 ||
         battle.sides[1].conditions.spikes != 0;
}

// Both Pokémon entered together (the battle start, or both sides
// replacing): each arrives in Showdown's order, the runSwitch actions'
// speed with the turn's Quick Claw (not at the start) and a random tie,
// and the battle can end before the second (Showdown checks after each
// action). The decomp orders replacements by battler ID (util:1880).
template <typename O>
constexpr void arrive_both(Battle& battle, bool start, O& options) {
  const Player first =
      arrival_order_matters(battle)
          ? detail::first(battle, speeds(battle, Move::None, Move::None, !start),
                          false, options)
          : Player::P1;
  arrive(battle, first, options);
  if (outcome(battle)) return;
  arrive(battle, foe(first), options);
}

// p's active Pokémon leaves for SLOT `slot`, which enters: a switch action,
// a faint replacement or Baton Pass (`baton`), before the new Pokémon
// arrives. Switch-out clears the foe's state that p's Pokémon was the
// source of (SwitchInClearSetData and FaintClearSetData, main:3158-3199;
// README decision 9). Baton Pass leaves the foe's escape prevention and its
// Lock-On (main:3158 skips them), and baton_pass_in restores what passes.
template <typename O>
constexpr void leave(Battle& battle, Player player, uint8_t slot, O& options,
                     bool baton = false) {
  Side& side = battle.side(player);
  assert(slot >= 2 && slot <= 6);
  assert(side.get(slot).species != Species::None && !side.get(slot).fainted());

  // TODO(plan step 4): Pursuit on the switching target (bs1:3088). Not on
  // Baton Pass (Showdown's skipBeforeSwitchOutEventFlag).
  // switchoutabilities (bsc:9590): Natural Cure, as Showdown's onSwitchOut.
  // A cured sleep is public, so its count resets before the SLOTs swap.
  Pokemon& out = side.get(1);
  if (side.active.ability == Ability::NaturalCure && out.status.any()) {
    out.status = Status::none();
    if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 0;
  }
  // Showdown's gen3 skippedTime (README, field map): the Sleep Talk and
  // Snore turns go back on the sleep counter, and off the public count.
  if (const uint8_t k = side.active.sleep_skipped; k != 0 && out.status.asleep()) {
    assert(out.status.sleep() + k <= Status::MAX_SLEEP);
    out.status.bits = uint8_t(out.status.bits + k);
    if constexpr (CHANCE<O>) {
      uint8_t& count = options.chance.durations.get(player).sleep[0];
      assert(count == 0 || count > k);
      if (count != 0) count = uint8_t(count - k);
    }
  }
  ActivePokemon& f = battle.side(foe(player)).active;
  f.volatiles.infatuated = false;
  f.volatiles.wrapped = 0;
  f.wrapped_move = Move::None;
  if (!baton) {
    f.volatiles.escape_prevention = false;
    f.volatiles.lock_on = 0;
  }

  std::swap(side.order[0], side.order[slot - 1]);
  if constexpr (CHANCE<O>) options.chance.durations.switched(player, slot, baton);
  enter(battle, player);
}

// A switch action: the Pokémon leaves, the new one enters and arrives
// (Showdown's switch action runs its runSwitch before the next action,
// sim/battle-queue.ts:181-182).
template <typename O>
constexpr void switch_to(Battle& battle, Player player, uint8_t slot,
                         O& options) {
  leave(battle, player, slot, options);
  arrive(battle, player, options);
}

// Baton Pass's switch-in (README decision 5): the passer leaves, the
// recipient enters, gets what passes and arrives (Spikes, its ability and
// item; switchineffects, bs1:1713). What passes is the decomp's keep-list
// (Cmd_switchindataupdate, bsc:4646; SwitchInClearSetData, main:3172-3209)
// with Showdown's differences (its copyVolatileFrom, sim/pokemon.ts:1246,
// copies every volatile without noCopy):
// - Passed: the stat stages, confusion with its counter, Focus Energy,
//   Substitute and its HP, escape prevention, Curse, Leech Seed, Perish Song
//   and its timer, Ingrain, Mud Sport and Water Sport.
// - Binding passes, as Showdown (partiallytrapped has no noCopy and its
//   binder is still in); the decomp clears STATUS2_WRAPPED.
// - Lock-On, as Showdown (gen4's lockon drops noCopy): the passer's lock on
//   the foe keeps its count, where the decomp resets it to 2 (main:3178);
//   the foe's lock on the passer is lost (its source has left), where the
//   decomp keeps STATUS3_ALWAYS_HITS on the recipient.
// - Not passed, by either: the toxic counter (decision 1), infatuation,
//   Disable, Encore, Torment, Nightmare, Yawn, Foresight, Flash Fire. A
//   Taunted Pokémon can't use Baton Pass, so Showdown passing Taunt can't
//   show. TODO: Showdown passes Charge (no noCopy); settle with Charge.
template <typename O>
constexpr void baton_pass_in(Battle& battle, Player player, uint8_t slot,
                             O& options) {
  const ActivePokemon old = battle.side(player).active;
  leave(battle, player, slot, options, true);
  ActivePokemon& a = battle.side(player).active;
  const Volatiles& v = old.volatiles;
  Volatiles& n = a.volatiles;
  a.boosts = old.boosts;
  n.confusion = v.confusion;
  n.focus_energy = v.focus_energy;
  n.substitute = v.substitute;
  a.substitute_hp = old.substitute_hp;
  n.escape_prevention = v.escape_prevention;
  n.cursed = v.cursed;
  n.leech_seed = v.leech_seed;
  n.perish_song = v.perish_song;
  a.perish_song_timer = old.perish_song_timer;
  n.rooted = v.rooted;
  n.mud_sport = v.mud_sport;
  n.water_sport = v.water_sport;
  n.wrapped = v.wrapped;
  a.wrapped_move = old.wrapped_move;
  arrive(battle, player, options);
}

// ---------------------------------------------------------------------------
// Faints and the end of the battle
// ---------------------------------------------------------------------------

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

// The replacements a faint request asked for: as Showdown's instaswitch
// actions, every replacement enters before any arrives (instaswitch comes
// before runSwitch, sim/battle-queue.ts:177), then they arrive in
// arrive_both's order.
template <typename O>
constexpr void replace(Battle& battle, Choice c1, Choice c2, O& options) {
  const Choice choices[2] = {c1, c2};
  for (const Player p : {Player::P1, Player::P2}) {
    const Choice c = choices[index(p)];
    assert((c.type() == Choice::Switch) == battle.side(p).get(1).fainted());
    if (c.type() == Choice::Switch) leave(battle, p, c.data(), options);
  }
  const bool s1 = c1.type() == Choice::Switch;
  const bool s2 = c2.type() == Choice::Switch;
  if (s1 && s2) {
    arrive_both(battle, false, options);
  } else {
    arrive(battle, s1 ? Player::P1 : Player::P2, options);
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
// GetWhoStrikesFirst ignoring chosen moves, then runs the switch-in weather
// (none in link battles), the switch-in abilities in that order,
// Intimidate, Trace and the switch-in items, then TurnValuesCleanUp. This
// follows Showdown instead: both enter, then each arrives in speed order
// without Quick Claw (arrive_both).
template <typename O> constexpr Result start(Battle& battle, O& options) {
  enter(battle, Player::P1);
  enter(battle, Player::P2);
  arrive_both(battle, true, options);
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

// MOVE_EFFECT_* bytes this code applies, in the decomp's numbering order.
enum class Secondary : uint8_t {
  None, Sleep, Poison, Burn, Freeze, Paralysis, Toxic, Confusion, Flinch,
  TriAttack,
  // MOVE_EFFECT_*_MINUS_1 on the target, in Boosts order (atk-acc), then
  // MOVE_EFFECT_ATK_PLUS_1 and DEF_PLUS_1 with MOVE_EFFECT_AFFECTS_USER.
  AtkDown, DefDown, SpeDown, SpaDown, SpdDown, AccDown, AtkUp, DefUp,
};

// The per-turn ProtectStruct (README decision 2): a local of actions(), so
// it starts empty each turn and on resuming after Baton Pass, reached
// through MoveState::turn (decided 2026-10-07). The residuals can't reach
// it. Indexed by player.
struct Turn {
  bool protect[2]; // ProtectStruct.protected: Protect or Detect worked
  bool endure[2];  // ProtectStruct.endured: Endure worked
  // A damaging move hit the Pokémon itself, not its Substitute (Showdown's
  // focuspunch lostFocus; the decomp's physicalDmg || specialDmg, which
  // also needs HP lost). Focus Punch reads it.
  bool hit[2];
  // ProtectStruct.physicalDmg and specialDmg: the HP the foe's last
  // physical or special move took from the Pokémon itself (gHpDealt) this
  // turn, 0 if none. Counter and Mirror Coat read them.
  uint16_t physical[2];
  uint16_t special[2];
};

// What one move execution tracks: the decomp's per-action globals
// (gCurrentMove, gCurrMovePos, gMoveResultFlags, gHpDealt) and the
// gHitMarker bits that this code reads. On the stack, as libpkmn gen2's
// State; HandleAction_ActionFinished (util:661) clears the hit markers
// between actions.
struct MoveState {
  Move move;         // gCurrentMove
  Move chosen;       // gChosenMove: the move itself, or Sleep Talk for the
                     // move Sleep Talk calls
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
  // HITMARKER_SYNCHRONIZE_EFFECT with synchronizeMoveEffect: the last poison,
  // toxic, paralysis or burn this move's execution set (bsc:2503), for
  // moveend's Synchronize cases. None when not armed.
  Secondary synchronize;
  // Called by Sleep Talk: the attack canceler already ran for Sleep Talk
  // (its tracker stays at CANCELER_END, as Showdown's useMove skips
  // BeforeMove), and the move uses no PP.
  bool called;
  // Roar or Whirlwind worked: the target is dragged out after the action,
  // as Showdown's forceSwitchFlag (sim/battle.ts:2828).
  bool drag;
  // Baton Pass worked: the user's side picks the recipient after the action
  // (Phase::BatonPass), as Showdown's switchFlag.
  bool pass;
  // This turn's ProtectStruct; set by use_move, null only for the
  // residuals' MoveState, which never reads it.
  Turn* turn;

  static constexpr uint8_t NO_SLOT = 4;
};

constexpr bool physical(Type type) { return type < Type::Mystery; }

constexpr void pp_reduce(Battle& battle, Player player, const MoveState& m);

// The sound moves (sSoundMovesTable, util:688). Showdown's gen3 `sound` flag
// is on the same moves, plus Perish Song and Heal Bell, which handle
// Soundproof themselves in both.
constexpr bool sound(Move move) {
  switch (move) {
  case Move::Growl:
  case Move::Roar:
  case Move::Sing:
  case Move::Supersonic:
  case Move::Screech:
  case Move::Snore:
  case Move::Uproar:
  case Move::MetalSound:
  case Move::GrassWhistle:
  case Move::HyperVoice: return true;
  default: return false;
  }
}

// ABILITYEFFECT_MOVES_BLOCK (util:2645) on the foe, from Cmd_attackcanceler
// (bsc:932): Soundproof stops a sound move after attackstring and ppreduce
// (BattleScript_SoundproofProtected, bs1:4158), with no result flag. It
// runs for a move Sleep Talk calls too, whose canceler tracker is already
// at its end. Showdown's Soundproof (onTryHit) agrees, and its move uses PP
// first. Returns whether it stopped the move. Log: Immune, from the ability.
constexpr bool soundproof(Battle& battle, Player player, MoveState& m) {
  if (battle.side(foe(player)).active.ability != Ability::Soundproof || !sound(m.move)) {
    return false;
  }
  m.attackstring = true;
  pp_reduce(battle, player, m);
  return true;
}

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
  // A move Sleep Talk calls: Sleep Talk's canceler already ran.
  if (m.called) {
    m.obeys = true;
    return !soundproof(battle, player, m);
  }

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
      wake(battle, player, options);
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
      // Showdown's gen3 skippedTime: Sleep Talk and Snore attempts since
      // the last asleep turn that did nothing (README, field map).
      if (woke) {
        v.nightmare = false;
        a.sleep_skipped = 0;
      } else if (m.move != Move::Snore && m.move != Move::SleepTalk) {
        a.sleep_skipped = 0;
        m.unable = true;
        return false;
      } else {
        a.sleep_skipped++;
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

  if (soundproof(battle, player, m)) return false;

  // No PP left (BattleScript_NoPPForMove, bs1:3556): only if the PP went
  // to 0 after selection, by Spite or Pressure from a faster foe.
  if (m.slot != MoveState::NO_SLOT && a.moves[m.slot].pp == 0 &&
      m.move != Move::Struggle && !v.multiple_turns) {
    m.attackstring = true;
    m.result |= MoveResult::MISSED;
    return false;
  }

  m.obeys = true;
  // TODO(plan step 4): Magic Coat and Snatch, in the per-turn Turn. Protect
  // is in the accuracy check (accuracy()).
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
// A protected target is a certain miss, as in Cmd_accuracycheck.
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
// Whether the foe's Protect or Detect blocks m (DEFENDER_IS_PROTECTED,
// bsc:57). Showdown's protect flag agrees with the decomp's
// FLAG_PROTECT_AFFECTED on every gen3 move that checks it, except Counter
// and Mirror Coat: Showdown's protect flag is on them, and this follows it.
// Log:
// Activate Protect.
constexpr bool protected_from(const MoveState& m, Player player) {
  assert(m.turn != nullptr);
  return m.turn->protect[index(foe(player))] &&
         ((get(m.move).flags & Flag::ProtectAffected) ||
          m.move == Move::Counter || m.move == Move::MirrorCoat);
}

constexpr uint32_t accuracy(const Battle& battle, Player player,
                            const MoveState& m) {
  const Side& side = battle.side(player);
  const ActivePokemon& a = side.active;
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& f = foe_side.active;
  const MoveData& data = get(m.move);
  const Weather weather = effective_weather(battle);

  // JumpIfMoveAffectedByProtect (bsc:1041), before anything else: the foe
  // protected itself and the move has the protect flag, so it misses.
  if (protected_from(m, player)) return 0;
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
// damage counts toward its Bide and, by the move's table type, toward
// Counter's or Mirror Coat's (bsc:1938): Hidden Power is Normal there, so
// always physical. Showdown's gen3 Counter counts Hidden Power and Mirror
// Coat leaves it out explicitly (data/mods/gen3/moves.ts:176, 420), so they
// agree. Returns gHpDealt.
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
  // Future Sight's hit, at the end of the turn, has no Turn: no Focus Punch
  // is left to read it.
  if (m.turn != nullptr) {
    m.turn->hit[index(target)] = true;
    const MoveData& data = get(m.move);
    const bool phys = data.effect == Effect::HiddenPower || physical(data.type);
    (phys ? m.turn->physical : m.turn->special)[index(target)] = dealt;
  }
  return dealt;
}

// HP restored by datahpupdate with negative damage: capped at max HP.
constexpr void heal(Pokemon& mon, uint32_t amount) {
  const uint32_t room = uint32_t(mon.stats.hp - mon.hp);
  mon.hp = uint16_t(mon.hp + (amount < room ? amount : room));
}

// Max HP / n, at least 1: every residual's fraction.
constexpr uint32_t fraction(const Pokemon& mon, uint32_t n) {
  const uint32_t d = mon.stats.hp / n;
  return d == 0 ? 1 : d;
}

// Passive damage to p's active Pokémon (residuals, Rough Skin): datahpupdate
// under HITMARKER_IGNORE_SUBSTITUTE | PASSIVE_HP_UPDATE and, for the
// residuals, IGNORE_BIDE (util:1465), so no Substitute, Bide or Counter
// bookkeeping; then tryfaintmon. Returns the HP lost (gHpDealt).
template <typename O>
constexpr uint16_t passive_damage(Battle& battle, Player player,
                                   uint32_t damage, O& options) {
  Pokemon& mon = battle.side(player).get(1);
  assert(!mon.fainted());
  const uint16_t dealt = mon.hp > damage ? uint16_t(damage) : mon.hp;
  mon.hp = uint16_t(mon.hp - dealt);
  if (mon.fainted()) faint(battle, player, options);
  return dealt;
}

// ---------------------------------------------------------------------------
// Held items (ItemBattleEffects, util:3243): one function per mode, called
// where the decomp calls it (decided 2026-10-07).
// ---------------------------------------------------------------------------

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
// (MOVE_EFFECT_RAPIDSPIN, bs1:1716), Brick Break (bs1:2418), and Counter
// and Mirror Coat (bs1:1217, 1801), Endeavor (bs1:2479), Knock Off
// (bs1:2475) and the draining moves (bs1:323).
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
  case Effect::FocusPunch:
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
  case Effect::Snore:
  case Effect::Counter:
  case Effect::MirrorCoat:
  case Effect::Absorb:
  case Effect::Endeavor:
  case Effect::KnockOff:
  case Effect::Superpower:
  case Effect::Overheat:
    return true;
  default:
    return false;
  }
}

// The stage a stat change moves: 0 Atk, 1 Def, 2 Spe, 3 SpA, 4 SpD, 5 Acc,
// 6 Eva (the decomp's STAT_* minus 1).
constexpr int8_t& stage(Boosts& b, uint8_t stat) {
  switch (stat) {
  case 0: return b.atk;
  case 1: return b.def;
  case 2: return b.spe;
  case 3: return b.spa;
  case 4: return b.spd;
  case 5: return b.acc;
  default: assert(stat == 6); return b.eva;
  }
}

// Lowers a stage by up to n, stopping at -6 (no check of who may lower it).
constexpr void lower(ActivePokemon& a, uint8_t stat, int8_t n) {
  int8_t& b = stage(a.boosts, stat);
  b = b - n < -6 ? int8_t(-6) : int8_t(b - n);
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
  case Effect::FlinchHit:
  case Effect::Snore: return Secondary::Flinch;
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

// Whether an Uproar is going on. Showdown's uproar blocks every sleep
// (onAnySetStatus, data/moves.ts:20253); the decomp exempts Soundproof
// (UproarWakeUpCheck, bsc:6807, and SetMoveEffect, bsc:2273), which Gen 3 OU
// bans anyway.
constexpr bool uproar(const Battle& battle) {
  return battle.sides[0].active.volatiles.uproar != 0 ||
         battle.sides[1].active.volatiles.uproar != 0;
}

// Sleep Clause Mod (Smogon, always on: Showdown's sleepclausemod,
// data/rulesets.ts:1379): a sleep from the foe fails while a Pokémon on
// `target`'s side is alive and asleep from the foe. Rest's sleep (Status
// EXT) doesn't count; it also isn't from the foe, so the clause never stops
// it.
constexpr bool sleep_clause(const Battle& battle, Player target) {
  for (const Pokemon& p : battle.side(target).pokemon) {
    if (!p.fainted() && p.status.asleep() && !p.status.self_inflicted()) return true;
  }
  return false;
}

// Whether a major status `s` can take hold on `target`'s active Pokémon:
// SetMoveEffect's checks (bsc:2408-2560) without Safeguard. Not on a
// fainted Pokémon (its preamble), then each status's type and ability
// immunities, and an existing status. Freeze Clause (Smogon, always on, as
// Showdown's freezeclausemod: no freeze while a Pokémon on that side is
// frozen) and sun stop a freeze.
constexpr bool can_status(const Battle& battle, Player target, Secondary s) {
  const Side& side = battle.side(target);
  const ActivePokemon& t = side.active;
  if (side.get(1).fainted() || side.get(1).status.any()) return false;
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
  case Secondary::Sleep:
    // SetMoveEffect's sleep case (bsc:2273): Uproar, Vital Spirit, Insomnia.
    return t.ability != Ability::VitalSpirit && t.ability != Ability::Insomnia &&
           !uproar(battle);
  default:
    assert(false && "not a major status");
    return false;
  }
}

// Whether a move's major status `s` takes hold on p's foe: Safeguard, then
// can_status, then Sleep Clause for a sleep.
constexpr bool status_takes(const Battle& battle, Player player, Secondary s) {
  return battle.side(foe(player)).conditions.safeguard == 0 &&
         can_status(battle, foe(player), s) &&
         !(s == Secondary::Sleep && sleep_clause(battle, foe(player)));
}

// Sets a major status on `target`, as SetMoveEffect does once statusChanged
// (bsc:2476): freezing also cancels its multi-turn move, and poison, toxic,
// paralysis and burn arm Synchronize (bsc:2503). Changing the status
// restarts the toxic counter (data.hpp, Status).
template <typename O>
constexpr void inflict(Battle& battle, Player target, Secondary s,
                       MoveState& m, O& options) {
  Side& side = battle.side(target);
  Status& status = side.get(1).status;
  switch (s) {
  case Secondary::Sleep:
    // (Random() & 3) + 2 turns, hidden, so drawn without Chance; the public
    // count starts at 1. Sleep cancels a multi-turn move (bsc:2296).
    status = Status::slp(uint8_t(Status::MIN_SLEEP + battle.rng.range(4)));
    side.active.cancel_multi_turn_moves();
    if constexpr (CHANCE<O>) options.chance.durations.get(target).sleep[0] = 1;
    break;
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
  if (s != Secondary::Freeze && s != Secondary::Sleep) m.synchronize = s;
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
    // stage already at -6 stop it. Its Protect check can't fire: the hit got
    // through.
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
    if (status_takes(battle, player, which)) inflict(battle, foe(player), which, m, options);
    break;
  }
  case Secondary::Toxic:
    if (status_takes(battle, player, s)) {
      inflict(battle, foe(player), s, m, options);
    } else {
      m.result |= MoveResult::DOESNT_AFFECT_FOE;
    }
    break;
  default:
    inflict(battle, foe(player), s, m, options);
  }
}

// ---------------------------------------------------------------------------
// Abilities after a move (AbilityBattleEffects, util:2560): one function per
// mode, called from moveend (decided 2026-10-07).
// ---------------------------------------------------------------------------

// ABILITYEFFECT_ABSORBING (util:2662), from JumpIfMoveFailed (bsc:1009)
// once the accuracy check passed: the foe's Volt Absorb or Water Absorb
// takes a damaging Electric or Water move, healing a quarter of its max HP
// through a Substitute unless already full (BattleScript_MoveHPDrain,
// bs1:4078, which marks the move DOESNT_AFFECT_FOE); Flash Fire takes any
// Fire move, Will-O-Wisp included, unless the holder is frozen
// (BattleScript_FlashFireBoost, bs1:4101). Showdown agrees (gen3 Volt
// Absorb skips Thunder Wave; gen4 Flash Fire's frozen check). Returns
// whether the move was absorbed; its script then goes to moveend, after
// attackstring and ppreduce.
constexpr bool absorbed(Battle& battle, Player player, MoveState& m) {
  Side& side = battle.side(foe(player));
  ActivePokemon& t = side.active;
  Pokemon& target = side.get(1);
  const MoveData& data = get(m.move);
  switch (t.ability) {
  case Ability::VoltAbsorb:
  case Ability::WaterAbsorb: {
    const Type type =
        t.ability == Ability::VoltAbsorb ? Type::Electric : Type::Water;
    if (data.type != type || data.bp == 0) return false;
    if (target.hp != target.stats.hp) heal(target, fraction(target, 4));
    m.result |= MoveResult::DOESNT_AFFECT_FOE;
    return true;
  }
  case Ability::FlashFire:
    if (data.type != Type::Fire || target.status.frozen()) return false;
    t.volatiles.flash_fire = true;
    return true;
  default:
    return false;
  }
}

// ABILITYEFFECT_SYNCHRONIZE and ATK_SYNCHRONIZE (util:2957, 2973): if
// `holder` has Synchronize and this execution armed it, the poison (toxic
// becomes poison), paralysis or burn passes to the other Pokémon, which
// arms it again for the next case. The cartridge skips Safeguard here
// (HITMARKER_STATUS_ABILITY_EFFECT); Showdown's Safeguard blocks it
// (data/moves.ts:15598), and this follows Showdown.
template <typename O>
constexpr void synchronize(Battle& battle, Player holder, MoveState& m,
                           O& options) {
  if (battle.side(holder).active.ability != Ability::Synchronize ||
      m.synchronize == Secondary::None) {
    return;
  }
  const Secondary s =
      m.synchronize == Secondary::Toxic ? Secondary::Poison : m.synchronize;
  m.synchronize = Secondary::None;
  const Player other = foe(holder);
  if (battle.side(other).conditions.safeguard == 0 && can_status(battle, other, s)) {
    inflict(battle, other, s, m, options);
  }
}

// ABILITYEFFECT_ON_DAMAGE (util:2735): the target's Color Change, or its
// contact ability acting on the attacker (p). Only after the target's own
// HP took damage from a move with an effect, so never on a self-targeting
// move or a confusion self-hit. Statuses skip Safeguard on the cartridge;
// Showdown's Safeguard blocks them, and this follows Showdown. Effect
// Spore's sleep is subject to Sleep Clause, as Showdown's onSetStatus.
template <typename O>
constexpr void ability_on_damage(Battle& battle, Player player, MoveState& m,
                                 O& options) {
  if (m.self || !m.damaged || (m.result & MoveResult::NO_EFFECT)) return;
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const Pokemon& target_mon = battle.side(target).get(1);
  const MoveData& data = get(m.move);

  if (t.ability == Ability::ColorChange) {
    if (m.move != Move::Struggle && data.bp != 0 && !has_type(t, data.type) &&
        !target_mon.fainted()) {
      t.types[0] = data.type;
      t.types[1] = data.type;
    }
    return;
  }

  Side& side = battle.side(player);
  Pokemon& user = side.get(1);
  if (user.fainted() || !(data.flags & Flag::MakesContact)) return;
  const auto takes = [&](Secondary s) {
    return side.conditions.safeguard == 0 && can_status(battle, player, s) &&
           !(s == Secondary::Sleep && sleep_clause(battle, player));
  };
  Secondary s = Secondary::None;
  switch (t.ability) {
  case Ability::RoughSkin:
    passive_damage(battle, player, fraction(user, 16), options);
    return;
  case Ability::EffectSpore: {
    if (!takes(Secondary::Sleep) && !takes(Secondary::Poison) &&
        !takes(Secondary::Paralysis)) {
      return;
    }
    if (!rolls::contact(battle, player, 0, 10, options)) return;
    const uint8_t which = rolls::spore(battle, player, options);
    s = which == 1   ? Secondary::Sleep
        : which == 2 ? Secondary::Poison
                     : Secondary::Paralysis;
    if (takes(s)) inflict(battle, player, s, m, options);
    return;
  }
  case Ability::PoisonPoint: s = Secondary::Poison; break;
  case Ability::Static: s = Secondary::Paralysis; break;
  case Ability::FlameBody: s = Secondary::Burn; break;
  case Ability::CuteCharm: {
    ActivePokemon& a = side.active;
    const Gender g1 = user.gender;
    const Gender g2 = target_mon.gender;
    if (target_mon.fainted() || a.ability == Ability::Oblivious ||
        a.volatiles.infatuated || g1 == Gender::None || g2 == Gender::None ||
        g1 == g2) {
      return;
    }
    if (rolls::contact(battle, player, 0, 3, options)) a.volatiles.infatuated = true;
    return;
  }
  default:
    return;
  }
  if (takes(s) && rolls::contact(battle, player, 0, 3, options)) {
    inflict(battle, player, s, m, options);
  }
}

// ABILITYEFFECT_IMMUNITY (util:2850), which moveend repeats until nothing
// changes, so it covers both Pokémon: an ability cures the status it
// prevents (one gained before the ability, by Baton Pass, Trace or Skill
// Swap). Cured sleep and confusion are public, so their counts reset.
template <typename O> constexpr void ability_immunity(Battle& battle, O& options) {
  for (const Player p : {Player::P1, Player::P2}) {
    Side& side = battle.side(p);
    Pokemon& mon = side.get(1);
    Volatiles& v = side.active.volatiles;
    const Status st = mon.status;
    switch (side.active.ability) {
    case Ability::Immunity:
      if (st.poisoned()) mon.status = Status::none();
      break;
    case Ability::OwnTempo:
      if (v.confusion != 0) {
        v.confusion = 0;
        if constexpr (CHANCE<O>) options.chance.durations.get(p).confusion = 0;
      }
      break;
    case Ability::Limber:
      if (st.paralyzed()) mon.status = Status::none();
      break;
    case Ability::Insomnia:
    case Ability::VitalSpirit:
      if (st.asleep()) {
        mon.status = Status::none();
        v.nightmare = false;
        if constexpr (CHANCE<O>) options.chance.durations.get(p).sleep[0] = 0;
      }
      break;
    case Ability::WaterVeil:
      if (st.burned()) mon.status = Status::none();
      break;
    case Ability::MagmaArmor:
      if (st.frozen()) mon.status = Status::none();
      break;
    case Ability::Oblivious:
      v.infatuated = false;
      break;
    default:
      break;
    }
  }
}

// Cmd_moveend (bsc:4210) with moveendall: the cases in the decomp's order.
template <typename O>
constexpr void moveend(Battle& battle, Player player, MoveState& m,
                       O& options) {
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

  // MOVEEND_SYNCHRONIZE_TARGET, ON_DAMAGE_ABILITIES, IMMUNITY_ABILITIES,
  // SYNCHRONIZE_ATTACKER. No self-targeting move arms Synchronize (Rest's
  // sleep doesn't), so its target case is the foe's.
  if (!m.self) synchronize(battle, foe(player), m, options);
  ability_on_damage(battle, player, m, options);
  ability_immunity(battle, options);
  synchronize(battle, player, m, options);

  // MOVEEND_CHOICE_MOVE. A Baton Pass that worked doesn't lock, and skips
  // the check that the locked move is still known.
  bool skip = false;
  if (m.obeys && holds(side, HoldEffect::ChoiceBand) &&
      m.chosen != Move::Struggle && a.choice_move == Move::None) {
    skip = m.chosen == Move::BatonPass && !(m.result & MoveResult::FAILED);
    if (!skip) a.choice_move = m.chosen;
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

  // MOVEEND_ITEM_EFFECTS_ALL: ITEMEFFECT_MOVE_END's cures. The decomp
  // stops at the first Pokémon whose item acts (util:3740); Showdown's
  // onUpdate cures both, and this follows it.
  for (const Player p : {Player::P1, Player::P2}) {
    if (!battle.side(p).get(1).fainted()) item_cure(battle, p, options);
  }

  // TODO(plan step 4): MOVEEND_KINGSROCK_SHELLBELL (ItemBattleEffects).

  // MOVEEND_SUBSTITUTE
  for (Side& s : battle.sides) {
    if (s.active.substitute_hp == 0) s.active.volatiles.substitute = false;
  }

  // MOVEEND_UPDATE_LAST_MOVES: gChosenMove (Sleep Talk for its called move)
  // for the printed and last moves and Mirror Move, gCurrentMove for the
  // resulting and landed ones.
  if (m.attackstring) a.last_printed_move = m.chosen;
  if (get(m.chosen).effect != Effect::BatonPass) {
    a.last_move = m.obeys ? m.chosen : UNAVAILABLE;
    a.last_resulting_move = m.obeys ? m.move : UNAVAILABLE;
    if (m.obeys && !no_effect) {
      t.last_landed_move = m.move;
      t.last_hit_type = type;
    } else {
      t.last_landed_move = UNAVAILABLE;
    }
  }

  // MOVEEND_MIRROR_MOVE
  if ((get(m.chosen).flags & Flag::MirrorMoveAffected) && m.obeys && !m.self &&
      !target.fainted() && !no_effect) {
    t.last_taken_move = m.chosen;
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

// adjustnormaldamage's and adjustsetdamage's survival (bsc:1677, 5878):
// a lethal hit `d` on the foe's Pokémon itself (not its Substitute) leaves
// it at 1 HP under False Swipe, Endure, or a Focus Band that activates
// (hit `i`'s draw). Endure comes first, so the band's draw can't matter
// then (Showdown's Endure onDamage has the higher priority). Returns the
// damage to deal. Log: Activate Endure, or Focus Band.
template <typename O>
constexpr uint32_t survive(Battle& battle, Player player, const MoveState& m,
                           uint32_t d, uint8_t i, O& options) {
  const Player defender = foe(player);
  const Side& foe_side = battle.side(defender);
  const Pokemon& target = foe_side.get(1);
  if (foe_side.active.volatiles.substitute || target.hp > d) return d;
  const ItemData item = item_data(target.item);
  bool survives = get(m.move).effect == Effect::FalseSwipe ||
                  m.turn->endure[index(defender)];
  if (!survives && item.effect == HoldEffect::FocusBand) {
    survives = rolls::focus_band(battle, player, i, item.param, options);
  }
  return survives ? target.hp - 1u : d;
}

// BattleScript_EffectHit (bs1:236), from the attack canceler on.
template <typename O>
constexpr void hit(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  const Effect effect = get(m.move).effect;
  // BattleScript_EffectSnore (bs1:1257): only while asleep, which the
  // canceler has just ticked (a Pokémon that woke can't Snore). Showdown's
  // onTry agrees.
  if (effect == Effect::Snore && !battle.side(player).get(1).status.asleep()) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  // BattleScript_EffectFocusPunch (bs1:2260): after the canceler,
  // jumpifnodamage. A user that a damaging move hit this turn loses its
  // focus: ppreduce, no attackstring, no result flag. Otherwise the plain
  // hit from its accuracy check (BattleScript_HitFromAccCheck). Showdown's
  // gen3 onTry agrees (data/mods/gen4/moves.ts:528); its lostFocus also
  // counts a hit that took no HP, and this follows it (Turn::hit). Log:
  // Cant Focus Punch.
  if (effect == Effect::FocusPunch && m.turn->hit[index(player)]) {
    pp_reduce(battle, player, m);
    return moveend(battle, player, m, options);
  }
  Pokemon& user = battle.side(player).get(1);

  // Counter and Mirror Coat (BattleScript_EffectCounter, bs1:1217;
  // BattleScript_EffectMirrorCoat, bs1:1801): Cmd_counterdamagecalculator
  // (bsc:7943) fails before the accuracy check (FailedFromAtkString) unless
  // the foe's physical or special move took HP this turn; then the plain
  // hit for twice that, with typecalc's immunities (Ghost to Counter, Dark
  // to Mirror Coat) and adjustsetdamage, but no crit or roll. Showdown's
  // gen3 onTry agrees. A failure under Pressure still costs 2 PP, as
  // Showdown deducts before onTry; the decomp spares it
  // (ppNotAffectedByPressure). Log: Fail.
  //
  // Endeavor (BattleScript_EffectEndeavor, bs1:2479): attackstring and
  // ppreduce, then Cmd_setdamagetohealthdifference (bsc:9366) fails it
  // unless the target has more HP than the user; then the plain hit for the
  // difference, with typecalc's immunities and no crit or roll. Showdown's
  // gen4 onTry agrees. Log: Fail.
  //
  // `fixed` is the damage of these, 0 for any other move.
  uint32_t fixed = 0;
  if (effect == Effect::Counter || effect == Effect::MirrorCoat ||
      effect == Effect::Endeavor) {
    const uint16_t foe_hp = battle.side(foe(player)).get(1).hp;
    if (effect == Effect::Endeavor) {
      fixed = foe_hp > user.hp ? foe_hp - user.hp : 0u;
    } else {
      fixed = 2u * (effect == Effect::Counter ? m.turn->physical
                                              : m.turn->special)[index(player)];
    }
    if (fixed == 0) {
      m.attackstring = true;
      pp_reduce(battle, player, m);
      m.result |= MoveResult::FAILED;
      return moveend(battle, player, m, options);
    }
  }

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
  // Protect is part of the accuracy check, which comes before typecalc, so
  // it blocks a move the target is immune to too (Showdown's Protect
  // TryHit also comes first).
  if (affects || breaks || protected_from(m, player)) {
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
  // JumpIfMoveFailed (bsc:1009) after a hit: the absorbing abilities.
  // TODO(plan step 4): Destiny Bond's setup (TrySetDestinyBondToHappen).
  if (affects && absorbed(battle, player, m)) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    moveend(battle, player, m, options);
    if (effect == Effect::Explosion) faint(battle, player, options);
    return;
  }

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
    uint32_t d = fixed != 0 ? fixed : user.level;
    if (effect != Effect::LevelDamage && fixed == 0) {
      if (can_crit(battle, player)) {
        h.crit = rolls::crit(battle, player, 0,
                             crit_stage(battle, player, m.move), options);
      }
      d = apply_roll(damage(battle, player, h),
                     rolls::damage(battle, player, 0, options));
    }
    d = survive(battle, player, m, d, 0, options);
    // datahpupdate
    m.hp_dealt = hp_update(battle, defender, d, m);
  }

  secondary(battle, player, m, options);

  // Knock Off (bs1:2475, MOVE_EFFECT_KNOCK_OFF, bsc:2858), as Showdown's
  // gen4 onAfterHit (data/mods/gen4/moves.ts:718; README decision 8): after
  // a hit on the target itself (not its Substitute) with the user still
  // in, its item becomes the inert Item::KnockedOff, unless a living
  // target's Sticky Hold keeps it. Its Choice Band lock ends with it
  // (bsc:2885). Log: EndItem [from] Knock Off, or Activate Sticky Hold.
  if (effect == Effect::KnockOff && m.damaged && !user.fainted() &&
      target.item != Item::None && target.item != Item::KnockedOff &&
      !(foe_side.active.ability == Ability::StickyHold && !target.fainted())) {
    target.item = Item::KnockedOff;
    foe_side.active.choice_move = Move::None;
  }

  // Superpower (MOVE_EFFECT_ATK_DEF_DOWN) and Overheat (SP_ATK_TWO_DOWN),
  // certain and on the user (bs1:2388, 2648; bsc:2839, 2895): after a hit that
  // affected the target, Substitute or not, Mist and the user's own
  // abilities aside, down to -6. Showdown's self boosts agree. Log:
  // Unboost.
  if (affects && (effect == Effect::Superpower || effect == Effect::Overheat)) {
    ActivePokemon& a = side.active;
    if (effect == Effect::Superpower) {
      lower(a, 0, 1);
      lower(a, 1, 1);
    } else {
      lower(a, 3, 2);
    }
  }

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

  // Absorb, Mega Drain and Giga Drain (BattleScript_EffectAbsorb, bs1:323):
  // negativedamage (bsc:6925), half the HP dealt, at least 1, healed past
  // the user's Substitute, or taken from it if the target has Liquid Ooze.
  // As Showdown, half a Substitute's damage rounds up (gen4 Substitute's
  // drain, data/mods/gen4/moves.ts:1332; the decomp floors it) and a move
  // with no effect drains nothing (the decomp still drains 1). The user's
  // Liquid Ooze faint comes before the target's (bs1:358). Log: Heal
  // [from] drain, or Damage [from] Liquid Ooze.
  if (effect == Effect::Absorb && affects && m.hp_dealt != 0) {
    uint16_t drain = m.damaged ? uint16_t(m.hp_dealt / 2) : uint16_t((m.hp_dealt + 1) / 2);
    if (drain == 0) drain = 1;
    if (foe_side.active.ability == Ability::LiquidOoze) {
      passive_damage(battle, player, drain, options);
    } else {
      heal(user, drain);
    }
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
  // accuracycheck's JumpIfMoveFailed: the absorbing abilities. Of the
  // status moves only Will-O-Wisp can be taken, by Flash Fire.
  if (absorbed(battle, player, m)) return moveend(battle, player, m, options);
  if (foe_side.conditions.safeguard != 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  confuse(battle, foe(player), options);
  moveend(battle, player, m, options);
}

// The major status a primary status move inflicts, or None.
constexpr Secondary primary_status(Effect effect) {
  switch (effect) {
  case Effect::Poison: return Secondary::Poison;
  case Effect::Toxic: return Secondary::Toxic;
  case Effect::Paralyze: return Secondary::Paralysis;
  case Effect::WillOWisp: return Secondary::Burn;
  case Effect::Sleep: return Secondary::Sleep;
  default: return Secondary::None;
  }
}

// The primary status moves: BattleScript_EffectPoison (bs1:986), Toxic
// (bs1:686), Paralyze (bs1:1007; Thunder Wave, Stun Spore, Glare),
// Will-O-Wisp (bs1:2174) and Sleep (bs1:284; Spore, Hypnosis, Sleep Powder,
// Lovely Kiss, Sing, Grass Whistle). After the canceler, attackstring and ppreduce,
// each script runs its checks in its own order, then accuracy, then
// Safeguard, then seteffectprimary. The order only changes the result
// flags, which moveend reads: the "already statused" and ability scripts
// set none, NotAffected sets DOESNT_AFFECT_FOE, ButItFailed sets FAILED.
// Paralyze's typecalc is only the type immunity (Thunder Wave into Ground,
// Glare into Ghost, unless Foresight): Wonder Guard needs power (bsc:1409),
// and Showdown's skips status moves too. Safeguard runs moveend as
// Showdown does (see confuse_move).
template <typename O>
constexpr void status_move(Battle& battle, Player player, MoveState& m,
                           Secondary s, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& t = foe_side.active;
  const Status status = foe_side.get(1).status;
  const bool poison_immune = has_type(t, Type::Poison) || has_type(t, Type::Steel);

  // The checks before accuracy: NONE fails with no flag, nonzero with these
  // flags, 0 goes on.
  constexpr uint8_t NONE = 0xFF;
  const uint8_t failed = [&]() -> uint8_t {
    switch (s) {
    case Secondary::Poison:
    case Secondary::Toxic:
      if (t.ability == Ability::Immunity) return NONE;
      if (t.volatiles.substitute) return MoveResult::FAILED;
      if (status.poisoned()) return NONE;
      // Poison checks the types before any other status, Toxic after.
      if (s == Secondary::Poison && poison_immune) return MoveResult::DOESNT_AFFECT_FOE;
      if (status.any()) return MoveResult::FAILED;
      if (poison_immune) return MoveResult::DOESNT_AFFECT_FOE;
      return 0;
    case Secondary::Paralysis: {
      if (t.ability == Ability::Limber) return NONE;
      if (t.volatiles.substitute) return MoveResult::FAILED;
      const Type type = get(m.move).type;
      for (const Type d : t.types) {
        const bool negated = t.volatiles.foresight && d == Type::Ghost &&
                             (type == Type::Normal || type == Type::Fighting);
        if (effectiveness(type, d) == Effectiveness::Immune && !negated) {
          return MoveResult::DOESNT_AFFECT_FOE | MoveResult::FAILED;
        }
      }
      if (status.paralyzed()) return NONE;
      if (status.any()) return MoveResult::FAILED;
      return 0;
    }
    case Secondary::Burn:
      if (t.volatiles.substitute) return MoveResult::FAILED;
      if (status.burned()) return NONE;
      if (has_type(t, Type::Fire)) return MoveResult::DOESNT_AFFECT_FOE;
      if (t.ability == Ability::WaterVeil) return NONE;
      if (status.any()) return MoveResult::FAILED;
      return 0;
    case Secondary::Sleep:
      // BattleScript_EffectSleep (bs1:284): jumpifcantmakeasleep is
      // Uproar, Vital Spirit and Insomnia.
      if (t.volatiles.substitute) return MoveResult::FAILED;
      if (status.asleep()) return NONE;
      if (!can_status(battle, foe(player), Secondary::Sleep) && !status.any()) {
        return NONE;
      }
      if (status.any()) return MoveResult::FAILED;
      return 0;
    default:
      assert(false && "not a primary status");
      return MoveResult::FAILED;
    }
  }();
  if (failed != 0) {
    if (failed != NONE) m.result |= failed;
    return moveend(battle, player, m, options);
  }

  const uint32_t calc = accuracy(battle, player, m);
  if (!(calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options)))) {
    m.result |= MoveResult::MISSED | MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  // accuracycheck's JumpIfMoveFailed: the absorbing abilities. Of the
  // status moves only Will-O-Wisp can be taken, by Flash Fire.
  if (absorbed(battle, player, m)) return moveend(battle, player, m, options);
  // Sleep Clause (Smogon) fails the move after the accuracy check, as
  // Showdown's onSetStatus.
  if (foe_side.conditions.safeguard != 0 ||
      (s == Secondary::Sleep && sleep_clause(battle, foe(player)))) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  inflict(battle, foe(player), s, m, options);
  moveend(battle, player, m, options);
}

// BattleScript_EffectRest (bs1:735): it fails while asleep, under Uproar,
// with Vital Spirit or Insomnia, and at full HP (Cmd_trysetrest); otherwise
// 3 turns of self-inflicted sleep (Status::slf), any other status replaced,
// and full HP. Showdown agrees (Rest's onTry and onHit; Sleep Clause skips
// a sleep from the Pokémon itself).
template <typename O>
constexpr void rest(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  const ActivePokemon& a = side.active;
  if (mon.status.asleep() || uproar(battle) || a.ability == Ability::VitalSpirit ||
      a.ability == Ability::Insomnia || mon.hp == mon.stats.hp) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  mon.status = Status::slf(Status::REST_SLEEP);
  side.active.toxic = 0;
  mon.hp = mon.stats.hp;
  if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 1;
  moveend(battle, player, m, options);
}

// BattleScript_EffectYawn (bs1:2451): it fails against Vital Spirit or
// Insomnia, a Substitute, Safeguard, a semi-invulnerable target (accuracy
// check without a roll, NO_ACC_CALC_CHECK_LOCK_ON; Showdown's accuracy is
// true), a status, or a Yawn already pending (Cmd_setyawn). The cartridge
// also fails it during an Uproar (jumpifcantmakeasleep); Showdown doesn't
// (Yawn's onTryHit checks only the status and sleep immunity), and this
// follows Showdown. ENDTURN_YAWN puts the target to sleep (battler_residuals).
template <typename O>
constexpr void yawn(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& foe_side = battle.side(foe(player));
  ActivePokemon& t = foe_side.active;
  if (t.ability == Ability::VitalSpirit || t.ability == Ability::Insomnia) {
    return moveend(battle, player, m, options);
  }
  if (t.volatiles.substitute || foe_side.conditions.safeguard != 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (accuracy(battle, player, m) == 0) {
    m.result |= MoveResult::MISSED | MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (foe_side.get(1).status.any() || t.volatiles.yawn != 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.volatiles.yawn = 2;
  moveend(battle, player, m, options);
}

// ChangeStatBuffs (bsc:6937) raising the user's own stat by n, as the
// self-targeting moves do (MOVE_EFFECT_AFFECTS_USER | STAT_CHANGE_ALLOW_PTR):
// only the cap stops it, and a raise at +6 marks the move MISSED
// (bsc:7097), which moveend reads. Returns the stages gained, which the
// log's Boost carries: Showdown prints `-boost|...|0` for a primary raise
// at the cap (sim/battle.ts:2083).
constexpr int8_t raise(ActivePokemon& a, uint8_t stat, int8_t n, MoveState& m) {
  int8_t& b = stage(a.boosts, stat);
  if (b >= 6) {
    m.result |= MoveResult::MISSED;
    return 0;
  }
  const int8_t gained = b + n > 6 ? int8_t(6 - b) : n;
  b = int8_t(b + gained);
  return gained;
}

// The stat and stages of a one-stat raise: setstatchanger in
// BattleScript_EffectAttackUp and its siblings (bs1:475-489, 927-945).
struct StatChange {
  uint8_t stat;
  int8_t n;
};

constexpr StatChange stat_up(Effect effect) {
  switch (effect) {
  case Effect::AttackUp: return {0, 1};
  case Effect::AttackUp2: return {0, 2};
  case Effect::DefenseUp: return {1, 1};
  case Effect::DefenseUp2: return {1, 2};
  case Effect::SpeedUp2: return {2, 2};
  case Effect::SpecialAttackUp: return {3, 1};
  case Effect::SpecialAttackUp2: return {3, 2};
  case Effect::SpecialDefenseUp2: return {4, 2};
  case Effect::EvasionUp: return {6, 1};
  default: return {0, 0};
  }
}

// The two stats of BattleScript_EffectCosmicPower, BulkUp, CalmMind and
// DragonDance (bs1:2687-2800), raised by 1 each in this order; {0, 0} if
// `effect` isn't one of them.
struct StatPair {
  uint8_t first;
  uint8_t second;

  constexpr bool any() const { return first != second; }
};

constexpr StatPair stat_pair(Effect effect) {
  switch (effect) {
  case Effect::CosmicPower: return {1, 4};
  case Effect::BulkUp: return {0, 1};
  case Effect::CalmMind: return {3, 4};
  case Effect::DragonDance: return {0, 2};
  default: return {0, 0};
  }
}

// BattleScript_EffectStatUp (bs1:489) and the scripts that share its tail
// after the canceler: Minimize sets its flag first (setminimize, bs1:1476),
// Defense Curl after attackstring and ppreduce (setdefensecurlbit,
// bs1:2014); neither undoes it when the raise fails. Showdown's boosts and
// volatiles agree; it prints `-boost|...|0` at the cap where the cartridge
// prints "won't go higher".
template <typename O>
constexpr void stat_up_move(Battle& battle, Player player, MoveState& m,
                            O& options) {
  ActivePokemon& a = battle.side(player).active;
  const Effect effect = get(m.move).effect;
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  if (effect == Effect::Minimize) a.volatiles.minimized = true;
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (effect == Effect::DefenseCurl) a.volatiles.defense_curl = true;
  const StatChange c = effect == Effect::Minimize      ? StatChange{6, 1}
                       : effect == Effect::DefenseCurl ? StatChange{1, 1}
                                                       : stat_up(effect);
  assert(c.n != 0);
  raise(a, c.stat, c.n, m); // Log: Boost
  moveend(battle, player, m, options);
}

// The stat and stages of a one-stat drop on the foe: setstatchanger in
// BattleScript_EffectAttackDown and its siblings (bs1:516-533, 958-975);
// {0, 0} for any other effect.
constexpr StatChange stat_down(Effect effect) {
  switch (effect) {
  case Effect::AttackDown: return {0, 1};
  case Effect::AttackDown2: return {0, 2};
  case Effect::DefenseDown: return {1, 1};
  case Effect::DefenseDown2: return {1, 2};
  case Effect::SpeedDown: return {2, 1};
  case Effect::SpeedDown2: return {2, 2};
  case Effect::SpecialDefenseDown2: return {4, 2};
  case Effect::AccuracyDown: return {5, 1};
  case Effect::EvasionDown: return {6, 1};
  default: return {0, 0};
  }
}

// Whether ChangeStatBuffs (bsc:6937) lets a primary drop of `stat` reach
// p's active Pokémon: not under Mist, Clear Body or White Smoke, Hyper
// Cutter for Attack or Keen Eye for accuracy, and not already at -6.
// Showdown's Mist and onTryBoost handlers agree.
constexpr bool can_lower(const Battle& battle, Player p, uint8_t stat) {
  const Side& side = battle.side(p);
  const ActivePokemon& t = side.active;
  return side.conditions.mist == 0 && t.ability != Ability::ClearBody &&
         t.ability != Ability::WhiteSmoke &&
         !(t.ability == Ability::HyperCutter && stat == 0) &&
         !(t.ability == Ability::KeenEye && stat == 5) &&
         [&] { Boosts b = t.boosts; return stage(b, stat); }() > -6;
}

// BattleScript_EffectStatDown (bs1:534), for Screech, Metal Sound and the
// other one-stat drops: after the canceler, a Substitute fails it
// (FailedFromAtkString); then accuracy (with Protect), attackstring,
// ppreduce and the drop. A drop that can't happen (can_lower) changes
// nothing whatever the accuracy draw, so nothing is drawn. Showdown's
// order differs only in its log (see leech_seed). Log: Unboost, or Fail
// or Miss.
template <typename O>
constexpr void stat_down_move(Battle& battle, Player player, MoveState& m,
                              StatChange c, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  if (t.volatiles.substitute) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const bool can = can_lower(battle, target, c.stat);
  const uint32_t calc = can ? accuracy(battle, player, m) : 100;
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (!(calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options)))) {
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  if (can) lower(t, c.stat, c.n);
  moveend(battle, player, m, options);
}

// BattleScript_EffectTickle (bs1:2652): after the canceler, attackstring and
// ppreduce it fails with Attack and Defense both at -6, then on a missed
// accuracy check (ButItFailed); then each drop that can happen. The decomp
// has no Substitute check; Showdown's Tickle (no bypasssub) fails into one,
// and this follows it. As stat_down_move, nothing is drawn when neither
// drop can happen. Log: Unboost each, or Fail.
template <typename O>
constexpr void tickle(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const bool atk = can_lower(battle, target, 0);
  const bool def = can_lower(battle, target, 1);
  if (t.volatiles.substitute || (t.boosts.atk == -6 && t.boosts.def == -6)) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const uint32_t calc = atk || def ? accuracy(battle, player, m) : 100;
  if (!(calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options)))) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (atk) lower(t, 0, 1);
  if (def) lower(t, 1, 1);
  moveend(battle, player, m, options);
}

// BattleScript_EffectHaze (bs1:562), Cmd_normalisebuffs: every active
// Pokémon's stages go to 0. Showdown's gen3 Haze agrees (it keeps Focus
// Energy, unlike gen4's). Never fails. Log: ClearAllBoost.
template <typename O>
constexpr void haze(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  for (Side& side : battle.sides) side.active.boosts = Boosts{};
  moveend(battle, player, m, options);
}

// BattleScript_EffectPsychUp (bs1:1790), Cmd_copyfoestats (bsc:8809): the
// user takes the foe's stages, through a Substitute and Protect; it never
// fails. Showdown's Psych Up also copies Focus Energy (its onHit's
// volatilesToCopy, which gen3 inherits), and this follows it. Log:
// CopyBoost.
template <typename O>
constexpr void psych_up(Battle& battle, Player player, MoveState& m,
                        O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  a.boosts = f.boosts;
  a.volatiles.focus_energy = f.volatiles.focus_energy;
  moveend(battle, player, m, options);
}

// Cosmic Power, Bulk Up, Calm Mind and Dragon Dance (bs1:2687-2800): they
// fail only when both stats are at +6 (BattleScript_CantRaiseMultipleStats,
// FAILED); otherwise each stat is raised in turn, and one already at +6
// marks the move MISSED as in raise(). Showdown raises the same stats in
// the same order (the moves' `boosts`).
template <typename O>
constexpr void stat_pair_move(Battle& battle, Player player, MoveState& m,
                              StatPair s, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  if (stage(a.boosts, s.first) >= 6 && stage(a.boosts, s.second) >= 6) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  raise(a, s.first, 1, m);  // Log: Boost
  raise(a, s.second, 1, m); // Log: Boost
  moveend(battle, player, m, options);
}

// BattleScript_EffectBellyDrum (bs1:1776), Cmd_maxattackhalvehp (bsc:8785):
// it fails unless Attack is below +6 and HP is above half (at least 1);
// then Attack goes to +6 and the user loses max HP / 2 (at least 1),
// bypassing its Substitute. Showdown's Belly Drum is the same (data/moves.ts:
// bellydrum; its directDamage floors the half). Log: SetBoost, then Damage.
template <typename O>
constexpr void belly_drum(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  const uint32_t half = fraction(mon, 2);
  if (side.active.boosts.atk >= 6 || mon.hp <= half) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  side.active.boosts.atk = 6;
  // The user can't faint: its HP was above the half it loses.
  passive_damage(battle, player, half, options);
  moveend(battle, player, m, options);
}

// BattleScript_EffectCurse (bs1:1482).
//
// Not a Ghost type: the move targets the user. It fails only if Speed is at
// -6 and Attack and Defense at +6; otherwise Speed falls by 1 (Mist and
// Clear Body don't apply to Curse, bsc:6962) and Attack and Defense rise
// by 1, each skipped at its cap (Attack or Defense at +6 marks it MISSED,
// as in raise()). Showdown's gen4 Curse (data/mods/gen4/moves.ts:268) gives
// the same boosts, spe -1, atk +1, def +1, in that order.
//
// A Ghost type (BattleScript_GhostCurse): it fails on the foe's
// Substitute, a semi-invulnerable foe (accuracycheck with
// NO_ACC_CALC_CHECK_LOCK_ON) or a foe already cursed (Cmd_cursetarget,
// bsc:8468); otherwise the foe is cursed and the user loses max HP / 2 (at
// least 1), bypassing its own Substitute, and may faint. Showdown agrees on
// the Substitute (gen4 onModifyMove drops the effect), an existing curse
// (onTryHit) and the half HP (directDamage). Protect doesn't stop it: the
// decomp's accuracycheck checks Protect only for FLAG_PROTECT_AFFECTED,
// which Curse lacks, and Showdown's gen4 Curse has no protect flag.
template <typename O>
constexpr void curse(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const bool ghost = has_type(a, Type::Ghost);
  m.self = !ghost;
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);

  if (!ghost) {
    Boosts& b = a.boosts;
    if (b.spe <= -6 && b.atk >= 6 && b.def >= 6) {
      m.result |= MoveResult::FAILED;
      return moveend(battle, player, m, options);
    }
    if (b.spe > -6) b.spe--; // Log: Boost (-1)
    raise(a, 0, 1, m);       // Log: Boost
    raise(a, 1, 1, m);       // Log: Boost
    return moveend(battle, player, m, options);
  }

  ActivePokemon& t = battle.side(foe(player)).active;
  if (t.volatiles.substitute) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  // NO_ACC_CALC_CHECK_LOCK_ON (Cmd_accuracycheck, bsc:1099): Lock-On, then
  // only the semi-invulnerable check. Curse's accuracy is 0 in the data, so
  // accuracy() doesn't apply.
  // A miss goes to BattleScript_ButItFailed, so FAILED only.
  if (t.volatiles.lock_on == 0 && t.volatiles.semi_invulnerable()) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (t.volatiles.cursed) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.volatiles.cursed = true; // Log: Start Curse, of the user
  // Log: Damage (plain, as Showdown's directDamage from the move)
  passive_damage(battle, player, fraction(battle.side(player).get(1), 2),
                 options);
  moveend(battle, player, m, options);
}

// BattleScript_EffectSubstitute (bs1:1085), Cmd_setsubstitute (bsc:7808):
// after the canceler, ppreduce and attackstring, it fails with a
// Substitute already up or at max HP / 4 (at least 1) or less, setting no
// result flag. Otherwise the Substitute gets max HP / 4 (at least 1), the
// user loses that much, bypassing the new Substitute, and a binding on the
// user ends. Showdown agrees on each point (data/moves.ts: substitute; its
// `hp <= maxhp / 4` and floored directDamage match on integer HP; it ends
// partiallytrapped silently). Log: Start Substitute, then Damage; or Fail
// (weak).
template <typename O>
constexpr void substitute(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  pp_reduce(battle, player, m);
  m.attackstring = true;
  ActivePokemon& a = battle.side(player).active;
  const Pokemon& mon = battle.side(player).get(1);
  const uint32_t quarter = fraction(mon, 4);
  if (a.volatiles.substitute || mon.hp <= quarter) {
    return moveend(battle, player, m, options);
  }
  assert(quarter <= UINT8_MAX);
  a.volatiles.substitute = true;
  a.substitute_hp = uint8_t(quarter);
  if (a.volatiles.wrapped != 0) {
    a.volatiles.wrapped = 0;
    a.wrapped_move = Move::None;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).binding = 0;
  }
  // The user can't faint: its HP was above the quarter it loses.
  passive_damage(battle, player, quarter, options);
  moveend(battle, player, m, options);
}

// BattleScript_EffectProtect and BattleScript_EffectEndure (bs1), for
// Protect, Detect and Endure; Cmd_setprotectlike (bsc:6503). The count of
// consecutive successes restarts unless the last move that resulted was
// one of the three. It fails when the user acts last this turn
// (gCurrentTurnActionNumber, Showdown's queue.willAct()), or on the
// success draw (rolls::protect); a failure restarts the count and marks
// the move MISSED. Showdown's gen3 stall agrees (it expires after a turn
// without one of the three, and is removed on a failure), except past the
// fourth success (see rolls::protect). Log: SingleTurn, or Fail.
template <typename O>
constexpr void protect(Battle& battle, Player player, MoveState& m,
                       O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  const Move last = a.last_resulting_move;
  if (last != Move::Protect && last != Move::Detect && last != Move::Endure) {
    a.protect_uses = 0;
  }
  const uint8_t n = a.protect_uses < 3 ? a.protect_uses : 3;
  if (!m.first || (n != 0 && !rolls::protect(battle, player, n, options))) {
    a.protect_uses = 0;
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  if (get(m.move).effect == Effect::Protect) {
    m.turn->protect[index(player)] = true;
  } else {
    m.turn->endure[index(player)] = true;
  }
  if (a.protect_uses != UINT8_MAX) a.protect_uses++;
  moveend(battle, player, m, options);
}

// BattleScript_EffectSpikes (bs1:1544), Cmd_trysetspikes: a layer on the
// foe's side, up to 3; at 3 it fails (BattleScript_FailedFromAtkString,
// FAILED). Showdown's spikes condition agrees (onSideRestart fails at 3
// layers). Pressure takes its extra PP (MOVE_TARGET_OPPONENTS_FIELD; Showdown's
// mustpressure). Log: SideStart Spikes, or Fail.
template <typename O>
constexpr void spikes_move(Battle& battle, Player player, MoveState& m,
                           O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  uint8_t& layers = battle.side(foe(player)).conditions.spikes;
  if (layers >= 3) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  layers++;
  moveend(battle, player, m, options);
}

// The healing moves: BattleScript_EffectRestoreHp (bs1:672; Recover, Slack
// Off) and BattleScript_EffectSoftboiled (bs1:2027; Soft-Boiled, Milk
// Drink), Cmd_tryhealhalfhealth: max HP / 2; BattleScript_EffectMorningSun
// (bs1:1731; Morning Sun, Synthesis, Moonlight), Cmd_recoverbasedonsunlight:
// 2/3 in sun, 1/4 in other weather, 1/2 without (the effective weather).
// At least 1, bypassing the user's Substitute; at full HP it does nothing,
// with no result flag (BattleScript_AlreadyAtFullHp). Showdown floors the
// same fractions (gen < 5 in sim/battle-actions.ts:1199; the gen4 mod's
// Morning Sun family, data/mods/gen4/moves.ts:940). Log: Heal, or Fail heal.
template <typename O>
constexpr void heal_move(Battle& battle, Player player, MoveState& m,
                         O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Pokemon& mon = battle.side(player).get(1);
  if (mon.hp == mon.stats.hp) return moveend(battle, player, m, options);
  const Effect effect = get(m.move).effect;
  uint32_t amount = mon.stats.hp / 2u;
  if (effect == Effect::MorningSun || effect == Effect::Synthesis ||
      effect == Effect::Moonlight) {
    const Weather w = effective_weather(battle);
    if (w == Weather::Sun) {
      amount = 20u * mon.stats.hp / 30u;
    } else if (w != Weather::None) {
      amount = mon.stats.hp / 4u;
    }
  }
  heal(mon, amount == 0 ? 1 : amount);
  moveend(battle, player, m, options);
}

// The SLOTs p's bench can send in, as a mask (bit s - 1 for SLOT s): the
// Pokémon present and not fainted.
constexpr uint8_t bench(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  uint8_t mask = 0;
  for (uint8_t s = 2; s <= 6; s++) {
    const Pokemon& mon = side.get(s);
    if (mon.species != Species::None && !mon.fainted()) mask |= uint8_t(1 << (s - 1));
  }
  return mask;
}

// BattleScript_EffectRoar (bs1:593), for Roar and Whirlwind. Showdown's
// gen3 Roar has accuracy 100 and the protect flag (data/mods/gen4/moves.ts:
// 1164, inheriting gen5's accuracy), as the decomp's.
// - After the canceler (Soundproof stops Roar, a sound move), attackstring
//   and ppreduce: Suction Cups and Ingrain stop it with no result flag
//   (Showdown's onDragOut), then a semi-invulnerable target fails it
//   (accuracycheck with NO_ACC_CALC_CHECK_LOCK_ON), then the accuracy roll
//   (ACC_CURR_MOVE: evasion can make it miss), then an empty bench fails
//   it (forcerandomswitch; Showdown's canSwitch). The bench is checked
//   before the roll, since the roll can't matter without one.
// - No level check: TryDoForceSwitchOut (bsc:7163) only draws when the
//   user's level is lower, and Showdown has none.
// - The drag happens after the whole action (Showdown's forceSwitchFlag;
//   the decomp drags before moveend), so this only sets m.drag.
// - It hits through a Substitute in both; Protect stops it (accuracy()).
template <typename O>
constexpr void roar(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const ActivePokemon& t = battle.side(foe(player)).active;
  // BattleScript_AbilityPreventsPhasingOut and BattleScript_PrintMonIsRooted
  // set no result flag. Log: Activate (Suction Cups, Ingrain).
  if (t.ability == Ability::SuctionCups || t.volatiles.rooted) {
    return moveend(battle, player, m, options);
  }
  const uint32_t calc = accuracy(battle, player, m);
  // accuracy() is 0 only for a semi-invulnerable target here: Roar's
  // accuracy is 100.
  if (calc == 0 || bench(battle, foe(player)) == 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (calc < 100 && !rolls::hit(battle, player, calc, options)) {
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  m.drag = true;
  moveend(battle, player, m, options);
}

// Roar or Whirlwind's drag (Showdown's dragIn, sim/battle-actions.ts:162):
// a random bench Pokémon replaces `target`'s, as a switch (switch-out
// effects, Spikes, the switch-in ability and item). `user` made the draw.
// Log: Drag.
template <typename O>
constexpr void drag(Battle& battle, Player user, O& options) {
  const Player target = foe(user);
  assert(!battle.side(target).get(1).fainted());
  const uint8_t slot = rolls::switch_target(battle, user, bench(battle, target), options);
  leave(battle, target, slot, options);
  arrive(battle, target, options);
}

// BattleScript_EffectBatonPass (bs1:1694): after the canceler, attackstring
// and ppreduce, it fails with no bench (jumpifcantswitch with
// SWITCH_IGNORE_ESCAPE_PREVENTION, so trapping doesn't stop it; Showdown's
// onHit checks canSwitch). Otherwise the user's side picks the recipient.
// The decomp switches before moveend, so its moveend sees the recipient;
// here moveend runs on the passer, which is the same for Baton Pass: it
// sets no last move, a working pass doesn't Choice-lock, and it changes no
// status for the item cures. Log: Fail, then the switch (Switch, [from]
// Baton Pass) on resuming.
template <typename O>
constexpr void baton_pass(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (bench(battle, player) == 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  m.pass = true;
  moveend(battle, player, m, options);
}

// Whether Sleep Talk can't call `move`, as Showdown's gen3 Sleep Talk: the
// nosleeptalk and charge flags, which are the decomp's list
// (Extra::NoSleepTalk: IsInvalidForSleepTalkOrAssist, Focus Punch, Uproar,
// IsTwoTurnsMove) except that Showdown's gen3 Bide drops its flags
// (data/mods/gen3/moves.ts) and Showdown adds Mimic, Nature Power and
// Sketch.
constexpr bool sleep_talk_banned(Move move) {
  if (move == Move::Bide) return false;
  if (move == Move::Mimic || move == Move::NaturePower || move == Move::Sketch) {
    return true;
  }
  return get(move).extra & Extra::NoSleepTalk;
}

template <typename O>
constexpr void execute(Battle& battle, Player player, MoveState& m, O& options);

// BattleScript_EffectSleepTalk (bs1:1311), as Showdown's gen3 Sleep Talk
// (data/mods/gen3/moves.ts:557, with gen4's onTryHit): only while asleep;
// it fails under a Choice Band lock or Encore (on the cartridge too, through
// CheckMoveLimitations); it picks uniformly among the move slots Sleep Talk
// may call, and fails if the pick has no PP. The cartridge instead leaves
// out disabled, taunted, tormented and imprisoned moves and calls a move
// with no PP (bsc:8241); this follows Showdown. The called move uses no PP
// and skips the attack canceler (MoveState::called), and its moveend
// records Sleep Talk as the chosen move.
template <typename O>
constexpr void sleep_talk(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const ActivePokemon& a = battle.side(player).active;
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const Move move = a.moves[i].id;
    if (move != Move::None && !sleep_talk_banned(move)) mask |= uint8_t(1 << i);
  }
  if (!battle.side(player).get(1).status.asleep() || a.choice_move != Move::None ||
      a.encore_timer != 0 || mask == 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const uint8_t slot = rolls::sleep_talk(battle, player, mask, options);
  if (a.moves[slot].pp == 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  MoveState c{};
  c.move = a.moves[slot].id;
  c.chosen = Move::SleepTalk;
  c.slot = slot;
  c.no_pp = true;
  c.called = true;
  c.first = m.first;
  c.turn = m.turn;
  assert(get(c.move).effect != Effect::SleepTalk);
  execute(battle, player, c, options);
  m.drag = c.drag;
  m.pass = c.pass;
}

// BattleScript_EffectWish (bs1:2358), Cmd_trywish (bsc:9292): after the
// canceler, attackstring and ppreduce it sets the user's side's counter to
// 2 and remembers the wisher, or fails while one is pending. Showdown's
// gen4 slot condition agrees (duration 2; addSlotCondition fails while it's
// there). It lands in field_residuals. Log: Fail.
template <typename O>
constexpr void wish(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& side = battle.side(player);
  if (side.conditions.wish != 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  side.conditions.wish = 2;
  side.conditions.wisher = side.order[0];
  moveend(battle, player, m, options);
}

// BattleScript_EffectLeechSeed (bs1:1157): after the canceler, attackstring
// and ppreduce, a Substitute fails it (ButItFailed); then accuracy; then
// Cmd_setseeded (bsc:6718) misses an already seeded or Grass target. Those
// two outcomes don't depend on the accuracy draw, so they're checked before
// it and nothing is drawn. Showdown checks Grass first (TryImmunity) and
// the Substitute and the seed after its accuracy draw, so its log can show
// a miss where this fails; the state is the same, and the status moves
// share this (settle with the log). The drain is in battler_residuals.
// Log: Start Leech Seed, or Fail, Immune or Miss.
template <typename O>
constexpr void leech_seed(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& t = battle.side(foe(player)).active;
  if (t.volatiles.substitute) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const uint32_t calc =
      t.volatiles.leech_seed || has_type(t, Type::Grass) ? 0 : accuracy(battle, player, m);
  if (!(calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options)))) {
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  t.volatiles.leech_seed = true;
  moveend(battle, player, m, options);
}

// The weather a weather move sets, or None.
constexpr Weather move_weather(Effect effect) {
  switch (effect) {
  case Effect::RainDance: return Weather::Rain;
  case Effect::SunnyDay: return Weather::Sun;
  case Effect::Sandstorm: return Weather::Sandstorm;
  case Effect::Hail: return Weather::Hail;
  default: return Weather::None;
  }
}

// BattleScript_EffectRainDance (bs1:1744), SunnyDay (bs1:1757), Sandstorm
// and Hail: after the canceler, attackstring and ppreduce, Cmd_setrain
// (bsc:6681), setsunny (bsc:8767) and the like fail (MISSED) if that
// weather is up, an ability's permanent one included; otherwise it replaces
// any weather for 5 turns, the field residual counting this one. Showdown's
// setWeather agrees (it fails on the same weather after gen 2). Log:
// Weather, or Fail.
template <typename O>
constexpr void weather_move(Battle& battle, Player player, MoveState& m,
                            Weather w, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Field& field = battle.field;
  if (field.weather == w) {
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  field.weather = w;
  field.weather_turns = 5;
  field.permanent = false;
  moveend(battle, player, m, options);
}

// Beat Up, as Showdown's gen3 Beat Up (data/mods/gen3/moves.ts:31, gen4's
// base power 10), which differs visibly from BattleScript_EffectBeatUp
// (bs1:1940, Cmd_trydobeatup, bsc:8957): after the canceler, one accuracy
// check (with Protect), attackstring and ppreduce, then one hit per party
// member that isn't fainted or statused when the move starts, in SLOT order
// (Showdown's side.pokemon), until the target faints. Each hit is a
// typeless special hit through damage() with that member's base Atk
// against the target's base Def (Hit::ally), with its own crit, damage
// roll and Focus Band draws; Light Screen halves it and Mirror Coat counts
// it. The decomp uses its own formula with no roll, ignores the screens
// and goes in party order. With no such member it fails. No contact, no
// secondary. Log: Activate Beat Up [of] the member per hit, then HitCount.
template <typename O>
constexpr void beat_up(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  const uint32_t calc = accuracy(battle, player, m);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (!(calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options)))) {
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  const Side& side = battle.side(player);
  const Player defender = foe(player);
  const Pokemon& target = battle.side(defender).get(1);
  Species allies[6] = {};
  uint8_t n = 0;
  for (uint8_t s = 1; s <= 6; s++) {
    const Pokemon& ally = side.get(s);
    if (ally.species != Species::None && !ally.fainted() && !ally.status.any()) {
      allies[n++] = ally.species;
    }
  }
  if (n == 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  for (uint8_t i = 0; i < n && !target.fainted(); i++) {
    Hit h = Hit::of(m.move);
    h.ally = allies[i];
    if (can_crit(battle, player)) {
      h.crit = rolls::crit(battle, player, i, crit_stage(battle, player, m.move), options);
    }
    uint32_t d = apply_roll(damage(battle, player, h), rolls::damage(battle, player, i, options));
    d = survive(battle, player, m, d, i, options);
    m.hp_dealt = hp_update(battle, defender, d, m);
  }
  if (target.fainted()) faint(battle, defender, options);
  moveend(battle, player, m, options);
}

// BattleScript_EffectPerishSong (bs1:1567), as Showdown's Perish Song
// (data/moves.ts onHitField): after the canceler, attackstring and
// ppreduce, every active Pokémon without a count gets 3, except a
// semi-invulnerable one, which it misses, and a Soundproof one other than
// the user. It fails only if nothing was missed, blocked or started. The
// decomp's Cmd_trysetperishsong (bsc:8508) skips the user's Soundproof too,
// ignores semi-invulnerability, and fails when every Pokémon is Soundproof
// or counting. Pressure takes no extra PP here (Target::User); the decomp
// takes one (PressurePPLoseOnUsingPerishSong), Showdown's is unverified
// *(verify)*. Log: FieldActivate Perish
// Song, Immune for Soundproof, Miss, or Fail.
template <typename O>
constexpr void perish_song(Battle& battle, Player player, MoveState& m,
                           O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  bool result = false;
  for (const Player p : {player, foe(player)}) {
    ActivePokemon& a = battle.side(p).active;
    if (p != player && (a.volatiles.semi_invulnerable() || a.ability == Ability::Soundproof)) {
      result = true;
    } else if (!a.volatiles.perish_song) {
      a.volatiles.perish_song = true;
      a.perish_song_timer = 3;
      result = true;
    }
  }
  if (!result) m.result |= MoveResult::FAILED;
  moveend(battle, player, m, options);
}

// BattleScript_EffectTaunt (bs1:2308): after the canceler, attackstring and
// ppreduce, a missed accuracy check (Protect, evasion) or an existing
// Taunt fails it (ButItFailed); Cmd_settaunt (bsc:9156) sets 2 turns. No
// Substitute check. Showdown's gen3 Taunt agrees (duration 2, bypasssub,
// protect). The selection and the canceler block power-0 moves, which are
// exactly Showdown's Status moves; the residual counts it down. Log: Start
// Taunt, or Fail.
template <typename O>
constexpr void taunt(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& t = battle.side(foe(player)).active;
  const uint32_t calc = t.taunt != 0 ? 0 : accuracy(battle, player, m);
  if (!(calc >= 100 || (calc > 0 && rolls::hit(battle, player, calc, options)))) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.taunt = 2;
  moveend(battle, player, m, options);
}

// BattleScript_EffectRefresh (bs1:2520), Cmd_cureifburnedparalyzedorpoisoned
// (bsc:9119): the user's poison, burn or paralysis is cured; otherwise it
// fails (asleep, frozen or healthy). Showdown agrees. Log: CureStatus, or
// Fail.
template <typename O>
constexpr void refresh(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Pokemon& mon = battle.side(player).get(1);
  if (!mon.status.poisoned() && !mon.status.burned() && !mon.status.paralyzed()) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  mon.status = Status::none();
  battle.side(player).active.toxic = 0; // status1 holds the counter
  moveend(battle, player, m, options);
}

// BattleScript_EffectHealBell (bs1:1361), Cmd_healpartystatus (bsc:8370),
// for Heal Bell and Aromatherapy: every Pokémon on the user's side is
// cured, the active one's Nightmare too, except that Heal Bell skips
// Soundproof ones (the active one by its battle ability, the others by
// their own). It never fails. Showdown's gen4 versions agree. Cured sleeps
// are public, so their counts reset. Log: Activate Heal Bell (and Immune
// for Soundproof) or CureTeam.
template <typename O>
constexpr void heal_bell(Battle& battle, Player player, MoveState& m,
                         O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& side = battle.side(player);
  const bool bell = m.move == Move::HealBell;
  for (uint8_t s = 1; s <= 6; s++) {
    Pokemon& mon = side.get(s);
    if (mon.species == Species::None) continue;
    const Ability ability = s == 1 ? side.active.ability : mon.ability;
    if (bell && ability == Ability::Soundproof) continue;
    if (s == 1) {
      if (mon.status.asleep()) wake(battle, player, options);
      side.active.volatiles.nightmare = false;
      side.active.toxic = 0;
    } else if constexpr (CHANCE<O>) {
      options.chance.durations.get(player).sleep[s - 1] = 0;
    }
    mon.status = Status::none();
  }
  moveend(battle, player, m, options);
}

// The healing moves' effects (heal_move).
constexpr bool heals(Effect effect) {
  return effect == Effect::RestoreHp || effect == Effect::Softboiled ||
         effect == Effect::MorningSun || effect == Effect::Synthesis ||
         effect == Effect::Moonlight;
}

// Whether use_move runs the effect's script (tools/coverage counts these);
// every other effect only uses PP for now.
constexpr bool handled(Effect effect) {
  return plain_hit(effect) || effect == Effect::Confuse ||
         primary_status(effect) != Secondary::None || effect == Effect::Rest ||
         effect == Effect::Yawn || effect == Effect::SleepTalk ||
         stat_up(effect).n != 0 || stat_pair(effect).any() ||
         effect == Effect::Minimize || effect == Effect::DefenseCurl ||
         effect == Effect::BellyDrum || effect == Effect::Curse ||
         effect == Effect::Roar || effect == Effect::Substitute ||
         effect == Effect::Protect || effect == Effect::Endure ||
         effect == Effect::Spikes || heals(effect) ||
         effect == Effect::BatonPass || effect == Effect::Wish ||
         effect == Effect::LeechSeed || effect == Effect::Refresh ||
         effect == Effect::HealBell || effect == Effect::Taunt ||
         move_weather(effect) != Weather::None || effect == Effect::BeatUp ||
         effect == Effect::PerishSong || stat_down(effect).n != 0 ||
         effect == Effect::Tickle || effect == Effect::Haze ||
         effect == Effect::PsychUp;
}

// The move's script, by effect: m.move set, m.chosen and the hit markers
// filled in here.
template <typename O>
constexpr void execute(Battle& battle, Player player, MoveState& m, O& options) {
  assert(m.move != Move::None);
  if (m.chosen == Move::None) m.chosen = m.move;
  m.self = get(m.move).target == Target::User;
  const Effect effect = get(m.move).effect;
  m.ignore_on_air = effect == Effect::Thunder || effect == Effect::SkyUppercut;
  m.ignore_underground = effect == Effect::Earthquake;

  if (plain_hit(effect)) return hit(battle, player, m, options);
  if (effect == Effect::Confuse) return confuse_move(battle, player, m, options);
  if (const Secondary s = primary_status(effect); s != Secondary::None) {
    return status_move(battle, player, m, s, options);
  }
  if (effect == Effect::Rest) return rest(battle, player, m, options);
  if (effect == Effect::Yawn) return yawn(battle, player, m, options);
  if (effect == Effect::SleepTalk) return sleep_talk(battle, player, m, options);
  if (stat_up(effect).n != 0 || effect == Effect::Minimize ||
      effect == Effect::DefenseCurl) {
    return stat_up_move(battle, player, m, options);
  }
  if (const StatPair s = stat_pair(effect); s.any()) {
    return stat_pair_move(battle, player, m, s, options);
  }
  if (effect == Effect::BellyDrum) return belly_drum(battle, player, m, options);
  if (effect == Effect::Curse) return curse(battle, player, m, options);
  if (effect == Effect::Roar) return roar(battle, player, m, options);
  if (effect == Effect::Substitute) return substitute(battle, player, m, options);
  if (effect == Effect::Protect || effect == Effect::Endure) {
    return protect(battle, player, m, options);
  }
  if (effect == Effect::Spikes) return spikes_move(battle, player, m, options);
  if (heals(effect)) return heal_move(battle, player, m, options);
  if (effect == Effect::BatonPass) return baton_pass(battle, player, m, options);
  if (effect == Effect::Wish) return wish(battle, player, m, options);
  if (effect == Effect::LeechSeed) return leech_seed(battle, player, m, options);
  if (effect == Effect::Refresh) return refresh(battle, player, m, options);
  if (effect == Effect::HealBell) return heal_bell(battle, player, m, options);
  if (effect == Effect::Taunt) return taunt(battle, player, m, options);
  if (effect == Effect::BeatUp) return beat_up(battle, player, m, options);
  if (effect == Effect::PerishSong) return perish_song(battle, player, m, options);
  if (const StatChange c = stat_down(effect); c.n != 0) {
    return stat_down_move(battle, player, m, c, options);
  }
  if (effect == Effect::Tickle) return tickle(battle, player, m, options);
  if (effect == Effect::Haze) return haze(battle, player, m, options);
  if (effect == Effect::PsychUp) return psych_up(battle, player, m, options);
  if (const Weather w = move_weather(effect); w != Weather::None) {
    return weather_move(battle, player, m, w, options);
  }

  // TODO(plan step 4): every other effect. Until then the move only uses
  // its PP (decided): the attack canceler, attackstring and ppreduce, then
  // moveend.
  if (cancel(battle, player, m, options)) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
  }
  moveend(battle, player, m, options);
}

// HandleAction_UseMove (util:78): which move and slot the action uses, then
// its script. Struggle uses no PP. A forced move is the locked move. Encore
// applies even when it began after selection: the encored move replaces the
// chosen one, unless it was overwritten, which ends the Encore. Returns
// what follows the action.
enum class After : uint8_t {
  None,
  Drag, // Roar or Whirlwind drags the foe out (MoveState::drag)
  Pass, // Baton Pass asks for the recipient (MoveState::pass)
};

template <typename O>
constexpr After use_move(Battle& battle, Player player, bool first, Turn& turn,
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
  m.first = first;
  m.turn = &turn;
  execute(battle, player, m, options);
  assert(!(m.drag && m.pass));
  return m.drag ? After::Drag : m.pass ? After::Pass : After::None;
}

// RunTurnActionsFunctions (main:4937) from action `from` (0, or after a
// Baton Pass the next one). Each action ends in HandleAction_TryFinish
// (util:638): a faint ends the actions (cancelallactions, bsc:5850).
//
// Roar's drag comes after its action, before the faint check, as
// Showdown's runAction (sim/battle.ts:2825). A dragged-out Pokémon's own
// queued move is cancelled (Showdown's runAction skips an inactive
// Pokémon's move); a switch can't be queued after a move.
//
// A Baton Pass that worked asks its side for the recipient, after the faint
// check (Showdown's switch requests, sim/battle.ts:2844), and the actions
// resume at the next one (update's BatonPass case).
template <typename O>
constexpr Flow actions(Battle& battle, uint8_t from, O& options) {
  Turn turn{};
  for (uint8_t i = from; i < Request::END_OF_ACTIONS; i++) {
    const Player p = i == 0 ? battle.request.first : foe(battle.request.first);
    const Choice c = battle.request.choice[index(p)];
    After after = After::None;
    if (c.type() == Choice::Switch) {
      switch_to(battle, p, c.data(), options);
    } else {
      after = use_move(battle, p, i == 0, turn, options);
      if (after == After::Drag) drag(battle, p, options);
    }
    if (Flow f = faints(battle, Phase::FaintAfterAction)) return f;
    if (after == After::Drag) break;
    if (after == After::Pass) {
      battle.request.phase = Phase::BatonPass;
      battle.request.next = uint8_t(i + 1);
      const Choice::Type c1 = p == Player::P1 ? Choice::Switch : Choice::Pass;
      const Choice::Type c2 = p == Player::P2 ? Choice::Switch : Choice::Pass;
      return Result::init(Result::None, c1, c2);
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// End of turn (BattleTurnPassed, main:3956): DoFieldEndTurnEffects
// (util:1181), then DoBattlerEndTurnEffects (util:1460), written out in the
// decomp's order (decided 2026-10-07: straight-line code, not a table).
// Nothing suspends inside them, so no case tracker is kept: residual faints
// are replaced after both passes. The battle ends at the first residual KO
// that leaves a side with nothing, as Showdown (fieldEvent checks after
// every handler, sim/battle.ts:569) and the decomp's checkteamslost after
// each damage script; the decomp's Leech Seed script has none (bs1:3283),
// and this follows Showdown there too.
//
// Showdown runs the per-Pokémon residuals in the same order: they share
// onResidualOrder 10 and comparePriority sorts by speed before subOrder
// (sim/battle.ts:408), so it is Pokémon-major as the decomp is.
// ---------------------------------------------------------------------------

// Whether some side could lose to the residuals: its only Pokémon left is
// active and takes residual damage (over-approximated: immunities and
// amounts are ignored).
constexpr bool could_lose(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  if (mon.fainted()) return false;
  for (uint8_t s = 2; s <= 6; s++) {
    const Pokemon& p = side.get(s);
    if (p.species != Species::None && !p.fainted()) return false;
  }
  const Volatiles& v = side.active.volatiles;
  return mon.status.poisoned() || mon.status.burned() || v.leech_seed ||
         v.nightmare || v.cursed || v.wrapped != 0 || v.perish_song ||
         battle.field.weather == Weather::Sandstorm ||
         battle.field.weather == Weather::Hail;
}

// Whether the residual order (ENDTURN_ORDER, util:1209) can change the
// result, so that a speed tie needs its draw. The passes touch each side's
// own Pokémon only, except Leech Seed (it heals or hurts the seeder); so
// the order matters only through Leech Seed or the battle ending first on
// one side. Over-approximated: a draw that turns out not to matter costs
// search a chance node, never correctness.
constexpr bool residual_order_matters(const Battle& battle) {
  return battle.sides[0].active.volatiles.leech_seed ||
         battle.sides[1].active.volatiles.leech_seed ||
         could_lose(battle, Player::P1) || could_lose(battle, Player::P2);
}

// ITEMEFFECT_NORMAL (util:3317) for p's active Pokémon, at
// ENDTURN_ITEMS1 (move_turn false) and ENDTURN_ITEMS2 (true). Leftovers,
// the HP berries and the pinch berries need move_turn false, so they act
// only once per end of turn; on the cartridge and in Showdown's gen3 mod
// (onResidual, data/mods/gen3/items.ts) they don't act right after a hit.
// TODO(plan step 4): Leppa Berry (RESTORE_PP) and the flavor berries
// (CONFUSE_*, which read the nature); none is on the teams.
template <typename O>
constexpr void item_end_turn(Battle& battle, Player player, bool move_turn,
                             O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  if (mon.fainted()) return;
  const ItemData item = item_data(mon.item);
  switch (item.effect) {
  case HoldEffect::RestoreHp:
    // Oran, Sitrus (30 HP in Gen III) and Berry Juice: a fixed amount.
    if (move_turn || mon.hp > mon.stats.hp / 2) return;
    heal(mon, item.param);
    consume_item(side);
    return;
  case HoldEffect::Leftovers:
    if (!move_turn && mon.hp != mon.stats.hp) heal(mon, fraction(mon, 16));
    return;
  case HoldEffect::AttackUp:
  case HoldEffect::DefenseUp:
  case HoldEffect::SpeedUp:
  case HoldEffect::SpAttackUp:
  case HoldEffect::SpDefenseUp: {
    // TRY_EAT_STAT_UP_BERRY (util:3230): +1 at max HP / param (4) or less.
    if (move_turn || mon.hp > mon.stats.hp / item.param) return;
    const uint8_t stat = uint8_t(item.effect) - uint8_t(HoldEffect::AttackUp);
    int8_t& b = stage(a.boosts, stat);
    if (b >= 6) return;
    b++;
    consume_item(side);
    return;
  }
  case HoldEffect::CriticalUp:
    // Lansat Berry: Focus Energy (util:3441).
    if (move_turn || mon.hp > mon.stats.hp / item.param ||
        a.volatiles.focus_energy) {
      return;
    }
    a.volatiles.focus_energy = true;
    consume_item(side);
    return;
  case HoldEffect::RandomStatUp: {
    // Starf Berry: +2 to a random stat below +6 (util:3450).
    if (move_turn || mon.hp > mon.stats.hp / item.param) return;
    const Boosts& b = a.boosts;
    if (b.atk >= 6 && b.def >= 6 && b.spe >= 6 && b.spa >= 6 && b.spd >= 6) return;
    int8_t& s = stage(a.boosts, uint8_t(rolls::starf(battle, player, options) - 1));
    s = s + 2 > 6 ? 6 : int8_t(s + 2);
    consume_item(side);
    return;
  }
  default:
    item_cure(battle, player, options);
    return;
  }
}

// DoFieldEndTurnEffects (util:1181), after ENDTURN_ORDER: the side timers
// (P1's side, then P2's, each in turn), Wish in the residual order, then
// the weather. Sand and hail hurt each Pokémon in the residual order
// (BattleScript_DamagingWeatherLoop, bs1:3205) and stop once the battle is
// over (bs1:3219).
template <typename O>
constexpr Flow field_residuals(Battle& battle, Player first, O& options) {
  // ENDTURN_REFLECT, LIGHT_SCREEN, MIST, SAFEGUARD (util:1218-1311): each
  // is one pass over both sides.
  for (Side& side : battle.sides) {
    if (side.conditions.reflect != 0) side.conditions.reflect--;
  }
  for (Side& side : battle.sides) {
    if (side.conditions.light_screen != 0) side.conditions.light_screen--;
  }
  for (Side& side : battle.sides) {
    if (side.conditions.mist != 0) side.conditions.mist--;
  }
  for (Side& side : battle.sides) {
    if (side.conditions.safeguard != 0) side.conditions.safeguard--;
  }

  // ENDTURN_WISH (util:1312): it lands on whoever is active, healing half of
  // its max HP (trywish, bsc:9311), unless it has fainted.
  for (const Player p : {first, foe(first)}) {
    Side& side = battle.side(p);
    SideConditions& c = side.conditions;
    if (c.wish == 0 || --c.wish != 0) continue;
    c.wisher = 0;
    Pokemon& mon = side.get(1);
    if (!mon.fainted()) heal(mon, mon.stats.hp / 2);
  }

  // ENDTURN_RAIN, SANDSTORM, SUN, HAIL (util:1331-1427): the timer, unless
  // an ability set the weather (hail has no permanent form in Gen III). The
  // turn it runs out, sand and hail deal no damage (BattleScript_
  // SandStormHailEnds), as in Showdown (fieldEvent ends the effect and moves
  // on, sim/battle.ts:519). Otherwise they hurt each Pokémon in the residual
  // order if the weather has effect (Cmd_weatherdamage, bsc:7602: not under
  // Cloud Nine or Air Lock); the timer runs either way.
  Field& field = battle.field;
  if (field.weather == Weather::None) return std::nullopt;
  if (!field.permanent && --field.weather_turns == 0) {
    field.weather = Weather::None;
    return std::nullopt;
  }
  const Weather weather = field.weather;
  if ((weather != Weather::Sandstorm && weather != Weather::Hail) ||
      effective_weather(battle) == Weather::None) {
    return std::nullopt;
  }
  for (const Player p : {first, foe(first)}) {
    Side& side = battle.side(p);
    const ActivePokemon& a = side.active;
    if (side.get(1).fainted()) continue;
    const bool immune =
        a.volatiles.underground || a.volatiles.underwater ||
        (weather == Weather::Sandstorm
             ? has_type(a, Type::Rock) || has_type(a, Type::Steel) ||
                   has_type(a, Type::Ground) || a.ability == Ability::SandVeil
             : has_type(a, Type::Ice));
    if (immune) continue;
    passive_damage(battle, p, fraction(side.get(1), 16), options);
    if (Flow over = outcome(battle)) return over;
  }
  return std::nullopt;
}

// DoBattlerEndTurnEffects (util:1460) for p's active Pokémon: every case for
// one Pokémon before the next. A Pokémon that faints partway skips the
// rest, as each case checks its HP. Returns the end of the battle if a
// residual KO ends it.
//
// TODO(plan step 4): ENDTURN_UPROAR and ENDTURN_THRASH
// (they read WasUnableToUseMove, README decision 2).
template <typename O>
constexpr Flow battler_residuals(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  Pokemon& mon = side.get(1);
  const auto alive = [&] { return !mon.fainted(); };

  // ENDTURN_INGRAIN (util:1476)
  if (v.rooted && alive() && mon.hp != mon.stats.hp) heal(mon, fraction(mon, 16));

  // ENDTURN_ABILITIES: ABILITYEFFECT_ENDTURN (util:2587).
  if (alive()) {
    switch (a.ability) {
    case Ability::RainDish:
      if (effective_weather(battle) == Weather::Rain && mon.hp != mon.stats.hp) {
        heal(mon, fraction(mon, 16));
      }
      break;
    case Ability::ShedSkin:
      // Random() % 3 == 0, only with a status to cure (util:2607). It cures
      // sleep too, which is public, so the sleep count just resets.
      if (mon.status.any() &&
          rolls::coin(battle, player, &Action::shed_skin, 1, 3, options)) {
        if (mon.status.asleep()) wake(battle, player, options);
        mon.status = Status::none();
      }
      break;
    case Ability::SpeedBoost:
      // Not on the turn it came in (isFirstTurn, decision 14).
      if (a.boosts.spe < 6 && a.first_turn != 2) a.boosts.spe++;
      break;
    case Ability::Truant:
      a.truant = !a.truant;
      break;
    default:
      break;
    }
  }

  // ENDTURN_ITEMS1 (util:1496)
  item_end_turn(battle, player, false, options);

  // ENDTURN_LEECH_SEED (util:1501): max HP / 8 from the seeded Pokémon, then
  // the seeder (the foe's active Pokémon) gains what was lost, or loses it
  // to the seeded Pokémon's Liquid Ooze (BattleScript_LeechSeedTurnDrain,
  // bs1:3265). The seeded Pokémon faints first (bs1:3283).
  const Player seeder = foe(player);
  Pokemon& seeder_mon = battle.side(seeder).get(1);
  if (v.leech_seed && alive() && !seeder_mon.fainted()) {
    const bool ooze = a.ability == Ability::LiquidOoze;
    const uint16_t drained = passive_damage(battle, player, fraction(mon, 8), options);
    if (ooze) {
      passive_damage(battle, seeder, drained, options);
    } else {
      heal(seeder_mon, drained);
    }
    if (Flow over = outcome(battle)) return over;
  }

  // ENDTURN_POISON, BAD_POISON, BURN (util:1517-1552): 1/8; toxic's
  // counter, at most 15, times 1/16; 1/8.
  if (alive() && mon.status.poisoned() && !mon.status.badly_poisoned()) {
    passive_damage(battle, player, fraction(mon, 8), options);
    if (Flow over = outcome(battle)) return over;
  }
  if (alive() && mon.status.badly_poisoned()) {
    if (a.toxic != ActivePokemon::MAX_TOXIC) a.toxic++;
    passive_damage(battle, player, fraction(mon, 16) * a.toxic, options);
    if (Flow over = outcome(battle)) return over;
  }
  if (alive() && mon.status.burned()) {
    passive_damage(battle, player, fraction(mon, 8), options);
    if (Flow over = outcome(battle)) return over;
  }

  // ENDTURN_NIGHTMARES (util:1553): 1/4 while asleep, else it ends.
  if (v.nightmare && alive()) {
    if (mon.status.asleep()) {
      passive_damage(battle, player, fraction(mon, 4), options);
      if (Flow over = outcome(battle)) return over;
    } else {
      v.nightmare = false;
    }
  }

  // ENDTURN_CURSE (util:1574): 1/4.
  if (v.cursed && alive()) {
    passive_damage(battle, player, fraction(mon, 4), options);
    if (Flow over = outcome(battle)) return over;
  }

  // ENDTURN_WRAP (util:1585): the hidden counter ticks first; while it
  // lasts, 1/16, then the Pokémon is freed (chance.hpp, Binding).
  if (v.wrapped != 0 && alive()) {
    v.wrapped--;
    const bool freed = v.wrapped == 0;
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).binding,
                  options.chance.durations.get(player).binding, freed, 3, 6,
                  1, options);
    }
    if (freed) {
      a.wrapped_move = Move::None;
    } else {
      passive_damage(battle, player, fraction(mon, 16), options);
      if (Flow over = outcome(battle)) return over;
    }
  }

  // ENDTURN_DISABLE (util:1692): it ends silently if the Pokémon no longer
  // knows the move (public), else the hidden timer ticks.
  if (a.disable_timer != 0) {
    bool known = false;
    for (const MoveSlot& ms : a.moves) known |= ms.id == a.disabled_move;
    if (!known) {
      a.disabled_move = Move::None;
      a.disable_timer = 0;
      if constexpr (CHANCE<O>) options.chance.durations.get(player).disable = 0;
    } else {
      const bool ended = --a.disable_timer == 0;
      if (ended) a.disabled_move = Move::None;
      if constexpr (CHANCE<O>) {
        rolls::tick(options.chance.actions.get(player).disable,
                    options.chance.durations.get(player).disable, ended, 2, 5,
                    1, options);
      }
    }
  }

  // ENDTURN_ENCORE (util:1713): it ends if the move was overwritten or its
  // PP ran out (public), else the hidden timer ticks.
  if (a.encore_timer != 0) {
    const MoveSlot& ms = a.moves[a.encored_slot];
    const bool lost = ms.id != a.encored_move || ms.pp == 0;
    const bool ended = lost || --a.encore_timer == 0;
    if constexpr (CHANCE<O>) {
      if (lost) {
        options.chance.durations.get(player).encore = 0;
      } else {
        rolls::tick(options.chance.actions.get(player).encore,
                    options.chance.durations.get(player).encore, ended, 3, 6,
                    1, options);
      }
    }
    if (ended) {
      a.encored_move = Move::None;
      a.encore_timer = 0;
    }
  }

  // ENDTURN_LOCK_ON, CHARGE, TAUNT (util:1734-1748). Lock-On lives on the
  // target here (README decision 9), so it ticks in the target's pass; in
  // singles the order can't matter.
  if (v.lock_on != 0) v.lock_on--;
  if (a.charge_timer != 0 && --a.charge_timer == 0) v.charged_up = false;
  if (a.taunt != 0) a.taunt--;

  // ENDTURN_YAWN (util:1752): the Pokémon falls asleep the turn after,
  // unless it has a status, Vital Spirit or Insomnia, or an Uproar is on.
  // Safeguard doesn't stop it, but Sleep Clause does (Showdown's yawn ends
  // with trySetStatus from the user, which the clause checks).
  if (v.yawn != 0 && --v.yawn == 0 && alive() &&
      can_status(battle, player, Secondary::Sleep) &&
      !sleep_clause(battle, player)) {
    MoveState none{};
    inflict(battle, player, Secondary::Sleep, none, options);
  }

  // ENDTURN_ITEMS2 (util:1501): the cures again, for statuses the passes
  // above set.
  item_end_turn(battle, player, true, options);
  return std::nullopt;
}

// Perish Song's count for p's active Pokémon, as Showdown (decided
// 2026-10-07): its gen4 condition (onResidualOrder 12,
// data/mods/gen4/moves.ts:1017) counts down in the residual pass, after the
// per-Pokémon residuals and in their order, so its faints are replaced with
// theirs. The decomp's HandleWishPerishSongOnTurnEnd (util:1825) runs it
// after the residual faints are replaced, with a request of its own. The
// count shows 3, 2, 1; at 0 the Pokémon faints, and the battle can end
// there. Showdown runs Speed Boost, Truant's toggle and White Herb after it
// (order 27-29); that changes only the log. Log: Start perishN, or the
// faint.
template <typename O>
constexpr Flow perish_count(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  Pokemon& mon = battle.side(player).get(1);
  if (!a.volatiles.perish_song || mon.fainted()) return std::nullopt;
  if (a.perish_song_timer != 0) {
    a.perish_song_timer--;
    return std::nullopt;
  }
  a.volatiles.perish_song = false;
  passive_damage(battle, player, mon.hp, options);
  return outcome(battle);
}

// HandleEndTurn_ContinueBattle (main:3932), then BattleTurnPassed's residuals
// (main:3956), Perish Song's count and HandleFaintedMonActions.
template <typename O> constexpr Flow end_of_turn(Battle& battle, O& options) {
  for (Side& side : battle.sides) {
    ActivePokemon& a = side.active;
    a.volatiles.flinched = false;
    if (side.get(1).status.asleep() && a.volatiles.multiple_turns) {
      a.cancel_multi_turn_moves();
    }
  }
  // ENDTURN_ORDER (util:1209): GetWhoStrikesFirst with the chosen moves'
  // priority and the turn's Quick Claw draw, and its own tie draw, taken
  // only when the order can matter.
  const Request& r = battle.request;
  const Player first =
      residual_order_matters(battle)
          ? detail::first(battle, speeds(battle, r.move[0], r.move[1], true),
                          true, options)
          : Player::P1;
  if (Flow over = field_residuals(battle, first, options)) return over;
  for (const Player p : {first, foe(first)}) {
    if (Flow over = battler_residuals(battle, p, options)) return over;
  }
  for (const Player p : {first, foe(first)}) {
    if (Flow over = perish_count(battle, p, options)) return over;
  }
  return faints(battle, Phase::FaintAfterResiduals);
}

// HandleWishPerishSongOnTurnEnd (util:1781), Future Sight and Doom Desire.
// TODO(plan step 4).
template <typename O> constexpr Flow future_sight(Battle&, O&) {
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
      // setup is only a message (bs1:3709), which the decomp skips for a
      // sleeping user (main:4915) and Showdown doesn't (its focuspunch
      // condition's onStart; Log: SingleTurn Focus Punch). Then
      // TryClearRageStatuses.
      for (Side& side : battle.sides) {
        const Move chosen = battle.request.move[&side - battle.sides];
        if (chosen != Move::Rage) side.active.volatiles.rage = false;
      }
    } else {
      // Finish Baton Pass's switch-in. A recipient that faints to Spikes
      // ends the actions, as any faint.
      Request& r = battle.request;
      const Player passer = r.next == 1 ? r.first : foe(r.first);
      baton_pass_in(battle, passer, (passer == Player::P1 ? c1 : c2).data(),
                    options);
      from = r.next;
      r.phase = Phase::None;
      r.next = 0;
      if ((f = faints(battle, Phase::FaintAfterAction))) return checked(battle, *f);
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
    return checked(battle, next_turn(battle));
  }
  assert(false && "invalid phase");
  return Result::error();
}

} // namespace pkmn::gen3
