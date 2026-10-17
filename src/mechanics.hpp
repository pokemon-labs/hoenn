#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "data.hpp"
#include "options.hpp"
#include "calc.hpp"
#include "chance.hpp"
#include "damage.hpp"
#include "protocol.hpp"

namespace hoenn {

inline constexpr uint8_t MAX_CHOICES = 9;

namespace detail {

template <typename O>
inline constexpr bool CHANCE = std::remove_cvref_t<O>::ChanceType::enabled;
template <typename O>
inline constexpr bool CALC = std::remove_cvref_t<O>::CalcType::enabled;

using Flow = std::optional<Result>;

constexpr uint8_t index(Player p) { return uint8_t(p); }

// Showdown's gen3 Endure keeps the base data's +4: its gen4 mod lowers
// Protect and Detect to +3 but not Endure. The decomp has all three at +3,
// so Endure goes first there only by speed. Visible in the order: this
// follows Showdown (README shortlist).
constexpr int8_t priority(Move move) {
  if (move == Move::Endure) return 4;
  return move == Move::None ? 0 : get(move).priority;
}

constexpr bool holds(const Side& side, HoldEffect effect) {
  return item_data(side.get(1).item).effect == effect;
}

constexpr bool has_type(const ActivePokemon& active, Type type) {
  return active.types[0] == type || active.types[1] == type;
}

// Whether the last move to hit `a` can matter: it knows Mirror Move, or
// Metronome, which can call it. Showdown keeps attackedBy for every
// Pokémon; here last_taken_move is kept only where Mirror Move can read it,
// so the accuracy roll of a move that changes nothing else stays log-only.
// Mimic, Sketch and Transform count too: they can gain Mirror Move later.
constexpr bool mirrors(const ActivePokemon& a) {
  for (const MoveSlot& ms : a.moves) {
    if (ms.id == Move::MirrorMove || ms.id == Move::Metronome || ms.id == Move::Mimic ||
        ms.id == Move::Sketch || ms.id == Move::Transform) {
      return true;
    }
  }
  return false;
}

constexpr bool two_turn(Effect effect) {
  return effect == Effect::SolarBeam || effect == Effect::RazorWind ||
         effect == Effect::SkullBash || effect == Effect::SkyAttack ||
         effect == Effect::SemiInvulnerable;
}

// Charging, rampage, Bide, Rollout, Uproar and recharge: the only choice is
// Move(0), and the action is the locked move (main:4160, util:107).
constexpr bool forced(const ActivePokemon& active) {
  return active.volatiles.multiple_turns || active.volatiles.recharge;
}

// The moves Metronome can call: Showdown's gen3 metronome flag, which is
// every cartridge move but the decomp's sMovesForbiddenToCopy (the typed
// Hidden Powers are the generic one, the caller's type).
constexpr bool metronome_can_call(Move m) {
  return m != Move::None && uint16_t(m) < CARTRIDGE_MOVES &&
         !(get(m).extra & Extra::NoMetronome);
}

struct MetronomeMoves {
  Move moves[CARTRIDGE_MOVES];
  uint16_t n;
};

inline constexpr MetronomeMoves METRONOME_MOVES = [] {
  MetronomeMoves list{};
  for (uint16_t i = 1; i < CARTRIDGE_MOVES; i++) {
    if (metronome_can_call(Move(i))) list.moves[list.n++] = Move(i);
  }
  return list;
}();
static_assert(METRONOME_MOVES.n == 336);

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
// the turn (main:4013). Drawn once, kept in the request. The residuals draw
// it before they know whether it matters (residual_speeds), so the draw
// (quick_claw_draw) and its record (quick_claw_keep) are apart.
template <typename O> constexpr bool quick_claw_draw(Battle& battle, O& options) {
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.turn.quick_claw;
  assert(forced <= Optional::False);
  return forced != Optional::None ? forced == Optional::True : battle.rng.chance(1, 5);
}

template <typename O>
constexpr void quick_claw_keep(Battle& battle, bool proc, O& options) {
  assert(battle.request.quick_claw == Optional::None);
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.turn.quick_claw, optional(proc));
    observe(options, proc ? 1 : 4, 5);
  }
  battle.request.quick_claw = optional(proc);
}

template <typename O> constexpr bool quick_claw(Battle& battle, O& options) {
  const Optional drawn = battle.request.quick_claw;
  if (drawn != Optional::None) return drawn == Optional::True;
  const bool proc = quick_claw_draw(battle, options);
  quick_claw_keep(battle, proc, options);
  return proc;
}

// A speed tie: `Random() & 1` in GetWhoStrikesFirst (main:4728, 4745), for
// the move order (or two switches). Returns who goes first.
template <typename O> constexpr Player speed_tie(Battle& battle, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.turn.speed_tie;
  assert(forced <= 2);
  const bool p1 = forced != 0 ? forced == 1 : battle.rng.chance(1, 2);

  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.turn.speed_tie, uint8_t(p1 ? 1 : 2));
    observe(options, 1, 2);
  }
  return p1 ? Player::P1 : Player::P2;
}

// A residual stage's tie coin, where the order changes the state (Showdown's
// speedSort among tied handlers; the end of turn, stage_order_matters):
// Leech Seed's in bits 0-1 of Turn::residual_tie, the damage stages' in bits
// 2-3, each 1 = P1 first, 2 = P2 first. Leech Seed is one stage, and a
// damage stage's order matters only when it ends the battle, so each field
// is drawn at most once per update.
template <typename O>
constexpr Player residual_tie(Battle& battle, bool leech_seed, O& options) {
  const unsigned shift = leech_seed ? 0 : 2;
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) {
    forced = uint8_t((options.calc.overrides.turn.residual_tie >> shift) & 3);
  }
  assert(forced <= 2);
  const bool p1 = forced != 0 ? forced == 1 : battle.rng.chance(1, 2);

  if constexpr (detail::CHANCE<O>) {
    uint8_t& field = options.chance.actions.turn.residual_tie;
    assert(((field >> shift) & 3) == 0 && "Actions field set twice in one update");
    field = uint8_t(field | (p1 ? 1 : 2) << shift);
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

template <typename O>
constexpr bool thaw(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::thaw, 1, 5, options);
}

template <typename O>
constexpr bool paralysis(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::paralysis, 1, 4, options);
}

template <typename O>
constexpr bool confusion(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::confusion, 1, 2, options);
}

template <typename O>
constexpr bool infatuation(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::infatuation, 1, 2, options);
}

template <typename O>
constexpr bool hit(Battle& battle, Player p, uint32_t calc, O& options) {
  assert(calc > 0 && calc < 100);
  return coin(battle, p, &Action::hit, calc, 100, options);
}

// A move's secondary effect on hit `i`: Random() % 100 < chance
// (seteffectwithchance, bsc:2923). Called only for 0 < chance < 100 and when
// the effect could take hold. Per hit, as Showdown's secondaries (only
// Twineedle has a secondary and more than one hit); the decomp draws once
// after its multi-hit loop.
template <typename O>
constexpr bool secondary(Battle& battle, Player p, uint8_t i, uint32_t chance,
                         O& options) {
  assert(chance > 0 && chance < 100 && i < MAX_HITS);
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].secondary;
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) field = &options.chance.actions.get(p).hits[i].secondary;
  return coin(battle, forced, field, chance, 100, options);
}

// The attacker's King's Rock flinch on hit `i`, `chance` of 100 (util:3763).
// Called only when the flinch could take hold.
template <typename O>
constexpr bool kings_rock(Battle& battle, Player p, uint8_t i, uint32_t chance,
                          O& options) {
  assert(chance > 0 && chance < 100 && i < MAX_HITS);
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].kings_rock;
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) field = &options.chance.actions.get(p).hits[i].kings_rock;
  return coin(battle, forced, field, chance, 100, options);
}

// Triple Kick's accuracy check before hit `i` (1 or 2), on `calc` of 100
// outcomes (Showdown's multiaccuracy; the decomp's accuracycheck inside
// BattleScript_TripleKickLoop, bs1:1397). A miss ends the loop. Actions
// keeps the hits that landed in multi_hit (1-3), written by the caller once
// the loop ends (kicks()); a forced count f makes the checks before hits
// 1..f-1 hit and the one before hit f miss.
template <typename O>
constexpr bool kick(Battle& battle, Player p, uint8_t i, uint32_t calc,
                    O& options) {
  assert(i >= 1 && i <= 2 && calc > 0 && calc < 100);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).multi_hit;
  assert(forced <= 3);
  const bool yes = forced != 0 ? i < forced : battle.rng.chance(calc, 100);
  observe(options, yes ? calc : 100 - calc, 100);
  return yes;
}

// Records Triple Kick's landed hits, 1-3 (see kick()).
template <typename O>
constexpr void kicks(Player p, uint8_t n, O& options) {
  assert(n >= 1 && n <= 3);
  if constexpr (detail::CHANCE<O>) record(options.chance.actions.get(p).multi_hit, n);
  (void)p, (void)n, (void)options;
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

// Present, Magnitude and Psywave's outcome (Action::variable), as Showdown's
// draws, which differ from the decomp's:
// - Present's random(10): 1 heals (2/10), 2, 3, 4 hit with 40, 80, 120 power
//   (4/10, 3/10, 1/10). The decomp's Random() & 0xFF gives 52, 102, 76 and
//   26 of 256 (bsc:8613).
// - Magnitude's random(100): the magnitude, 4-10, by MAGNITUDE's bounds (the
//   same as the decomp's, bsc:8670).
// - Psywave's random(50, 151): the percentage of the user's level, 50-150
//   (the decomp's steps of 10, bsc:7936).
// variable() draws it, or takes the override; it's recorded by
// variable_matters() once its outcome can change the state (Magnitude's is
// drawn and logged before the accuracy roll).
template <typename O>
constexpr uint8_t variable(Battle& battle, Player p, Effect effect, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).variable;
  if (effect == Effect::Present) {
    assert(forced <= 4);
    if (forced != 0) return forced;
    const uint32_t r = battle.rng.range(10);
    return r < 2 ? 1 : r < 6 ? 2 : r < 9 ? 3 : 4;
  }
  if (effect == Effect::Magnitude) {
    assert(forced == 0 || (forced >= 4 && forced <= 10));
    if (forced != 0) return forced;
    const uint32_t r = battle.rng.range(100);
    for (const MagnitudeEntry& e : MAGNITUDE) {
      if (r < e.bound) return e.magnitude;
    }
    assert(false);
  }
  assert(effect == Effect::Psywave);
  assert(forced == 0 || (forced >= 50 && forced <= 150));
  return forced != 0 ? forced : uint8_t(50 + battle.rng.range(101));
}

template <typename O>
constexpr void variable_matters(Player p, Effect effect, uint8_t value, O& options) {
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).variable, value);
    if (effect == Effect::Present) {
      constexpr uint8_t TENTHS[4] = {2, 4, 3, 1};
      observe(options, TENTHS[value - 1], 10);
    } else if (effect == Effect::Magnitude) {
      const uint8_t i = uint8_t(value - 4);
      observe(options, MAGNITUDE[i].bound - (i == 0 ? 0 : MAGNITUDE[i - 1].bound), 100);
    } else {
      observe(options, 1, 101);
    }
  }
  (void)p, (void)effect, (void)value, (void)options;
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

// The hit index of a future attack's landing: its draws are Action::future.
inline constexpr uint8_t FUTURE = MAX_HITS;

constexpr HitDraws& draws(Action& a, uint8_t i) {
  assert(i <= FUTURE);
  return i == FUTURE ? a.future : a.hits[i];
}

// The defender's Focus Band on hit `i` (or FUTURE): Random() % 100 < param
// (bsc:1677). Called only when the hit would be lethal.
template <typename O>
constexpr bool focus_band(Battle& battle, Player p, uint8_t i, uint8_t param,
                          O& options) {
  assert(i <= FUTURE);
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) {
    forced = draws(options.calc.overrides.get(p), i).focus_band;
  }
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) {
    field = &draws(options.chance.actions.get(p), i).focus_band;
  }
  return coin(battle, forced, field, param, 100, options);
}

// A future attack's accuracy at its landing, on its user's side: a hit on
// `calc` of 100 outcomes. Called only for 0 < calc < 100.
template <typename O>
constexpr bool future_hit(Battle& battle, Player p, uint32_t calc, O& options) {
  assert(calc > 0 && calc < 100);
  return coin(battle, p, &Action::future_hit, calc, 100, options);
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

// Metronome's pick (Showdown's gen3: sample over the moves with the
// metronome flag, the decomp's sMovesForbiddenToCopy left out, the user's
// own moves included): uniform over METRONOME_MOVES.
template <typename O> constexpr Move metronome(Battle& battle, Player p, O& options) {
  Move forced = Move::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).metronome.get();
  const Move move = forced != Move::None
                        ? forced
                        : detail::METRONOME_MOVES.moves[battle.rng.range(detail::METRONOME_MOVES.n)];
  assert(detail::metronome_can_call(move));
  if constexpr (detail::CHANCE<O>) {
    MovePick& field = options.chance.actions.get(p).metronome;
    assert(field.get() == Move::None && "Actions field set twice in one update");
    field.set(move);
    observe(options, 1, detail::METRONOME_MOVES.n);
  }
  return move;
}

// A uniform pick from the `n` entries of `list`, which may repeat (Assist's
// moves, Conversion's types), or the entry equal to `*forced` (the Calc
// override). Its probability is its value's count over n: the value is
// what's recorded. Returns its index.
template <typename T, typename O>
constexpr uint8_t sample(Battle& battle, const T* list, uint8_t n, const T* forced,
                         O& options) {
  assert(n != 0);
  uint8_t k = n;
  if (forced != nullptr) {
    for (uint8_t i = 0; i < n; i++) {
      if (list[i] == *forced) k = i;
    }
    assert(k < n);
  } else {
    k = uint8_t(battle.rng.range(n));
  }
  if constexpr (detail::CHANCE<O>) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < n; i++) count = uint8_t(count + (list[i] == list[k]));
    observe(options, count, n);
  }
  (void)options;
  return k;
}

// Assist's pick: Showdown's gen4 Assist (gen3 inherits), sample over every
// move of the user's party but the user, fainted Pokémon included, with
// repeats.
template <typename O>
constexpr Move assist(Battle& battle, Player p, const Move* list, uint8_t n, O& options) {
  Move forced = Move::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).assist.get();
  const Move move =
      list[sample(battle, list, n, forced == Move::None ? nullptr : &forced, options)];
  if constexpr (detail::CHANCE<O>) {
    MovePick& field = options.chance.actions.get(p).assist;
    assert(field.get() == Move::None && "Actions field set twice in one update");
    field.set(move);
  }
  return move;
}

// Conversion's or Conversion 2's type: Showdown's sample over `list`
// (Action::variable holds the type + 1).
template <typename O>
constexpr Type conversion(Battle& battle, Player p, const Type* list, uint8_t n, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).variable;
  assert(forced <= TYPES);
  const Type f = Type(forced - 1);
  const Type type = list[sample(battle, list, n, forced == 0 ? nullptr : &f, options)];
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).variable, uint8_t(uint8_t(type) + 1));
  }
  return type;
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

// Spite's PP, 2-5 (Showdown's gen3 random(2, 6)). Called only where the
// roll can change the PP taken: the move has at least 3 left.
template <typename O> constexpr uint8_t spite(Battle& battle, Player p, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).spite;
  assert(forced == 0 || (forced >= 2 && forced <= 5));
  const uint8_t n = forced != 0 ? forced : uint8_t(2 + battle.rng.range(4));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).spite, n);
    observe(options, 1, 4);
  }
  return n;
}

// A 2-5 hit move's count: Cmd_setmultihitcounter (bsc:7139) draws twice
// for 2 or 3 at 3/8 each and 4 or 5 at 1/8 each; Showdown's gen3 samples
// [2, 2, 2, 3, 3, 3, 4, 5], the same.
template <typename O>
constexpr uint8_t multi_hit(Battle& battle, Player p, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).multi_hit;
  assert(forced == 0 || (forced >= 2 && forced <= 5));
  uint8_t n = forced;
  if (n == 0) {
    const uint32_t r = battle.rng.range(8);
    n = r < 3 ? 2 : r < 6 ? 3 : r == 6 ? 4 : 5;
  }
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).multi_hit, n);
    observe(options, n <= 3 ? 3 : 1, 8);
  }
  return n;
}

// Effect Spore's status: Random() & 3 redrawn while 0, so 1 sleep, 2
// poison, 3 paralysis (MOVE_EFFECT_BURN replaced, util:2776); Showdown
// samples the same three.
template <typename O>
constexpr uint8_t spore(Battle& battle, Player p, uint8_t i, O& options) {
  assert(i < MAX_HITS);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].spore;
  assert(forced <= 3);
  const uint8_t which = forced != 0 ? forced : uint8_t(1 + battle.rng.range(3));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).hits[i].spore, which);
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
// paralysis. The badge boost is off in link battles. The decomp floors after
// each factor (the weather doubling before the stage). This follows
// Showdown's arithmetic instead, which changes the order: the stage first
// (getStat, floored), then the modifiers chained in 4096ths and rounded
// once, half down (ModifySpe: Swift Swim and Chlorophyll 2, Macho Brace 0.5,
// gen4 mod's paralysis 0.25; Battle.modify). All are powers of two, so the
// chain is exact. Paralysis differs most: 147 and 149 both give 37.
constexpr uint32_t speed(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  const ActivePokemon& active = side.active;
  const Weather weather = effective_weather(battle);
  const Fraction& stage = STAT_STAGE_RATIOS[unsigned(active.boosts.spe + 6) % 13];
  const uint32_t spe = active.stats.spe * stage.num / stage.den;
  uint32_t modifier = 4096;
  if ((active.ability == Ability::SwiftSwim && weather == Weather::Rain) ||
      (active.ability == Ability::Chlorophyll && weather == Weather::Sun)) {
    modifier *= 2;
  }
  if (holds(side, HoldEffect::MachoBrace)) modifier /= 2;
  if (side.get(1).status.paralyzed()) modifier /= 4;
  return (spe * modifier + 2047) / 4096;
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
constexpr Player first(Battle& battle, const Speeds& s, O& options) {
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
  return rolls::speed_tie(battle, options);
}

// The order of two events whose order can't change the state, only the
// log's order of lines (two arrivals or residual passes that don't
// interact): Speed, and on a tie a coin that isn't recorded, since its
// outcome can't matter to the state (CLAUDE.md, "Chance"). Showdown orders
// them by Speed with a random tie.
constexpr Player speed_order(Battle& battle, uint32_t a, uint32_t b) {
  if (a != b) return a > b ? Player::P1 : Player::P2;
  return battle.rng.chance(1, 2) ? Player::P1 : Player::P2;
}

constexpr Player log_order(Battle& battle) {
  return speed_order(battle, speed(battle, Player::P1), speed(battle, Player::P2));
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
  r.first = first(battle, speeds(battle, r.move[0], r.move[1], true), options);
}

// Whether two moves are the same move: the typed Hidden Powers are all the
// cartridge's MOVE_HIDDEN_POWER (and Showdown's `hiddenpower`).
constexpr bool same_move(Move a, Move b) {
  return a == b || (is_hidden_power(a) && is_hidden_power(b));
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
      for (const MoveSlot& fm : f.moves) imprisoned |= same_move(fm.id, m);
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
      // A two-turn move's charge lasts as long (charge()): Showdown's
      // twoturnmove and the move's own volatile have duration 2, so a
      // second turn that never came (a faint ended the actions first) ends
      // with the turn. The decomp keeps the lock.
      if (two_turn(get(a.locked_move).effect)) {
        a.volatiles.multiple_turns = false;
        a.volatiles.on_air = false;
        a.volatiles.underground = false;
        a.volatiles.underwater = false;
      }
    }
    if (a.substitute_hp == 0) a.volatiles.substitute = false;
    // Showdown's endTurn drops an attackedBy entry whose source isn't active
    // (sim/battle.ts:1707): one that left and came back keeps it.
    const Side& other = battle.sides[&side == &battle.sides[0] ? 1 : 0];
    if (a.last_taken_from != other.order[0]) {
      a.last_taken_move = Move::None;
      a.last_taken_from = 0;
    }
    // Showdown's choicelock onDisableMove (cancel()).
    bool known = false;
    for (const MoveSlot& ms : a.moves) known |= ms.id == a.choice_move;
    if (!known || !holds(side, HoldEffect::ChoiceBand)) a.choice_move = Move::None;
  }
}

// Forecast, as Showdown's onWeatherChange (data/abilities.ts:1476): an
// active Castform with Forecast takes the type of the effective weather
// `w` (Fire in sun, Water in rain, Ice in hail, otherwise Normal). Showdown
// runs it at Castform's own switch-in, after every weather change, and when
// a Cloud Nine or Air Lock Pokémon leaves (its End); in gen3 one entering
// doesn't (data/mods/gen4/abilities.ts:37), so Castform keeps its form
// until the next change. The decomp's CastformDataTypeChange (util:2504)
// also runs at the end of every move; this follows Showdown. The form is
// kept as the type (no species change: Castform's forms share its stats).
// Log: -formechange, or after `after`'s Faint if set.
template <typename O>
constexpr void forecast(Battle& battle, Player p, Weather w, O& options,
                        ID after = protocol::NO_ID) {
  ActivePokemon& a = battle.side(p).active;
  // Not a transformed Pokémon (onWeatherChange, data/abilities.ts:1477).
  if (a.species != Species::Castform || a.ability != Ability::Forecast ||
      a.volatiles.transformed || battle.side(p).get(1).fainted()) {
    return;
  }
  const Type t = w == Weather::Sun    ? Type::Fire
                 : w == Weather::Rain ? Type::Water
                 : w == Weather::Hail ? Type::Ice
                                      : Type::Normal;
  if (a.types[0] == t && a.types[1] == t) return;
  a.types[0] = t;
  a.types[1] = t;
  if (after.bits != 0) {
    protocol::forme_change_after(options.log, after, protocol::ident(battle, p), t);
  } else {
    protocol::forme_change(options.log, protocol::ident(battle, p), t);
  }
}

// Showdown's eachEvent('WeatherChange'): Forecast on both sides, in Speed
// order (a tie's coin only orders the log). `leaving`, if set, is a Pokémon
// whose Cloud Nine or Air Lock is ending, so it no longer suppresses.
template <typename O>
constexpr void weather_change(Battle& battle, O& options,
                              std::optional<Player> leaving = std::nullopt) {
  const bool p1 = battle.sides[0].active.species == Species::Castform;
  const bool p2 = battle.sides[1].active.species == Species::Castform;
  if (!p1 && !p2) return;
  Weather w = effective_weather(battle);
  if (leaving) {
    const Side& other = battle.side(foe(*leaving));
    const Ability ab = other.active.ability;
    const bool suppressed =
        (ab == Ability::CloudNine || ab == Ability::AirLock) && !other.get(1).fainted();
    w = suppressed ? Weather::None : battle.field.weather;
  }
  const Player first = p1 && p2 ? log_order(battle) : p1 ? Player::P1 : Player::P2;
  forecast(battle, first, w, options);
  forecast(battle, foe(first), w, options);
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
  protocol::faint(options.log, protocol::ident(battle, player));
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
  // Showdown's faintMessages runs the ability's End, after the faint line
  // (only the foe can be a Castform).
  if (a.ability == Ability::CloudNine || a.ability == Ability::AirLock) {
    forecast(battle, foe(player), effective_weather(battle), options,
             protocol::ident(battle, player));
  }

  ActivePokemon& f = battle.side(foe(player)).active;
  f.volatiles.escape_prevention = false;
  // Showdown keeps the foe's attract until the replacement comes in
  // (leave()): its onUpdate runs then.
  // Showdown keeps the foe's partiallytrapped until its residual (Stage::Bind).
  if (f.volatiles.wrapped != 0) f.volatiles.wrapper_gone = true;

  if constexpr (CHANCE<O>) {
    Durations::Side& d = options.chance.durations.get(player);
    d.sleep[0] = 0;
    d.confusion = 0;
    d.disable = 0;
    d.encore = 0;
    d.binding = 0;
    d.attacking = 0;
  }
}

constexpr void deduct_pp(Battle& battle, Player player, uint8_t slot, uint8_t n);

// tryfaintmon BS_TARGET after p's move hit the foe (bsc:2965): the foe
// faints if it's at 0 HP, and if it had used Destiny Bond, p faints too
// unless it already has (BattleScript_DestinyBondTakesLife, bs1:3379). The
// decomp arms Destiny Bond when the move passes its accuracy check
// (TrySetDestinyBondToHappen, bsc:1020); Showdown checks the volatile when
// the foe faints to a move (data/moves.ts destinybond onFaint), which is the
// same in singles. A future attack doesn't trigger it in Showdown.
//
// Showdown's onFaint prints the -activate even when the user has already
// fainted (Explosion, recoil): the queued Faint carries it (protocol::faint).
//
// Grudge, as Showdown's onFaint: the user's last move (`used`, Showdown's
// lastMove: Sleep Talk for a move it called) loses all its PP, unless the
// user's own faint came first (Explosion). The loss waits for the end of
// moveend (`grudge`, MoveState::grudge).
//
// `bonded_hp` keeps the user's HP before Destiny Bond takes it, for Shell
// Bell's log (moveend).
template <typename O>
constexpr void faint_target(Battle& battle, Player player, Move used, bool& grudge,
                            uint16_t& bonded_hp, O& options) {
  const Player defender = foe(player);
  if (!battle.side(defender).get(1).fainted()) return;
  const bool bond = battle.side(defender).active.volatiles.destiny_bond;
  if (bond) protocol::faint(options.log, protocol::ident(battle, defender), true);
  if (battle.side(defender).active.volatiles.grudge &&
      get(used).effect != Effect::Explosion) {
    ActivePokemon& a = battle.side(player).active;
    for (uint8_t s = 0; s < 4; s++) {
      if (a.moves[s].id != used || a.moves[s].pp == 0) continue;
      grudge = true;
      protocol::faint(options.log, protocol::ident(battle, defender));
      protocol::grudge(options.log, protocol::ident(battle, defender),
                       protocol::ident(battle, player), used);
    }
  }
  faint(battle, defender, options);
  Pokemon& user = battle.side(player).get(1);
  if (bond && !user.fainted()) {
    bonded_hp = user.hp;
    user.hp = 0;
    faint(battle, player, options);
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
  protocol::damage(options.log, protocol::ident(battle, player), mon,
                   protocol::Damage::Spikes);
  if (!mon.fainted()) return false;
  faint(battle, player, options);
  return true;
}

// Cmd_removeitem (bsc:5901): the item is used up, and Recycle remembers it.
constexpr void consume_item(Side& side) {
  side.recycle_item = side.get(1).item;
  side.get(1).item = Item::None;
}

constexpr bool is_berry(Item i) { return i >= Item::CheriBerry && i <= Item::EnigmaBerry; }

// p's active Pokémon uses up its item: Showdown's eatItem for a berry
// (-enditem [eat]) or useItem (-enditem), both before the item's effect
// (sim/pokemon.ts:1783, 1831).
template <typename O>
constexpr void use_item(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  const Item item = side.get(1).item;
  protocol::end_item(options.log, protocol::ident(battle, player), item,
                     is_berry(item) ? protocol::EndItemFrom::Eat : protocol::EndItemFrom::None);
  consume_item(side);
}

// What ending p's active Pokémon's sleep clears besides the status: the
// Nightmare, the Sleep Talk turns kept for switch-out (sleep_skipped, which
// a later sleep mustn't inherit) and, since the cure is public, the sleep
// count. Callers clear the status, and call this after its -curestatus:
// Showdown's cureStatus and clearStatus print -end Nightmare [silent] after
// it (sim/pokemon.ts:1677).
template <typename O>
constexpr void wake(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.volatiles.nightmare) {
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Nightmare);
  }
  a.volatiles.nightmare = false;
  a.sleep_skipped = 0;
  if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 0;
}

// The cures ITEMEFFECT_NORMAL (util:3485-3606) and ITEMEFFECT_MOVE_END
// (util:3627-3752) share. Both run White Herb too, which here waits for
// Showdown's checks (white_herb). Cured sleep and confusion are public, so
// their counts reset. Returns whether the item was used.
template <typename O>
constexpr bool item_cure(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  // The berries' removeVolatile('confusion'): -end confusion.
  const auto unconfuse = [&] {
    if (v.confusion == 0) return;
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Confusion);
    v.confusion = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).confusion = 0;
  };
  // Showdown's berries cure with Pokemon.cureStatus: -curestatus [msg],
  // after the item's -enditem.
  const auto cure = [&] {
    use_item(battle, player, options);
    protocol::cure_status(options.log, protocol::ident(battle, player), mon.status);
    mon.status = Status::none();
  };
  switch (item_data(mon.item).effect) {
  case HoldEffect::CurePar:
    if (!mon.status.paralyzed()) return false;
    cure();
    break;
  case HoldEffect::CurePsn:
    if (!mon.status.poisoned()) return false;
    cure();
    a.toxic = 0;
    break;
  case HoldEffect::CureBrn:
    if (!mon.status.burned()) return false;
    cure();
    break;
  case HoldEffect::CureFrz:
    if (!mon.status.frozen()) return false;
    cure();
    break;
  case HoldEffect::CureSlp:
    if (!mon.status.asleep()) return false;
    cure();
    wake(battle, player, options);
    break;
  case HoldEffect::CureConfusion:
    if (v.confusion == 0) return false;
    use_item(battle, player, options);
    unconfuse();
    break;
  case HoldEffect::CureStatus: {
    // Lum Berry's onEat: cureStatus, then removeVolatile('confusion').
    if (!mon.status.any() && v.confusion == 0) return false;
    const bool asleep = mon.status.asleep();
    use_item(battle, player, options);
    if (mon.status.any()) {
      protocol::cure_status(options.log, protocol::ident(battle, player), mon.status);
      mon.status = Status::none();
    }
    if (asleep) wake(battle, player, options);
    a.toxic = 0;
    unconfuse();
    break;
  }
  case HoldEffect::RestorePp: {
    // Leppa Berry, as Showdown's (onUpdate, data/items.ts:3360): once a
    // move slot is at 0 PP, 10 PP back to the first such slot, up to its
    // maximum (a Transformed or Mimicked slot's maximum is taken from its
    // PP Ups too, unverified against Showdown). The decomp waits for the
    // end of the turn (ITEMEFFECT_NORMAL); Showdown eats it at once.
    uint8_t slot = 0;
    while (slot < 4 && (a.moves[slot].id == Move::None || a.moves[slot].pp != 0)) slot++;
    if (slot == 4) return false;
    use_item(battle, player, options);
    MoveSlot& ms = a.moves[slot];
    const uint8_t max = max_pp(ms.id, ms.pp_ups);
    ms.pp = uint8_t(max < 10 ? max : 10);
    if (!a.volatiles.transformed && !(a.mimicked & (1 << slot))) mon.moves[slot].pp = ms.pp;
    protocol::activate_with(options.log, protocol::ident(battle, player),
                            protocol::Activate::Leppa, ms.id);
    break;
  }
  case HoldEffect::CureAttract: {
    // Mental Herb (gen4 onUpdate): useItem's -enditem, removeVolatile's
    // silent -end, then its own.
    if (!v.infatuated) return false;
    use_item(battle, player, options);
    v.infatuated = false;
    const ID id = protocol::ident(battle, player);
    protocol::end_effect(options.log, id, protocol::End::Attract);
    protocol::end_effect(options.log, id, protocol::End::AttractMentalHerb);
    break;
  }
  default:
    return false;
  }
  return true;
}

// White Herb (HOLD_EFFECT_RESTORE_STATS): every lowered stage back to 0.
// The decomp checks it with the other cures (item_cure) and at switch-in;
// Showdown's has no onUpdate, only onStart (at switch-in, or when Trick
// hands it over), onAnySwitchIn, onAnyAfterMove and a residual at order 29
// (data/items.ts:7679), and this follows it. Log: EndItem, then
// -clearnegativeboost (its onUse).
template <typename O>
constexpr void white_herb(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  if (!holds(side, HoldEffect::RestoreStats) || side.get(1).fainted()) return;
  bool lowered = false;
  const auto reset = [&](int8_t& b) {
    if (b < 0) {
      b = 0;
      lowered = true;
    }
  };
  Boosts& b = side.active.boosts;
  reset(b.atk), reset(b.def), reset(b.spe), reset(b.spa), reset(b.spd);
  reset(b.acc), reset(b.eva);
  if (!lowered) return;
  use_item(battle, player, options);
  protocol::clear_negative_boost(options.log, protocol::ident(battle, player));
}

// Both White Herbs, by Speed (an onAny handler on each holder): the order
// changes only the log.
template <typename O> constexpr void white_herbs(Battle& battle, O& options) {
  const Player first = log_order(battle);
  white_herb(battle, first, options);
  white_herb(battle, foe(first), options);
}

constexpr bool lost(const Side& side) {
  for (const Pokemon& p : side.pokemon) {
    if (p.species != Species::None && !p.fainted()) return false;
  }
  return true;
}

// Cmd_checkteamslost (bsc:3534): both sides out is a draw (B_OUTCOME_DREW
// is WON | LOST), whoever fainted first, Explosion included. Showdown agrees
// for Gen III.
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

// Showdown's TryBoost (sim/battle.ts:2038) for primary drops, from the foe,
// of the stats in `mask` (bit i for stage(b, i)) on p's active Pokémon, as
// ChangeStatBuffs (bsc:6937) for each. A stat at -6 can't fall and gets no
// message here (getCappedBoost runs first; its -unboost of 0 is the
// caller's). Then the Pokémon's ability, whose handler runs before its
// side's (Speed first, comparePriority): Clear Body and White Smoke stop
// every drop, Hyper Cutter Attack's and Keen Eye accuracy's, with one
// -fail|unboost; then Mist stops the rest, with -activate. The decomp
// checks Mist first; only the message differs. Returns the stats that
// fall.
template <typename L>
constexpr uint8_t drops(const Battle& battle, Player p, uint8_t mask, L& log) {
  const Side& side = battle.side(p);
  const ActivePokemon& t = side.active;
  Boosts b = t.boosts;
  for (uint8_t i = 0; i < 7; i++) {
    if (stage(b, i) <= -6) mask = uint8_t(mask & ~(1 << i));
  }
  if (mask == 0) return 0;
  uint8_t stopped = 0;
  switch (t.ability) {
  case Ability::ClearBody:
  case Ability::WhiteSmoke: stopped = mask; break;
  case Ability::HyperCutter: stopped = mask & 1; break;
  case Ability::KeenEye: stopped = mask & (1 << 5); break;
  default: break;
  }
  const ID id = protocol::ident(battle, p);
  if (stopped != 0) {
    protocol::fail_unboost(log, id, t.ability);
    mask = uint8_t(mask & ~stopped);
  }
  if (mask != 0 && side.conditions.mist != 0) {
    protocol::activate(log, id, protocol::Activate::Mist);
    mask = 0;
  }
  return mask;
}

// Lowers a stage by up to n, stopping at -6 (no check of who may lower it).
// Returns the stages lost.
constexpr int8_t lower(ActivePokemon& a, uint8_t stat, int8_t n) {
  int8_t& b = stage(a.boosts, stat);
  const int8_t lost = b - n < -6 ? int8_t(b + 6) : n;
  b = int8_t(b - lost);
  return lost;
}

// A primary drop of n stages from the foe on each stat of `mask` that
// drops() lets fall, after its messages. Showdown's Battle.boost then
// prints -unboost per stat in the move's order (which is stage order for
// every gen3 drop): the stages lost, or 0 for a stat already at -6, which
// getCappedBoost leaves in the table and no TryBoost handler removes.
// Returns the stats that fell.
template <typename L>
constexpr uint8_t lower_stats(Battle& battle, Player p, uint8_t mask, int8_t n, L& log) {
  ActivePokemon& t = battle.side(p).active;
  const uint8_t falls = drops(battle, p, mask, log);
  const ID id = protocol::ident(battle, p);
  for (uint8_t i = 0; i < 7; i++) {
    if (!(mask & (1 << i))) continue;
    if (falls & (1 << i)) {
      protocol::boost(log, id, i, lower(t, i, n), true);
    } else if (stage(t.boosts, i) <= -6) {
      protocol::boost(log, id, i, 0, true);
    }
  }
  return falls;
}

// Intimidate (bs1:4024; Showdown's gen3 onStart, data/mods/gen3/abilities.ts:61):
// -1 Attack to the foe unless it has a Substitute, Clear Body, White Smoke
// or Hyper Cutter, its side has Mist (Showdown's onTryBoost, drops()), or it
// is at -6. White Herb doesn't react at once: Showdown's gen3 checks it in
// the SwitchIn event, before the switcher's ability Start, then only after
// a move, at a switch-in or at the residuals (data/items.ts:7679). It prints
// -ability|P|Intimidate|boost, then lower_stats()'s messages (Showdown's
// boost is a secondary from an ability, which prints its 0 at -6);
// nothing into a Substitute (gen3).
template <typename O>
constexpr void intimidate(Battle& battle, Player player, O& options) {
  const Player target = foe(player);
  Side& side = battle.side(target);
  if (side.get(1).fainted() || side.active.volatiles.substitute) return;
  protocol::ability(options.log, protocol::ident(battle, player), Ability::Intimidate,
                    protocol::AbilityFrom::Boost);
  lower_stats(battle, target, 1, 1, options.log);
}

template <typename O> constexpr void ability_immunity(Battle& battle, O& options);

// ABILITYEFFECT_ON_SWITCHIN (util:2470) with Intimidate and Trace:
// - Drizzle, Drought and Sand Stream set their weather with no timer, unless
//   it is already there from an ability (Showdown's setWeather and its gen <= 5
//   onFieldStart agree).
// - Trace copies the foe's ability if the foe stands; in Gen III the copy
//   doesn't take effect on entry (Showdown's setAbility starts it only for
//   gen > 3, sim/pokemon.ts:1941; the decomp doesn't run it either).
// - Pressure announces itself, in the omniscient stream only (Showdown's
//   gen3 onStart, data/mods/gen3/abilities.ts:148).
template <typename O>
constexpr void ability_switch_in(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (const Weather w = ability_weather(a.ability); w != Weather::None) {
    Field& field = battle.field;
    if (field.weather != w || !field.permanent) {
      field.weather = w;
      field.weather_turns = 0;
      field.permanent = true;
      protocol::weather_ability(options.log, w, a.ability, protocol::ident(battle, player));
      weather_change(battle, options);
    }
    return;
  }
  if (a.ability == Ability::Forecast) {
    return forecast(battle, player, effective_weather(battle), options);
  }
  if (a.ability == Ability::Intimidate) return intimidate(battle, player, options);
  if (a.ability == Ability::Trace) {
    const Side& f = battle.side(foe(player));
    if (!f.get(1).fainted() && f.active.ability != Ability::None) {
      a.ability = f.active.ability;
      protocol::ability_copy(options.log, protocol::ident(battle, player), a.ability,
                             Ability::Trace, protocol::AbilityFrom::Trace,
                             protocol::ident(battle, foe(player)));
    }
    return;
  }
  if (a.ability == Ability::Pressure) {
    protocol::ability(options.log, protocol::ident(battle, player), Ability::Pressure,
                      protocol::AbilityFrom::Pressure);
  }
}

// ITEMEFFECT_ON_SWITCH_IN (util:3291): White Herb.
template <typename O>
constexpr void item_switch_in(Battle& battle, Player player, O& options) {
  white_herb(battle, player, options);
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
  a.hidden_power = mon.hidden_power;
  a.hidden_power_type = mon.hidden_power_type;
  a.first_turn = 2; // main:3220, decision 14
}

// switchineffects (bsc:5180) for the Pokémon that just entered: Spikes, and
// unless they faint it, Truant's counter and the switch-in ability and item.
template <typename O>
constexpr void arrive(Battle& battle, Player player, O& options) {
  // Each arrival is its own Showdown action (runSwitch), with faintMessages
  // after it. Showdown's Update after the switch action, before runSwitch:
  // an immunity ability cures a status it got while it had another ability,
  // and the berries act (a Leppa Berry whose move Grudge emptied).
  ability_immunity(battle, options);
  {
    const Player first = log_order(battle);
    for (const Player p : {first, foe(first)}) {
      if (!battle.side(p).get(1).fainted()) item_cure(battle, p, options);
    }
  }
  if (spikes(battle, player, options)) return protocol::faints(options.log);
  // Showdown's gen3 runSwitch (data/mods/gen4/scripts.ts:18): Spikes, then
  // the SwitchIn event, where every White Herb on the field checks
  // (onAnySwitchIn), then the ability's Start, then the item's
  // (item_switch_in).
  white_herbs(battle, options);
  ActivePokemon& a = battle.side(player).active;
  // bsc:5258. The battle start doesn't run switchineffects, so a Truant
  // Pokémon that starts the battle acts on turn 1 (Showdown's gen3
  // onSwitchIn agrees: truantTurn = turn !== 0).
  if (a.ability == Ability::Truant) a.truant = battle.turn != 0;
  ability_switch_in(battle, player, options);
  // And after runSwitch: a traced immunity.
  ability_immunity(battle, options);
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
                          options)
          : log_order(battle);
  arrive(battle, first, options);
  if (outcome(battle)) return;
  // A first arrival that faints to Spikes cancels the second's: Showdown's
  // gen3 faint handling cancels every queued action (sim/battle.ts:2596),
  // and in its traces the other replacement's SwitchIn handlers don't run
  // (no Pressure line, no toxic reset; seen in 3 games, the queue order
  // behind it unread). Its toxic counter still resets here (enter()),
  // where Showdown's carries on from before it left.
  if (battle.side(first).get(1).fainted()) return;
  arrive(battle, foe(first), options);
}

// p's active Pokémon leaves for SLOT `slot`, which enters: a switch action,
// a faint replacement, Baton Pass (`baton`) or Roar's drag (`dragged`),
// before the new Pokémon arrives. Switch-out clears the foe's state that p's Pokémon was the
// source of (SwitchInClearSetData and FaintClearSetData, main:3158-3199;
// README decision 9). Baton Pass leaves the foe's escape prevention and its
// Lock-On (main:3158 skips them), and baton_pass_in restores what passes.
template <typename O>
constexpr void leave(Battle& battle, Player player, uint8_t slot, O& options,
                     bool baton = false, bool dragged = false) {
  Side& side = battle.side(player);
  assert(slot >= 2 && slot <= 6);
  assert(side.get(slot).species != Species::None && !side.get(slot).fainted());

  // switchoutabilities (bsc:9590): Natural Cure, as Showdown's onSwitchOut.
  // A cured sleep is public, so its count resets before the SLOTs swap.
  Pokemon& out = side.get(1);
  // Showdown's clearStatus after the -curestatus ends a Nightmare (wake).
  if (side.active.ability == Ability::NaturalCure && out.status.any()) {
    protocol::cure_status(options.log, protocol::ident(battle, player), out.status,
                          protocol::Cure::NaturalCure);
    if (out.status.asleep()) wake(battle, player, options);
    out.status = Status::none();
    if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 0;
  }
  // Showdown's switchIn then runs the ability's End: Flash Fire's boost
  // ends with a silent -end.
  if (side.active.volatiles.flash_fire) {
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::FlashFire);
  }
  // Cloud Nine and Air Lock's End lets the foe's Castform see the weather.
  if (side.active.ability == Ability::CloudNine || side.active.ability == Ability::AirLock) {
    weather_change(battle, options, player);
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
  const bool infatuated = f.volatiles.infatuated;
  f.volatiles.infatuated = false;
  // The binder leaves: Showdown's partiallytrapped stays on the foe until
  // its residual, no longer trapping it (Stage::Bind). Baton Pass too: the
  // recipient isn't the source.
  if (f.volatiles.wrapped != 0) f.volatiles.wrapper_gone = true;
  if (!baton) f.volatiles.escape_prevention = false;

  std::swap(side.order[0], side.order[slot - 1]);
  if constexpr (CHANCE<O>) options.chance.durations.switched(player, slot, baton);
  enter(battle, player);
  protocol::switched(options.log,
                     dragged ? protocol::ArgType::Drag : protocol::ArgType::Switch,
                     protocol::ident(battle, player), side.get(1),
                     baton ? protocol::SwitchFrom::BatonPass
                           : protocol::SwitchFrom::None);
  // Attract's onUpdate, once its source is gone.
  if (infatuated) {
    protocol::end_effect(options.log, protocol::ident(battle, foe(player)), protocol::End::Attract);
  }
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
//   the foe's lock on the passer no longer applies (its target has left),
//   where the decomp keeps STATUS3_ALWAYS_HITS on the recipient.
// - Not passed, by either: the toxic counter (decision 1), infatuation,
//   Disable, Encore, Torment, Nightmare, Yawn, Foresight, Flash Fire. A
//   Taunted Pokémon can't use Baton Pass, so Showdown passing Taunt can't
//   show; nor Charge, which ends after the Baton Pass itself (use_move).
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
  n.wrapper_gone = v.wrapper_gone;
  n.lock_on = v.lock_on;
  a.wrapped_move = old.wrapped_move;
  // Showdown's gen3 Update after the Baton Pass action, before the
  // recipient's runSwitch: a berry or White Herb takes what was passed.
  item_cure(battle, player, options);
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

// The order two replacements enter in, which changes only the log: that of
// Showdown's instaswitch actions, whose Pokémon is the fainted one
// (sim/side.ts:1012). A fainted Pokémon isn't active, so none of its own
// handlers run (findEventHandlers needs isActive, sim/battle.ts): its Speed
// is the stored stat, with no stage (clearVolatile), weather ability, Macho
// Brace or paralysis. Its Quick Claw still counts with the turn's roll
// (gen3 getActionSpeed; ignoringItem skips inactive holders only from gen
// 5); this uses the roll only if the turn already drew it, since a draw
// kept here unrecorded would be reused unrecorded by a later order that
// matters. A tie is an unrecorded coin.
constexpr Player replacement_order(Battle& battle) {
  uint32_t s[2];
  for (const Player p : {Player::P1, Player::P2}) {
    const Side& side = battle.side(p);
    s[index(p)] = side.get(1).stats.spe;
    if (battle.request.quick_claw == Optional::True &&
        holds(side, HoldEffect::QuickClaw)) {
      s[index(p)] = UINT32_MAX;
    }
  }
  if (s[0] != s[1]) return s[0] > s[1] ? Player::P1 : Player::P2;
  return battle.rng.chance(1, 2) ? Player::P1 : Player::P2;
}

// The replacements a faint request asked for: as Showdown's instaswitch
// actions, every replacement enters before any arrives (instaswitch comes
// before runSwitch, sim/battle-queue.ts:177), then they arrive in
// arrive_both's order. Two replacements enter in replacement_order.
template <typename O>
constexpr void replace(Battle& battle, Choice c1, Choice c2, O& options) {
  const Choice choices[2] = {c1, c2};
  const Player first = c1.type() == Choice::Switch && c2.type() == Choice::Switch
                           ? replacement_order(battle)
                           : Player::P1;
  for (const Player p : {first, foe(first)}) {
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
  for (const Player p : {Player::P1, Player::P2}) {
    enter(battle, p);
    protocol::switched(options.log, protocol::ArgType::Switch,
                       protocol::ident(battle, p), battle.side(p).get(1));
  }
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
// matters to Encore, Disable, Mimic and the like.
inline constexpr Move UNAVAILABLE = Move::None;

// MOVE_EFFECT_* bytes this code applies, in the decomp's numbering order.
enum class Secondary : uint8_t {
  None, Sleep, Poison, Burn, Freeze, Paralysis, Toxic, Confusion, Flinch,
  TriAttack,
  // MOVE_EFFECT_*_MINUS_1 on the target, in Boosts order (atk-acc), then
  // MOVE_EFFECT_ATK_PLUS_1 and DEF_PLUS_1 with MOVE_EFFECT_AFFECTS_USER.
  AtkDown, DefDown, SpeDown, SpaDown, SpdDown, AccDown, AtkUp, DefUp,
  // MOVE_EFFECT_ALL_STATS_UP with MOVE_EFFECT_AFFECTS_USER (Ancient Power,
  // Silver Wind).
  AllUp,
};

// The per-turn ProtectStruct (README decision 2): a local of actions(), so
// it starts empty each turn and on resuming after Baton Pass, reached
// through MoveState::turn (decided 2026-10-07). The residuals can't reach
// it. Indexed by player.
struct Turn {
  bool protect[2]; // ProtectStruct.protected: Protect or Detect worked
  bool endure[2];  // ProtectStruct.endured: Endure worked
  // A damaging move (or Present's heal) hit the Pokémon itself, not its
  // Substitute (Showdown's
  // focuspunch lostFocus; the decomp's physicalDmg || specialDmg, which
  // also needs HP lost). Focus Punch reads it.
  bool hit[2];
  // ProtectStruct.physicalDmg and specialDmg: the HP the foe's last
  // physical or special move took from the Pokémon itself (gHpDealt) this
  // turn, 0 if none. Counter and Mirror Coat read them.
  uint16_t physical[2];
  uint16_t special[2];
  // Showdown's magiccoat and snatch volatiles (duration 1): the move was
  // used this turn and hasn't bounced or stolen a move yet.
  bool magic_coat[2];
  bool snatch[2];
};

// What one move execution tracks: the decomp's per-action globals
// (gCurrentMove, gCurrMovePos, gMoveResultFlags, gHpDealt) and the
// gHitMarker bits that this code reads. On the stack, as libpkmn gen2's
// State; HandleAction_ActionFinished (util:661) clears the hit markers
// between actions.
struct MoveState {
  Move move;         // gCurrentMove
  Move chosen;       // gChosenMove: the move itself, or Sleep Talk, Snatch
                     // or Magic Coat for the move it calls
  Type type;         // the move's type as used: the table's, or Weather
                     // Ball's (move_type), resolved once in execute() as
                     // Showdown's onModifyMove
  uint8_t slot;      // gCurrMovePos, 0-3, or NO_SLOT
  uint8_t result;    // gMoveResultFlags (MoveResult)
  uint16_t hp_dealt; // gHpDealt: HP the target or its Substitute lost
  uint16_t total;    // HP the target itself lost to every hit (Showdown's
                     // totalDamage, which Shell Bell reads)
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
  Player synchronized; // who took the status that armed it
  // Called by another move (`from`; `chosen` is the move the action
  // chose): the attack canceler already ran for the caller (its tracker
  // stays at CANCELER_END, as Showdown's useMove skips BeforeMove), and the
  // move uses no PP.
  bool called;
  Move from;
  // Past the accuracy check and TryHit on the foe: Showdown's gotAttacked
  // (gen3 scripts.ts:464), which Mirror Move reads (last_taken_move).
  bool attacked;
  // Pursuit on a target switching out (pursues()): double power, and it
  // can't miss.
  bool pursuing;
  // Bide's release on its third turn (bide()): no move line, no PP.
  bool release;
  // The forced turn of a locked move (Solar Beam's second turn), which
  // Showdown logs [from] lockedmove.
  bool locked;
  // Roar or Whirlwind worked: the target is dragged out after the action,
  // as Showdown's forceSwitchFlag (sim/battle.ts:2828).
  bool drag;
  // Baton Pass worked: the user's side picks the recipient after the action
  // (Phase::BatonPass), as Showdown's switchFlag.
  bool pass;
  // hit() ran the target's contact abilities: after each hit of a multi-hit
  // loop (moveendto MOVEEND_NEXT_TARGET), or after a single hit's
  // secondaries, so moveend doesn't run them again.
  bool contact_done;
  // A multi-hit loop ran King's Rock per hit (flinch_item), so moveend
  // doesn't run it again.
  bool kings_rock_done;
  // gBattleStruct->changedItems for the user: the item Thief or Covet took,
  // given to the user at MOVEEND_CHANGED_ITEMS. None when nothing was taken.
  Item stolen;
  // The target fainted under Grudge (faint_target): the used move loses its
  // PP at the end of moveend, Showdown's faintMessages after the move.
  bool grudge;
  // The last move a call ran (call_move, through nested calls), or None:
  // Showdown's activeMove after useMove, which AfterMove sees
  // (sim/battle-actions.ts:309).
  Move ran;
  // moveend ran the AfterMove handlers (after_move).
  bool after_move;
  // The user's HP before the foe's Destiny Bond took it (faint_target), or
  // 0.
  uint16_t bonded_hp;
  // This turn's ProtectStruct; set by use_move, null only for the
  // residuals' MoveState, which never reads it.
  Turn* turn;

  static constexpr uint8_t NO_SLOT = 4;
};

constexpr bool physical(Type type) { return type < Type::Mystery; }

constexpr void pp_reduce(Battle& battle, Player player, const MoveState& m);

// The sound moves (sSoundMovesTable, util:688), which Soundproof stops
// (try_hit). Showdown's gen3 `sound` flag is on the same moves, plus Perish
// Song and Heal Bell, which handle Soundproof themselves in both.
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

// Whether the move is aimed at the user (MoveState::self): MOVE_TARGET_USER,
// and Showdown's 'self' for the DEPENDS moves other than Sleep Talk and
// Curse; Conversion 2 is the decomp's user but Showdown's 'normal'.
constexpr bool targets_user(Move move) {
  switch (move) {
  case Move::MagicCoat:
  case Move::Snatch:
  case Move::Metronome:
  case Move::Assist:
  case Move::MirrorMove:
  case Move::NaturePower: return true;
  case Move::Conversion2: return false;
  default: return get(move).target == Target::User;
  }
}

// Whether Showdown's |move| line names the user as the target: the moves
// aimed at the user or the whole field (Showdown's self, all and allySide
// targets, which getRandomTarget resolves to the user, sim/battle.ts:2506),
// and a non-Ghost's Curse. Every other move names the foe.
constexpr bool aims_at_user(const ActivePokemon& a, Move move) {
  if (move == Move::Curse) return !has_type(a, Type::Ghost);
  return targets_user(move) || move == Move::SleepTalk;
}

// Showdown's |move| line (useMoveInner, data/mods/gen3/scripts.ts:165): once
// the move is past the attack canceler, before anything it does, even if it
// then fails. A move Sleep Talk, Snatch or Magic Coat calls has [from] the
// caller, Pursuit on a
// switching target [from] Pursuit, a locked move's forced turn [from]
// lockedmove.
template <typename O>
constexpr void log_move(const Battle& battle, Player player, const MoveState& m,
                        O& options) {
  if constexpr (!std::remove_cvref_t<decltype(options.log)>::enabled) return;
  const Player target =
      aims_at_user(battle.side(player).active, m.move) ? player : foe(player);
  protocol::MoveFrom from = protocol::MoveFrom::None;
  Move by = Move::None;
  if (m.called) {
    from = protocol::MoveFrom::From;
    by = m.from;
  } else if (m.pursuing) {
    from = protocol::MoveFrom::From;
    by = Move::Pursuit;
  } else if (m.locked) {
    from = protocol::MoveFrom::LockedMove;
  }
  protocol::move(options.log, protocol::ident(battle, player), m.move,
                 protocol::ident(battle, target), from, by);
}

// lockedmove's onRestart, for a hit (hit()) or a Protect that stops the
// locked move (try_hit(), the gen4 mod's protect onTryHit): the lock is
// renewed for the turn while trueDuration is at least 2. trueDuration
// changes only at the residuals, so the bit records the restart and
// lock_residuals reads trueDuration there, where the outcome shows.
constexpr void lock_renew(Battle& battle, Player player) {
  battle.side(player).active.renewed |= ActivePokemon::RENEWED_LOCK;
}

// Ends p's Bide: its onEnd's silent -end, and the lock. Showdown's
// removeVolatile does nothing at 0 HP (sim/pokemon.ts:2036), so a
// confusion self-KO logs no -end.
template <typename O>
constexpr void end_bide(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  a.volatiles.bide = 0;
  a.volatiles.multiple_turns = false;
  if (battle.side(player).get(1).fainted()) return;
  protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::BideSilent);
}

// Showdown's MoveAborted, for a move the canceler stops: a two-turn move's
// charge is lost (twoturnmove's onMoveAborted) and Bide ends (its
// onMoveAborted). The other locks (lockedmove, uproar, rollout, furycutter)
// stay, and run out by their durations at the residuals (lock_residuals).
// The decomp's CancelMultiTurnMoves (util:877) ends them all.
template <typename O>
constexpr void abort_move(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  Volatiles& v = a.volatiles;
  if (v.multiple_turns && two_turn(get(a.locked_move).effect)) {
    v.multiple_turns = false;
    v.on_air = false;
    v.underground = false;
    v.underwater = false;
  }
  if (v.bide != 0) end_bide(battle, player, options);
}

// AtkCanceler_UnableToUseMove (util:1987), then the rest of
// Cmd_attackcanceler (bsc:915). Returns whether the move goes ahead; if not,
// the caller goes to moveend, as every canceling script does. The first case
// that stops the move ends the check. The cases run in the order of
// Showdown's BeforeMove handlers (onBeforeMovePriority, gen3 mod and what it
// inherits): recharge 11, sleep and freeze 10, Truant 9, flinch 8, Disable
// 7, Imprison 4 (onFoeBeforeMove), confusion 3, Attract 2, paralysis 1,
// Taunt 0 (gen3 drops gen4's 5). The decomp checks recharge after sleep,
// freeze and Truant (so a sleeping Pokémon that must recharge ticks its
// sleep), Taunt before Imprison, and paralysis before Attract.
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
  // A move Sleep Talk calls: Sleep Talk's canceler already ran. Bide's
  // release: its own canceler ran (CANCELER_BIDE below), and it has no move
  // line. Pursuit on a switching target: Showdown's useMove runs no
  // BeforeMove handler, so nothing here stops it or ticks.
  if (m.called || m.release || m.pursuing) {
    m.obeys = true;
    if (!m.release) {
      log_move(battle, player, m, options);
      a.last_used_move = m.move;
      a.last_used_type = m.type;
    }
    return true;
  }
  // Showdown's rage onBeforeMove (priority 100, before every other
  // handler): Rage ends at the user's next move attempt, whatever stops
  // it. The decomp's TryClearRageStatuses clears it at the start of a turn
  // whose chosen move isn't Rage.
  v.rage = false;
  const auto cant = [&](protocol::Cant reason) {
    protocol::cant(options.log, protocol::ident(battle, player), reason, m.move);
  };

  // CANCELER_FLAGS
  v.destiny_bond = false;
  v.grudge = false;

  // CANCELER_RECHARGE
  if (v.recharge) {
    cant(protocol::Cant::Recharge);
    v.recharge = false;
    a.recharge_timer = 0;
    abort_move(battle, player, options);
    m.unable = true;
    return false;
  }

  // CANCELER_ASLEEP. The counter ticks, and it still can't move unless it
  // woke up or uses Snore or Sleep Talk. The decomp's Uproar wakes it
  // outright (UproarWakeUpCheck, bsc:6801); Showdown's wakes only the
  // Pokémon an Uproar hits (its onTryHit, hit()), and this follows it.
  if (mon.status.asleep()) {
    const uint8_t by = a.ability == Ability::EarlyBird ? 2 : 1;
    const bool rest = mon.status.self_inflicted();
    const Status before = mon.status;
    const bool woke = !mon.status.tick_sleep(by);
    // Showdown's slp onBeforeMove: Snore and Sleep Talk go on after it.
    if (woke) {
      protocol::cure_status(options.log, protocol::ident(battle, player), before);
    } else {
      cant(protocol::Cant::Sleep);
    }
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
      wake(battle, player, options);
    } else if (m.move != Move::Snore && m.move != Move::SleepTalk) {
      a.sleep_skipped = 0;
      // As Showdown's onMoveAborted: a charged move is lost. The decomp
      // keeps it until the end of the turn (main:3944).
      abort_move(battle, player, options);
      m.unable = true;
      return false;
    } else {
      a.sleep_skipped++;
    }
  }

  // CANCELER_FROZEN: it thaws with 1/5. A thawing move (EFFECT_THAW_HIT)
  // that stays frozen goes on, and CANCELER_THAW thaws it below if nothing
  // else stops the move first. A frozen Pokémon that can't move prints no
  // attack string (HITMARKER_NO_ATTACKSTRING).
  const bool thaw_hit = get(m.move).effect == Effect::ThawHit;
  if (mon.status.frozen()) {
    if (rolls::thaw(battle, player, options)) {
      protocol::cure_status(options.log, protocol::ident(battle, player), mon.status);
      mon.status = Status::none();
    } else if (!thaw_hit) {
      // As Showdown's onMoveAborted; the decomp keeps a charged move.
      cant(protocol::Cant::Freeze);
      abort_move(battle, player, options);
      return false;
    }
  }

  // CANCELER_TRUANT. The counter toggles at the end of each turn
  // (ABILITYEFFECT_ENDTURN, util:2640; battler_stage).
  if (a.ability == Ability::Truant && a.truant) {
    cant(protocol::Cant::Truant);
    abort_move(battle, player, options);
    m.unable = true;
    m.result |= MoveResult::MISSED;
    return false;
  }

  // CANCELER_FLINCH
  if (v.flinched) {
    cant(protocol::Cant::Flinch);
    v.flinched = false;
    abort_move(battle, player, options);
    m.unable = true;
    return false;
  }

  // CANCELER_DISABLED
  if (a.disabled_move != Move::None && a.disabled_move == m.move) {
    cant(protocol::Cant::Disable);
    abort_move(battle, player, options);
    m.unable = true;
    return false;
  }

  // CANCELER_IMPRISONED: GetImprisonedMovesCount, the foe Imprisoned and
  // knows the move.
  if (f.volatiles.imprisoned_others) {
    bool imprisoned = false;
    for (const MoveSlot& fm : f.moves) imprisoned |= same_move(fm.id, m.move);
    if (imprisoned) {
      cant(protocol::Cant::Imprison);
      abort_move(battle, player, options);
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
  // Log, as Showdown's confusion onBeforeMove: -end confusion when it ends,
  // else -activate confusion before the draw.
  if (v.confusion != 0) {
    v.confusion--;
    const bool ended = v.confusion == 0;
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).confusion_end,
                  options.chance.durations.get(player).confusion, ended, 2, 5,
                  1, options);
    }
    const ID id = protocol::ident(battle, player);
    if (ended) {
      protocol::end_effect(options.log, id, protocol::End::Confusion);
    } else {
      protocol::activate(options.log, id, protocol::Activate::Confusion);
    }
    if (!ended && rolls::confusion(battle, player, options)) {
      m.self = true;
      m.unable = true;
      uint32_t d = apply_roll(damage(battle, player, player, Hit::confusion()),
                              rolls::damage(battle, player, 0, options));
      const ItemData item = item_data(mon.item);
      if (mon.hp <= d && item.effect == HoldEffect::FocusBand &&
          rolls::focus_band(battle, player, 0, item.param, options)) {
        protocol::activate(options.log, protocol::ident(battle, player),
                           protocol::Activate::FocusBand);
        d = mon.hp - 1u;
      }
      a.bide_damage =
          a.bide_damage > UINT32_MAX - d ? UINT32_MAX : a.bide_damage + d;
      mon.hp = mon.hp > d ? uint16_t(mon.hp - d) : 0;
      protocol::damage(options.log, protocol::ident(battle, player), mon,
                       protocol::Damage::Confusion);
      // Showdown's MoveAborted comes after the self-hit (Bide's -end).
      abort_move(battle, player, options);
      if (mon.fainted()) faint(battle, player, options);
      return false;
    }
  }

  // CANCELER_IN_LOVE. Showdown's attract onBeforeMove prints -activate [of]
  // the foe before its draw.
  if (v.infatuated) {
    protocol::activate_with(options.log, protocol::ident(battle, player),
                            protocol::Activate::Attract, Move::None,
                            protocol::ident(battle, foe(player)));
  }
  if (v.infatuated && rolls::infatuation(battle, player, options)) {
    cant(protocol::Cant::Attract);
    abort_move(battle, player, options);
    m.unable = true;
    return false;
  }

  // CANCELER_PARALYZED. The decomp's own CancelMultiTurnMoves is commented
  // out, but the script it jumps to runs cancelmultiturnmoves (bs1:3773).
  if (mon.status.paralyzed() && rolls::paralysis(battle, player, options)) {
    cant(protocol::Cant::Paralysis);
    abort_move(battle, player, options);
    m.unable = true;
    return false;
  }

  // Showdown's choicelock onBeforeMove (priority 0): a lock whose band is
  // gone ends here or at the end of the turn (turn_cleanup), not when the
  // band leaves, so a band Tricked back the same turn finds it still set.
  if (!holds(side, HoldEffect::ChoiceBand)) a.choice_move = Move::None;

  // CANCELER_TAUNTED
  if (a.taunt != 0 && get(m.move).bp == 0) {
    cant(protocol::Cant::Taunt);
    abort_move(battle, player, options);
    m.unable = true;
    return false;
  }

  // CANCELER_BIDE, as Showdown's gen3 bide onBeforeMove (priority 0, after
  // the others). With its duration at 2 (bide(): 3 at the start, one less
  // per residual) it prints -activate and the action ends there
  // (beforeMoveCallback: no move line, no PP). At 1 it ends: -end, then
  // -fail if no move took HP from it (abort: the silent -end), else the
  // release, twice the damage taken, with no move line (bide()). The
  // decomp's is the same two turns (Cmd_setbide, bs1:1149).
  if (v.bide != 0) {
    assert(m.move == Move::Bide && v.bide <= 2);
    const ID id = protocol::ident(battle, player);
    if (v.bide == 2) {
      protocol::activate(options.log, id, protocol::Activate::Bide);
      m.unable = true;
      return false;
    }
    protocol::end_effect(options.log, id, protocol::End::Bide);
    if (a.bide_damage == 0) {
      protocol::fail(options.log, id);
      abort_move(battle, player, options);
      m.unable = true;
      return false;
    }
    m.release = true;
    m.obeys = true;
    return true;
  }

  // CANCELER_THAW
  // Showdown's frz onModifyMove, before the move line.
  if (mon.status.frozen()) {
    assert(thaw_hit);
    protocol::cure_status(options.log, protocol::ident(battle, player), mon.status,
                          protocol::Cure::Move, false, m.move);
    mon.status = Status::none();
  }

  // No PP left (BattleScript_NoPPForMove, bs1:3556): only if the PP went
  // to 0 after selection, by Spite or Pressure from a faster foe. Showdown
  // checks it before the move line (runMove, sim/battle-actions.ts:283);
  // the decomp checks Soundproof first (see below).
  if (m.slot != MoveState::NO_SLOT && a.moves[m.slot].pp == 0 &&
      m.move != Move::Struggle && !v.multiple_turns) {
    cant(protocol::Cant::PP);
    m.attackstring = true;
    m.result |= MoveResult::MISSED;
    return false;
  }

  log_move(battle, player, m, options);
  a.last_used_move = m.move;
  a.last_used_type = m.type;
  // ABILITYEFFECT_MOVES_BLOCK (util:2645), Soundproof, comes here in the
  // decomp; Showdown's is a TryHit handler, after the accuracy roll and
  // Protect, and this follows it (try_hit). Both take the PP first.
  m.obeys = true;
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

// Whether the foe's Protect or Detect blocks m (DEFENDER_IS_PROTECTED,
// bsc:57). Showdown's protect flag agrees with the decomp's
// FLAG_PROTECT_AFFECTED on every move on teams/ou/ (checked 2026-10-07)
// except Counter and Mirror Coat, which Showdown's protect flag covers;
// this follows Showdown. (Mean Look has it in both: Showdown's gen5 mod,
// which gen3 inherits, adds it to the base data's.) Log: Activate Protect
// (try_hit).
constexpr bool protected_from(const MoveState& m, Player player) {
  assert(m.turn != nullptr);
  return m.turn->protect[index(foe(player))] &&
         ((get(m.move).flags & Flag::ProtectAffected) ||
          m.move == Move::Counter || m.move == Move::MirrorCoat);
}

// Whether p's Lock-On or Mind Reader is on the foe now in: Showdown's
// lockon volatile on the user, with its target (Volatiles::lock_on).
constexpr bool locked_on(const Battle& battle, Player p) {
  const uint8_t lock = battle.side(p).active.volatiles.lock_on;
  return lock != 0 && lock >> 2 == battle.side(foe(p)).order[0];
}

// Showdown's Invulnerability event (gen3 tryMoveHit, before the accuracy
// roll): a semi-invulnerable target the move can't reach. Lock-On and Mind
// Reader reach it (the only possible locker in singles is the attacker);
// Surf and Whirlpool hit Dive (HITMARKER_IGNORE_UNDERWATER, bs1:239,
// and BattleScript_EffectTrap), and the moves that
// set ignore_on_air or ignore_underground reach Fly and Dig (Earthquake
// and Magnitude, as Showdown's dig condition).
constexpr bool invulnerable(const Battle& battle, Player player,
                            const MoveState& m) {
  const Volatiles& v = battle.side(foe(player)).active.volatiles;
  if (m.pursuing || locked_on(battle, player)) return false;
  return (v.on_air && !m.ignore_on_air) ||
         (v.underground && !m.ignore_underground) ||
         (v.underwater && m.move != Move::Surf && m.move != Move::Whirlpool);
}

// The accuracy check's arithmetic from the move's accuracy `acc` on, for an
// attack on `target`'s Pokémon by one with accuracy stage `boost` and
// ability `ability` (accuracy() has the details): the stages, then the
// modifiers, then the count of the 100 outcomes that hit.
constexpr uint32_t staged_accuracy(const Battle& battle, Player target, double acc,
                                   int8_t boost, Ability ability, bool phys) {
  const Side& foe_side = battle.side(target);
  const ActivePokemon& f = foe_side.active;
  const Weather weather = effective_weather(battle);
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
  if (ability == Ability::CompoundEyes) modifier = js_chain(modifier, 1.3);
  if (f.ability == Ability::SandVeil && weather == Weather::Sandstorm) {
    modifier = js_chain(modifier, 0.8);
  }
  if (ability == Ability::Hustle && phys) {
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

// The accuracy check without its draw, for a target that isn't protected
// or semi-invulnerable (try_hit checks those): 100 or more for a certain
// hit, otherwise the number of the 100 outcomes that hit (at least 1).
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
  const MoveData& data = get(m.move);
  const Weather weather = effective_weather(battle);

  assert(!invulnerable(battle, player, m));
  // Pursuit on a switching target can't miss (Showdown's onModifyMove sets
  // accuracy true; the decomp's script has no accuracycheck).
  if (m.pursuing) return 100;
  // Lock-On and Mind Reader.
  if (locked_on(battle, player)) return 100;
  // OHKO moves, as Showdown's gen3 tryMoveHit: 30 plus the level
  // difference (try_hit has stopped a lower-level user), replacing the
  // stages and the accuracy modifiers. The decomp's Cmd_tryKO (bsc:7490)
  // skips them too, but hits on Random() % 100 + 1 < 30: 29 in 100.
  if (data.effect == Effect::Ohko) {
    return 30u + side.get(1).level - foe_side.get(1).level;
  }
  if ((weather == Weather::Rain && data.effect == Effect::Thunder) ||
      data.effect == Effect::AlwaysHit || data.effect == Effect::VitalThrow) {
    return 100;
  }

  double acc = data.accuracy;
  if (weather == Weather::Sun && data.effect == Effect::Thunder) acc = 50;
  return staged_accuracy(battle, foe(player), acc, a.boosts.acc, a.ability,
                         physical(m.type));
}

// Whether the foe's Pressure takes an extra PP from `move`, aimed at the
// user if `self` (MoveState::self). Cmd_ppreduce (bsc:1205) takes none from
// a move aimed at the user. Showdown takes it from any move whose targets
// include the foe (getMoveTargets, sim/pokemon.ts:791; gen3 scripts.ts:173),
// which adds the field-wide moves the decomp aims at the user (Showdown's
// target 'all': Haze, Perish Song, Mud Sport, Water Sport and the weathers)
// and Conversion 2 ('normal'). This follows Showdown. The reverse: Sleep
// Talk is MOVE_TARGET_DEPENDS in the decomp, so Pressure takes 2 PP from it,
// but Showdown's 'self' target takes none from Sleep Talk itself, only from
// the move it calls (sleep_talk). Snatch and Magic Coat are DEPENDS too,
// and Showdown's 'self': it takes none from them (its gen4 Snatch drops
// mustpressure and takes the PP when it steals, snatched()). Metronome and
// Assist are DEPENDS; check them when they're written.
constexpr bool pressure_applies(const Battle& battle, Player player, Move move,
                                bool self) {
  if (battle.side(foe(player)).active.ability != Ability::Pressure) return false;
  if (move == Move::SleepTalk) return false;
  if (!self) return true;
  switch (move) {
  case Move::Haze:
  case Move::PerishSong:
  case Move::MudSport:
  case Move::WaterSport:
  case Move::RainDance:
  case Move::SunnyDay:
  case Move::Sandstorm:
  case Move::Hail:
  case Move::Conversion2: return true;
  default: return false;
  }
}

// Takes `n` PP (at most what's left) from the active Pokémon's move slot. A
// move that isn't Mimic's or Transform's (MOVE_IS_PERMANENT) writes the PP
// back to the party.
constexpr void deduct_pp(Battle& battle, Player player, uint8_t slot, uint8_t n) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  MoveSlot& ms = a.moves[slot];
  ms.pp = ms.pp > n ? uint8_t(ms.pp - n) : 0;
  if (!a.volatiles.transformed && !(a.mimicked & (1 << slot))) {
    side.get(1).moves[slot].pp = ms.pp;
  }
}

// Cmd_ppreduce (bsc:1205): one PP, or two into Pressure
// (pressure_applies), unless the move uses none (Struggle).
constexpr void pp_reduce(Battle& battle, Player player, const MoveState& m) {
  if (m.no_pp || m.slot == MoveState::NO_SLOT) return;
  if (battle.side(player).active.moves[m.slot].pp == 0) return;
  deduct_pp(battle, player, m.slot,
            uint8_t(1 + pressure_applies(battle, player, m.move, m.self)));
}

// Cmd_datahpupdate (bsc:1844) on p's target, for a move's damage: a
// Substitute takes it if there is one, otherwise the Pokémon does, and the
// damage counts toward its Bide and, by the move's table type, toward
// Counter's or Mirror Coat's (bsc:1938): Hidden Power is Normal there, so
// always physical. Showdown's gen3 Counter counts Hidden Power and Mirror
// Coat leaves it out explicitly (data/mods/gen3/moves.ts:176, 420), so they
// agree. Returns gHpDealt.
//
// A Substitute that takes the hit logs -activate|[damage], or -end if it
// breaks (Showdown's gen4 onTryPrimaryHit, data/mods/gen4/moves.ts:1300).
// The volatile itself stays until MOVEEND_SUBSTITUTE.
template <typename O>
constexpr uint16_t hp_update(Battle& battle, Player target, uint32_t damage,
                             MoveState& m, O& options) {
  Side& side = battle.side(target);
  ActivePokemon& a = side.active;
  if (a.volatiles.substitute && a.substitute_hp != 0) {
    const uint8_t dealt =
        a.substitute_hp >= damage ? uint8_t(damage) : a.substitute_hp;
    a.substitute_hp = uint8_t(a.substitute_hp - dealt);
    const ID id = protocol::ident(battle, target);
    if (a.substitute_hp == 0) {
      protocol::end_effect(options.log, id, protocol::End::Substitute);
    } else {
      protocol::activate(options.log, id, protocol::Activate::SubstituteDamage);
    }
    return dealt;
  }
  a.bide_damage = a.bide_damage > UINT32_MAX - damage ? UINT32_MAX
                                                      : a.bide_damage + damage;
  Pokemon& mon = side.get(1);
  const uint16_t dealt = mon.hp > damage ? uint16_t(damage) : mon.hp;
  mon.hp = uint16_t(mon.hp - dealt);
  m.damaged = true;
  m.total = uint16_t(m.total + dealt);
  // Future Sight's hit, at the end of the turn, has no Turn: no Focus Punch
  // is left to read it.
  if (m.turn != nullptr) {
    m.turn->hit[index(target)] = true;
    const MoveData& data = get(m.move);
    const bool phys = data.effect == Effect::HiddenPower || physical(m.type);
    (phys ? m.turn->physical : m.turn->special)[index(target)] = dealt;
  }
  return dealt;
}

// The -damage line for a move's hit on p's foe itself (not its
// Substitute), and its Faint queued at once: Showdown queues it when the HP
// reaches 0, before the recoil or drain that follows.
template <typename O>
constexpr void log_hit(const Battle& battle, Player player, const MoveState& m,
                       O& options) {
  if (!m.damaged) return;
  const Player target = foe(player);
  const Pokemon& mon = battle.side(target).get(1);
  protocol::damage(options.log, protocol::ident(battle, target), mon);
  // The flags are the faint's (protocol::faint): a move's KO, so Destiny
  // Bond applies.
  if (mon.fainted()) {
    const Volatiles& v = battle.side(target).active.volatiles;
    protocol::faint(options.log, protocol::ident(battle, target), v.destiny_bond);
  }
}

// Showdown's gen3 modifyDamage messages for one hit on p's foe, before its
// damage (data/mods/gen3/scripts.ts:92-106): the type effectiveness, then
// -crit. Only damage from the formula has them, not fixed damage (Seismic
// Toss, Counter, Endeavor...), which skips modifyDamage.
template <typename O>
constexpr void log_modifiers(const Battle& battle, Player player, const MoveState& m,
                             bool crit, O& options) {
  const ID id = protocol::ident(battle, foe(player));
  protocol::effectiveness(options.log, id, m.result & MoveResult::SUPER_EFFECTIVE,
                          m.result & MoveResult::NOT_VERY_EFFECTIVE);
  if (crit) protocol::crit(options.log, id);
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

// A move's type as used: the table's, or Weather Ball's in effective
// weather (setweatherballtype, bsc:9787; Showdown's gen3 onModifyMove):
// Fire in sun, Water in rain, Rock in sand, Ice in hail. Hidden Power's is
// in its move ID. Whether it's physical follows the type, as both agree.
constexpr Type move_type(const Battle& battle, Move move) {
  const MoveData& data = get(move);
  // Showdown's gen3 Beat Up is '???' (onModifyMove): no Color Change, and
  // nothing for Conversion 2 to resist.
  if (data.effect == Effect::BeatUp) return Type::Mystery;
  if (data.effect != Effect::WeatherBall) return data.type;
  switch (effective_weather(battle)) {
  case Weather::Sun: return Type::Fire;
  case Weather::Rain: return Type::Water;
  case Weather::Sandstorm: return Type::Rock;
  case Weather::Hail: return Type::Ice;
  default: return data.type;
  }
}

// The damage of a move with no formula, or 0 for any other: Showdown's
// getDamage before the formula (sim/battle-actions.ts:1593), after the
// immunities. The decomp agrees except for Psywave (rolls::variable).
// - Seismic Toss and Night Shade (dmgtolevel, bsc:7926): the user's level.
// - Sonic Boom and Dragon Rage (bs1:1720, 819): 20 and 40.
// - Super Fang (damagetohalftargethp, bsc:7577): half the target's HP
//   (not its Substitute's), at least 1. Showdown doesn't floor the half:
//   a Substitute can keep a fractional HP, which flooring here matches
//   exactly against every later whole-number hit.
// - Psywave (bs1:1206): `psywave` percent of the user's level, floored and
//   at least 1 (Showdown's Pokemon.damage) where the level isn't 100.
// - The OHKO moves (BattleScript_EffectOHKO, bs1:762): the target's HP, as
//   Showdown's gen3; Endure, Focus Band and a Substitute work as for any
//   hit (survive(), hp_update()).
constexpr uint32_t fixed_damage(const Battle& battle, Player player, Effect effect,
                                uint8_t psywave) {
  const Pokemon& user = battle.side(player).get(1);
  const Pokemon& target = battle.side(foe(player)).get(1);
  switch (effect) {
  case Effect::LevelDamage: return user.level;
  case Effect::Sonicboom: return 20;
  case Effect::DragonRage: return 40;
  case Effect::SuperFang: return target.hp / 2u == 0 ? 1u : target.hp / 2u;
  case Effect::Psywave: {
    assert(psywave >= 50 && psywave <= 150);
    const uint32_t d = uint32_t(psywave) * user.level / 100u;
    return d == 0 ? 1u : d;
  }
  case Effect::Ohko: return target.hp;
  default: return 0;
  }
}

// ChangeStatBuffs (bsc:6937) raising the user's own stat by n, as the
// self-targeting moves do (MOVE_EFFECT_AFFECTS_USER | STAT_CHANGE_ALLOW_PTR):
// only the cap stops it, and a raise at +6 marks the move MISSED
// (bsc:7097), which moveend reads (only Conversion 2's last-landed fields,
// which Showdown doesn't keep). Returns the stages gained, which the log's
// Boost carries: Showdown prints `-boost|...|0` for a primary raise at the
// cap (sim/battle.ts:2083) and no -fail (Battle.boost returns null, a
// silent failure).
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

// The effects whose script is BattleScript_EffectHit, maybe after setting a
// move effect or a hit marker: the plain hits (Quick Attack and Vital Throw
// differ only in the table; Pursuit only when the target switches,
// pursues()), Effect::Recoil (MOVE_EFFECT_RECOIL_25, bs1:897), the
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
  case Effect::Thief:
  case Effect::SolarBeam:
  case Effect::RazorWind:
  case Effect::SkullBash:
  case Effect::SkyAttack:
  case Effect::SemiInvulnerable:
  case Effect::Recharge:
  case Effect::MultiHit:
  case Effect::Superpower:
  case Effect::Overheat:
  case Effect::Return:
  case Effect::Frustration:
  case Effect::Flail:
  case Effect::Eruption:
  case Effect::Gust:
  case Effect::Twister:
  case Effect::FakeOut:
  case Effect::FlinchMinimizeHit:
  case Effect::AllStatsUpHit:
  case Effect::Twineedle:
  case Effect::DoubleHit:
  case Effect::TripleKick:
  case Effect::Facade:
  case Effect::Revenge:
  case Effect::Smellingsalt:
  case Effect::WeatherBall:
  case Effect::LowKick:
  case Effect::SecretPower:
  case Effect::PayDay:
  case Effect::RecoilIfMiss:
  // Step 5 group 2: fixed and variable damage, and the OHKO moves; see
  // fixed_damage() and hit().
  case Effect::Sonicboom:
  case Effect::DragonRage:
  case Effect::SuperFang:
  case Effect::Psywave:
  case Effect::Ohko:
  case Effect::Present:
  case Effect::Magnitude:
  // Step 5 group 4: the locked moves and Rage (hit(); Bide is bide()).
  case Effect::Rampage:
  case Effect::Uproar:
  case Effect::Rollout:
  case Effect::FuryCutter:
  case Effect::Rage:
  // Step 5 group 5: the binding moves and Spit Up (hit()).
  case Effect::Trap:
  case Effect::SpitUp:
  case Effect::DreamEater:
    return true;
  default:
    return false;
  }
}

// setmoveeffect in each effect's script (bs1). Blaze Kick and Poison Tail
// share Burn Hit's and Poison Hit's scripts; Thunder sets PARALYSIS.
// Stomp, Astonish, Needle Arm and Extrasensory flinch (BattleScript_EffectStomp, bs1:1898); Ancient
// Power and Silver Wind raise all five stats (bs1:1772); Twineedle poisons
// (sMULTIHIT_EFFECT, bs1:1078); Secret Power paralyzes in a link battle
// (getsecretpowereffect's default, tables.hpp), as Showdown's gen4
// secretpower.
constexpr Secondary secondary_of(Effect effect) {
  switch (effect) {
  case Effect::PoisonHit:
  case Effect::PoisonTail:
  case Effect::Twineedle: return Secondary::Poison;
  case Effect::PoisonFang: return Secondary::Toxic;
  case Effect::BurnHit:
  case Effect::BlazeKick:
  case Effect::ThawHit: return Secondary::Burn;
  case Effect::FreezeHit: return Secondary::Freeze;
  case Effect::ParalyzeHit:
  case Effect::Thunder:
  case Effect::SemiInvulnerable: // Bounce; Fly, Dig and Dive have chance 0
  case Effect::SecretPower: return Secondary::Paralysis;
  case Effect::ConfuseHit: return Secondary::Confusion;
  case Effect::FlinchHit:
  case Effect::FlinchMinimizeHit:
  case Effect::Twister:
  case Effect::FakeOut:
  case Effect::SkyAttack:
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
  case Effect::AllStatsUpHit: return Secondary::AllUp;
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

// The uproar's onAnySetStatus message for a sleep it stops on p's active
// Pokémon: -fail|P|slp|[from] Uproar, with [msg] if P is causing it.
template <typename O>
constexpr void uproar_stops(const Battle& battle, Player p, O& options) {
  const bool own = battle.side(p).active.volatiles.uproar != 0;
  protocol::fail(options.log, protocol::ident(battle, p),
                 own ? protocol::Fail::UproarMsg : protocol::Fail::Uproar);
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

// Showdown's gen3 focuspunch volatile, from the start of a turn p chose
// Focus Punch (beforeTurnCallback) to its residuals: its onTryAddVolatile
// stops a flinch (data/moves.ts focuspunch condition), so Fake Out only
// breaks the focus. The decomp sets the flinch, and the flinch stops Focus
// Punch first.
constexpr bool focusing(const Battle& battle, Player p) {
  const Request& r = battle.request;
  return r.choice[index(p)].type() == Choice::Move && r.move[index(p)] == Move::FocusPunch;
}

// Whether a move's major status `s` takes hold on p's foe: Safeguard, then
// can_status, then Sleep Clause for a sleep.
constexpr bool status_takes(const Battle& battle, Player player, Secondary s) {
  return battle.side(foe(player)).conditions.safeguard == 0 &&
         can_status(battle, foe(player), s) &&
         !(s == Secondary::Sleep && sleep_clause(battle, foe(player)));
}

// Sets a major status on `target`, as SetMoveEffect does once statusChanged
// (bsc:2476): poison, toxic, paralysis and burn arm Synchronize (bsc:2503).
// The decomp's sleep and freeze also cancel a multi-turn move here
// (bsc:2296); Showdown keeps it until the move is aborted (onMoveAborted,
// in cancel()), so a Pokémon that wakes or thaws first still fires a
// charged move, and this follows Showdown. A forced choice then always
// finds its move locked at the action. Changing the status
// restarts the toxic counter (data.hpp, Status). Log: Status, from `src`
// as Showdown prints it (protocol::status).
template <typename O>
constexpr void inflict(Battle& battle, Player target, Secondary s,
                       MoveState& m, O& options,
                       protocol::StatusSource src = protocol::StatusSource::none()) {
  Side& side = battle.side(target);
  Status& status = side.get(1).status;
  switch (s) {
  case Secondary::Sleep:
    // (Random() & 3) + 2 turns, hidden, so drawn without Chance; the public
    // count starts at 1.
    status = Status::slp(uint8_t(Status::MIN_SLEEP + battle.rng.range(4)));
    if constexpr (CHANCE<O>) options.chance.durations.get(target).sleep[0] = 1;
    break;
  case Secondary::Poison: status = Status::psn(); break;
  case Secondary::Toxic: status = Status::tox(); break;
  case Secondary::Burn: status = Status::brn(); break;
  case Secondary::Freeze: status = Status::frz(); break;
  case Secondary::Paralysis: status = Status::par(); break;
  default: assert(false && "not a major status");
  }
  side.active.toxic = 0;
  if (s != Secondary::Freeze && s != Secondary::Sleep) {
    m.synchronize = s;
    m.synchronized = target;
  }
  protocol::status(options.log, protocol::ident(battle, target), status, src);
  // Showdown's Lum Berry eats at once (onAfterSetStatus, after Synchronize's,
  // which is deferred here: a Synchronize holder's Lum waits for moveend).
  if (item_data(side.get(1).item).effect == HoldEffect::CureStatus &&
      side.active.ability != Ability::Synchronize) {
    item_cure(battle, target, options);
  }
}

// MOVE_EFFECT_CONFUSION taking hold on `target` (bsc:2541): the counter is
// Random() % 4 + 2, hidden, so it's drawn without Chance (CLAUDE.md,
// "Chance"), and the public count starts at 1. Log: Start confusion.
template <typename O>
constexpr void confuse(Battle& battle, Player target, O& options,
                       protocol::Start how = protocol::Start::Confusion) {
  ActivePokemon& t = battle.side(target).active;
  assert(t.volatiles.confusion == 0);
  protocol::start(options.log, protocol::ident(battle, target), how);
  t.volatiles.confusion = uint8_t(2 + battle.rng.range(4));
  if constexpr (CHANCE<O>) options.chance.durations.get(target).confusion = 1;
}

// seteffectwithchance (bsc:2923) and SetMoveEffect (bsc:2380) for a move's
// secondary effect on its target, after the damage. The chance is the
// table's, doubled by Serene Grace; it's drawn only when the effect could
// take hold (or, for Poison Fang, change the result flags), since otherwise
// no outcome changes anything. `i` is the hit (Twineedle's loop runs it
// per hit, as Showdown's secondaries).
template <typename O>
constexpr void secondary(Battle& battle, Player player, MoveState& m,
                         O& options, uint8_t i = 0) {
  const MoveData& data = get(m.move);
  const Secondary s = secondary_of(data.effect);
  if (s == Secondary::None || (m.result & MoveResult::NO_EFFECT)) return;
  // Fake Out's flinch is certain (MOVE_EFFECT_CERTAIN, table chance 0).
  const uint32_t chance =
      data.effect == Effect::FakeOut
          ? 100u
          : data.chance * (battle.side(player).active.ability ==
                                   Ability::SereneGrace
                               ? 2u
                               : 1u);
  if (chance == 0) return;

  // A stat raise on the user (MOVE_EFFECT_AFFECTS_USER): SetMoveEffect's
  // preamble checks the user, which is alive, and ChangeStatBuffs only fails
  // at +6 (bsc:6937).
  // Ancient Power and Silver Wind (MOVE_EFFECT_ALL_STATS_UP, bsc:2814):
  // Atk, Def, Spe, SpA and SpD up one each, whichever aren't at +6. Drawn
  // only if one isn't. Log, as Showdown's self boosts: one -boost per stat
  // that rose, in its key order (atk, def, spa, spd, spe), none at the cap.
  if (s == Secondary::AllUp) {
    ActivePokemon& a = battle.side(player).active;
    constexpr uint8_t ORDER[5] = {0, 1, 3, 4, 2};
    bool any = false;
    for (const uint8_t stat : ORDER) any |= stage(a.boosts, stat) < 6;
    if (!any) return;
    if (chance < 100 && !rolls::secondary(battle, player, i, chance, options)) {
      return;
    }
    for (const uint8_t stat : ORDER) {
      int8_t& b = stage(a.boosts, stat);
      if (b >= 6) continue;
      b++;
      protocol::boost(options.log, protocol::ident(battle, player), stat, 1, false);
    }
    return;
  }
  if (s == Secondary::AtkUp || s == Secondary::DefUp) {
    int8_t& b = stage(battle.side(player).active.boosts,
                      s == Secondary::AtkUp ? 0 : 1);
    if (b >= 6) return;
    if (chance < 100 && !rolls::secondary(battle, player, i, chance, options)) {
      return;
    }
    b++;
    protocol::boost(options.log, protocol::ident(battle, player),
                    s == Secondary::AtkUp ? 0 : 1, 1, false);
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
              t.ability != Ability::InnerFocus && !focusing(battle, foe(player));
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
  if (chance < 100 && !rolls::secondary(battle, player, i, chance, options)) {
    return;
  }

  switch (s) {
  case Secondary::AtkDown:
  case Secondary::DefDown:
  case Secondary::SpeDown:
  case Secondary::SpaDown:
  case Secondary::SpdDown:
  case Secondary::AccDown: {
    const uint8_t stat = uint8_t(uint8_t(s) - uint8_t(Secondary::AtkDown));
    stage(t.boosts, stat)--;
    protocol::boost(options.log, protocol::ident(battle, foe(player)), stat, 1, true);
    break;
  }
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
// Fire move unless the holder is frozen (BattleScript_FlashFireBoost,
// bs1:4101). Showdown agrees (gen3 Volt Absorb skips Thunder Wave; gen4
// Flash Fire's frozen check), except that its Flash Fire lets a
// Will-O-Wisp through to a Fire type, a statused holder or one behind a
// Substitute (its onTryHit), where the move then fails as usual; this
// follows Showdown. Showdown's absorbing abilities are TryHit handlers, so
// they come after the accuracy roll and Protect (try_hit).
enum class Absorb : uint8_t {
  None,
  Idle,  // absorbed with no effect: -immune [from] the ability
  Takes, // absorbed, healing or starting Flash Fire
};

constexpr Absorb absorbs(const Battle& battle, Player player, const MoveState& m) {
  const Side& side = battle.side(foe(player));
  const ActivePokemon& t = side.active;
  const Pokemon& target = side.get(1);
  const MoveData& data = get(m.move);
  switch (t.ability) {
  case Ability::VoltAbsorb:
  case Ability::WaterAbsorb: {
    const Type type =
        t.ability == Ability::VoltAbsorb ? Type::Electric : Type::Water;
    if (m.type != type || data.bp == 0) return Absorb::None;
    return target.hp != target.stats.hp ? Absorb::Takes : Absorb::Idle;
  }
  case Ability::FlashFire:
    if (m.type != Type::Fire || target.status.frozen()) return Absorb::None;
    if (data.effect == Effect::WillOWisp &&
        (has_type(t, Type::Fire) || target.status.any() || t.volatiles.substitute)) {
      return Absorb::None;
    }
    return t.volatiles.flash_fire ? Absorb::Idle : Absorb::Takes;
  default:
    return Absorb::None;
  }
}

// The absorption absorbs() found; its script then goes to moveend.
template <typename O>
constexpr void absorbed(Battle& battle, Player player, MoveState& m, Absorb a,
                        O& options) {
  assert(a != Absorb::None);
  Side& side = battle.side(foe(player));
  ActivePokemon& t = side.active;
  Pokemon& target = side.get(1);
  const ID id = protocol::ident(battle, foe(player));
  if (t.ability != Ability::FlashFire) m.result |= MoveResult::DOESNT_AFFECT_FOE;
  if (a == Absorb::Idle) {
    protocol::immune(options.log, id, protocol::Immune::Ability, t.ability);
  } else if (t.ability == Ability::FlashFire) {
    t.volatiles.flash_fire = true;
    protocol::start(options.log, id, protocol::Start::FlashFire);
  } else {
    heal(target, fraction(target, 4));
    protocol::heal_ability(options.log, id, target, t.ability,
                           protocol::ident(battle, player));
  }
}

// How a move aimed at the foe meets it, for try_hit.
struct Aim {
  // A natural immunity: the type chart (Showdown's runImmunity, which
  // includes Levitate) or Leech Seed's Grass (onTryImmunity). Reported as
  // -immune whether the roll hits or not, after TryHit if it does.
  bool immune = false;
  // Showdown's accuracy `true`: no roll (Nightmare, Pain Split, Mimic,
  // Memento, Mean Look, Yawn, Ghost Curse; the decomp's
  // NO_ACC_CALC_CHECK_LOCK_ON).
  bool sure = false;
  // Whether a hit that gets past TryHit changes the state. If not, the roll
  // changes only the log (a miss, or the move's own failure after it).
  bool matters = true;
  // The target's Wonder Guard stops the move (its TryHit, after Protect's
  // and the other abilities'). On a miss a natural immunity is still a
  // plain -immune, so the roll changes the log.
  bool wonder_guard = false;
};

template <typename O>
constexpr void execute(Battle& battle, Player player, MoveState& m, O& options);

// Whether Magic Coat bounces the move (Showdown's reflectable flag): the
// decomp's FLAG_MAGIC_COAT_AFFECTED, which Showdown also gives Kinesis.
constexpr bool reflectable(Move move) {
  return (get(move).flags & Flag::MagicCoatAffected) || move == Move::Kinesis;
}

// The target's Magic Coat bounces m, as Showdown's gen4 magiccoat onTryHit
// (which gen3 inherits): once past the accuracy roll and Protect, the
// target uses the move back on the user, [from] Magic Coat, with no PP, and
// the volatile goes. A bounced move isn't bounced again. The decomp bounces
// in the attack canceler, before the accuracy check, and takes Pressure's
// PP for it.
constexpr bool bounces(Player player, const MoveState& m) {
  return m.turn != nullptr && m.turn->magic_coat[index(foe(player))] &&
         m.chosen != Move::MagicCoat && reflectable(m.move);
}

// The move that runs for `user` on behalf of Snatch or Magic Coat (`by`).
template <typename O>
constexpr void call_back(Battle& battle, Player user, Move move, Move by,
                         const MoveState& m, O& options) {
  MoveState c{};
  c.move = move;
  c.chosen = by;
  c.from = by;
  c.slot = MoveState::NO_SLOT;
  c.no_pp = true;
  c.called = true;
  c.turn = m.turn;
  execute(battle, user, c, options);
  assert(!c.drag && !c.pass);
}

// Snatch, as Showdown's gen4 snatch onAnyPrepareHit (which gen3 inherits):
// a snatchable move (the decomp's FLAG_SNATCH_AFFECTED, which matches
// Showdown's snatch flag) is stolen once the user has paid its PP, before
// any of the move's own checks. The snatcher's volatile goes, it takes
// another PP from Snatch if the user has Pressure, and it uses the move
// itself, [from] Snatch, with no PP; the user's move does nothing more.
// A snatched move isn't snatched again. The decomp steals in the attack
// canceler, before the PP. Returns whether the move was stolen. Log:
// Activate Snatch [of] the user.
template <typename O>
constexpr bool snatched(Battle& battle, Player player, MoveState& m, O& options) {
  const Player snatcher = foe(player);
  if (m.turn == nullptr || !m.turn->snatch[index(snatcher)] ||
      m.chosen == Move::Snatch || !(get(m.move).flags & Flag::SnatchAffected)) {
    return false;
  }
  m.turn->snatch[index(snatcher)] = false;
  protocol::activate_with(options.log, protocol::ident(battle, snatcher),
                          protocol::Activate::Snatch, Move::None,
                          protocol::ident(battle, player));
  if (battle.side(player).active.ability == Ability::Pressure) {
    const ActivePokemon& s = battle.side(snatcher).active;
    for (uint8_t i = 0; i < 4; i++) {
      if (s.moves[i].id == Move::Snatch) {
        deduct_pp(battle, snatcher, i, 1);
        break;
      }
    }
  }
  call_back(battle, snatcher, m.move, Move::Snatch, m, options);
  m.result |= MoveResult::FAILED;
  return true;
}

// Showdown's gen3 tryMoveHit (data/mods/gen3/scripts.ts:263) from the
// Invulnerability check through TryHit, for a move aimed at the foe, with
// its messages: a semi-invulnerable target is missed with no roll; then
// the accuracy roll; on a miss -immune for a natural immunity, else -miss;
// on a hit TryHit's handlers in their order (Protect's priority 3, Magic
// Coat's 2 (bounces), then the target's ability: Soundproof and the
// absorbing abilities), then -immune for a natural immunity. Returns whether the move goes on, to its
// own checks (TryPrimaryHit's Substitute, then its effect), which the
// caller makes; if not, the log is written and the result flags set.
//
// The decomp checks most of a move's own failures before its accuracy
// check, and Protect and semi-invulnerability inside it; the state is the
// same either way, and the order of the messages is Showdown's. The roll is
// recorded only if its outcome can change the state: not into Protect or
// Soundproof, not for an absorption that changes nothing, and not when
// `aim.matters` is false. Otherwise it is drawn for the log alone,
// unrecorded (CLAUDE.md, "Chance"), and not at all when both outcomes log
// the same -immune.
template <typename O>
constexpr bool try_hit(Battle& battle, Player player, MoveState& m, Aim aim,
                       O& options) {
  const Player defender = foe(player);
  const ID user = protocol::ident(battle, player);
  const ID target = protocol::ident(battle, defender);
  if (invulnerable(battle, player, m)) {
    protocol::miss(options.log, user, target);
    m.result |= MoveResult::MISSED;
    return false;
  }
  const ActivePokemon& t = battle.side(defender).active;
  // An OHKO move from a lower level fails before the roll (Showdown's gen3
  // tryMoveHit; Cmd_tryKO, bsc:7490, checks it with the accuracy roll).
  const bool ohko = get(m.move).effect == Effect::Ohko;
  if (ohko && battle.side(player).get(1).level < battle.side(defender).get(1).level) {
    protocol::immune(options.log, target, protocol::Immune::OHKO);
    m.result |= MoveResult::DOESNT_AFFECT_FOE;
    return false;
  }
  const bool protect = protected_from(m, player);
  const bool soundproof = t.ability == Ability::Soundproof && sound(m.move);
  // Sturdy stops OHKO moves (its onTryHit, after Protect's).
  const bool sturdy = ohko && t.ability == Ability::Sturdy;
  const Absorb absorb = absorbs(battle, player, m);
  const bool bounce = bounces(player, m);
  const bool blocked = protect || soundproof || sturdy || absorb != Absorb::None;

  bool hits = true;
  if (const uint32_t calc = aim.sure ? 100 : accuracy(battle, player, m); calc < 100) {
    assert(calc > 0);
    // A block by Protect, Sturdy, Water Absorb or Volt Absorb with no heal
    // leaves the state a miss would (a NO_EFFECT flag). Soundproof and
    // Flash Fire set none, so moveend records the move as landed
    // (last_landed_move, last_taken_move) and their roll matters even when
    // the block itself changes nothing. A locked rampage's block by Protect
    // renews the lock, which a miss doesn't.
    const bool renews = protect && get(m.move).effect == Effect::Rampage && m.locked;
    const bool recorded =
        renews ||
        (!protect && !sturdy &&
         (bounce || soundproof || absorb == Absorb::Takes ||
          (absorb == Absorb::Idle && t.ability == Ability::FlashFire) ||
          (absorb == Absorb::None && !aim.immune &&
           (aim.matters || (mirrors(t) && !aim.wonder_guard)))));
    if (recorded) {
      hits = rolls::hit(battle, player, calc, options);
    } else if (!aim.immune || blocked || aim.wonder_guard) {
      hits = battle.rng.chance(calc, 100);
    }
  }
  if (!hits) {
    if (aim.immune) {
      protocol::immune(options.log, target);
      m.result |= MoveResult::DOESNT_AFFECT_FOE;
    } else {
      protocol::miss(options.log, user, target);
      m.result |= MoveResult::MISSED;
    }
    return false;
  }
  if (protect) {
    protocol::activate(options.log, target, protocol::Activate::Protect);
    m.result |= MoveResult::MISSED;
    if (get(m.move).effect == Effect::Rampage && m.locked) {
      lock_renew(battle, player);
    }
    return false;
  }
  if (bounce) {
    m.turn->magic_coat[index(defender)] = false;
    call_back(battle, defender, m.move, Move::MagicCoat, m, options);
    m.result |= MoveResult::FAILED;
    return false;
  }
  // BattleScript_SoundproofProtected (bs1:4158) sets no result flag.
  // Sturdy's block sets MISSED (Cmd_tryKO, bsc:7490), as a miss would.
  if (soundproof || sturdy) {
    protocol::immune(options.log, target, protocol::Immune::Ability, t.ability);
    if (sturdy) m.result |= MoveResult::MISSED;
    return false;
  }
  if (absorb != Absorb::None) {
    absorbed(battle, player, m, absorb, options);
    return false;
  }
  if (aim.wonder_guard) {
    protocol::immune(options.log, target, protocol::Immune::Ability, Ability::WonderGuard);
    m.result |= MoveResult::DOESNT_AFFECT_FOE;
    return false;
  }
  if (aim.immune) {
    protocol::immune(options.log, target);
    m.result |= MoveResult::DOESNT_AFFECT_FOE;
    return false;
  }
  m.attacked = true;
  return true;
}

// ABILITYEFFECT_SYNCHRONIZE and ATK_SYNCHRONIZE (util:2957, 2973): if
// `holder` has Synchronize and this execution armed it, the poison (toxic
// becomes poison), paralysis or burn passes to the other Pokémon, which
// arms it again for the next case. The cartridge skips Safeguard here
// (HITMARKER_STATUS_ABILITY_EFFECT); Showdown's Safeguard blocks it
// (data/moves.ts:15598), with its -activate, and this follows Showdown.
template <typename O>
constexpr void synchronize(Battle& battle, Player holder, MoveState& m,
                           O& options) {
  if (battle.side(holder).active.ability != Ability::Synchronize ||
      m.synchronize == Secondary::None || m.synchronized != holder) {
    return;
  }
  const Secondary s =
      m.synchronize == Secondary::Toxic ? Secondary::Poison : m.synchronize;
  m.synchronize = Secondary::None;
  const Player other = foe(holder);
  if (!can_status(battle, other, s)) return;
  if (battle.side(other).conditions.safeguard != 0) {
    protocol::activate(options.log, protocol::ident(battle, other), protocol::Activate::Safeguard);
    return;
  }
  inflict(battle, other, s, m, options,
          protocol::StatusSource::by_ability(Ability::Synchronize,
                                             protocol::ident(battle, holder)));
}

// MOVEEND_DEFROST: a Fire move's special damage thaws the target. Hidden
// Power's damage is recorded under its base type, Normal, so Hidden Power
// Fire doesn't thaw (datahpupdate, bsc:1852). The table's type, not the type
// as used: Showdown's gen3 frz checks the base move's type
// (data/mods/gen3/conditions.ts:47), so Weather Ball in sun doesn't thaw.
// It's a DamagingHit handler there, run before the abilities' (a condition's
// sub-order), so the hit calls it before ability_on_damage.
template <typename O>
constexpr void defrost(Battle& battle, Player player, const MoveState& m, O& options) {
  Pokemon& target = battle.side(foe(player)).get(1);
  const MoveData& data = get(m.move);
  if (!m.self && target.status.frozen() && !target.fainted() && m.damaged &&
      !(m.result & MoveResult::NO_EFFECT) && data.type == Type::Fire &&
      data.effect != Effect::HiddenPower) {
    protocol::cure_status(options.log, protocol::ident(battle, foe(player)), target.status);
    target.status = Status::none();
  }
}

// ABILITYEFFECT_ON_DAMAGE (util:2735): the target's Color Change, or its
// contact ability acting on the attacker (p). Only after the target's own
// HP took damage from a move with an effect, so never on a self-targeting
// move or a confusion self-hit, and as Showdown's gen3 handlers (`if
// (damage && ...)`) not after a hit that took no HP (Endure at 1 HP).
// Statuses skip Safeguard on the cartridge; Showdown's Safeguard blocks
// them, and this follows Showdown. Effect Spore's sleep is subject to Sleep
// Clause, as Showdown's onSetStatus.
template <typename O>
constexpr void ability_on_damage(Battle& battle, Player player, MoveState& m,
                                 uint8_t i, O& options) {
  if (m.self || !m.damaged || m.hp_dealt == 0 || (m.result & MoveResult::NO_EFFECT)) return;
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const Pokemon& target_mon = battle.side(target).get(1);
  const MoveData& data = get(m.move);

  if (t.ability == Ability::ColorChange) {
    if (m.move != Move::Struggle && data.bp != 0 && m.type != Type::Mystery &&
        !has_type(t, m.type) &&
        !target_mon.fainted()) {
      t.types[0] = m.type;
      t.types[1] = m.type;
      protocol::start_type(options.log, protocol::ident(battle, target), m.type, true);
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
  const auto from_ability = [&] {
    return protocol::StatusSource::by_ability(t.ability, protocol::ident(battle, target));
  };
  Secondary s = Secondary::None;
  switch (t.ability) {
  case Ability::RoughSkin:
    passive_damage(battle, player, fraction(user, 16), options);
    protocol::damage_ability(options.log, protocol::ident(battle, player), user,
                             Ability::RoughSkin, protocol::ident(battle, target));
    return;
  case Ability::EffectSpore: {
    if (!takes(Secondary::Sleep) && !takes(Secondary::Poison) &&
        !takes(Secondary::Paralysis)) {
      return;
    }
    if (!rolls::contact(battle, player, i, 10, options)) return;
    const uint8_t which = rolls::spore(battle, player, i, options);
    s = which == 1   ? Secondary::Sleep
        : which == 2 ? Secondary::Poison
                     : Secondary::Paralysis;
    if (takes(s)) inflict(battle, player, s, m, options, from_ability());
    return;
  }
  case Ability::PoisonPoint: s = Secondary::Poison; break;
  case Ability::Static: s = Secondary::Paralysis; break;
  case Ability::FlameBody: s = Secondary::Burn; break;
  case Ability::CuteCharm: {
    ActivePokemon& a = side.active;
    const Gender g1 = user.personality.gender();
    const Gender g2 = target_mon.personality.gender();
    // As Showdown, a holder that faints to the hit still attracts.
    if (a.ability == Ability::Oblivious || a.volatiles.infatuated ||
        g1 == Gender::None || g2 == Gender::None || g1 == g2) {
      return;
    }
    if (rolls::contact(battle, player, i, 3, options)) {
      a.volatiles.infatuated = true;
      protocol::start(options.log, protocol::ident(battle, player),
                      protocol::Start::AttractCuteCharm, protocol::ident(battle, target));
    }
    return;
  }
  default:
    return;
  }
  if (takes(s) && rolls::contact(battle, player, i, 3, options)) {
    inflict(battle, player, s, m, options, from_ability());
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
    // Showdown's onUpdate cures: -activate, then -curestatus [msg].
    const ID id = protocol::ident(battle, p);
    const auto cure = [&] {
      protocol::activate_ability(options.log, id, side.active.ability);
      protocol::cure_status(options.log, id, st);
      mon.status = Status::none();
    };
    switch (side.active.ability) {
    case Ability::Immunity:
      if (st.poisoned()) cure();
      break;
    case Ability::OwnTempo:
      if (v.confusion != 0) {
        protocol::activate_ability(options.log, id, Ability::OwnTempo);
        protocol::end_effect(options.log, id, protocol::End::Confusion);
        v.confusion = 0;
        if constexpr (CHANCE<O>) options.chance.durations.get(p).confusion = 0;
      }
      break;
    case Ability::Limber:
      if (st.paralyzed()) cure();
      break;
    case Ability::Insomnia:
    case Ability::VitalSpirit:
      if (st.asleep()) {
        cure();
        wake(battle, p, options);
      }
      break;
    case Ability::WaterVeil:
      if (st.burned()) cure();
      break;
    case Ability::MagmaArmor:
      if (st.frozen()) cure();
      break;
    case Ability::Oblivious:
      // Showdown's gen5 onUpdate (no -activate before gen 6).
      if (v.infatuated) {
        protocol::end_effect(options.log, id, protocol::End::Attract);
        protocol::end_effect(options.log, id, protocol::End::AttractOblivious);
      }
      v.infatuated = false;
      break;
    default:
      break;
    }
  }
}

// King's Rock (ItemBattleEffects' ITEMEFFECT_KINGSROCK_SHELLBELL, util:3763),
// as Showdown's gen3 King's Rock (data/mods/gen3/items.ts): a 10% flinch
// secondary added to the moves the decomp flags, so it follows the
// secondaries' rules. Serene Grace doubles it (its onModifyMove runs after
// the item's); the decomp's draw ignores Serene Grace. Hit `i` must have
// taken the target's own HP; a Substitute, Shield Dust, Inner Focus or a
// target that has already moved stops it.
template <typename O>
constexpr void flinch_item(Battle& battle, Player player, const MoveState& m,
                           uint8_t i, O& options) {
  const Side& side = battle.side(player);
  if (m.release || !holds(side, HoldEffect::Flinch) || !m.damaged ||
      !(get(m.move).flags & Flag::KingsRockAffected)) {
    return;
  }
  Side& target_side = battle.side(foe(player));
  ActivePokemon& t = target_side.active;
  if (target_side.get(1).fainted() || !m.first || t.volatiles.flinched ||
      t.ability == Ability::ShieldDust || t.ability == Ability::InnerFocus ||
      focusing(battle, foe(player))) {
    return;
  }
  const uint32_t chance = item_data(side.get(1).item).param *
                          (side.active.ability == Ability::SereneGrace ? 2u : 1u);
  if (rolls::kings_rock(battle, player, i, chance, options)) t.volatiles.flinched = true;
}

// Cmd_moveend (bsc:4210) with moveendall: the cases in the decomp's order.
// The Stockpile volatile's end (its onEnd). Log: End Stockpile.
template <typename O>
constexpr void stockpile_end(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.stockpile == 0) return;
  a.stockpile = 0;
  protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Stockpile);
}

// The AfterMove handlers (Showdown's runMove, sim/battle-actions.ts:311),
// for `ran`, the last move the action called (`activeMove`): moveend's, or
// use_move's when no moveend ran them. `charged`: the user had Charge
// before the move.
// - Charge's onAfterMove and onMoveAborted (self_volatile), for any move but
//   Charge (gen8's condition, which gen3 inherits) or one that called it;
//   not after Pursuit on a switching target, which Showdown runs with
//   useMove, nor for a user that fainted (Showdown prints no -end then;
//   faint() clears it).
// - Spit Up's onAfterMove: once the move ran, hit or not, the layers go.
template <typename O>
constexpr void after_move(Battle& battle, Player player, const MoveState& m,
                          Move ran, bool charged, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (charged && !m.pursuing && ran != Move::Charge && !battle.side(player).get(1).fainted()) {
    a.volatiles.charged_up = false;
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Charge);
  }
  if (m.obeys && get(ran).effect == Effect::SpitUp) stockpile_end(battle, player, options);
}

template <typename O>
constexpr void moveend(Battle& battle, Player player, MoveState& m,
                       O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Side& target_side = m.self ? side : battle.side(foe(player));
  ActivePokemon& t = target_side.active;
  const bool no_effect = m.result & MoveResult::NO_EFFECT;

  // MOVEEND_RAGE is rage(), at Showdown's onHit.

  // MOVEEND_DEFROST, where the hit didn't already run it (defrost()).
  defrost(battle, player, m, options);

  // MOVEEND_SYNCHRONIZE_TARGET, ON_DAMAGE_ABILITIES, IMMUNITY_ABILITIES,
  // SYNCHRONIZE_ATTACKER. No self-targeting move arms Synchronize (Rest's
  // sleep doesn't), so its target case is the foe's.
  if (!m.self) synchronize(battle, foe(player), m, options);
  if (!m.contact_done) ability_on_damage(battle, player, m, 0, options);
  ability_immunity(battle, options);
  synchronize(battle, player, m, options);

  // MOVEEND_CHANGED_ITEMS (Thief and Covet; Trick swaps in trick()),
  // which the decomp runs after MOVEEND_CHOICE_MOVE, so a Choice Band taken
  // with Thief doesn't lock its new holder. Showdown's lock comes after the
  // move (gen4 items.ts onAfterMove) and locks it into Thief; this follows
  // Showdown.
  if (m.stolen != Item::None) side.get(1).item = m.stolen;

  // MOVEEND_CHOICE_MOVE, as Showdown's (gen4 items.ts onAfterMove): any
  // move locks, Struggle or one the user doesn't know too; such a lock
  // lasts until the end of the turn (turn_cleanup), as choicelock's
  // onDisableMove. A Baton Pass that worked doesn't lock.
  if (m.obeys && holds(side, HoldEffect::ChoiceBand) && a.choice_move == Move::None &&
      !(m.chosen == Move::BatonPass && !(m.result & MoveResult::FAILED))) {
    a.choice_move = m.chosen;
  }

  // MOVEEND_ATTACKER_VISIBLE: a move that failed or was stopped ends the
  // attacker's semi-invulnerable turn.
  if (no_effect || m.unable) {
    a.volatiles.on_air = false;
    a.volatiles.underground = false;
    a.volatiles.underwater = false;
  }

  // Showdown's AfterMove comes before the Update that cures by item.
  after_move(battle, player, m, m.move, a.volatiles.charged_up, options);
  m.after_move = true;

  // MOVEEND_ITEM_EFFECTS_ALL: ITEMEFFECT_MOVE_END's cures. The decomp
  // stops at the first Pokémon whose item acts (util:3740); Showdown's
  // onUpdate cures both, and this follows it.
  for (const Player p : {Player::P1, Player::P2}) {
    if (!battle.side(p).get(1).fainted()) item_cure(battle, p, options);
  }

  // MOVEEND_KINGSROCK_SHELLBELL: King's Rock for a single hit, then Shell
  // Bell, as Showdown's (onAfterMoveSecondarySelf, data/items.ts:5656): an
  // eighth of the HP the move took from the target itself (not its
  // Substitute) over all its hits, at least 1. The decomp reads gHpDealt
  // (the last hit, Substitute included). Bide's release calls tryMoveHit
  // directly, so neither item acts there. A user that Destiny Bond took
  // still heals in Showdown's log, whose faintMessages come after; the
  // state keeps it at 0.
  if (!m.kings_rock_done) flinch_item(battle, player, m, 0, options);
  Pokemon& user = side.get(1);
  const uint16_t hp = user.fainted() ? m.bonded_hp : user.hp;
  if (!m.release && m.total != 0 && holds(side, HoldEffect::ShellBell) &&
      hp != 0 && hp != user.stats.hp) {
    const uint32_t h = m.total / item_data(user.item).param;
    Pokemon healed = user;
    healed.hp = hp;
    heal(healed, h == 0 ? 1 : h);
    if (!user.fainted()) user.hp = healed.hp;
    protocol::heal_item(options.log, protocol::ident(battle, player), healed, user.item,
                        protocol::ident(battle, foe(player)));
  }

  // MOVEEND_SUBSTITUTE
  for (Side& s : battle.sides) {
    if (s.active.substitute_hp == 0) s.active.volatiles.substitute = false;
  }

  // MOVEEND_UPDATE_LAST_MOVES: gChosenMove (Sleep Talk for its called move)
  // for the printed and last moves and Mirror Move, gCurrentMove for the
  // resulting and landed ones.
  if (m.attackstring) a.last_printed_move = m.chosen;
  // A Baton Pass that worked leaves the fields to the recipient; one that
  // failed is the user's last move, as Showdown's (Encore can then lock it).
  if (get(m.chosen).effect != Effect::BatonPass || (m.result & MoveResult::FAILED)) {
    // As Showdown's lastMove (moveUsed): a move stopped before it was used
    // leaves the last move as it was. The decomp writes MOVE_UNAVAILABLE,
    // so Encore after a full paralysis fails there and works here.
    // Bide's release runs from its onBeforeMove, so moveUsed doesn't run: a
    // Bide that Metronome called leaves Metronome the last move (Torment).
    if (m.obeys && !m.release) a.last_move = m.chosen;
    a.last_resulting_move = m.obeys ? m.move : UNAVAILABLE;
  }

  // MOVEEND_MIRROR_MOVE, as Showdown's attackedBy (mirror_move()).
  if (m.attacked && !m.self && mirrors(t)) {
    t.last_taken_move = m.move;
    t.last_taken_from = side.order[0];
  }

  // Showdown's AfterMove, which a move stopped before it ran skips
  // (runMove returns at BeforeMove): White Herb.
  if (!m.unable) white_herbs(battle, options);

  // Then faintMessages: Grudge takes the move's PP (faint_target), after
  // the item cures above, so a Leppa Berry waits for the next Update.
  if (m.grudge) {
    for (uint8_t s = 0; s < 4; s++) {
      if (a.moves[s].id == m.chosen && a.moves[s].pp != 0) {
        deduct_pp(battle, player, s, a.moves[s].pp);
      }
    }
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
// damage to deal. Log, as Showdown's onDamage handlers by priority (Endure
// -10, False Swipe -20, Focus Band -40): Activate Endure, nothing for False
// Swipe, or Activate Focus Band.
template <typename O>
constexpr uint32_t survive(Battle& battle, Player player, const MoveState& m,
                           uint32_t d, uint8_t i, O& options) {
  const Player defender = foe(player);
  const Side& foe_side = battle.side(defender);
  const Pokemon& target = foe_side.get(1);
  if (foe_side.active.volatiles.substitute || target.hp > d) return d;
  const ID id = protocol::ident(battle, defender);
  // A future attack lands at the end of the turn, with no Turn. Showdown's
  // endure volatile (duration 1) would still be up at that residual
  // (order 11; a duration with no order expires last), so an Endure that
  // turn should save the target there too. Not kept: neither move is on
  // teams/ou/.
  if (m.turn != nullptr && m.turn->endure[index(defender)]) {
    protocol::activate(options.log, id, protocol::Activate::Endure);
    return target.hp - 1u;
  }
  if (get(m.move).effect == Effect::FalseSwipe) return target.hp - 1u;
  const ItemData item = item_data(target.item);
  if (item.effect == HoldEffect::FocusBand &&
      rolls::focus_band(battle, player, i, item.param, options)) {
    protocol::activate(options.log, id, protocol::Activate::FocusBand);
    return target.hp - 1u;
  }
  return d;
}

// Jump Kick and Hi Jump Kick's crash (BattleScript_MoveMissedDoDamage,
// bs1:853), as Showdown's gen3 onMoveFail (data/mods/gen3/moves.ts:357,
// 372): when the move fails against a target not immune to Fighting (a
// miss, Protect, a semi-invulnerable target, Wonder Guard, or a hit that
// took no HP), the user loses half the damage the hit `h` would have
// dealt, at least 1 and at most half the target's max HP. Showdown
// computes that damage with getDamage, so it draws a crit and a roll, kept
// in hits[1] (the move has one hit), and prints neither -crit nor the
// effectiveness; the user's own Focus Band can save it (its onDamage
// takes any move's damage), drawn in hits[1] too. The decomp's crash has
// no crit and no cap (DMG_RECOIL_FROM_MISS, bsc:6747). Rock Head doesn't
// stop it, nor does a Substitute. Log: a plain -damage, the faint queued.
template <typename O>
constexpr void crash(Battle& battle, Player player, const MoveState& m, Hit h,
                     O& options) {
  Pokemon& user = battle.side(player).get(1);
  const Pokemon& target = battle.side(foe(player)).get(1);
  if (user.fainted() || (matchup(battle, player, h) & MoveResult::DOESNT_AFFECT_FOE)) {
    return;
  }
  h.crit = can_crit(battle, player) &&
           rolls::crit(battle, player, 1, crit_stage(battle, player, m.move), options);
  uint32_t d = apply_roll(damage(battle, player, h), rolls::damage(battle, player, 1, options)) / 2;
  const uint32_t cap = target.stats.hp / 2u;
  if (d > cap) d = cap;
  if (d == 0) d = 1;
  const ItemData item = item_data(user.item);
  if (d >= user.hp && item.effect == HoldEffect::FocusBand &&
      rolls::focus_band(battle, player, 1, item.param, options)) {
    protocol::activate(options.log, protocol::ident(battle, player),
                       protocol::Activate::FocusBand);
    d = user.hp - 1u;
  }
  if (d > user.hp) d = user.hp;
  user.hp = uint16_t(user.hp - d);
  protocol::damage(options.log, protocol::ident(battle, player), user);
  if (user.fainted()) faint(battle, player, options);
}

// The target's rage onHit (Showdown's base rage condition,
// data/moves.ts:14570): each hit of a non-Status move from the foe on the
// Pokémon itself (not its Substitute) raises its Attack, a plain boost, so
// -boost 0 at +6. It comes after the move's own onHit and the drain,
// before the self effects, secondaries and contact abilities. The decomp's MOVEEND_RAGE
// runs once, in moveend.
template <typename O>
constexpr void rage_boost(Battle& battle, Player defender, O& options) {
  ActivePokemon& t = battle.side(defender).active;
  const int8_t n = t.boosts.atk < 6 ? 1 : 0;
  t.boosts.atk = int8_t(t.boosts.atk + n);
  protocol::boost(options.log, protocol::ident(battle, defender), 0, n, false);
}

template <typename O>
constexpr void rage(Battle& battle, Player player, const MoveState& m, O& options) {
  const Player defender = foe(player);
  if (m.self || !m.damaged || !battle.side(defender).active.volatiles.rage ||
      get(m.move).bp == 0 || battle.side(defender).get(1).fainted()) {
    return;
  }
  rage_boost(battle, defender, options);
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
    m.unable = true; // Showdown's beforeMoveCallback: no AfterMove
    protocol::last_still(options.log);
    protocol::cant(options.log, protocol::ident(battle, player),
                   protocol::Cant::FocusPunch);
    pp_reduce(battle, player, m);
    return moveend(battle, player, m, options);
  }
  // BattleScript_EffectFakeOut (bs1:2048): only on the user's first turn
  // out (jumpifnotfirstturn, bsc:6786; README decision 14), else
  // FailedFromAtkString; then the plain hit with a certain flinch
  // (secondary()). Showdown counts move actions instead (activeMoveActions
  // > 1, ActivePokemon::move_actions), so a first action that a faint
  // cancelled leaves Fake Out working the turn after; this follows it. No
  // message (onTry returns false).
  if (effect == Effect::FakeOut && battle.side(player).active.move_actions > 1) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  // Spit Up (BattleScript_EffectSpitUp, bs1:2154), as Showdown: its onTry
  // fails with no Stockpile, silently (gen3 tryMoveHit returns at a failed
  // Try with no message; the decomp prints its own failure). Its power is
  // 100 per layer (basePowerCallback); the layers go after the move
  // (spit_up_end). Showdown's gen3 counts it as physical (Normal), with a
  // crit and a roll as any hit; the decomp's has no roll.
  if (effect == Effect::SpitUp && battle.side(player).active.stockpile == 0) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  // The two-turn moves: BattleScript_EffectSolarBeam (bs1:1903), RazorWind
  // (bs1:778), SkullBash (bs1:1811), SkyAttack (bs1:1064) and
  // SemiInvulnerable (bs1:1973: Fly, Dig, Dive, Bounce). Solar Beam in sun
  // (not suppressed by Cloud Nine or Air Lock) fires at once: the plain
  // hit. Otherwise the first turn charges (BattleScriptFirstChargingTurn,
  // bs1:795): ppreduce without an attack string, then MOVE_EFFECT_CHARGING
  // locks the move (STATUS2_MULTIPLETURNS and gLockedMoves, bsc:2571);
  // Skull Bash raises Defense (at +6, -boost 0 as stat_up_move); Fly and
  // Bounce take to the air, Dig goes underground and Dive underwater
  // (setsemiinvulnerablebit); then moveend. Protect doesn't stop the charge
  // (the canceler skips a two-turn move's first turn; here Protect is in
  // accuracy(), which the charge doesn't reach). The forced second turn
  // (BattleScript_TwoTurnMovesSecondTurn, bs1:785;
  // BattleScript_SecondTurnSemiInvulnerable, bs1:2000) clears the lock and
  // the semi-invulnerable flag after the canceler and hits from the
  // accuracy check with no PP; Sky Attack's flinch and Bounce's paralysis
  // are their secondaries. Showdown's gen3 agrees: twoturnmove, PP on the
  // first turn, the move's own volatile removed at onTryMove, onMoveAborted
  // (see cancel()). Where they differ, this follows Showdown: the charge
  // ends with the next turn even if that turn's move never came
  // (turn_cleanup, recharge_timer), and Razor Wind's crit ratio
  // (damage.hpp). Rain, sand and hail halve Solar Beam's power
  // (damage.hpp). Log: Prepare (then Skull Bash's Boost; in sun, Anim),
  // then Move [from] lockedmove on the second turn.
  if (two_turn(effect)) {
    ActivePokemon& a = battle.side(player).active;
    Volatiles& v = a.volatiles;
    if (v.multiple_turns) {
      v.multiple_turns = false;
      v.on_air = false;
      v.underground = false;
      v.underwater = false;
      m.no_pp = true;
    } else if (effect == Effect::SolarBeam &&
               effective_weather(battle) == Weather::Sun) {
      // Showdown's onTryMove still prepares, then marks the move line
      // [still] and plays the attack as -anim (data/moves.ts, solarbeam).
      const ID id = protocol::ident(battle, player);
      protocol::prepare(options.log, id, m.move);
      protocol::last_still(options.log);
      protocol::anim(options.log, id, m.move, protocol::ident(battle, foe(player)));
    } else {
      const ID id = protocol::ident(battle, player);
      protocol::last_still(options.log);
      protocol::prepare(options.log, id, m.move);
      pp_reduce(battle, player, m);
      if (effect == Effect::SkullBash) {
        protocol::boost(options.log, id, 1, raise(a, 1, 1, m), false);
      }
      v.multiple_turns = true;
      a.locked_move = m.move;
      a.recharge_timer = 2;
      if (effect == Effect::SemiInvulnerable) {
        v.on_air = m.move == Move::Fly || m.move == Move::Bounce;
        v.underground = m.move == Move::Dig;
        v.underwater = m.move == Move::Dive;
        assert(v.semi_invulnerable());
      }
      return moveend(battle, player, m, options);
    }
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
  // (ppNotAffectedByPressure). No message. Showdown's also fails when Sleep
  // Talk calls it: onTry needs the counter volatile, which only the chosen
  // move's beforeTurnCallback adds (data/moves.ts:3000); the cartridge
  // counters as usual.
  //
  // Endeavor (BattleScript_EffectEndeavor, bs1:2479): attackstring and
  // ppreduce, then Cmd_setdamagetohealthdifference (bsc:9366) fails it
  // unless the target has more HP than the user; then the plain hit for the
  // difference, with typecalc's immunities and no crit or roll. Showdown's
  // gen4 onTry agrees. Log: Fail.
  //
  // `fixed` is the damage of these, 0 for any other move.
  uint32_t fixed = 0;
  if (m.release) {
    const uint32_t taken = battle.side(player).active.bide_damage;
    fixed = taken > UINT32_MAX / 2 ? UINT32_MAX : 2 * taken;
    assert(fixed != 0);
  }
  if (effect == Effect::Counter || effect == Effect::MirrorCoat ||
      effect == Effect::Endeavor) {
    const uint16_t foe_hp = battle.side(foe(player)).get(1).hp;
    if (effect == Effect::Endeavor) {
      fixed = foe_hp > user.hp ? foe_hp - user.hp : 0u;
    } else if (!m.called) {
      fixed = 2u * (effect == Effect::Counter ? m.turn->physical
                                              : m.turn->special)[index(player)];
    }
    if (fixed == 0) {
      m.attackstring = true;
      pp_reduce(battle, player, m);
      m.result |= MoveResult::FAILED;
      // Endeavor's onTry prints a -fail without [still]; Counter and Mirror
      // Coat fail with no message (onTry returns false).
      if (effect == Effect::Endeavor) {
        protocol::fail(options.log, protocol::ident(battle, player));
      }
      return moveend(battle, player, m, options);
    }
  }

  // Explosion (bs1:374): attackstring and ppreduce come first, which
  // changes nothing; tryexplosion fails if any active Pokémon has Damp
  // (BattleScript_DampStopsExplosion goes to moveend); otherwise the user's
  // HP drops to 0 before the hit (setatkhptozero, bsc:6577) and it faints
  // after moveend, whether or not the hit lands. Log: Showdown's Damp
  // (onAnyTryMove, data/abilities.ts:812) marks the move line [still],
  // then Cant from the holder, the faster one if both hold it.
  if (effect == Effect::Explosion) {
    const bool d1 = battle.sides[0].active.ability == Ability::Damp;
    const bool d2 = battle.sides[1].active.ability == Ability::Damp;
    if (d1 || d2) {
      m.attackstring = true;
      pp_reduce(battle, player, m);
      const Player holder = d1 && d2 ? log_order(battle) : d1 ? Player::P1 : Player::P2;
      protocol::last_still(options.log);
      protocol::cant(options.log, protocol::ident(battle, holder), protocol::Cant::Damp,
                     m.move, protocol::ident(battle, player));
      return moveend(battle, player, m, options);
    }
    // Showdown faints the user here (selfdestruct 'always'), so its Faint
    // comes before the target's.
    user.hp = 0;
    protocol::faint(options.log, protocol::ident(battle, player));
  }

  Side& side = battle.side(player);
  const Player defender = foe(player);
  Side& foe_side = battle.side(defender);
  Pokemon& target = foe_side.get(1);
  const MoveData& data = get(m.move);
  Hit h = Hit::of(m.move);
  h.type = m.type;
  // Pursuit on a switching target: Showdown doubles the base power
  // (basePowerCallback); the decomp doubles the final damage
  // (dmgMultiplier, bs1:3091). This follows Showdown's arithmetic.
  if (m.pursuing) h.power = uint16_t(h.power * 2);
  // hiddenpowercalc (bsc:8889): the type is in the move ID, the power on the
  // Pokémon.
  if (data.effect == Effect::HiddenPower) {
    assert(m.move != Move::HiddenPower && side.active.hidden_power != 0);
    h.power = side.active.hidden_power;
  }
  // Return and Frustration (friendshiptodamagecalculation): 10 / 25 of the
  // happiness or its complement, at least 1 as Showdown's gen3 (the `|| 1`
  // of its basePowerCallback). Flail and Reversal (remaininghptopower): by
  // hp * 48 / max HP, at least 1, as Showdown's gen3 basePowerCallback and
  // the decomp's sFlailHpScaleToPowerTable agree.
  if (data.effect == Effect::Return || data.effect == Effect::Frustration) {
    const uint8_t happiness = side.get(1).happiness;
    const uint16_t f = data.effect == Effect::Return ? happiness : uint16_t(255 - happiness);
    h.power = f * 10 / 25 == 0 ? uint16_t(1) : uint16_t(f * 10 / 25);
  } else if (data.effect == Effect::Flail) {
    const Pokemon& mon = side.get(1);
    uint32_t ratio = uint32_t(mon.hp) * 48 / mon.stats.hp;
    if (ratio == 0) ratio = 1;
    h.power = ratio < 2 ? 200 : ratio < 5 ? 150 : ratio < 10 ? 100 : ratio < 17 ? 80 : ratio < 33 ? 40 : 20;
  } else if (data.effect == Effect::Eruption) {
    // Eruption and Water Spout (scaledamagebyhealthratio, bsc:9379): power *
    // hp / max HP, at least 1. Showdown's basePowerCallback, floored by
    // getDamage's clampIntRange, agrees.
    const Pokemon& mon = side.get(1);
    const uint32_t power = uint32_t(data.bp) * mon.hp / mon.stats.hp;
    h.power = power == 0 ? uint16_t(1) : uint16_t(power);
  } else if (data.effect == Effect::WeatherBall) {
    // Weather Ball (setweatherballtype, bsc:9787): in effective weather
    // the type changes (move_type) and the power doubles. Showdown's gen3
    // onModifyMove doubles the base power; the decomp doubles the damage
    // (dmgMultiplier). This follows Showdown's arithmetic.
    if (effective_weather(battle) != Weather::None) h.power = uint16_t(h.power * 2);
  } else if (data.effect == Effect::Revenge) {
    // Revenge (doubledamagedealtifdamaged, bsc:9339): doubled if the foe's
    // move took HP from the user itself this turn (Turn::physical and
    // special; a hit on its Substitute doesn't count). Showdown's
    // basePowerCallback reads the same (attackedBy with damage > 0 this
    // turn) and doubles the base power, where the decomp doubles the
    // damage; this follows Showdown's arithmetic.
    if (m.turn->physical[index(player)] != 0 || m.turn->special[index(player)] != 0) {
      h.power = uint16_t(h.power * 2);
    }
  } else if (data.effect == Effect::Smellingsalt) {
    // Smelling Salt (bs1:2268): doubled into a paralyzed target. The decomp
    // doesn't double into a Substitute (jumpifstatus2 SUBSTITUTE, then the
    // plain hit); Showdown's basePowerCallback does, and this follows it.
    if (foe_side.get(1).status.paralyzed()) h.power = uint16_t(h.power * 2);
  } else if (data.effect == Effect::SpitUp) {
    h.power = uint16_t(100 * side.active.stockpile);
  } else if (data.effect == Effect::LowKick) {
    // Low Kick (weightdamagecalculation, bsc:9467): by the target's weight.
    // Showdown's basePowerCallback has the same thresholds.
    const uint16_t weight = get(battle.side(foe(player)).active.species).weight;
    h.power = LOW_KICK_MAX_POWER;
    for (const WeightEntry& e : LOW_KICK_POWER) {
      if (weight < e.weight) {
        h.power = e.power;
        break;
      }
    }
  }
  // Present, Magnitude or Psywave's draw (rolls::variable).
  uint8_t variable = 0;
  // Magnitude (BattleScript_EffectMagnitude, bs1:1683): the magnitude is
  // drawn and logged before the accuracy check, as Showdown's onModifyMove
  // and onUseMoveMessage (the decomp has no accuracy check before it
  // either), so it is recorded only once the move reaches its target. It
  // hits Dig (ignore_underground) for double damage (damage.hpp).
  if (effect == Effect::Magnitude) {
    variable = rolls::variable(battle, player, effect, options);
    h.power = MAGNITUDE[variable - 4].power;
    protocol::activate_magnitude(options.log, protocol::ident(battle, player), variable);
  }

  // typecalc's immunities first, as Showdown (decided; README "Damage"). A
  // move with no effect draws no crit, and its accuracy roll changes only
  // the log (try_hit).
  m.result |= matchup(battle, player, h);
  // Wonder Guard is Showdown's TryHit, so its -immune names it, even where
  // a type immunity would also stop the move, but not where only Dream
  // Eater's own check does (its onTryHit asks the type chart alone,
  // data/mods/gen4/abilities.ts:553).
  const bool wonder_guard = (m.result & MoveResult::NO_EFFECT) &&
                            foe_side.active.ability == Ability::WonderGuard;
  // Dream Eater: Showdown's gen4 onTryImmunity (which gen3 inherits), a
  // natural immunity unless the target sleeps with no Substitute.
  if (effect == Effect::DreamEater &&
      (!target.status.asleep() || foe_side.active.volatiles.substitute)) {
    m.result |= MoveResult::DOESNT_AFFECT_FOE;
  }
  const bool affects = !(m.result & MoveResult::NO_EFFECT);
  // Brick Break breaks screens even into a Ghost type on the cartridge
  // (removelightscreenreflect before typecalc). Showdown's onTryHit runs in
  // moveHit, which an immunity or Wonder Guard never reaches
  // (data/mods/gen3/scripts.ts:376), and this follows Showdown.
  SideConditions& foe_conditions = foe_side.conditions;
  const bool breaks = effect == Effect::BrickBreak && affects &&
                      (foe_conditions.reflect != 0 || foe_conditions.light_screen != 0);
  // A natural immunity (the type chart or Levitate), whether or not Wonder
  // Guard would also stop the move.
  const bool immune = (m.result & MoveResult::DOESNT_AFFECT_FOE) != 0;
  m.attackstring = true;
  pp_reduce(battle, player, m);
  // Protect is part of the accuracy check, which comes before typecalc, so
  // it blocks a move the target is immune to too (Showdown's Protect
  // TryHit also comes first).
  if (!try_hit(battle, player, m,
               {.immune = immune, .matters = affects, .wonder_guard = wonder_guard},
               options)) {
    if (effect == Effect::RecoilIfMiss) crash(battle, player, m, h, options);
    moveend(battle, player, m, options);
    if (effect == Effect::Explosion) faint(battle, player, options);
    return;
  }
  // removelightscreenreflect (bsc:9883), before the damage. Log: Reflect's
  // SideEnd, then Light Screen's (Showdown's gen3 Brick Break TryHit).
  if (breaks) {
    if (foe_conditions.reflect != 0) {
      protocol::side_end(options.log, defender, protocol::SideCondition::Reflect);
    }
    if (foe_conditions.light_screen != 0) {
      protocol::side_end(options.log, defender, protocol::SideCondition::LightScreen);
    }
    foe_conditions.reflect = 0;
    foe_conditions.light_screen = 0;
  }
  if (!affects) {
    const ID id = protocol::ident(battle, defender);
    if (wonder_guard) {
      protocol::immune(options.log, id, protocol::Immune::Ability, Ability::WonderGuard);
    } else {
      protocol::immune(options.log, id);
    }
    if (effect == Effect::RecoilIfMiss) crash(battle, player, m, h, options);
    moveend(battle, player, m, options);
    if (effect == Effect::Explosion) faint(battle, player, options);
    return;
  }

  // Uproar's onTryHit, after the immunity and before the Substitute: the
  // target, then the user, wakes. The decomp wakes them at the canceler
  // instead (cancel()). Log: CureStatus [msg].
  if (effect == Effect::Uproar) {
    for (const Player p : {defender, player}) {
      Pokemon& mon = battle.side(p).get(1);
      if (!mon.status.asleep()) continue;
      protocol::cure_status(options.log, protocol::ident(battle, p), mon.status);
      mon.status = Status::none();
      wake(battle, p, options);
    }
  }
  // The power callbacks, once the damage is computed (Showdown's
  // basePowerCallback, so not on a miss, into Protect or an immunity):
  // - Rollout and Ice Ball (base data/moves.ts; the decomp's
  //   rolloutdamagecalculation, bsc:7896, agrees on the power): 30 times
  //   2 to the hits so far, doubled after Defense Curl. Unless the user is
  //   asleep (Sleep Talk called it), the hit counts, and below 5 it locks
  //   the user and renews the lock for the turn (RENEWED_LOCK; it runs out
  //   at the residuals otherwise, lock_residuals).
  // - Fury Cutter (gen3 mod: power 10, up to 160): the volatile starts at
  //   1, renewed; a hit on the target itself doubles it below.
  if (effect == Effect::Rollout) {
    ActivePokemon& a = side.active;
    h.power = uint16_t(data.bp << a.rollout);
    if (!user.status.asleep()) {
      a.rollout++;
      if (a.rollout < 5) {
        a.renewed |= ActivePokemon::RENEWED_LOCK;
        a.volatiles.multiple_turns = true;
        a.locked_move = m.move;
      }
    }
    if (a.volatiles.defense_curl) h.power = uint16_t(h.power * 2);
  } else if (effect == Effect::FuryCutter) {
    ActivePokemon& a = side.active;
    if (a.fury_cutter == 0) {
      a.fury_cutter = 1;
      a.renewed |= ActivePokemon::RENEWED_FURY_CUTTER;
    }
    h.power = uint16_t(std::min(data.bp << (a.fury_cutter - 1), 160));
  }

  if (effect == Effect::Magnitude) rolls::variable_matters(player, effect, variable, options);
  if (effect == Effect::Psywave) {
    variable = rolls::variable(battle, player, effect, options);
    rolls::variable_matters(player, effect, variable, options);
  }
  // Present (BattleScript_EffectPresent, bs1:1664), as Showdown's
  // onModifyMove, whose draw comes before the accuracy roll but matters
  // only after it: 40, 80 or 120 power, or a heal of a quarter of the
  // target's max HP, with -fail|heal at full HP (runMoveEffects,
  // sim/battle-actions.ts:1190). The heal sets infiltrates, but gen4
  // Substitute's onTryPrimaryHit ignores it: getDamage gives no damage, so
  // the user's move fails into a Substitute (data/mods/gen4/moves.ts:1309).
  // A heal that lands raises a Rage's Attack (runMoveEffects goes on to the
  // Hit event; the cartridge's MOVEEND_RAGE needs damage). The decomp's
  // odds differ (rolls::variable). Log: Heal (and Rage's Boost), Fail Heal,
  // or the user's Fail, each failure with [still].
  if (effect == Effect::Present) {
    variable = rolls::variable(battle, player, effect, options);
    rolls::variable_matters(player, effect, variable, options);
    if (variable == 1) {
      const ID id = protocol::ident(battle, defender);
      if (foe_side.active.volatiles.substitute) {
        protocol::failed(options.log, protocol::ident(battle, player));
      } else if (target.hp >= target.stats.hp) {
        protocol::fail(options.log, id, protocol::Fail::Heal, true);
      } else {
        target.hp = uint16_t(std::min<uint32_t>(target.stats.hp,
                                                target.hp + target.stats.hp / 4u));
        protocol::heal(options.log, id, target);
        // The same Hit event breaks a Focus Punch's focus (lostFocus, any
        // non-Status move); Counter's damage stays 0.
        if (m.turn != nullptr) m.turn->hit[index(defender)] = true;
        if (foe_side.active.volatiles.rage) rage_boost(battle, defender, options);
      }
      return moveend(battle, player, m, options);
    }
    h.power = variable == 2 ? 40 : variable == 3 ? 80 : 120;
  }

  if (effect == Effect::MultiHit || effect == Effect::DoubleHit ||
      effect == Effect::Twineedle || effect == Effect::TripleKick) {
    // BattleScript_EffectMultiHit (bs1:604) and the fixed counts that share
    // its loop (Double Kick and Bonemerang, bs1:839; Twineedle, bs1:1075),
    // and Triple Kick (bs1:1384), as Showdown's gen3 loop
    // (data/mods/gen3/scripts.ts:393): after the accuracy check, 2-5, 2 or 3
    // hits, each with its own crit, roll and survival, while both Pokémon
    // have HP and the user isn't asleep (unless Sleep Talk called the move).
    // Each hit that took the target's own HP runs its contact ability
    // (Showdown's DamagingHit per hit; the decomp's moveendto
    // MOVEEND_NEXT_TARGET). The decomp also stops once the target endures a
    // hit (MOVE_RESULT_FOE_ENDURED, bs1:638); Showdown goes on, and this
    // follows it. As Showdown, more per hit:
    // - Triple Kick's power is 10, 20, 30, and its second and third hits
    //   each check accuracy again (multiaccuracy); a miss ends the loop with
    //   no message. The decomp agrees (bs1:1397).
    // - Twineedle's poison is drawn per hit (secondaries in moveHit, with
    //   Synchronize passing it back at once); the decomp draws once after
    //   the loop (seteffectwithchance, bs1:651).
    // - After each hit the status-curing items act (eachEvent('Update')),
    //   so a Lum Berry cures an Effect Spore sleep before the next hit.
    // Log: per hit its modifiers and Damage (and Twineedle's Status), then
    // HitCount.
    const uint8_t n = effect == Effect::MultiHit    ? rolls::multi_hit(battle, player, options)
                      : effect == Effect::TripleKick ? uint8_t(3)
                                                     : uint8_t(2);
    bool damaged = false;
    uint8_t hits = 0;
    for (uint8_t i = 0; i < n && !target.fainted() && !user.fainted(); i++) {
      if (i > 0 && user.status.asleep() && m.chosen != Move::SleepTalk) break;
      Hit hi = h;
      if (effect == Effect::TripleKick) {
        hi.power = uint16_t(10 * (i + 1));
        if (i > 0) {
          const uint32_t calc = accuracy(battle, player, m);
          if (calc < 100 && !rolls::kick(battle, player, i, calc, options)) break;
        }
      }
      if (can_crit(battle, player)) {
        hi.crit = rolls::crit(battle, player, i,
                              crit_stage(battle, player, m.move), options);
      }
      uint32_t d = apply_roll(damage(battle, player, hi),
                              rolls::damage(battle, player, i, options));
      // Showdown recomputes the effectiveness per hit: Color Change may have
      // changed the target's type.
      if (i > 0) {
        constexpr uint8_t eff = MoveResult::SUPER_EFFECTIVE | MoveResult::NOT_VERY_EFFECTIVE;
        m.result = uint8_t((m.result & ~eff) | (matchup(battle, player, hi) & eff));
      }
      log_modifiers(battle, player, m, hi.crit, options);
      d = survive(battle, player, m, d, i, options);
      m.damaged = false;
      m.hp_dealt = hp_update(battle, defender, d, m, options);
      log_hit(battle, player, m, options);
      rage(battle, player, m, options);
      if (effect == Effect::Twineedle) {
        secondary(battle, player, m, options, i);
        synchronize(battle, defender, m, options);
      }
      flinch_item(battle, player, m, i, options);
      defrost(battle, player, m, options);
      ability_on_damage(battle, player, m, i, options);
      synchronize(battle, player, m, options);
      for (const Player p : {Player::P1, Player::P2}) {
        if (!battle.side(p).get(1).fainted()) item_cure(battle, p, options);
      }
      damaged |= m.damaged;
      hits++;
    }
    if (effect == Effect::TripleKick) rolls::kicks(player, hits, options);
    m.damaged = damaged;
    m.contact_done = true;
    m.kings_rock_done = true;
    // Gen3's tryMoveHit prints no faint here: they wait for the end of
    // runMove, after AfterMove (Charge's -end).
    protocol::hit_count(options.log, protocol::ident(battle, defender), hits);
  } else {
    // critcalc, damagecalc, typecalc, then adjustnormaldamage: the roll,
    // then whatever lets the target survive at 1 HP (not behind a
    // Substitute). Focus Band is drawn only when the hit would be lethal,
    // and not for False Swipe, which spares the target anyway.
    // The moves with no formula (fixed_damage) have no crit or roll
    // (adjustsetdamage, bsc:5878, has the same survival checks).
    uint32_t d = fixed != 0 ? fixed : fixed_damage(battle, player, effect, variable);
    if (d == 0) {
      if (can_crit(battle, player)) {
        h.crit = rolls::crit(battle, player, 0,
                             crit_stage(battle, player, m.move), options);
      }
      d = apply_roll(damage(battle, player, h),
                     rolls::damage(battle, player, 0, options));
      log_modifiers(battle, player, m, h.crit, options);
    }
    d = survive(battle, player, m, d, 0, options);
    // datahpupdate
    m.hp_dealt = hp_update(battle, defender, d, m, options);
    log_hit(battle, player, m, options);
    // An OHKO move's -ohko after any hit on a Substitute (gen4
    // Substitute's onTryPrimaryHit); the KO's is after the contact ability.
    if (effect == Effect::Ohko && !m.damaged) protocol::ohko(options.log);
  }

  // Bind, Wrap, Clamp, Fire Spin, Whirlpool and Sand Tomb
  // (BattleScript_EffectTrap, bs1:1225; MOVE_EFFECT_WRAP, certain,
  // bsc:2595), as Showdown's volatileStatus partiallytrapped (gen4 mod,
  // data/mods/gen4/conditions.ts:110): after a hit on the target itself
  // (not its Substitute) that left it standing, it is bound for a hidden
  // 3-6 turns (Volatiles::wrapped; the decomp rolls 3-6 too), unless
  // already bound, even by a binder that has left (addVolatile fails).
  // Its residual is Stage::Bind. Log: Activate Bound, of the user.
  if (effect == Effect::Trap && m.damaged && !target.fainted() &&
      foe_side.active.volatiles.wrapped == 0) {
    ActivePokemon& t = foe_side.active;
    t.volatiles.wrapped = uint8_t(3 + battle.rng.range(4));
    t.wrapped_move = m.move;
    if constexpr (CHANCE<O>) options.chance.durations.get(defender).binding = 1;
    protocol::activate_with(options.log, protocol::ident(battle, defender),
                            protocol::Activate::Bound, m.move,
                            protocol::ident(battle, player));
  }

  // Showdown's onHit, before the secondaries, on a hit that reached the
  // target itself (not its Substitute), fainted or not.
  // - Pay Day (MOVE_EFFECT_PAYDAY, certain, bsc:2583): the coins change
  //   nothing in a link battle. Log: FieldActivate Pay Day (gen4 onHit).
  // - Smelling Salt (MOVE_EFFECT_REMOVE_PARALYSIS, bsc:2822): a paralyzed
  //   target that's still standing is cured (Showdown's cureStatus needs
  //   HP). Log: CureStatus [msg].
  if (effect == Effect::PayDay && m.damaged) {
    protocol::field_activate(options.log, protocol::FieldActivate::PayDay);
  }
  // - Fury Cutter (gen3 mod onHit): the volatile restarts, doubling the
  //   power up to 16 times and renewing it for the turn.
  if (effect == Effect::FuryCutter && m.damaged) {
    ActivePokemon& a = side.active;
    if (a.fury_cutter < 5) a.fury_cutter++;
    a.renewed |= ActivePokemon::RENEWED_FURY_CUTTER;
  }
  if (effect == Effect::Smellingsalt && m.damaged && !target.fainted() &&
      target.status.paralyzed()) {
    protocol::cure_status(options.log, protocol::ident(battle, defender), target.status);
    target.status = Status::none();
  }

  // Absorb, Mega Drain, Giga Drain and Dream Eater (bs1:323, 1290):
  // negativedamage (bsc:6925), half the HP dealt, at least 1, healed past
  // the user's Substitute, or taken from it if the target has Liquid Ooze.
  // As Showdown, half a Substitute's damage rounds up (gen4 Substitute's
  // drain, data/mods/gen4/moves.ts:1332; the decomp floors it) and a move
  // with no effect drains nothing (the decomp still drains 1). The user's
  // Liquid Ooze faint comes before the target's (bs1:358). Showdown drains
  // with the damage (spreadDamage, sim/battle.ts:2168), so before the
  // contact ability below; no drain move has a secondary. Log: Heal [from]
  // drain, or Damage [from] Liquid Ooze.
  if ((effect == Effect::Absorb || effect == Effect::DreamEater) && affects &&
      m.hp_dealt != 0) {
    uint16_t drain = m.damaged ? uint16_t(m.hp_dealt / 2) : uint16_t((m.hp_dealt + 1) / 2);
    if (drain == 0) drain = 1;
    // Dream Eater's own script skips Liquid Ooze (bs1:434), as Showdown's
    // (data/mods/gen4/abilities.ts:255), whose instafaint damage writes the
    // faints at once, the user's first.
    if (foe_side.active.ability == Ability::LiquidOoze && effect != Effect::DreamEater) {
      passive_damage(battle, player, drain, options);
      protocol::damage_ability(options.log, protocol::ident(battle, player), user,
                               Ability::LiquidOoze, protocol::ident(battle, defender));
      if (user.fainted()) protocol::faints_last_first(options.log);
    } else if (user.hp != user.stats.hp) {
      heal(user, drain);
      protocol::heal_drain(options.log, protocol::ident(battle, player), user,
                           protocol::ident(battle, defender));
    }
  }

  // Rage, after the drain (spreadDamage) and the move's own onHit
  // (Smelling Salt's cure): runMoveEffects runs the move's Hit, then the
  // target's.
  if (effect != Effect::MultiHit && effect != Effect::DoubleHit &&
      effect != Effect::Twineedle && effect != Effect::TripleKick) {
    rage(battle, player, m, options);
  }

  // Hyper Beam, Blast Burn, Hydro Cannon and Frenzy Plant
  // (BattleScript_EffectRecharge, bs1:1111; MOVE_EFFECT_RECHARGE, certain
  // and on the user, bsc:2727): after a hit that affected the target,
  // Substitute or not, fainted or not, the user must recharge, locked to
  // the move with the timer at 2 (turn_cleanup; the canceler spends the
  // next turn). Showdown's self mustrecharge agrees (duration 2, applied
  // by selfDrops after the damage and before the secondaries). Log:
  // MustRecharge.
  if (effect == Effect::Recharge) {
    ActivePokemon& a = side.active;
    a.volatiles.recharge = true;
    a.recharge_timer = 2;
    a.locked_move = m.move;
    protocol::must_recharge(options.log, protocol::ident(battle, player));
  }

  // The self volatiles of Thrash, Petal Dance and Outrage (lockedmove),
  // Uproar and Rage, from the same selfDrops after a hit, Substitute or
  // not, fainted or not:
  // - lockedmove (data/conditions.ts:253; the gen4 mod drops its
  //   onAfterMove): the lock starts with a hidden length of 2-3 turns
  //   (trueDuration, Volatiles::thrash), or restarts (lock_renew). The
  //   decomp's BattleScript_EffectRampage (bs1:1101) rolls 2-3 too.
  // - Uproar (gen3 mod: 2-5 turns, hidden in Volatiles::uproar; the
  //   decomp's is the same, bsc:2759): it starts once, locking the user.
  //   Log: Start Uproar.
  // - Rage: until the user's next move attempt (cancel()). Log:
  //   SingleMove Rage, only as it starts.
  if (effect == Effect::Rampage) {
    ActivePokemon& a = side.active;
    if (a.volatiles.multiple_turns) {
      assert(a.locked_move == m.move);
      lock_renew(battle, player);
    } else {
      a.volatiles.multiple_turns = true;
      a.locked_move = m.move;
      a.volatiles.thrash = uint8_t(2 + battle.rng.range(2));
      a.renewed |= ActivePokemon::RENEWED_LOCK;
      if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 1;
    }
  } else if (effect == Effect::Uproar && side.active.volatiles.uproar == 0) {
    ActivePokemon& a = side.active;
    a.volatiles.uproar = uint8_t(2 + battle.rng.range(4));
    a.volatiles.multiple_turns = true;
    a.locked_move = m.move;
    protocol::start(options.log, protocol::ident(battle, player), protocol::Start::Uproar);
    if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 1;
  } else if (effect == Effect::Rage && !side.active.volatiles.rage) {
    side.active.volatiles.rage = true;
    protocol::single_move(options.log, protocol::ident(battle, player),
                          protocol::SingleMove::Rage);
  }

  // Rapid Spin's certain MOVE_EFFECT_RAPIDSPIN on the user, if the move
  // affected the target: Cmd_rapidspinfree (bsc:8821) runs once per release, so
  // it frees the user from binding and Leech Seed and clears its side's
  // Spikes, all three.
  // Log, as Showdown's gen4 self onHit: End Leech Seed [from] Rapid Spin,
  // then Spikes' SideEnd, then the binding's End. It is a selfDrops
  // effect, so it comes with the self volatiles above, before the target's
  // contact ability.
  if (effect == Effect::RapidSpin && affects && !user.fainted()) {
    ActivePokemon& a = side.active;
    const ID id = protocol::ident(battle, player);
    if (a.volatiles.leech_seed) {
      protocol::end_effect(options.log, id, protocol::End::LeechSeedRapidSpin);
    }
    if (side.conditions.spikes != 0) {
      protocol::side_end(options.log, player, protocol::SideCondition::SpikesRapidSpin, id);
    }
    if (a.volatiles.wrapped != 0) {
      protocol::end_effect(options.log, id, protocol::End::Bound, a.wrapped_move);
    }
    a.volatiles.wrapped = 0;
    a.volatiles.wrapper_gone = false;
    a.wrapped_move = Move::None;
    a.volatiles.leech_seed = false;
    side.conditions.spikes = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).binding = 0;
  }

  if (effect != Effect::Twineedle) secondary(battle, player, m, options);

  // Knock Off (bs1:2475, MOVE_EFFECT_KNOCK_OFF, bsc:2858), as Showdown's
  // gen4 onAfterHit (data/mods/gen4/moves.ts:718; README decision 8): after
  // a hit on the target itself (not its Substitute) with the user still
  // in, its item becomes the inert Item::KnockedOff, unless the target's
  // Sticky Hold keeps it, fainted or not (the gen4 onTakeItem has no HP
  // check, data/mods/gen4/abilities.ts:438). Its Choice Band lock ends with it
  // (bsc:2885).
  if (effect == Effect::KnockOff && m.damaged && !user.fainted() &&
      target.item != Item::None && target.item != Item::KnockedOff) {
    if (foe_side.active.ability == Ability::StickyHold) {
      protocol::activate_ability(options.log, protocol::ident(battle, defender),
                                 Ability::StickyHold);
    } else {
      protocol::end_item(options.log, protocol::ident(battle, defender), target.item,
                         protocol::EndItemFrom::KnockOff, protocol::ident(battle, player));
      target.item = Item::KnockedOff;
      foe_side.active.choice_move = Move::None;
    }
  }

  // Thief and Covet (bs1:1440, MOVE_EFFECT_STEAL_ITEM, bsc:2738): after a
  // hit on the target itself (not its Substitute), fainted or not, an
  // empty-handed user takes the target's item, unless the target's Sticky
  // Hold keeps it (also when fainted: Showdown's gen4 onTakeItem runs before
  // the faint is processed). The target's Choice Band lock ends (bsc:2799);
  // the user gets the item at MOVEEND_CHANGED_ITEMS (see moveend for its
  // Choice Band lock). A Knocked Off item can't be
  // taken, nor can a user whose item was knocked off take one (Showdown's
  // takeItem, gen <= 4). Showdown's gen3 doesn't keep the decomp's Enigma
  // Berry and Mail out, and this follows it. Showdown logs the theft here,
  // at onAfterHit (Covet prints no -enditem).
  if (effect == Effect::Thief && m.damaged && !user.fainted() &&
      user.item == Item::None && target.item != Item::None &&
      target.item != Item::KnockedOff) {
    if (foe_side.active.ability == Ability::StickyHold) {
      protocol::activate_ability(options.log, protocol::ident(battle, defender),
                                 Ability::StickyHold);
    } else {
      const ID thief = protocol::ident(battle, player);
      const ID robbed = protocol::ident(battle, defender);
      const Item item = target.item;
      target.item = Item::None;
      foe_side.active.choice_move = Move::None;
      // setItem runs the new item's onStart (White Herb, as trick()) before
      // the lines; an item it uses up isn't handed over at moveend.
      user.item = item;
      white_herb(battle, player, options);
      m.stolen = user.item;
      user.item = Item::None;
      if (m.move == Move::Thief) {
        protocol::end_item(options.log, robbed, item, protocol::EndItemFrom::Thief, thief);
        protocol::item(options.log, thief, item, protocol::ItemFrom::Thief, robbed);
      } else {
        protocol::item(options.log, thief, item, protocol::ItemFrom::Covet, robbed);
      }
    }
  }

  // Superpower (MOVE_EFFECT_ATK_DEF_DOWN) and Overheat (SP_ATK_TWO_DOWN),
  // certain and on the user (bs1:2388, 2648; bsc:2839, 2895): after a hit that
  // affected the target, Substitute or not, Mist and the user's own
  // abilities aside, down to -6. Showdown's self boosts agree, and print
  // -unboost only for a stat that fell (isSelf).
  if (affects && (effect == Effect::Superpower || effect == Effect::Overheat)) {
    ActivePokemon& a = side.active;
    const ID id = protocol::ident(battle, player);
    const auto drop = [&](uint8_t stat, int8_t n) {
      if (const int8_t lost = lower(a, stat, n)) protocol::boost(options.log, id, stat, lost, true);
    };
    if (effect == Effect::Superpower) {
      drop(0, 1);
      drop(1, 1);
    } else {
      drop(3, 2);
    }
  }

  // The target's contact ability and Color Change, as Showdown's gen < 5
  // DamagingHit (spreadMoveHit, sim/battle-actions.ts:1120): after the self
  // drops, the secondaries and the AfterHit handlers (Knock Off, Thief),
  // before the recoil (applyRecoilDamage in tryMoveHit). The decomp runs it
  // in moveend (MOVEEND_ON_DAMAGE_ABILITIES), after all of them. Rage's
  // boost is rage(), at onHit. A status it gives a Synchronize attacker
  // goes back at once (onAfterSetStatus), before the recoil.
  if (!m.contact_done) {
    defrost(battle, player, m, options);
    ability_on_damage(battle, player, m, 0, options);
    synchronize(battle, player, m, options);
    m.contact_done = true;
  }
  // An OHKO move's -ohko after a hit that took the target's last HP (gen3
  // tryMoveHit, after moveHit).
  if (effect == Effect::Ohko && m.damaged && battle.side(foe(player)).get(1).fainted()) {
    protocol::ohko(options.log);
  }

  // seteffectwithchance: the certain recoil of Effect::Recoil
  // (MOVE_EFFECT_RECOIL_25, a quarter of the HP dealt, bsc:2636) and
  // Double-Edge (MOVE_EFFECT_RECOIL_33, a third, bsc:2843), at least 1.
  // Rock Head prevents it except for Struggle (BattleScript_MoveEffectRecoil,
  // bs1:3938). The recoil script faints the user before the target's
  // tryfaintmon below.
  // As Showdown (applyRecoilDamage only if move.totalDamage), a hit that
  // took no HP (Endure or Focus Band at 1 HP) has no recoil; the decomp's
  // is at least 1.
  // A user that the target's Rough Skin KO'd takes none (Showdown's damage
  // to a fainted Pokémon does nothing).
  if ((effect == Effect::Recoil || effect == Effect::DoubleEdge) && affects &&
      m.hp_dealt != 0 && !user.fainted() &&
      (m.move == Move::Struggle || side.active.ability != Ability::RockHead)) {
    uint16_t recoil =
        uint16_t(m.hp_dealt / (effect == Effect::DoubleEdge ? 3 : 4));
    if (recoil == 0) recoil = 1;
    user.hp = user.hp > recoil ? uint16_t(user.hp - recoil) : 0;
    protocol::damage(options.log, protocol::ident(battle, player), user,
                     protocol::Damage::Recoil);
    if (user.fainted()) faint(battle, player, options);
  }

  // tryfaintmon
  faint_target(battle, player, m.chosen, m.grudge, m.bonded_hp, options);
  moveend(battle, player, m, options);
  if (effect == Effect::Explosion) faint(battle, player, options);
}

// BattleScript_EffectConfuse (bs1:903): Confuse Ray, Supersonic and Sweet
// Kiss. After the canceler, attackstring and ppreduce, it stops at Own
// Tempo, a Substitute or an existing confusion, then checks accuracy, then
// Safeguard. On the cartridge Safeguard's script ends with end2 (bs1:3257),
// skipping moveend, so the move isn't recorded as used; Showdown fails the
// move normally, and this follows it. The checks and their messages come
// in Showdown's order: try_hit, the Substitute (TryPrimaryHit), an existing
// confusion (addVolatile), then TryAddVolatile's Own Tempo (whose onHit
// prints its -immune) and Safeguard.
template <typename O>
constexpr void confuse_move(Battle& battle, Player player, MoveState& m,
                            O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& t = foe_side.active;
  const ID target = protocol::ident(battle, foe(player));
  const bool takes = !t.volatiles.substitute && t.volatiles.confusion == 0 &&
                     t.ability != Ability::OwnTempo && foe_side.conditions.safeguard == 0;
  if (!try_hit(battle, player, m, {.matters = takes}, options)) {
    return moveend(battle, player, m, options);
  }
  // BattleScript_OwnTempoPrevents and BattleScript_AlreadyConfused set no
  // result flag.
  if (t.volatiles.substitute) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
  } else if (t.volatiles.confusion != 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
  } else if (t.ability == Ability::OwnTempo) {
    protocol::immune(options.log, target, protocol::Immune::Confusion, Ability::OwnTempo);
  } else if (foe_side.conditions.safeguard != 0) {
    protocol::activate(options.log, target, protocol::Activate::Safeguard);
    m.result |= MoveResult::FAILED;
  } else {
    confuse(battle, foe(player), options);
  }
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

// Why a primary status move that hits stops, in Showdown's order: the
// Substitute (TryPrimaryHit), then setStatus (sim/pokemon.ts:1685): the
// status the target already has (-fail|T|status if it's this move's, else
// -fail|user [still]: trySetStatus passes the current status), the type
// immunities (runStatusImmunity: Poison and Steel to poison, Fire to burn),
// then the SetStatus handlers, the target's ability before its side's
// Safeguard before the format's Sleep Clause (Speed first, then subOrder:
// comparePriority, sim/battle.ts:408).
enum class StatusStop : uint8_t {
  None,
  Substitute,
  Same,
  Other,
  Type,
  Ability,
  Safeguard,
  Clause, // Sleep Clause Mod: only a -message, which the log drops
  Uproar,
};

constexpr StatusStop status_stop(const Battle& battle, Player player, Secondary s) {
  const Side& side = battle.side(foe(player));
  const ActivePokemon& t = side.active;
  const Status status = side.get(1).status;
  if (t.volatiles.substitute) return StatusStop::Substitute;
  if (status.any()) {
    const bool same = (s == Secondary::Sleep && status.asleep()) ||
                      (s == Secondary::Poison && status.poisoned() && !status.badly_poisoned()) ||
                      (s == Secondary::Toxic && status.badly_poisoned()) ||
                      (s == Secondary::Burn && status.burned()) ||
                      (s == Secondary::Paralysis && status.paralyzed());
    return same ? StatusStop::Same : StatusStop::Other;
  }
  switch (s) {
  case Secondary::Poison:
  case Secondary::Toxic:
    if (has_type(t, Type::Poison) || has_type(t, Type::Steel)) return StatusStop::Type;
    if (t.ability == Ability::Immunity) return StatusStop::Ability;
    break;
  case Secondary::Burn:
    if (has_type(t, Type::Fire)) return StatusStop::Type;
    if (t.ability == Ability::WaterVeil) return StatusStop::Ability;
    break;
  case Secondary::Paralysis:
    if (t.ability == Ability::Limber) return StatusStop::Ability;
    break;
  case Secondary::Sleep:
    if (t.ability == Ability::Insomnia || t.ability == Ability::VitalSpirit) {
      // An uproar's onAnySetStatus (a Condition) goes first if its holder is
      // the target or at least as fast (Speed, then subOrder).
      const Player target = foe(player);
      const bool first = t.volatiles.uproar != 0 ||
                         (battle.side(player).active.volatiles.uproar != 0 &&
                          speed(battle, player) >= speed(battle, target));
      return first ? StatusStop::Uproar : StatusStop::Ability;
    }
    break;
  default:
    assert(false && "not a primary status");
  }
  if (side.conditions.safeguard != 0) return StatusStop::Safeguard;
  // Showdown's uproar onAnySetStatus comes before Sleep Clause Mod's
  // onSetStatus.
  if (s == Secondary::Sleep && uproar(battle)) return StatusStop::Uproar;
  if (s == Secondary::Sleep && sleep_clause(battle, foe(player))) return StatusStop::Clause;
  return StatusStop::None;
}

// -fail's form for the status a target already has.
constexpr protocol::Fail fail_status(Status status) {
  if (status.asleep()) return protocol::Fail::Sleep;
  if (status.badly_poisoned()) return protocol::Fail::Toxic;
  if (status.poisoned()) return protocol::Fail::Poison;
  if (status.burned()) return protocol::Fail::Burn;
  if (status.frozen()) return protocol::Fail::Freeze;
  return protocol::Fail::Paralysis;
}

// The primary status moves: BattleScript_EffectPoison (bs1:986), Toxic
// (bs1:686), Paralyze (bs1:1007; Thunder Wave, Stun Spore, Glare),
// Will-O-Wisp (bs1:2174) and Sleep (bs1:284; Spore, Hypnosis, Sleep Powder,
// Lovely Kiss, Sing, Grass Whistle). After the canceler, attackstring and
// ppreduce, each script runs its checks in its own order, then accuracy,
// then Safeguard, then seteffectprimary. Every check fails the move the
// same way, so the order changes only the messages, and this takes
// Showdown's: try_hit (Thunder Wave's Ground and Glare's Ghost immunity are
// natural ones, as Showdown's ignoreImmunity false; Glare's is negated by
// Foresight), then status_stop. Paralyze's typecalc is only that type
// immunity: Wonder Guard needs power (bsc:1409), and Showdown's skips
// status moves too. Safeguard runs moveend as Showdown does (see
// confuse_move). Sleep Clause (Smogon) fails the move after the accuracy
// check, as Showdown's onSetStatus.
template <typename O>
constexpr void status_move(Battle& battle, Player player, MoveState& m,
                           Secondary s, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& t = foe_side.active;
  bool immune = false;
  if (m.move == Move::ThunderWave || m.move == Move::Glare) {
    const Type type = get(m.move).type;
    for (const Type d : t.types) {
      const bool negated = t.volatiles.foresight && d == Type::Ghost &&
                           (type == Type::Normal || type == Type::Fighting);
      immune |= effectiveness(type, d) == Effectiveness::Immune && !negated;
    }
  }
  const StatusStop stop = status_stop(battle, player, s);
  if (!try_hit(battle, player, m, {.immune = immune, .matters = stop == StatusStop::None},
               options)) {
    return moveend(battle, player, m, options);
  }
  const ID user = protocol::ident(battle, player);
  const ID target = protocol::ident(battle, foe(player));
  switch (stop) {
  case StatusStop::None:
    inflict(battle, foe(player), s, m, options, protocol::StatusSource::by_move(m.move));
    return moveend(battle, player, m, options);
  case StatusStop::Substitute:
  case StatusStop::Other: protocol::failed(options.log, user); break;
  case StatusStop::Same:
    protocol::fail(options.log, target, fail_status(foe_side.get(1).status));
    break;
  case StatusStop::Type: protocol::immune(options.log, target); break;
  case StatusStop::Ability:
    protocol::immune(options.log, target, protocol::Immune::Ability, t.ability);
    break;
  case StatusStop::Safeguard:
    protocol::activate(options.log, target, protocol::Activate::Safeguard);
    break;
  case StatusStop::Clause: break;
  case StatusStop::Uproar: uproar_stops(battle, foe(player), options); break;
  }
  m.result |= MoveResult::FAILED;
  moveend(battle, player, m, options);
}

// BattleScript_EffectRest (bs1:735): it fails while asleep, under Uproar,
// with Vital Spirit or Insomnia, and at full HP (Cmd_trysetrest); otherwise
// 3 turns of self-inflicted sleep (Status::slf), any other status replaced,
// and full HP. Showdown agrees (Rest's onTry and onHit; Sleep Clause skips
// a sleep from the Pokémon itself). Its messages are Showdown's onTry's, in
// its order: none while asleep, then -fail|heal at full HP, then -fail
// [from] the ability.
template <typename O>
constexpr void rest(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  const ActivePokemon& a = side.active;
  if (mon.status.asleep() || uproar(battle) || a.ability == Ability::VitalSpirit ||
      a.ability == Ability::Insomnia || mon.hp == mon.stats.hp) {
    const ID id = protocol::ident(battle, player);
    if (!mon.status.asleep() && mon.hp == mon.stats.hp) {
      protocol::fail(options.log, id, protocol::Fail::Heal);
    } else if (!mon.status.asleep() &&
               (a.ability == Ability::VitalSpirit || a.ability == Ability::Insomnia)) {
      protocol::fail_ability(options.log, id, a.ability);
    } else if (!mon.status.asleep()) {
      // Rest's onHit: setStatus meets the uproar.
      uproar_stops(battle, player, options);
    }
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  mon.status = Status::slf(Status::REST_SLEEP);
  side.active.toxic = 0;
  mon.hp = mon.stats.hp;
  const ID id = protocol::ident(battle, player);
  protocol::status(options.log, id, mon.status, protocol::StatusSource::by_move(Move::Rest));
  if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 1;
  // Showdown's Lum Berry eats before the heal (onAfterSetStatus, inflict()).
  if (item_data(mon.item).effect == HoldEffect::CureStatus) item_cure(battle, player, options);
  protocol::heal(options.log, id, mon, protocol::Heal::Silent);
  moveend(battle, player, m, options);
}

// BattleScript_EffectYawn (bs1:2451): it fails against Vital Spirit or
// Insomnia, a Substitute, Safeguard, a semi-invulnerable target (accuracy
// check without a roll, NO_ACC_CALC_CHECK_LOCK_ON; Showdown's accuracy is
// true), a status, or a Yawn already pending (Cmd_setyawn). The cartridge
// also fails it during an Uproar (jumpifcantmakeasleep); Showdown doesn't
// (Yawn's onTryHit checks only the status and sleep immunity), and this
// follows Showdown. ENDTURN_YAWN puts the target to sleep (battler_stage).
// The messages come in Showdown's order: try_hit, a status (onTryHit), the
// Substitute, a pending Yawn (addVolatile), then TryAddVolatile's ability
// and Safeguard.
template <typename O>
constexpr void yawn(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& foe_side = battle.side(foe(player));
  ActivePokemon& t = foe_side.active;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return moveend(battle, player, m, options);
  }
  const ID user = protocol::ident(battle, player);
  const ID target = protocol::ident(battle, foe(player));
  if (foe_side.get(1).status.any() || t.volatiles.substitute || t.volatiles.yawn != 0) {
    protocol::failed(options.log, user);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (t.ability == Ability::VitalSpirit || t.ability == Ability::Insomnia) {
    protocol::immune(options.log, target, protocol::Immune::Ability, t.ability);
    return moveend(battle, player, m, options);
  }
  if (foe_side.conditions.safeguard != 0) {
    protocol::activate(options.log, target, protocol::Activate::Safeguard);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.volatiles.yawn = 2;
  protocol::start(options.log, target, protocol::Start::Yawn, user);
  moveend(battle, player, m, options);
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
// bs1:2014); neither undoes it when the raise fails. Here both come after
// ppreduce and Snatch, as Showdown's volatiles. Showdown's boosts and
// volatiles agree; it prints `-boost|...|0` at the cap where the cartridge
// prints "won't go higher".
template <typename O>
constexpr void stat_up_move(Battle& battle, Player player, MoveState& m,
                            O& options) {
  ActivePokemon& a = battle.side(player).active;
  const Effect effect = get(m.move).effect;
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  if (effect == Effect::Minimize) a.volatiles.minimized = true;
  if (effect == Effect::DefenseCurl) a.volatiles.defense_curl = true;
  const StatChange c = effect == Effect::Minimize      ? StatChange{6, 1}
                       : effect == Effect::DefenseCurl ? StatChange{1, 1}
                                                       : stat_up(effect);
  assert(c.n != 0);
  protocol::boost(options.log, protocol::ident(battle, player), c.stat,
                  raise(a, c.stat, c.n, m), false);
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

// BattleScript_EffectStatDown (bs1:534), for Screech, Metal Sound and the
// other one-stat drops: after the canceler, a Substitute fails it
// (FailedFromAtkString); then accuracy (with Protect), attackstring,
// ppreduce and the drop. Every failure is the same state, so the checks
// and their messages come in Showdown's order: try_hit (Soundproof stops
// Screech, Metal Sound and Growl), the Substitute (TryPrimaryHit), then
// lower_stats(). Log: Unboost (0 at -6).
template <typename O>
constexpr void stat_down_move(Battle& battle, Player player, MoveState& m,
                              StatChange c, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const uint8_t bit = uint8_t(1 << c.stat);
  const bool sub = t.volatiles.substitute;
  // A hit that Mist, an ability or -6 stops still lands (moveend's
  // last_landed_move and last_taken_move), so its roll matters.
  if (!try_hit(battle, player, m, {.matters = !sub}, options)) {
    return moveend(battle, player, m, options);
  }
  if (sub) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  lower_stats(battle, target, bit, c.n, options.log);
  moveend(battle, player, m, options);
}

// BattleScript_EffectTickle (bs1:2652): after the canceler, attackstring and
// ppreduce it fails with Attack and Defense both at -6, then on a missed
// accuracy check (ButItFailed); then each drop that can happen. Neither the
// decomp nor Showdown's gen3 (bypasssub, data/mods/gen3/moves.ts:639)
// checks a Substitute. The order of the messages is Showdown's: try_hit,
// then lower_stats(). Log: Unboost each (0 at -6).
template <typename O>
constexpr void tickle(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  // As stat_down_move, a blocked drop still lands: the roll matters.
  if (!try_hit(battle, player, m, {}, options)) {
    return moveend(battle, player, m, options);
  }
  lower_stats(battle, target, 0b11, 1, options.log);
  moveend(battle, player, m, options);
}

// Memento, as Showdown's gen3 (accuracy true, boosts -2/-2, selfdestruct
// "ifHit"): after attackstring and ppreduce, try_hit (Protect, a
// semi-invulnerable target unless Locked-On) or a Substitute
// (TryPrimaryHit) stops it and the user lives; otherwise each drop that
// can happen (drops()), then the user faints, even if neither stat could
// fall. The decomp (bs1:2204, Cmd_trymemento, bsc:9072) faints the user
// through Protect, a Substitute and Fly, and fails with both stats at -6
// and the user alive. Log: Unboost each, then Faint.
template <typename O>
constexpr void memento(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (t.volatiles.substitute) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  lower_stats(battle, target, 1 << 0 | 1 << 3, 2, options.log);
  battle.side(player).get(1).hp = 0;
  moveend(battle, player, m, options);
  faint(battle, player, options);
}

// BattleScript_EffectImprison (bs1:2509), Cmd_tryimprison (bsc:9413): it
// fails if the user already imprisons, or shares no move with the foe;
// otherwise the foe can't select (usable_moves) or use (the canceler) the
// user's moves while it stays in. Showdown's gen4 onTryHit agrees, and its
// gen3 drops the decomp's extra PP under Pressure
// (PressurePPLoseOnUsingImprison), as pp_reduce does for a move on the user.
// Log: Start Imprison, or Fail.
template <typename O>
constexpr void imprison(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  bool shared = false;
  for (const MoveSlot& ms : a.moves) {
    for (const MoveSlot& fm : f.moves) {
      shared |= ms.id != Move::None && same_move(ms.id, fm.id);
    }
  }
  if (a.volatiles.imprisoned_others || !shared) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  a.volatiles.imprisoned_others = true;
  protocol::start(options.log, protocol::ident(battle, player), protocol::Start::Imprison);
  moveend(battle, player, m, options);
}

// BattleScript_EffectNightmare (bs1:1459): after attackstring and ppreduce
// it fails on a Substitute, a target already in a Nightmare or one not
// asleep (Rest counts). No accuracy check; Showdown's gen3 (accuracy true,
// protect flag) also fails it into Protect and a semi-invulnerable target
// (try_hit), and this follows it. The residual takes 1/4 while it sleeps;
// waking ends it. Log: Start Nightmare.
template <typename O>
constexpr void nightmare(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& foe_side = battle.side(foe(player));
  Volatiles& v = foe_side.active.volatiles;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (v.substitute || v.nightmare || !foe_side.get(1).status.asleep()) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  v.nightmare = true;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Nightmare);
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
  protocol::clear_all_boost(options.log);
  moveend(battle, player, m, options);
}

// BattleScript_EffectPsychUp (bs1:1790), Cmd_copyfoestats (bsc:8809): the
// user takes the foe's stages, through a Substitute and Protect; it never
// fails. As in the decomp, Focus Energy isn't copied: gen3 inherits the
// gen5 mod's onHit (data/mods/gen5/moves.ts:641), stages only, not the
// base one's volatilesToCopy. Showdown's misses a semi-invulnerable target
// (its target is 'normal', so tryMoveHit's Invulnerability check applies;
// accuracy true); this follows Showdown. Log: CopyBoost.
template <typename O>
constexpr void psych_up(Battle& battle, Player player, MoveState& m,
                        O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return moveend(battle, player, m, options);
  }
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  a.boosts = f.boosts;
  protocol::copy_boost(options.log, protocol::ident(battle, player),
                       protocol::ident(battle, foe(player)));
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
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  ActivePokemon& a = battle.side(player).active;
  const ID id = protocol::ident(battle, player);
  if (stage(a.boosts, s.first) >= 6 && stage(a.boosts, s.second) >= 6) {
    // Showdown prints both 0s and no -fail.
    protocol::boost(options.log, id, s.first, 0, false);
    protocol::boost(options.log, id, s.second, 0, false);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  protocol::boost(options.log, id, s.first, raise(a, s.first, 1, m), false);
  protocol::boost(options.log, id, s.second, raise(a, s.second, 1, m), false);
  moveend(battle, player, m, options);
}

// BattleScript_EffectBellyDrum (bs1:1776), Cmd_maxattackhalvehp (bsc:8785):
// it fails unless Attack is below +6 and HP is above half (at least 1);
// then Attack goes to +6 and the user loses max HP / 2 (at least 1),
// bypassing its Substitute. Showdown's Belly Drum is the same (data/moves.ts:
// bellydrum; its directDamage floors the half). Log: Damage, then SetBoost
// (onHit's order), or Fail.
template <typename O>
constexpr void belly_drum(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  const uint32_t half = fraction(mon, 2);
  if (side.active.boosts.atk >= 6 || mon.hp <= half) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  side.active.boosts.atk = 6;
  // The user can't faint: its HP was above the half it loses.
  passive_damage(battle, player, half, options);
  const ID id = protocol::ident(battle, player);
  protocol::damage(options.log, id, mon);
  protocol::set_boost(options.log, id);
  moveend(battle, player, m, options);
}

// BattleScript_EffectCurse (bs1:1482).
//
// Not a Ghost type: the move targets the user. It fails only if Speed is at
// -6 and Attack and Defense at +6; otherwise Speed falls by 1 (Mist and
// Clear Body don't apply to Curse, bsc:6962) and Attack and Defense rise
// by 1, each skipped at its cap (Attack or Defense at +6 marks it MISSED,
// as in raise()). Showdown's Curse gives the same self boosts, spe -1,
// atk +1, def +1 in that order (gen4's onModifyMove sets them, and the base
// onTryHit it inherits sets them again in this order, data/moves.ts:3287):
// a line for each stat that moved, and no -fail.
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
    const bool slower = b.spe > -6;
    if (slower) b.spe--;
    const int8_t atk = raise(a, 0, 1, m);
    const int8_t def = raise(a, 1, 1, m);
    const ID id = protocol::ident(battle, player);
    if (slower) protocol::boost(options.log, id, 2, 1, true);
    if (atk != 0) protocol::boost(options.log, id, 0, atk, false);
    if (def != 0) protocol::boost(options.log, id, 1, def, false);
    return moveend(battle, player, m, options);
  }

  ActivePokemon& t = battle.side(foe(player)).active;
  // NO_ACC_CALC_CHECK_LOCK_ON (Cmd_accuracycheck, bsc:1099): Lock-On, then
  // only the semi-invulnerable check (try_hit, which Showdown's accuracy
  // true agrees with). A miss goes to BattleScript_ButItFailed (here
  // MISSED, which moveend reads the same). Showdown's messages: -miss, then
  // -fail for a curse already there (onTryHit) or a Substitute
  // (TryPrimaryHit).
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return moveend(battle, player, m, options);
  }
  if (t.volatiles.substitute || t.volatiles.cursed) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.volatiles.cursed = true;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Curse,
                  protocol::ident(battle, player));
  // Damage (plain, as Showdown's directDamage from the move)
  passive_damage(battle, player, fraction(battle.side(player).get(1), 2),
                 options);
  protocol::damage(options.log, protocol::ident(battle, player),
                   battle.side(player).get(1));
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
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  ActivePokemon& a = battle.side(player).active;
  const Pokemon& mon = battle.side(player).get(1);
  const uint32_t quarter = fraction(mon, 4);
  if (a.volatiles.substitute || mon.hp <= quarter) {
    protocol::fail(options.log, protocol::ident(battle, player),
                   a.volatiles.substitute ? protocol::Fail::Substitute
                                          : protocol::Fail::SubstituteWeak);
    return moveend(battle, player, m, options);
  }
  assert(quarter <= UINT8_MAX);
  a.volatiles.substitute = true;
  a.substitute_hp = uint8_t(quarter);
  const ID id = protocol::ident(battle, player);
  protocol::start(options.log, id, protocol::Start::Substitute);
  if (a.volatiles.wrapped != 0) {
    protocol::end_effect(options.log, id, protocol::End::BoundSilent, a.wrapped_move);
    a.volatiles.wrapped = 0;
    a.volatiles.wrapper_gone = false;
    a.wrapped_move = Move::None;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).binding = 0;
  }
  // The user can't faint: its HP was above the quarter it loses.
  passive_damage(battle, player, quarter, options);
  protocol::damage(options.log, id, battle.side(player).get(1));
  moveend(battle, player, m, options);
}

// BattleScript_EffectProtect and BattleScript_EffectEndure (bs1), for
// Protect, Detect and Endure; Cmd_setprotectlike (bsc:6503). It fails when
// the user acts last this turn (gCurrentTurnActionNumber, Showdown's
// queue.willAct()), or on the success draw (rolls::protect); a failure
// restarts the count and marks the move MISSED. The count of consecutive
// successes restarts, as Showdown's gen3 stall (gen5 conditions.ts:24:
// duration 2, so it expires at the end of a turn without a success), after
// any turn without a successful use, `stall` timing it. The decomp restarts
// it only when the last resulting move (bsc:6506) isn't one of the three,
// so a turn the user doesn't act at all (its action cancelled by a faint)
// keeps the chain there. Past the fourth success, see rolls::protect. Log:
// SingleTurn, or Fail.
template <typename O>
constexpr void protect(Battle& battle, Player player, MoveState& m,
                       O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  if (a.stall == 0) a.protect_uses = 0;
  const uint8_t n = a.protect_uses < 3 ? a.protect_uses : 3;
  if (!m.first || (n != 0 && !rolls::protect(battle, player, n, options))) {
    protocol::failed(options.log, protocol::ident(battle, player));
    a.protect_uses = 0;
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  const bool endure = get(m.move).effect != Effect::Protect;
  (endure ? m.turn->endure : m.turn->protect)[index(player)] = true;
  protocol::single_turn(options.log, protocol::ident(battle, player),
                        endure ? protocol::SingleTurn::Endure : protocol::SingleTurn::Protect);
  if (a.protect_uses != UINT8_MAX) a.protect_uses++;
  a.stall = 2;
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
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  layers++;
  protocol::side_start(options.log, foe(player), protocol::SideCondition::Spikes);
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
// Morning Sun family, data/mods/gen4/moves.ts:940). That family's gen4
// onHit is a bare heal, so at full HP Showdown prints nothing after the
// move line, where the others fail. Log: Heal, or Fail heal (not for the
// Morning Sun family).
template <typename O>
constexpr void heal_move(Battle& battle, Player player, MoveState& m,
                         O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  Pokemon& mon = battle.side(player).get(1);
  const Effect effect = get(m.move).effect;
  const bool weather = effect == Effect::MorningSun ||
                       effect == Effect::Synthesis || effect == Effect::Moonlight;
  if (mon.hp == mon.stats.hp) {
    if (!weather) {
      protocol::fail(options.log, protocol::ident(battle, player), protocol::Fail::Heal, true);
    }
    return moveend(battle, player, m, options);
  }
  uint32_t amount = mon.stats.hp / 2u;
  if (weather) {
    const Weather w = effective_weather(battle);
    if (w == Weather::Sun) {
      amount = 20u * mon.stats.hp / 30u;
    } else if (w != Weather::None) {
      amount = mon.stats.hp / 4u;
    }
  }
  heal(mon, amount == 0 ? 1 : amount);
  protocol::heal(options.log, protocol::ident(battle, player), mon);
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
// - After the canceler, attackstring and ppreduce: Suction Cups and
//   Ingrain stop it with no result flag (Showdown's onDragOut), then a
//   semi-invulnerable target fails it (accuracycheck with
//   NO_ACC_CALC_CHECK_LOCK_ON; here MISSED, which moveend reads the same),
//   then the accuracy roll (ACC_CURR_MOVE: evasion can make it miss), then
//   an empty bench fails it (forcerandomswitch; Showdown's canSwitch).
// - The messages come in Showdown's order: try_hit (Protect, Soundproof
//   for Roar), then -fail for an empty bench (forceSwitch's canSwitch),
//   and only at the drag Suction Cups' or Ingrain's -activate (DragOut).
//   The roll is recorded only when it decides a drag.
// - No level check: TryDoForceSwitchOut (bsc:7163) only draws when the
//   user's level is lower, and Showdown has none.
// - The drag happens after the whole action (Showdown's forceSwitchFlag;
//   the decomp drags before moveend), so this only sets m.drag.
// - It hits through a Substitute in both.
template <typename O>
constexpr void roar(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const ActivePokemon& t = battle.side(foe(player)).active;
  const bool anchored = t.ability == Ability::SuctionCups || t.volatiles.rooted;
  const bool bench_empty = bench(battle, foe(player)) == 0;
  if (!try_hit(battle, player, m, {.matters = !anchored && !bench_empty}, options)) {
    return moveend(battle, player, m, options);
  }
  if (bench_empty) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  // BattleScript_AbilityPreventsPhasingOut and BattleScript_PrintMonIsRooted
  // set no result flag. With both, Suction Cups speaks (Showdown's
  // onDragOutPriority 1, data/abilities.ts:4694; the decomp checks Ingrain
  // first).
  if (anchored) {
    protocol::activate(options.log, protocol::ident(battle, foe(player)),
                       t.ability == Ability::SuctionCups ? protocol::Activate::SuctionCups
                                                         : protocol::Activate::Ingrain);
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
  leave(battle, target, slot, options, false, true);
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
    protocol::failed(options.log, protocol::ident(battle, player));
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
// (data/mods/gen3/moves.ts). Nature Power is allowed: gen4's flags
// (data/mods/gen4/moves.ts:980), which gen3 inherits, drop nosleeptalk.
constexpr bool sleep_talk_banned(Move move) {
  if (move == Move::Bide) return false;
  return get(move).extra & Extra::NoSleepTalk;
}

template <typename O>
constexpr void execute(Battle& battle, Player player, MoveState& m, O& options);

// Showdown's useMove for a move that `m` calls (Sleep Talk, Metronome,
// Assist, Mirror Move, Nature Power): no attack canceler and no PP
// (MoveState::called), [from] the caller, and Pressure's PP for the called
// move comes off the caller's slot (callerMoveForPressure, gen3
// scripts.ts:174; the cartridge takes none). A Hidden Power is the user's
// own type. `slot` is the called move's slot (Sleep Talk's), or NO_SLOT.
template <typename O>
constexpr void call_move(Battle& battle, Player player, MoveState& m, Move move,
                         uint8_t slot, O& options) {
  const ActivePokemon& a = battle.side(player).active;
  if (move == Move::HiddenPower || is_hidden_power(move)) {
    move = hidden_power(a.hidden_power_type);
  }
  const bool self = targets_user(move) || (move == Move::Curse && !has_type(a, Type::Ghost));
  if (pressure_applies(battle, player, move, self) && m.slot != MoveState::NO_SLOT) {
    deduct_pp(battle, player, m.slot, 1);
  }
  MoveState c{};
  c.move = move;
  c.chosen = m.chosen;
  c.from = m.move;
  c.slot = slot;
  c.no_pp = true;
  c.called = true;
  c.first = m.first;
  c.turn = m.turn;
  execute(battle, player, c, options);
  m.drag = c.drag;
  m.pass = c.pass;
  m.ran = c.ran != Move::None ? c.ran : c.move;
  m.after_move = c.after_move;
}

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
    // Showdown's onTry fails silently when awake; its onTryHit (the lock)
    // and onHit (nothing to call) print -fail.
    if (battle.side(player).get(1).status.asleep()) {
      protocol::failed(options.log, protocol::ident(battle, player));
    }
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const uint8_t slot = rolls::sleep_talk(battle, player, mask, options);
  if (a.moves[slot].pp == 0) {
    protocol::cant(options.log, protocol::ident(battle, player),
                   protocol::Cant::SleepTalkPP, a.moves[slot].id);
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  assert(get(a.moves[slot].id).effect != Effect::SleepTalk);
  call_move(battle, player, m, a.moves[slot].id, slot, options);
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
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  side.conditions.wish = 2;
  side.conditions.wisher = side.order[0];
  moveend(battle, player, m, options);
}

// BattleScript_EffectLeechSeed (bs1:1157): after the canceler, attackstring
// and ppreduce, a Substitute fails it (ButItFailed); then accuracy; then
// Cmd_setseeded (bsc:6718) misses an already seeded or Grass target. The
// messages come in Showdown's order: try_hit, with Grass a natural
// immunity (onTryImmunity), then the Substitute (TryPrimaryHit) and an
// existing seed (addVolatile), each -fail. The drain is in battler_stage.
// Log: Start Leech Seed.
template <typename O>
constexpr void leech_seed(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& t = battle.side(foe(player)).active;
  const bool takes = !t.volatiles.substitute && !t.volatiles.leech_seed;
  if (!try_hit(battle, player, m,
               {.immune = has_type(t, Type::Grass), .matters = takes}, options)) {
    return moveend(battle, player, m, options);
  }
  if (!takes) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= t.volatiles.substitute ? MoveResult::FAILED : MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  t.volatiles.leech_seed = true;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::LeechSeed);
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
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  field.weather = w;
  field.weather_turns = 5;
  field.permanent = false;
  protocol::weather(options.log, w);
  weather_change(battle, options);
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
  m.attackstring = true;
  pp_reduce(battle, player, m);
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
  if (!try_hit(battle, player, m, {.matters = n != 0}, options)) {
    return moveend(battle, player, m, options);
  }
  if (n == 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  uint8_t i = 0;
  for (; i < n && !target.fainted(); i++) {
    Hit h = Hit::of(m.move);
    h.ally = allies[i];
    if (can_crit(battle, player)) {
      h.crit = rolls::crit(battle, player, i, crit_stage(battle, player, m.move), options);
    }
    uint32_t d = apply_roll(damage(battle, player, h), rolls::damage(battle, player, i, options));
    log_modifiers(battle, player, m, h.crit, options);
    d = survive(battle, player, m, d, i, options);
    const bool damaged = m.damaged;
    m.damaged = false;
    m.hp_dealt = hp_update(battle, defender, d, m, options);
    log_hit(battle, player, m, options);
    rage(battle, player, m, options);
    flinch_item(battle, player, m, i, options);
    m.damaged |= damaged;
  }
  m.kings_rock_done = true;
  protocol::hit_count(options.log, protocol::ident(battle, defender), i);
  faint_target(battle, player, m.chosen, m.grudge, m.bonded_hp, options);
  moveend(battle, player, m, options);
}

// BattleScript_EffectPerishSong (bs1:1567), as Showdown's Perish Song
// (data/moves.ts onHitField): after the canceler, attackstring and
// ppreduce, every active Pokémon without a count gets 3, except a
// semi-invulnerable one, which it misses, and a Soundproof one, the user
// included (gen7's Soundproof, which gen3 inherits, drops the user check:
// data/mods/gen7/abilities.ts:100). It fails only if nothing was missed,
// blocked or started. The decomp's Cmd_trysetperishsong (bsc:8508) ignores
// semi-invulnerability, and fails when every Pokémon is Soundproof or
// counting. Pressure takes an extra PP (pressure_applies; the decomp's
// PressurePPLoseOnUsingPerishSong agrees). Log, in Showdown's onHitField
// order: per Pokémon a Miss, an Immune for Soundproof or Start perish3
// [silent]; then FieldActivate if one started, or Fail.
template <typename O>
constexpr void perish_song(Battle& battle, Player player, MoveState& m,
                           O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  // Showdown's getAllActive order, P1 first.
  bool result = false;
  bool started = false;
  for (const Player p : {Player::P1, Player::P2}) {
    ActivePokemon& a = battle.side(p).active;
    const ID id = protocol::ident(battle, p);
    if (p != player && a.volatiles.semi_invulnerable()) {
      protocol::miss(options.log, protocol::ident(battle, player), id, false);
      result = true;
    } else if (a.ability == Ability::Soundproof) {
      protocol::immune(options.log, id, protocol::Immune::Ability, Ability::Soundproof);
      result = true;
    } else if (!a.volatiles.perish_song) {
      a.volatiles.perish_song = true;
      a.perish_song_timer = 3;
      protocol::start(options.log, id, protocol::Start::PerishSilent);
      result = true;
      started = true;
    }
  }
  if (!result) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
  } else if (started) {
    protocol::field_activate(options.log, protocol::FieldActivate::PerishSong);
  }
  moveend(battle, player, m, options);
}

// BattleScript_EffectReflect (bs1:974), LightScreen, Mist (bs1:874) and
// Safeguard (bs1:1672): after the canceler, attackstring and ppreduce (and
// Snatch), Cmd_setreflect (bsc:6697), setlightscreen, setmist (bsc:7732)
// and setsafeguard (bsc:8652) fail while the condition is up (FAILED for
// Mist, MISSED for the others), otherwise set 5 turns on the user's side;
// field_residuals counts them down. Showdown's side conditions agree.
// Log: SideStart, or Fail.
template <typename O>
constexpr void screen(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  SideConditions& c = battle.side(player).conditions;
  using SC = protocol::SideCondition;
  const Effect effect = get(m.move).effect;
  uint8_t& turns = effect == Effect::Reflect   ? c.reflect
                   : effect == Effect::Mist    ? c.mist
                   : effect == Effect::Safeguard ? c.safeguard
                                               : c.light_screen;
  if (turns != 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= effect == Effect::Mist ? MoveResult::FAILED : MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  turns = 5;
  protocol::side_start(options.log, player,
                       effect == Effect::Reflect     ? SC::Reflect
                       : effect == Effect::Mist      ? SC::Mist
                       : effect == Effect::Safeguard ? SC::Safeguard
                                                     : SC::LightScreen);
  moveend(battle, player, m, options);
}

// BattleScript_EffectMagicCoat (bs1:2392) and Snatch (bs1:2543), as
// Showdown's one-turn volatiles: after the canceler, attackstring and
// ppreduce, the user bounces (try_hit) or steals (snatched) the next such
// move this turn. Cmd_trysetmagiccoat and trysetsnatch (bsc:9537, 9553) fail
// when the user moves last; Showdown's never fail, and this follows it.
// Log: SingleTurn Magic Coat or Snatch.
template <typename O>
constexpr void coat_or_snatch(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  assert(m.turn != nullptr);
  const bool coat = m.move == Move::MagicCoat;
  (coat ? m.turn->magic_coat : m.turn->snatch)[index(player)] = true;
  protocol::single_turn(options.log, protocol::ident(battle, player),
                        coat ? protocol::SingleTurn::MagicCoat : protocol::SingleTurn::Snatch);
  moveend(battle, player, m, options);
}

// BattleScript_EffectMeanLook (bs1:1444): after the canceler, attackstring
// and ppreduce, Protect stops it and it misses a semi-invulnerable target
// (accuracycheck with NO_ACC_CALC, no draw: try_hit, as Showdown's
// accuracy true), and it fails on one already trapped or behind a
// Substitute; otherwise the target can't switch (escape_prevention, which
// ends when the user switches out normally, README decision 9). Showdown
// agrees (its protect flag comes from the gen5 mod). Log: Activate
// trapped.
template <typename O>
constexpr void mean_look(Battle& battle, Player player, MoveState& m,
                         O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Volatiles& v = battle.side(foe(player)).active.volatiles;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return moveend(battle, player, m, options);
  }
  if (v.escape_prevention || v.substitute) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  v.escape_prevention = true;
  protocol::activate(options.log, protocol::ident(battle, foe(player)),
                     protocol::Activate::Trapped);
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
  if (!try_hit(battle, player, m, {.matters = t.taunt == 0}, options)) {
    return moveend(battle, player, m, options);
  }
  if (t.taunt != 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.taunt = 2;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Taunt);
  moveend(battle, player, m, options);
}

// BattleScript_EffectEncore (bs1:1227), Cmd_trysetencore (bsc:8017): the
// target's last move must be in its moveset with PP left, not one of
// Showdown's gen3 failencore moves (Encore, Mimic, Mirror Move, Sketch,
// Struggle, Transform: a superset of the decomp's Struggle, Encore and Mirror
// Move, since the others replace their own slot), and the target must not
// already be encored; otherwise it fails (ButItFailed). Then the accuracy
// check (Protect, evasion; skipped when it fails anyway, as Taunt) and a
// hidden 3-6 turn timer ((Random() & 3) + 3; Showdown's random(3, 7)). No
// Substitute check (Showdown: bypasssub). A target that hasn't moved yet
// this turn uses the encored move (use_move, as Showdown's
// onOverrideAction); the residual counts it down. Log: Start Encore, or
// Fail.
template <typename O>
constexpr void encore(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& t = battle.side(foe(player)).active;
  uint8_t slot = 0;
  while (slot < 4 && t.moves[slot].id != t.last_move) slot++;
  bool fails = slot == 4 || t.last_move == Move::None ||
               t.encored_move != Move::None;
  switch (t.last_move) {
  case Move::Encore:
  case Move::Mimic:
  case Move::MirrorMove:
  case Move::Sketch:
  case Move::Struggle:
  case Move::Transform: fails = true; break;
  default: break;
  }
  fails = fails || t.moves[slot].pp == 0;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return moveend(battle, player, m, options);
  }
  if (fails) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.encored_move = t.last_move;
  t.encored_slot = slot;
  t.encore_timer = uint8_t(3 + battle.rng.range(4));
  if constexpr (CHANCE<O>) options.chance.durations.get(foe(player)).encore = 1;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Encore);
  moveend(battle, player, m, options);
}

// BattleScript_EffectDestinyBond (bs1:1334), Cmd_setdestinybond
// (bsc:8279): it never fails. The canceler clears it at the user's next
// move attempt (CANCELER_FLAGS), a switch-in's Spikes check and fainting
// clear it, and Baton Pass doesn't pass it; faint_target() acts on it.
// Showdown's gen3 agrees (its onBeforeMove, onMoveAborted, noCopy). Log:
// SingleMove Destiny Bond.
template <typename O>
constexpr void destiny_bond(Battle& battle, Player player, MoveState& m,
                            O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  battle.side(player).active.volatiles.destiny_bond = true;
  protocol::single_move(options.log, protocol::ident(battle, player),
                        protocol::SingleMove::DestinyBond);
  moveend(battle, player, m, options);
}

// BattleScript_EffectMimic (bs1:1134), Cmd_mimicattackcopy (bsc:7844):
// after attackstring and ppreduce it fails (ButItFailed) on a Substitute,
// then on Protect or a semi-invulnerable target unless Locked-On, with no
// accuracy roll (NO_ACC_CALC_CHECK_LOCK_ON); then if the user is
// transformed, the target has no last move, the move can't be mimicked
// (Metronome, Struggle, Sketch, Mimic) or the user knows it already.
// Otherwise Mimic's slot holds the move with min(5, base PP) until the user
// leaves the field (`mimicked`; pp_reduce leaves the party's PP alone).
// Showdown's gen3 (gen4's onHit, data/mods/gen4/moves.ts:882) agrees,
// on the target's lastMove (last_move follows it; see moveend). Log:
// Activate Mimic + move, or Fail.
template <typename O>
constexpr void mimic(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& t = battle.side(foe(player)).active;
  // Hidden Power is one move on the cartridge (MOVE_HIDDEN_POWER): the user
  // knows it if it knows any typed one, and a copy is the user's own type.
  const Move last = t.last_move;
  bool known = false;
  for (const MoveSlot& ms : a.moves) {
    known |= same_move(ms.id, last);
  }
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (t.volatiles.substitute || a.volatiles.transformed || last == Move::None ||
      (get(last).extra & Extra::NoMimic) || known) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  assert(m.slot < 4);
  const uint8_t base = get(last).pp;
  a.moves[m.slot].id = is_hidden_power(last)
                           ? hidden_power(a.hidden_power_type)
                           : last;
  a.moves[m.slot].pp = base < 5 ? base : 5;
  a.mimicked = uint8_t(a.mimicked | 1 << m.slot);
  protocol::activate_with(options.log, protocol::ident(battle, player), protocol::Activate::Mimic,
                          a.moves[m.slot].id);
  moveend(battle, player, m, options);
}

// BattleScript_EffectPainSplit (bs1:1239), Cmd_painsplitdmgcalc
// (bsc:8049): after attackstring and ppreduce it fails (ButItFailed) on
// Protect or a semi-invulnerable target unless Locked-On, with no accuracy
// roll (NO_ACC_CALC_CHECK_LOCK_ON), or on a Substitute; otherwise both
// Pokémon go to the average of their HP, capped at max HP. Showdown agrees
// on the state, but sets the HP directly where the decomp runs datahpupdate,
// so here the change doesn't count as damage (Counter, Focus Punch, Bide).
// Log: SetHP for the target ([silent]), then the user.
template <typename O>
constexpr void pain_split(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& foe_side = battle.side(foe(player));
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  if (foe_side.active.volatiles.substitute) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  Pokemon& user = battle.side(player).get(1);
  Pokemon& target = foe_side.get(1);
  const uint16_t average = uint16_t((uint32_t(user.hp) + target.hp) / 2);
  user.hp = average < user.stats.hp ? average : user.stats.hp;
  target.hp = average < target.stats.hp ? average : target.stats.hp;
  protocol::set_hp(options.log, protocol::ident(battle, foe(player)), target, true);
  protocol::set_hp(options.log, protocol::ident(battle, player), user, false);
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
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  Pokemon& mon = battle.side(player).get(1);
  if (!mon.status.poisoned() && !mon.status.burned() && !mon.status.paralyzed()) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  protocol::cure_status(options.log, protocol::ident(battle, player), mon.status);
  mon.status = Status::none();
  battle.side(player).active.toxic = 0; // status1 holds the counter
  moveend(battle, player, m, options);
}

// BattleScript_EffectHealBell (bs1:1361), Cmd_healpartystatus (bsc:8370),
// for Heal Bell and Aromatherapy: every Pokémon on the user's side is
// cured, the active one's Nightmare too, except that Heal Bell skips
// Soundproof ones (the active one by its battle ability, the others by
// their own). It never fails. Showdown's gen4 versions agree. Cured sleeps
// are public, so their counts reset. Log, as Showdown's gen4 mod: Heal
// Bell's Activate, then by SLOT an Immune for an active Soundproof one or
// a silent CureStatus; Aromatherapy's one CureTeam.
template <typename O>
constexpr void heal_bell(Battle& battle, Player player, MoveState& m,
                         O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  Side& side = battle.side(player);
  const bool bell = m.move == Move::HealBell;
  const ID user = protocol::ident(battle, player);
  if (bell) {
    protocol::activate(options.log, user, protocol::Activate::HealBell);
  } else {
    protocol::cure_team(options.log, user);
  }
  for (uint8_t s = 1; s <= 6; s++) {
    Pokemon& mon = side.get(s);
    if (mon.species == Species::None) continue;
    const Ability ability = s == 1 ? side.active.ability : mon.ability;
    if (bell && ability == Ability::Soundproof) {
      if (s == 1) protocol::immune(options.log, user, protocol::Immune::Ability, ability);
      continue;
    }
    if (bell && mon.status.any() && !mon.fainted()) {
      protocol::cure_status(options.log, ID::init(player, side.order[s - 1]), mon.status,
                            protocol::Cure::Silent, s != 1);
    }
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

// Bide (BattleScript_EffectBide, bs1:1149), as Showdown's gen3: the first
// turn starts it (duration 3, the damage count reset, the user locked),
// the second only prints its -activate and the third releases twice the
// damage taken since (both in cancel()). The release is hit() with that
// fixed damage, aimed at the foe, with no move line and no PP: Showdown's
// gen3 tryMoveHit on its moveData (accuracy 100, so the stages count;
// Protect; Normal, so a Ghost is immune; contact; Physical for Counter).
// The damage counted is the foe's moves' on the user itself, before the cap
// at the HP left and after Endure and Focus Band (Showdown's onDamage,
// priority -101; hp_update). The decomp's is the same two turns, with the
// release's typecalc ignoring immunities. Log: Start Bide; Activate Bide;
// End Bide, then the hit or a Fail, then End Bide [silent].
template <typename O>
constexpr void bide(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  ActivePokemon& a = battle.side(player).active;
  if (m.release) {
    m.self = false;
    hit(battle, player, m, options);
    if (a.volatiles.bide != 0) end_bide(battle, player, options);
    return;
  }
  m.attackstring = true;
  pp_reduce(battle, player, m);
  // Showdown's gen3 Bide is aimed at the user with accuracy 100, so its own
  // stages count, as Nature Power's, and Hustle's 0.8 (it's Physical).
  const uint32_t calc = staged_accuracy(battle, player, get(m.move).accuracy,
                                        a.boosts.acc, a.ability, true);
  if (calc < 100 && !rolls::hit(battle, player, calc, options)) {
    const ID id = protocol::ident(battle, player);
    protocol::miss(options.log, id, id);
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  a.volatiles.bide = 3;
  a.bide_damage = 0;
  a.volatiles.multiple_turns = true;
  a.locked_move = m.move;
  protocol::start(options.log, protocol::ident(battle, player), protocol::Start::Bide);
  moveend(battle, player, m, options);
}

// Stockpile (BattleScript_EffectStockpile, bs1:2147), as Showdown's gen3
// stockpile (data/mods/gen3/moves.ts:591; no Def or SpD raise there): up to
// 3 layers; at 3 its onTry fails, silently (gen3 tryMoveHit). The layers
// aren't passed by Baton Pass (noCopy) and go silently at a switch.
// Log: Start stockpileN.
template <typename O>
constexpr void stockpile(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  ActivePokemon& a = battle.side(player).active;
  if (a.stockpile >= 3) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  a.stockpile++;
  protocol::start_count(options.log, protocol::ident(battle, player), protocol::Start::Stockpile,
                        a.stockpile);
  moveend(battle, player, m, options);
}

// Swallow (BattleScript_EffectSwallow, bs1:2167), as Showdown (gen4 mod's
// onTry, base onHit): with no Stockpile it fails silently; otherwise it
// heals modify(max HP, 1/4, 1/2 or 1) by the layers (the decomp divides),
// or at full HP prints -fail heal, and either way the layers go. Log: Heal
// or Fail heal, then End Stockpile.
template <typename O>
constexpr void swallow(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  const ActivePokemon& a = battle.side(player).active;
  if (a.stockpile == 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  Pokemon& mon = battle.side(player).get(1);
  const ID id = protocol::ident(battle, player);
  if (mon.hp == mon.stats.hp) {
    protocol::fail(options.log, id, protocol::Fail::Heal);
  } else {
    constexpr uint32_t SHARE[3] = {1024, 2048, 4096};
    heal(mon, uint32_t(modify(mon.stats.hp, SHARE[a.stockpile - 1])));
    protocol::heal(options.log, id, mon);
  }
  stockpile_end(battle, player, options);
  moveend(battle, player, m, options);
}

// Future Sight and Doom Desire's use (BattleScript_EffectFutureSight,
// bs1:2061; Cmd_trysetfutureattack, bsc:8357), as Showdown's gen3 onTry
// (data/mods/gen3/moves.ts:218; gen4 mod's futuresight): it fails, silently,
// while a future attack is pending on the foe's side (addSlotCondition).
// Otherwise the damage is computed now, against the foe's Pokémon as it is:
// a typeless hit (damage.hpp) of power 80 (special) or 120 (physical) with
// no crit, and the roll (hits[0]). Protect and semi-invulnerability don't
// stop it (onTry comes before them). It lands at the end of the turn after
// next (future_attack). The decomp's damage has no roll and keeps the
// move's type. Log: Start Future Sight or Doom Desire, on the user.
template <typename O>
constexpr void future_sight_move(Battle& battle, Player player, MoveState& m,
                                 O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& foe_side = battle.side(foe(player));
  if (foe_side.conditions.future_sight != 0) {
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const Hit h = Hit::of(m.move);
  foe_side.future_sight_damage =
      apply_roll(damage(battle, player, h), rolls::damage(battle, player, 0, options));
  foe_side.future_sight_move = m.move;
  foe_side.conditions.future_sight = 3;
  foe_side.conditions.future_user = battle.side(player).order[0];
  protocol::start(options.log, protocol::ident(battle, player),
                  m.move == Move::DoomDesire ? protocol::Start::DoomDesire
                                             : protocol::Start::FutureSight);
  moveend(battle, player, m, options);
}

// Confusion from a status move, after its own checks (Showdown's
// addVolatile('confusion')): an existing confusion fails it silently
// (false), Own Tempo and Safeguard stop it (TryAddVolatile; Safeguard's
// -activate here, Own Tempo's -immune is the caller's, at onHit), else it
// takes hold. Returns 1 if it took hold, 0 if stopped, -1 if it failed.
template <typename O>
constexpr int8_t add_confusion(Battle& battle, Player player, O& options) {
  const Player target = foe(player);
  const Side& foe_side = battle.side(target);
  const ActivePokemon& t = foe_side.active;
  if (t.volatiles.confusion != 0) return -1;
  if (t.ability == Ability::OwnTempo) return 0;
  if (foe_side.conditions.safeguard != 0) {
    protocol::activate(options.log, protocol::ident(battle, target),
                       protocol::Activate::Safeguard);
    return 0;
  }
  confuse(battle, target, options);
  return 1;
}

// Swagger and Flatter, as Showdown (boosts, then volatileStatus confusion):
// try_hit, a Substitute (TryPrimaryHit), the raise (-boost 0 at +6), then
// add_confusion; Own Tempo's -immune comes last (its onHit). It fails
// (-fail) only if the raise did nothing and the target was confused
// already. The decomp fails it first when both hold
// (jumpifconfusedandstatmaxed, bs1:1559); the state is the same.
template <typename O>
constexpr void swagger(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const StatChange c = get(m.move).effect == Effect::Swagger ? StatChange{0, 2}
                                                             : StatChange{3, 1};
  int8_t& b = stage(t.boosts, c.stat);
  const bool sub = t.volatiles.substitute;
  const bool confusable = t.volatiles.confusion == 0 && t.ability != Ability::OwnTempo &&
                          battle.side(target).conditions.safeguard == 0;
  if (!try_hit(battle, player, m, {.matters = !sub && (b < 6 || confusable)}, options)) {
    return moveend(battle, player, m, options);
  }
  const ID id = protocol::ident(battle, target);
  if (sub) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  // At +6 the raise marks the move MISSED, as raise() and the decomp.
  const int8_t gained = b + c.n > 6 ? int8_t(6 - b) : c.n;
  b = int8_t(b + gained);
  if (gained == 0) m.result |= MoveResult::MISSED;
  protocol::boost(options.log, id, c.stat, gained, false);
  const int8_t confused = add_confusion(battle, player, options);
  if (t.ability == Ability::OwnTempo) {
    protocol::immune(options.log, id, protocol::Immune::Confusion, Ability::OwnTempo);
  }
  if (gained == 0 && confused < 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
  }
  moveend(battle, player, m, options);
}

// Attract, as Showdown: a target of the same gender or genderless is
// immune (onTryImmunity, so -immune after try_hit's checks); an existing
// infatuation or Oblivious (onImmunity('attract')) fails it. No Substitute
// check (bypasssub). It ends when the user leaves (leave(), faint()).
// Mental Herb cures it at moveend. Log: Start Attract.
template <typename O>
constexpr void attract(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const Gender g1 = battle.side(player).get(1).personality.gender();
  const Gender g2 = battle.side(target).get(1).personality.gender();
  const bool immune = g1 == Gender::None || g2 == Gender::None || g1 == g2;
  const bool fails = t.volatiles.infatuated || t.ability == Ability::Oblivious;
  if (!try_hit(battle, player, m, {.immune = immune, .matters = !fails}, options)) {
    return moveend(battle, player, m, options);
  }
  if (fails) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  t.volatiles.infatuated = true;
  protocol::start(options.log, protocol::ident(battle, target), protocol::Start::Attract);
  moveend(battle, player, m, options);
}

// Disable, as Showdown's gen3: it fails without a last move or on Struggle
// (onTryHit), on a target already disabled, or if the last move isn't known
// or has no PP (gen4's onStart). No Substitute check (bypasssub). Its
// duration is random(2, 6), plus 1 if the target won't move after this
// (gen4's onStart: !willMove), so it always covers 2-5 of the target's
// moves; the decomp's (Random() & 3) + 2 has no +1. The hidden timer
// counts residuals; its Durations count starts at 2, or at 1 with the +1,
// over a 3-6 roll (chance.hpp). Log: Start Disable.
template <typename O>
constexpr void disable(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  uint8_t slot = 0;
  while (slot < 4 && t.moves[slot].id != t.last_move) slot++;
  const bool fails = t.last_move == Move::None || t.last_move == Move::Struggle ||
                     t.disabled_move != Move::None || slot == 4 || t.moves[slot].pp == 0;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return moveend(battle, player, m, options);
  }
  if (fails) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const bool will_move =
      m.first && battle.request.choice[index(target)].type() == Choice::Move;
  const uint8_t count = will_move ? 2 : 1;
  t.disabled_move = t.last_move;
  t.disable_timer = uint8_t(3 + battle.rng.range(4) - (count - 1));
  if constexpr (CHANCE<O>) options.chance.durations.get(target).disable = count;
  protocol::start_disable(options.log, protocol::ident(battle, target), t.last_move);
  moveend(battle, player, m, options);
}

// Torment, as Showdown (bypasssub): it fails on a target already tormented;
// the selection then leaves out the last move (usable_moves). It ends when
// the target leaves. Log: Start Torment.
template <typename O>
constexpr void torment(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  Volatiles& v = battle.side(target).active.volatiles;
  if (!try_hit(battle, player, m, {.matters = !v.torment}, options)) {
    return moveend(battle, player, m, options);
  }
  if (v.torment) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  v.torment = true;
  protocol::start(options.log, protocol::ident(battle, target), protocol::Start::Torment);
  moveend(battle, player, m, options);
}

// Spite, as Showdown's gen3 (bypasssub): the target's last move loses 2-5
// PP (at most what it has); it fails if the target has no last move, doesn't
// know it or it has no PP. The roll is drawn only when it can change the PP
// taken (rolls::spite); below 3 PP it changes only the log, which prints
// the roll. Log: Activate Spite.
template <typename O>
constexpr void spite(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  const ActivePokemon& t = battle.side(target).active;
  uint8_t slot = 0;
  while (slot < 4 && t.moves[slot].id != t.last_move) slot++;
  const bool fails = t.last_move == Move::None || slot == 4 || t.moves[slot].pp == 0;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return moveend(battle, player, m, options);
  }
  if (fails) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const uint8_t n = t.moves[slot].pp >= 3 ? rolls::spite(battle, player, options)
                                          : uint8_t(2 + battle.rng.range(4));
  deduct_pp(battle, target, slot, n);
  protocol::activate_spite(options.log, protocol::ident(battle, target), t.last_move, n);
  moveend(battle, player, m, options);
}

// Foresight and Odor Sleuth, as Showdown's gen3 (accuracy 100, so evasion
// can make it miss; bypasssub): it fails on a target already identified.
// The target's evasion above 0 counts as 0 against it and a Ghost loses its
// Normal and Fighting immunity (accuracy(), damage.hpp) until it leaves.
// Log: Start Foresight.
template <typename O>
constexpr void foresight(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  Volatiles& v = battle.side(target).active.volatiles;
  if (!try_hit(battle, player, m, {.matters = !v.foresight}, options)) {
    return moveend(battle, player, m, options);
  }
  if (v.foresight) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  v.foresight = true;
  protocol::start(options.log, protocol::ident(battle, target), protocol::Start::Foresight);
  moveend(battle, player, m, options);
}

// Lock-On and Mind Reader, as Showdown's gen3 (accuracy 100, no bypasssub):
// a lock already on (onTryHit), even on a target that has left, or a
// Substitute (TryPrimaryHit) fails it; otherwise the user's moves hit the
// target until the end of the next turn (lock_on, locked_on()). Log:
// Activate Lock-On or Mind Reader [of] the target.
template <typename O>
constexpr void lock_on(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Player target = foe(player);
  Volatiles& v = battle.side(player).active.volatiles;
  const bool fails = v.lock_on != 0 || battle.side(target).active.volatiles.substitute;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return moveend(battle, player, m, options);
  }
  if (fails) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  v.lock_on = uint8_t(2 | battle.side(target).order[0] << 2);
  protocol::activate_with(options.log, protocol::ident(battle, player),
                          m.move == Move::MindReader ? protocol::Activate::MindReader
                                                     : protocol::Activate::LockOn,
                          Move::None, protocol::ident(battle, target));
  moveend(battle, player, m, options);
}

// Focus Energy, Ingrain, Charge, Mud Sport and Water Sport: a volatile on
// the user. All but Charge fail if it's on already (addVolatile; the
// Sports' settypebasedhalvers, bsc:9758); Charge restarts, as Showdown's
// onRestart, and its gen3 version raises no Special Defense. The Sports are
// Showdown's gen4 volatiles (gen3 inherits them, not the field condition).
// Showdown's Charge (gen8 mod, which gen3 inherits) has no duration: it
// ends after the user's next move other than Charge, or that move's abort
// (use_move); the decomp's lasts to the next turn's end, Electric or not.
// Log: Start Focus Energy, Ingrain or Charge.
template <typename O>
constexpr void self_volatile(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  Volatiles& v = battle.side(player).active.volatiles;
  const Effect effect = get(m.move).effect;
  bool& on = effect == Effect::FocusEnergy ? v.focus_energy
             : effect == Effect::Ingrain   ? v.rooted
             : effect == Effect::MudSport  ? v.mud_sport
             : effect == Effect::WaterSport ? v.water_sport
                                           : v.charged_up;
  if (on && effect != Effect::Charge) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  on = true;
  protocol::start(options.log, protocol::ident(battle, player),
                  effect == Effect::FocusEnergy  ? protocol::Start::FocusEnergy
                  : effect == Effect::Ingrain    ? protocol::Start::Ingrain
                  : effect == Effect::MudSport   ? protocol::Start::MudSport
                  : effect == Effect::WaterSport ? protocol::Start::WaterSport
                                                 : protocol::Start::Charge);
  moveend(battle, player, m, options);
}

// Grudge, as Showdown: it never fails; the canceler clears it at the user's
// next move attempt. If the user faints to a foe's move first, that move
// loses its PP (faint_target). Log: SingleMove Grudge.
template <typename O>
constexpr void grudge(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  battle.side(player).active.volatiles.grudge = true;
  protocol::single_move(options.log, protocol::ident(battle, player),
                        protocol::SingleMove::Grudge);
  moveend(battle, player, m, options);
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
         effect == Effect::PsychUp || effect == Effect::Reflect ||
         effect == Effect::LightScreen || effect == Effect::MeanLook ||
         effect == Effect::Encore || effect == Effect::DestinyBond ||
         effect == Effect::Mimic || effect == Effect::PainSplit ||
         effect == Effect::Memento || effect == Effect::Imprison ||
         effect == Effect::Nightmare || effect == Effect::Bide ||
         effect == Effect::Stockpile || effect == Effect::Swallow ||
         effect == Effect::FutureSight || effect == Effect::Swagger ||
         effect == Effect::Flatter || effect == Effect::TeeterDance ||
         effect == Effect::Attract || effect == Effect::Disable ||
         effect == Effect::Torment || effect == Effect::Spite ||
         effect == Effect::Foresight || effect == Effect::LockOn ||
         effect == Effect::FocusEnergy || effect == Effect::Ingrain ||
         effect == Effect::Charge || effect == Effect::Grudge ||
         effect == Effect::Mist || effect == Effect::Safeguard ||
         effect == Effect::MudSport || effect == Effect::WaterSport ||
         effect == Effect::MagicCoat || effect == Effect::Snatch ||
         effect == Effect::Splash || effect == Effect::Teleport ||
         effect == Effect::Transform || effect == Effect::Sketch ||
         effect == Effect::MirrorMove || effect == Effect::Metronome ||
         effect == Effect::Assist || effect == Effect::NaturePower ||
         effect == Effect::Camouflage || effect == Effect::Conversion ||
         effect == Effect::Conversion2 || effect == Effect::RolePlay ||
         effect == Effect::SkillSwap || effect == Effect::Trick ||
         effect == Effect::Recycle;
}

// ---------------------------------------------------------------------------
// Step 5 group 8: copying, calling and swapping, as Showdown's gen3.
// ---------------------------------------------------------------------------

// Splash: Showdown's onTryHit prints -nothing. Teleport: gen7's onTry
// (which gen3 inherits) fails silently in a trainer battle.
template <typename O>
constexpr void splash(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (get(m.move).effect == Effect::Teleport) {
    m.result |= MoveResult::FAILED;
  } else {
    protocol::nothing(options.log);
  }
  moveend(battle, player, m, options);
}

// p's ability becomes `ability`, as Showdown's setAbility: the old one's End
// first (Flash Fire's boost ends silently; Cloud Nine and Air Lock let the
// weather back in for Forecast). Gen 3 runs no Start for the new one, so
// Intimidate, Trace and the weather abilities don't act.
template <typename O>
constexpr void set_ability(Battle& battle, Player player, Ability ability, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const Ability old = a.ability;
  if (a.volatiles.flash_fire) {
    a.volatiles.flash_fire = false;
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::FlashFire);
  }
  a.ability = ability;
  if (old == Ability::CloudNine || old == Ability::AirLock) weather_change(battle, options);
}

// Transform, as Showdown's transformInto (sim/pokemon.ts:1270): accuracy
// true, through a Substitute and Protect, but it misses a semi-invulnerable
// target; it fails on a transformed target. The user takes the target's
// species, types, stats but HP, stat stages, ability, Hidden Power and
// moves, each with min(5, base PP). Log: Transform.
template <typename O>
constexpr void transform(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& t = battle.side(foe(player)).active;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return moveend(battle, player, m, options);
  }
  if (t.volatiles.transformed) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  a.species = t.species;
  a.types[0] = t.types[0];
  a.types[1] = t.types[1];
  const uint16_t hp = a.stats.hp;
  a.stats = t.stats;
  a.stats.hp = hp;
  for (uint8_t i = 0; i < 4; i++) {
    const Move move = t.moves[i].id;
    const uint8_t base = move == Move::None ? 0 : get(move).pp;
    a.moves[i] = {move, uint8_t(base < 5 ? base : 5), a.moves[i].pp_ups};
  }
  a.mimicked = 0;
  a.boosts = t.boosts;
  a.hidden_power = t.hidden_power;
  a.hidden_power_type = t.hidden_power_type;
  a.volatiles.transformed = true;
  protocol::transform(options.log, protocol::ident(battle, player),
                      protocol::ident(battle, foe(player)));
  set_ability(battle, player, t.ability, options);
  moveend(battle, player, m, options);
}

// Sketch, as Showdown's gen4 mod (which gen3 inherits): accuracy true,
// through Protect, but it misses a semi-invulnerable target. It fails if the
// user is transformed, the target has no last move or is behind a
// Substitute, the move is Sketch or Struggle, or the user knows it.
// Otherwise Sketch's slot holds the move for good (the party's too), with
// its base PP and the slot's PP Ups. Log: Activate Mimic + move (Showdown's
// gen4 Sketch prints Mimic).
template <typename O>
constexpr void sketch(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Move last = battle.side(foe(player)).active.last_move;
  if (is_hidden_power(last)) last = hidden_power(side.get(1).hidden_power_type);
  bool known = false;
  for (const MoveSlot& ms : a.moves) {
    known |= same_move(ms.id, last);
  }
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return moveend(battle, player, m, options);
  }
  if (a.volatiles.transformed || last == Move::None ||
      battle.side(foe(player)).active.volatiles.substitute || last == Move::Sketch ||
      last == Move::Struggle || known) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  assert(m.slot < 4);
  a.moves[m.slot] = {last, get(last).pp, side.get(1).moves[m.slot].pp_ups};
  side.get(1).moves[m.slot] = a.moves[m.slot];
  protocol::activate_with(options.log, protocol::ident(battle, player),
                          protocol::Activate::Mimic, last);
  moveend(battle, player, m, options);
}

// Showdown's gen3 Mirror Move noMirror list (data/mods/gen3/moves.ts).
constexpr bool no_mirror(Move move) {
  switch (move) {
  case Move::Assist:
  case Move::Curse:
  case Move::DoomDesire:
  case Move::FocusPunch:
  case Move::FutureSight:
  case Move::MagicCoat:
  case Move::Metronome:
  case Move::Mimic:
  case Move::MirrorMove:
  case Move::NaturePower:
  case Move::PsychUp:
  case Move::RolePlay:
  case Move::Sketch:
  case Move::SleepTalk:
  case Move::Spikes:
  case Move::SpitUp:
  case Move::Taunt:
  case Move::TeeterDance:
  case Move::Transform: return true;
  default: return false;
  }
}

// Mirror Move, as Showdown's gen3 (data/mods/gen3/moves.ts:432): the last
// move that got past the accuracy check and TryHit on the user
// (last_taken_move, Showdown's attackedBy), if its user is in (the same
// Pokémon, last_taken_from), has used a move since it came in (its
// lastMove, which clearVolatile drops), still knows it and it isn't on the
// noMirror list; else it fails. The decomp
// keeps the flag FLAG_MIRROR_MOVE_AFFECTED instead.
template <typename O>
constexpr void mirror_move(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const ActivePokemon& a = battle.side(player).active;
  const Move move = a.last_taken_move;
  const Side& source = battle.side(foe(player));
  bool known = a.last_taken_from == source.order[0] && source.active.last_move != Move::None;
  bool knows = false;
  for (const MoveSlot& ms : source.active.moves) {
    knows |= same_move(ms.id, move);
  }
  known &= knows;
  if (move == Move::None || no_mirror(move) || !known) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  call_move(battle, player, m, move, MoveState::NO_SLOT, options);
}

// Metronome, as Showdown's gen3 (gen4's onHit): a uniform pick among
// METRONOME_MOVES (rolls::metronome); it never fails.
template <typename O>
constexpr void metronome(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  call_move(battle, player, m, rolls::metronome(battle, player, options),
            MoveState::NO_SLOT, options);
}

// Showdown's gen3 noassist flag: the moves Metronome can't call, plus
// Assist and Mirror Move. The decomp also leaves out the two-turn moves
// (IsInvalidForSleepTalkOrAssist).
constexpr bool assist_banned(Move move) {
  return (get(move).extra & Extra::NoMetronome) || move == Move::Assist ||
         move == Move::MirrorMove;
}

// Assist, as Showdown's gen4 (which gen3 inherits): a pick among the moves
// of the user's party but itself, fainted ones included, counted with
// repeats (rolls::assist); it fails if there are none.
template <typename O>
constexpr void assist(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Side& side = battle.side(player);
  Move list[20] = {};
  uint8_t n = 0;
  for (uint8_t i = 0; i < 6; i++) {
    if (i + 1 == side.order[0] || side.pokemon[i].species == Species::None) continue;
    for (const MoveSlot& ms : side.pokemon[i].moves) {
      if (ms.id == Move::None || assist_banned(ms.id)) continue;
      list[n++] = is_hidden_power(ms.id) ? Move::HiddenPower : ms.id;
    }
  }
  if (n == 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  call_move(battle, player, m, rolls::assist(battle, player, list, n, options),
            MoveState::NO_SLOT, options);
}

// Nature Power, as Showdown's gen3 (data/mods/gen3/moves.ts): aimed at the
// user with accuracy 95, so its own accuracy and evasion stages, Sand Veil
// and BrightPowder count (tryMoveHit); on a hit it calls Swift. The
// decomp calls Swift in a link battle with no accuracy check of its own.
template <typename O>
constexpr void nature_power(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const ActivePokemon& a = battle.side(player).active;
  const uint32_t calc = staged_accuracy(battle, player, get(m.move).accuracy,
                                        a.boosts.acc, a.ability, false);
  if (calc < 100 && !rolls::hit(battle, player, calc, options)) {
    const ID id = protocol::ident(battle, player);
    protocol::miss(options.log, id, id);
    m.result |= MoveResult::MISSED;
    return moveend(battle, player, m, options);
  }
  call_move(battle, player, m, Move::Swift, MoveState::NO_SLOT, options);
}

// Camouflage, as Showdown's gen4 (which gen3 inherits): Normal, or a
// failure if the user is Normal already. Snatch steals it. Log: Start
// typechange.
template <typename O>
constexpr void camouflage(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  ActivePokemon& a = battle.side(player).active;
  if (has_type(a, Type::Normal)) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  a.types[0] = a.types[1] = Type::Normal;
  protocol::start_type(options.log, protocol::ident(battle, player), Type::Normal, false);
  moveend(battle, player, m, options);
}

// Conversion, as Showdown's gen3: a pick among the types of the user's
// moves (repeats count; Hidden Power and Weather Ball are Normal, the
// dex's) but Curse's and the user's own (rolls::conversion); it fails if
// there are none. Conversion 2, as Showdown's gen3: the foe's last move used
// since it came in (last_used_move), by its type as used (Struggle's is
// Normal), and a uniform pick among the types that resist it or are immune
// (the user's own included); it fails with none. Accuracy true, through a
// Substitute and Protect, but it misses a semi-invulnerable target. The
// decomp's reads the last move to hit the user. Log: Start typechange.
template <typename O>
constexpr void conversion(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  ActivePokemon& a = battle.side(player).active;
  Type list[TYPES] = {};
  uint8_t n = 0;
  if (m.move == Move::Conversion) {
    for (const MoveSlot& ms : a.moves) {
      if (ms.id == Move::None || ms.id == Move::Curse) continue;
      const Type t = is_hidden_power(ms.id) ? Type::Normal : get(ms.id).type;
      if (!has_type(a, t)) list[n++] = t;
    }
  } else {
    const ActivePokemon& f = battle.side(foe(player)).active;
    if (!try_hit(battle, player, m, {.sure = true}, options)) {
      return moveend(battle, player, m, options);
    }
    if (f.last_used_move != Move::None) {
      for (uint8_t i = 0; i < TYPES; i++) {
        const Type t = Type(i);
        if (t == Type::Mystery) continue;
        const Effectiveness e = effectiveness(f.last_used_type, t);
        if (e == Effectiveness::Resisted || e == Effectiveness::Immune) list[n++] = t;
      }
    }
  }
  if (n == 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const Type t = rolls::conversion(battle, player, list, n, options);
  a.types[0] = a.types[1] = t;
  protocol::start_type(options.log, protocol::ident(battle, player), t, false);
  moveend(battle, player, m, options);
}

// Role Play, as Showdown's (gen3 runs no ability Start): accuracy true,
// through a Substitute and Protect, but it misses a semi-invulnerable
// target. Its TryHit fails on the same ability or Wonder Guard. Skill Swap,
// as Showdown's skillSwap (sim/battle.ts:1315): Protect stops it; it fails
// if either has Wonder Guard or both have the same ability. Log: Ability
// [from] Role Play, or Activate Skill Swap.
template <typename O>
constexpr void role_play(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  const Ability mine = battle.side(player).active.ability;
  const Ability theirs = battle.side(foe(player)).active.ability;
  const bool swap = m.move == Move::SkillSwap;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return moveend(battle, player, m, options);
  }
  if (mine == theirs || theirs == Ability::WonderGuard ||
      (swap && mine == Ability::WonderGuard)) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const ID user = protocol::ident(battle, player);
  const ID target = protocol::ident(battle, foe(player));
  if (swap) {
    protocol::activate_with(options.log, user, protocol::Activate::SkillSwap, Move::None, target);
    set_ability(battle, player, theirs, options);
    set_ability(battle, foe(player), mine, options);
  } else {
    set_ability(battle, player, theirs, options);
    protocol::ability_copy(options.log, user, theirs, mine, protocol::AbilityFrom::RolePlay,
                           target);
  }
  moveend(battle, player, m, options);
}

// Trick, as Showdown's (data/moves.ts): accuracy 100 and Protect; the
// target's Sticky Hold makes it immune (onTryImmunity), then a Substitute
// stops it. It fails if either item was knocked off (takeItem, gen 4 and
// earlier), neither holds one, or either holds Mail (its onTakeItem).
// Otherwise the items swap; a Choice Band lock outlives its band until
// the holder's next move or the end of the turn (cancel()), and the user's
// new band locks it into Trick (moveend, as Thief). Log: Activate Trick, then the target's
// Item or silent EndItem, then the user's.
template <typename O>
constexpr void trick(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& side = battle.side(player);
  Side& foe_side = battle.side(foe(player));
  Pokemon& user = side.get(1);
  Pokemon& target = foe_side.get(1);
  const Item mine = user.item;
  const Item theirs = target.item;
  const bool swaps = !foe_side.active.volatiles.substitute &&
                     mine != Item::KnockedOff && theirs != Item::KnockedOff &&
                     (mine != Item::None || theirs != Item::None) &&
                     mine != Item::Mail && theirs != Item::Mail;
  if (!try_hit(battle, player, m,
               {.immune = foe_side.active.ability == Ability::StickyHold, .matters = swaps},
               options)) {
    return moveend(battle, player, m, options);
  }
  if (!swaps) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  const ID id = protocol::ident(battle, player);
  const ID of = protocol::ident(battle, foe(player));
  protocol::activate_with(options.log, id, protocol::Activate::Trick, Move::None, of);
  // Each setItem runs the new item's onStart (White Herb) before its line.
  user.item = Item::None;
  target.item = mine;
  if (mine != Item::None) {
    white_herb(battle, foe(player), options);
    protocol::item(options.log, of, mine, protocol::ItemFrom::Trick);
  } else {
    protocol::end_item(options.log, of, theirs, protocol::EndItemFrom::Trick);
  }
  user.item = theirs;
  if (theirs != Item::None) {
    white_herb(battle, player, options);
    protocol::item(options.log, id, theirs, protocol::ItemFrom::Trick);
  } else {
    protocol::end_item(options.log, id, mine, protocol::EndItemFrom::Trick);
  }
  moveend(battle, player, m, options);
}

// Recycle, as Showdown's (gen4 drops its snatch flag): the item last used
// up in this position (Side::recycle_item, Showdown's lastItem, which gen 4
// and earlier hand to the switch-in) comes back to an empty hand; it fails
// otherwise. A knocked-off hand counts as empty, as Showdown's (Knock Off
// sets no lastItem, so only an item used up earlier comes back). Showdown's
// itemKnockedOff then still stops Trick and Thief from taking it; here the
// recycled item replaces the sentinel and they can. Log: Item [from] Recycle.
template <typename O>
constexpr void recycle(Battle& battle, Player player, MoveState& m, O& options) {
  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  if ((mon.item != Item::None && mon.item != Item::KnockedOff) ||
      side.recycle_item == Item::None) {
    protocol::failed(options.log, protocol::ident(battle, player));
    m.result |= MoveResult::FAILED;
    return moveend(battle, player, m, options);
  }
  mon.item = side.recycle_item;
  side.recycle_item = Item::None;
  protocol::item(options.log, protocol::ident(battle, player), mon.item,
                 protocol::ItemFrom::Recycle);
  moveend(battle, player, m, options);
}

// The move's script, by effect: m.move set, m.chosen and the hit markers
// filled in here.
template <typename O>
constexpr void execute(Battle& battle, Player player, MoveState& m, O& options) {
  assert(m.move != Move::None);
  if (m.chosen == Move::None) m.chosen = m.move;
  m.self = targets_user(m.move);
  m.type = move_type(battle, m.move);
  const Effect effect = get(m.move).effect;
  // Gust and Twister (bs1:1892, 1826) hit a target in the air, doubled
  // (damage.hpp, as Showdown's Fly condition).
  m.ignore_on_air = effect == Effect::Thunder || effect == Effect::SkyUppercut ||
                    effect == Effect::Gust || effect == Effect::Twister;
  m.ignore_underground = effect == Effect::Earthquake || effect == Effect::Magnitude;

  if (plain_hit(effect)) return hit(battle, player, m, options);
  if (effect == Effect::Confuse || effect == Effect::TeeterDance) {
    return confuse_move(battle, player, m, options);
  }
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
  if (effect == Effect::Reflect || effect == Effect::LightScreen ||
      effect == Effect::Mist || effect == Effect::Safeguard) {
    return screen(battle, player, m, options);
  }
  if (effect == Effect::MagicCoat || effect == Effect::Snatch) {
    return coat_or_snatch(battle, player, m, options);
  }
  if (effect == Effect::MeanLook) return mean_look(battle, player, m, options);
  if (effect == Effect::Encore) return encore(battle, player, m, options);
  if (effect == Effect::DestinyBond) return destiny_bond(battle, player, m, options);
  if (effect == Effect::Mimic) return mimic(battle, player, m, options);
  if (effect == Effect::PainSplit) return pain_split(battle, player, m, options);
  if (effect == Effect::Memento) return memento(battle, player, m, options);
  if (effect == Effect::Imprison) return imprison(battle, player, m, options);
  if (effect == Effect::Nightmare) return nightmare(battle, player, m, options);
  if (effect == Effect::Bide) return bide(battle, player, m, options);
  if (effect == Effect::Stockpile) return stockpile(battle, player, m, options);
  if (effect == Effect::Swallow) return swallow(battle, player, m, options);
  if (effect == Effect::FutureSight) return future_sight_move(battle, player, m, options);
  if (effect == Effect::Swagger || effect == Effect::Flatter) {
    return swagger(battle, player, m, options);
  }
  if (effect == Effect::Attract) return attract(battle, player, m, options);
  if (effect == Effect::Disable) return disable(battle, player, m, options);
  if (effect == Effect::Torment) return torment(battle, player, m, options);
  if (effect == Effect::Spite) return spite(battle, player, m, options);
  if (effect == Effect::Foresight) return foresight(battle, player, m, options);
  if (effect == Effect::LockOn) return lock_on(battle, player, m, options);
  if (effect == Effect::FocusEnergy || effect == Effect::Ingrain ||
      effect == Effect::Charge || effect == Effect::MudSport ||
      effect == Effect::WaterSport) {
    return self_volatile(battle, player, m, options);
  }
  if (effect == Effect::Grudge) return grudge(battle, player, m, options);
  if (const Weather w = move_weather(effect); w != Weather::None) {
    return weather_move(battle, player, m, w, options);
  }
  if (effect == Effect::Splash || effect == Effect::Teleport) {
    return splash(battle, player, m, options);
  }
  if (effect == Effect::Transform) return transform(battle, player, m, options);
  if (effect == Effect::Sketch) return sketch(battle, player, m, options);
  if (effect == Effect::MirrorMove) return mirror_move(battle, player, m, options);
  if (effect == Effect::Metronome) return metronome(battle, player, m, options);
  if (effect == Effect::Assist) return assist(battle, player, m, options);
  if (effect == Effect::NaturePower) return nature_power(battle, player, m, options);
  if (effect == Effect::Camouflage) return camouflage(battle, player, m, options);
  if (effect == Effect::Conversion || effect == Effect::Conversion2) {
    return conversion(battle, player, m, options);
  }
  if (effect == Effect::RolePlay || effect == Effect::SkillSwap) {
    return role_play(battle, player, m, options);
  }
  if (effect == Effect::Trick) return trick(battle, player, m, options);
  if (effect == Effect::Recycle) return recycle(battle, player, m, options);

  // Follow Me and Helping Hand (doubles only) only use their PP in singles
  // (the attack canceler, attackstring and ppreduce, then moveend). Log, as
  // Showdown's singles: Follow Me's move line alone; Helping Hand, with no
  // ally to target, [notarget] and -notarget.
  if (cancel(battle, player, m, options)) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    if (effect == Effect::HelpingHand) {
      protocol::no_target(options.log, protocol::ident(battle, player));
    }
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
                        O& options, bool pursuing = false) {
  const Request& r = battle.request;
  const uint8_t i = index(player);
  ActivePokemon& a = battle.side(player).active;
  const uint8_t data = r.choice[i].data();

  // Showdown's runMove counts the action before anything can stop it.
  if (a.move_actions < 2) a.move_actions++;

  MoveState m{};
  m.slot = MoveState::NO_SLOT;
  if (data == 0 && r.move[i] == Move::Struggle) {
    m.move = Move::Struggle;
    m.no_pp = true;
  } else if (forced(a)) {
    // gCurrMovePos keeps the slot chosen when the lock began *(verify)*.
    // A locked move uses no PP (Showdown's runMove skips deductPP while
    // getLockedMove names one). Known gap: Showdown's Encore on a lock that
    // Metronome called runs Metronome as the locked move instead.
    m.move = a.locked_move;
    m.locked = true;
    m.no_pp = true;
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
  m.pursuing = pursuing;
  const bool charged = a.volatiles.charged_up;
  execute(battle, player, m, options);
  if (!m.after_move) {
    after_move(battle, player, m, m.ran != Move::None ? m.ran : m.move, charged, options);
  }
  assert(!(m.drag && m.pass));
  return m.drag ? After::Drag : m.pass ? After::Pass : After::None;
}

// Whether `player`, who acts after the foe's switch, uses Pursuit on the
// switching Pokémon first: it chose Pursuit (Encore and Choice Band applied
// at selection) and isn't fainted, asleep, frozen or loafing (Truant).
// Showdown's gen3 condition (data/mods/gen3/moves.ts:486) and the decomp's
// Cmd_jumpifnopursuitswitchdmg (bsc:8723) agree on these.
constexpr bool pursues(const Battle& battle, Player player) {
  const Request& r = battle.request;
  const uint8_t i = index(player);
  const Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  return r.choice[i].type() == Choice::Move && r.move[i] == Move::Pursuit &&
         !mon.fainted() && !mon.status.asleep() && !mon.status.frozen() &&
         !(side.active.ability == Ability::Truant && side.active.truant);
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
//
// Pursuit on a switching target (BattleScript_ActionSwitch, bs1:3088; as
// Showdown's gen3 onFoeBeforeSwitchOut): before a switch action, a foe that
// pursues() uses Pursuit on the Pokémon leaving, and its own action is used
// up. It goes through the move as Showdown's useMove: no attack canceler
// (no BeforeMove handler: paralysis, confusion, infatuation and Disable
// don't stop it, and nothing ticks; the decomp's script skips it too),
// double base power (pursuing), the full moveend (a Choice Band locks it). If it KOs the switching Pokémon, the chosen switch
// still happens, in both (Showdown requeues it in gen 2-4,
// sim/battle.ts:2772). Not on Baton Pass, Roar or a faint replacement.
// Log: Activate Pursuit on the Pokémon leaving, then the move with [from]
// Pursuit.
template <typename O>
constexpr Flow actions(Battle& battle, uint8_t from, O& options) {
  Turn turn{};
  bool pursued = false;
  for (uint8_t i = from; i < Request::END_OF_ACTIONS; i++) {
    const Player p = i == 0 ? battle.request.first : foe(battle.request.first);
    const Choice c = battle.request.choice[index(p)];
    After after = After::None;
    if (pursued) continue; // the pursuer's action was its Pursuit
    if (c.type() == Choice::Switch) {
      bool survived = false;
      if (i == 0 && pursues(battle, foe(p))) {
        protocol::activate(options.log, protocol::ident(battle, p),
                           protocol::Activate::Pursuit);
        const Status status = battle.side(p).get(1).status;
        use_move(battle, foe(p), false, turn, options, true);
        // A KO: Showdown's switch action goes on, and its SwitchOut prints
        // Natural Cure's cure before the queued faint; gen3's Update after
        // the action then ends the foe's Attract before the switch line.
        const bool ko = battle.side(p).get(1).fainted();
        if (ko && battle.side(p).active.ability == Ability::NaturalCure && status.any()) {
          protocol::cure_status(options.log, protocol::ident(battle, p), status,
                                protocol::Cure::NaturalCure);
        }
        if (ko) protocol::faints(options.log);
        if (ActivePokemon& f = battle.side(foe(p)).active; ko && f.volatiles.infatuated) {
          protocol::end_effect(options.log, protocol::ident(battle, foe(p)),
                               protocol::End::Attract);
          f.volatiles.infatuated = false;
        }
        survived = !ko;
        pursued = true;
      }
      if (survived) {
        // The switch goes on, so a pursuer's faint (Rough Skin) prints after
        // the switch line and before the queued runSwitch, which doesn't run
        // if the battle ended (sim/battle.ts:2839).
        leave(battle, p, c.data(), options);
        protocol::faints(options.log);
        if (!lost(battle.side(foe(p)))) arrive(battle, p, options);
      } else {
        switch_to(battle, p, c.data(), options);
      }
    } else {
      after = use_move(battle, p, i == 0, turn, options);
      // Showdown's faintMessages after the move (sim/battle-actions.ts:347),
      // before the drag.
      protocol::faints(options.log);
      if (after == After::Drag) drag(battle, p, options);
    }
    protocol::faints(options.log);
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
// and this follows Showdown there too. Two exceptions, as Showdown: sand
// and hail hurt both Pokémon first (one field handler), and both Perish
// Song counts run first (a duration's end skips the faint check); so both
// last Pokémon fainting there is a tie.
//
// The order follows Showdown, which sorts every residual handler by
// onResidualOrder, then Speed, then onResidualSubOrder (comparePriority,
// sim/battle.ts:408), with a random order among exact ties (speedSort).
// The side timers, Wish (order 7) and the weather (8) come first, then the
// per-Pokémon handlers (10), then Perish Song (12). With unequal Speeds the
// per-Pokémon handlers are Pokémon-major, as the decomp's; on an exact
// Speed tie they interleave by subOrder (Stage), with a coin per stage
// where both Pokémon have a handler. The decomp re-rolls one order for the
// whole pass (ENDTURN_ORDER, util:1209). A coin is recorded only where the
// two orders give different states (stage_order_matters); the others,
// Wish's, the weather's and Perish Song's coins change only the log
// (log_order).
// ---------------------------------------------------------------------------

// ITEMEFFECT_NORMAL (util:3317) for p's active Pokémon, at ENDTURN_ITEMS1.
// Leftovers, the HP berries and the pinch berries act only here; on the
// cartridge and in Showdown's gen3 mod (onResidual, data/mods/gen3/items.ts)
// they don't act right after a hit.
template <typename O>
constexpr void item_end_turn(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  if (mon.fainted()) return;
  const Item held = mon.item;
  const ItemData item = item_data(held);
  switch (item.effect) {
  case HoldEffect::RestoreHp:
    // Oran, Sitrus (30 HP in Gen III) and Berry Juice: a fixed amount.
    // Showdown's gen3 eatItem or useItem, then the heal from the item.
    if (mon.hp > mon.stats.hp / 2) return;
    use_item(battle, player, options);
    heal(mon, item.param);
    protocol::heal_item(options.log, protocol::ident(battle, player), mon, held);
    return;
  case HoldEffect::ConfuseSpicy:
  case HoldEffect::ConfuseDry:
  case HoldEffect::ConfuseSweet:
  case HoldEffect::ConfuseBitter:
  case HoldEffect::ConfuseSour: {
    // Figy, Wiki, Mago, Aguav and Iapapa (TRY_EAT_CONFUSE_BERRY, util:3213):
    // at half HP or less, an eighth of the max HP, then confusion if the
    // nature dislikes the flavor. As Showdown's gen3 (data/mods/gen3/
    // items.ts onResidual, the gen6 mod's onEat): -enditem [eat], -heal,
    // then addVolatile('confusion'), which Own Tempo and an existing
    // confusion stop silently; Safeguard doesn't, the holder being the
    // source.
    if (mon.hp > mon.stats.hp / 2) return;
    use_item(battle, player, options);
    heal(mon, fraction(mon, item.param));
    protocol::heal_item(options.log, protocol::ident(battle, player), mon, held);
    const Flavor f = Flavor(uint8_t(item.effect) - uint8_t(HoldEffect::ConfuseSpicy));
    if (dislikes(mon.personality.nature(), f) && a.ability != Ability::OwnTempo &&
        a.volatiles.confusion == 0) {
      confuse(battle, player, options);
    }
    return;
  }
  case HoldEffect::Leftovers:
    if (mon.hp != mon.stats.hp) {
      heal(mon, fraction(mon, 16));
      protocol::heal_item(options.log, protocol::ident(battle, player), mon, mon.item);
    }
    return;
  case HoldEffect::AttackUp:
  case HoldEffect::DefenseUp:
  case HoldEffect::SpeedUp:
  case HoldEffect::SpAttackUp:
  case HoldEffect::SpDefenseUp: {
    // TRY_EAT_STAT_UP_BERRY (util:3230): +1 at max HP / param (4) or less.
    // The decomp keeps the berry at +6; Showdown's gen3 eats it anyway
    // (data/items.ts:3392, onEat's boost prints its 0), and this follows it.
    if (mon.hp > mon.stats.hp / item.param) return;
    const uint8_t stat = uint8_t(item.effect) - uint8_t(HoldEffect::AttackUp);
    int8_t& b = stage(a.boosts, stat);
    const int8_t n = b < 6 ? 1 : 0;
    b = int8_t(b + n);
    use_item(battle, player, options);
    protocol::boost(options.log, protocol::ident(battle, player), stat, n, false, held);
    return;
  }
  case HoldEffect::CriticalUp:
    // Lansat Berry: Focus Energy (util:3441). The decomp keeps it under
    // Focus Energy; Showdown eats it anyway (data/items.ts:3267), as here.
    // Its addVolatile prints nothing if Focus Energy is already there.
    if (mon.hp > mon.stats.hp / item.param) return;
    use_item(battle, player, options);
    if (!a.volatiles.focus_energy) {
      protocol::start(options.log, protocol::ident(battle, player), protocol::Start::FocusEnergy);
    }
    a.volatiles.focus_energy = true;
    return;
  case HoldEffect::RandomStatUp: {
    // Starf Berry: +2 to a random stat below +6 (util:3450). With none,
    // the decomp keeps it; Showdown eats it with no boost and no draw
    // (data/items.ts:6003), as here.
    if (mon.hp > mon.stats.hp / item.param) return;
    const Boosts& b = a.boosts;
    use_item(battle, player, options);
    if (b.atk >= 6 && b.def >= 6 && b.spe >= 6 && b.spa >= 6 && b.spd >= 6) return;
    const uint8_t stat = uint8_t(rolls::starf(battle, player, options) - 1);
    int8_t& s = stage(a.boosts, stat);
    const int8_t n = s + 2 > 6 ? int8_t(6 - s) : int8_t(2);
    s = int8_t(s + n);
    protocol::boost(options.log, protocol::ident(battle, player), stat, n, false, held);
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
constexpr Flow field_residuals(Battle& battle, uint32_t s1, uint32_t s2,
                               O& options) {
  // ENDTURN_REFLECT, LIGHT_SCREEN, MIST, SAFEGUARD (util:1218-1311): each
  // is one pass over both sides. Showdown's gen4 orders agree (1-4, before
  // Wish's 7 and the weather's 8). Log: SideEnd when one runs out. A side
  // condition's handler has no Speed, so when both sides' run out in one
  // turn their lines come in random order: a coin that isn't recorded
  // (CLAUDE.md, "Chance").
  using SC = protocol::SideCondition;
  const auto count_down = [&](uint8_t SideConditions::*turns, SC c) {
    bool ends[2];
    for (const Player p : {Player::P1, Player::P2}) {
      uint8_t& t = battle.side(p).conditions.*turns;
      ends[uint8_t(p)] = t != 0 && --t == 0;
    }
    Player first = Player::P1;
    if (ends[0] && ends[1] && battle.rng.chance(1, 2)) first = Player::P2;
    for (const Player p : {first, foe(first)}) {
      if (ends[uint8_t(p)]) protocol::side_end(options.log, p, c);
    }
  };
  count_down(&SideConditions::reflect, SC::Reflect);
  count_down(&SideConditions::light_screen, SC::LightScreen);
  count_down(&SideConditions::mist, SC::Mist);
  count_down(&SideConditions::safeguard, SC::Safeguard);

  // ENDTURN_WISH (util:1312): it lands on whoever is active, healing half of
  // its max HP (trywish, bsc:9311), unless it has fainted. `s1` and `s2`
  // are the residual Speeds, with Quick Claw (residual_speeds).
  const Player first = speed_order(battle, s1, s2);
  for (const Player p : {first, foe(first)}) {
    Side& side = battle.side(p);
    SideConditions& c = side.conditions;
    if (c.wish == 0 || --c.wish != 0) continue;
    const uint8_t wisher = c.wisher;
    c.wisher = 0;
    Pokemon& mon = side.get(1);
    // Showdown's Wish logs only a heal that restores HP.
    if (!mon.fainted() && mon.hp != mon.stats.hp) {
      heal(mon, mon.stats.hp / 2);
      protocol::heal_wish(options.log, protocol::ident(battle, p), mon, wisher);
    }
  }

  // ENDTURN_RAIN, SANDSTORM, SUN, HAIL (util:1331-1427): the timer, unless
  // an ability set the weather (hail has no permanent form in Gen III). The
  // turn it runs out, sand and hail deal no damage (BattleScript_
  // SandStormHailEnds), as in Showdown (fieldEvent ends the effect and moves
  // on, sim/battle.ts:519). Otherwise they hurt each Pokémon in the residual
  // order if the weather has effect (Cmd_weatherdamage, bsc:7602: not under
  // Cloud Nine or Air Lock); the timer runs either way. The decomp's loop
  // stops once a side is out (bs1:3219), so the faster of two last Pokémon
  // dies first and loses. Showdown's weather is one field handler that hurts
  // both before its faint queue and checkWin run (sim/battle.ts:2539, 2610),
  // so both last Pokémon fainting is a tie; this follows Showdown.
  // Log: Weather none when it runs out, otherwise its upkeep (printed even
  // under Cloud Nine or Air Lock), then the damage.
  Field& field = battle.field;
  if (field.weather == Weather::None) return std::nullopt;
  if (!field.permanent && --field.weather_turns == 0) {
    field.weather = Weather::None;
    protocol::weather(options.log, Weather::None);
    weather_change(battle, options);
    return std::nullopt;
  }
  const Weather weather = field.weather;
  protocol::weather(options.log, weather, protocol::WeatherFrom::Upkeep);
  if ((weather != Weather::Sandstorm && weather != Weather::Hail) ||
      effective_weather(battle) == Weather::None) {
    return std::nullopt;
  }
  // Showdown's weather hurts the Pokémon in its own Speed order (eachEvent,
  // sim/battle.ts:470), with Quick Claw and a tie coin of its own.
  const Player order = speed_order(battle, s1, s2);
  for (const Player p : {order, foe(order)}) {
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
    protocol::damage(options.log, protocol::ident(battle, p), side.get(1),
                     weather == Weather::Sandstorm ? protocol::Damage::Sandstorm
                                                   : protocol::Damage::Hail);
  }
  protocol::faints(options.log);
  return outcome(battle);
}

// DoBattlerEndTurnEffects (util:1460), split into Showdown's residual
// handlers by onResidualSubOrder (the gen4 and gen3 mods, which share
// onResidualOrder 10), so that a Speed tie can interleave them (end_of_turn).
// Ingrain 1; Rain Dish, Shed Skin and Speed Boost 3; Leftovers and the
// berries 4; Leech Seed 5; poison, toxic and burn 6; Nightmare 7; Curse 8;
// binding 9; then Uproar 11, Encore 14, Taunt 15, Yawn 19, Lock-On and
// Charge later still (Disable has no order in gen3: lock_residuals). Those last
// only change their own Pokémon, so they share a stage, as does Truant's
// toggle (27, in the ability stage). The decomp's order is the same up to
// those own-Pokémon steps.
enum class Stage : uint8_t {
  Ingrain,
  Ability,
  Item,
  LeechSeed,
  Status,
  Nightmare,
  Curse,
  Bind,
  Uproar,
  Timers,
};
inline constexpr Stage STAGES[] = {
    Stage::Ingrain,   Stage::Ability, Stage::Item, Stage::LeechSeed,
    Stage::Status,    Stage::Nightmare, Stage::Curse, Stage::Bind,
    Stage::Uproar,    Stage::Timers,
};

// The HP p's handler takes in a damage stage, or 0 if it deals none, before
// the handler runs: toxic's counter steps before its damage, and Bind's
// hidden counter ticks first, so a Pokémon freed now or whose binder has gone
// takes none. battler_stage and stage_order_matters both use it.
constexpr uint32_t stage_damage(const Battle& battle, Player player, Stage stage) {
  const Side& side = battle.side(player);
  const ActivePokemon& a = side.active;
  const Volatiles& v = a.volatiles;
  const Pokemon& mon = side.get(1);
  if (mon.fainted()) return 0;
  switch (stage) {
  case Stage::Status:
    if (mon.status.badly_poisoned()) {
      const uint32_t toxic = a.toxic != ActivePokemon::MAX_TOXIC ? a.toxic + 1u : a.toxic;
      return fraction(mon, 16) * toxic;
    }
    return mon.status.poisoned() || mon.status.burned() ? fraction(mon, 8) : 0;
  case Stage::Nightmare:
    return v.nightmare && mon.status.asleep() ? fraction(mon, 4) : 0;
  case Stage::Curse:
    return v.cursed ? fraction(mon, 4) : 0;
  case Stage::Bind:
    return v.wrapped >= 2 && !v.wrapper_gone ? fraction(mon, 16) : 0;
  default:
    assert(false && "not a damage stage");
    return 0;
  }
}

// One stage of DoBattlerEndTurnEffects for p's active Pokémon. A Pokémon
// that has fainted skips it, as each case checks its HP. Returns the end of
// the battle if a residual KO ends it.
//
// ENDTURN_UPROAR and ENDTURN_THRASH follow Showdown: they don't read
// WasUnableToUseMove (README decision 2). Uproar is its own stage, so that
// on a Speed tie it ends before either Pokémon's Yawn; the Thrash family's
// lock runs out after Perish Song (lock_residuals).
template <typename O>
constexpr Flow battler_stage(Battle& battle, Player player, Stage stage,
                             O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  Pokemon& mon = side.get(1);
  const auto alive = [&] { return !mon.fainted(); };
  const ID id = protocol::ident(battle, player);
  // Each damaging residual is its own Showdown handler, with
  // faintMessages after it (sim/battle.ts:569).
  const auto hurt = [&](uint32_t n, protocol::Damage reason) {
    passive_damage(battle, player, n, options);
    protocol::damage(options.log, id, mon, reason);
    protocol::faints(options.log);
    return outcome(battle);
  };

  switch (stage) {
  case Stage::Ingrain:
    // ENDTURN_INGRAIN (util:1476)
    if (v.rooted && alive() && mon.hp != mon.stats.hp) {
      heal(mon, fraction(mon, 16));
      protocol::heal(options.log, id, mon, protocol::Heal::Ingrain);
    }
    return std::nullopt;

  case Stage::Ability:
    // ENDTURN_ABILITIES: ABILITYEFFECT_ENDTURN (util:2587).
    if (!alive()) return std::nullopt;
    switch (a.ability) {
    case Ability::RainDish:
      if (effective_weather(battle) == Weather::Rain && mon.hp != mon.stats.hp) {
        heal(mon, fraction(mon, 16));
        protocol::heal_ability(options.log, id, mon, Ability::RainDish);
      }
      break;
    case Ability::ShedSkin:
      // Random() % 3 == 0, only with a status to cure (util:2607). It cures
      // sleep too, which is public, so the sleep count just resets.
      if (mon.status.any() &&
          rolls::coin(battle, player, &Action::shed_skin, 1, 3, options)) {
        const bool asleep = mon.status.asleep();
        protocol::activate_ability(options.log, id, Ability::ShedSkin);
        protocol::cure_status(options.log, id, mon.status);
        mon.status = Status::none();
        if (asleep) wake(battle, player, options);
      }
      break;
    case Ability::SpeedBoost:
      // Not on the turn it came in (isFirstTurn, decision 14). Battle.boost
      // prints -ability|P|Speed Boost|boost before a boost from an ability,
      // and neither at +6 (sim/battle.ts:2073).
      if (a.boosts.spe < 6 && a.first_turn != 2) {
        a.boosts.spe++;
        protocol::ability(options.log, protocol::ident(battle, player), Ability::SpeedBoost,
                          protocol::AbilityFrom::Boost);
        protocol::boost(options.log, protocol::ident(battle, player), 2, 1, false);
      }
      break;
    case Ability::Truant:
      a.truant = !a.truant;
      break;
    default:
      break;
    }
    return std::nullopt;

  case Stage::Item:
    // ENDTURN_ITEMS1 (util:1496)
    item_end_turn(battle, player, options);
    return std::nullopt;

  case Stage::LeechSeed: {
    // ENDTURN_LEECH_SEED (util:1501): max HP / 8 from the seeded Pokémon,
    // then the seeder (the foe's active Pokémon) gains what was lost, or
    // loses it to the seeded Pokémon's Liquid Ooze
    // (BattleScript_LeechSeedTurnDrain, bs1:3265). The seeded Pokémon
    // faints first (bs1:3283).
    const Player seeder = foe(player);
    Pokemon& seeder_mon = battle.side(seeder).get(1);
    if (!v.leech_seed || !alive() || seeder_mon.fainted()) return std::nullopt;
    const bool ooze = a.ability == Ability::LiquidOoze;
    const uint16_t drained = passive_damage(battle, player, fraction(mon, 8), options);
    const ID seeder_id = protocol::ident(battle, seeder);
    protocol::damage_seeded(options.log, id, mon, seeder_id);
    if (ooze) {
      // Showdown's gen4 Liquid Ooze damages with instafaint
      // (data/mods/gen4/abilities.ts:256): a seeder it KOs faints first.
      passive_damage(battle, seeder, drained, options);
      protocol::damage_ability(options.log, seeder_id, seeder_mon,
                               Ability::LiquidOoze, id);
      if (seeder_mon.fainted()) protocol::faints_last_first(options.log);
    } else if (seeder_mon.hp != seeder_mon.stats.hp) {
      heal(seeder_mon, drained);
      protocol::heal(options.log, seeder_id, seeder_mon, protocol::Heal::Silent);
    }
    protocol::faints(options.log);
    return outcome(battle);
  }

  case Stage::Status: {
    // ENDTURN_POISON, BAD_POISON, BURN (util:1517-1552): 1/8; toxic's
    // counter, at most 15, times 1/16; 1/8. One status, so one handler.
    const uint32_t n = stage_damage(battle, player, stage);
    if (n == 0) return std::nullopt;
    if (mon.status.badly_poisoned() && a.toxic != ActivePokemon::MAX_TOXIC) a.toxic++;
    return hurt(n, mon.status.burned() ? protocol::Damage::Burn : protocol::Damage::Poison);
  }

  case Stage::Nightmare:
    // ENDTURN_NIGHTMARES (util:1553): 1/4 while asleep, else it ends.
    if (!v.nightmare || !alive()) return std::nullopt;
    if (mon.status.asleep()) {
      return hurt(stage_damage(battle, player, stage), protocol::Damage::Nightmare);
    }
    v.nightmare = false;
    return std::nullopt;

  case Stage::Curse:
    // ENDTURN_CURSE (util:1574): 1/4.
    if (!v.cursed || !alive()) return std::nullopt;
    return hurt(stage_damage(battle, player, stage), protocol::Damage::Curse);

  case Stage::Bind: {
    // ENDTURN_WRAP (util:1585), as Showdown's partiallytrapped (gen4 mod:
    // 3-6 turns, order 10, subOrder 9; gen5 mod's onResidual, 1/16): the
    // hidden counter ticks first, and at 0 the Pokémon is freed; otherwise,
    // if the binder has left (wrapper_gone), it ends silently; else 1/16.
    // The decomp frees it as the binder leaves. With the binder gone the
    // binding ends here either way, so the tick changes only the log: it
    // isn't recorded (the count is kept until then, so randomize_hidden
    // still resamples the counter). Log: End Bound, or End BoundSilent, or
    // Damage Bound.
    if (v.wrapped == 0 || !alive()) return std::nullopt;
    const uint32_t n = stage_damage(battle, player, stage);
    v.wrapped--;
    const bool freed = v.wrapped == 0;
    if constexpr (CHANCE<O>) {
      if (v.wrapper_gone) {
        options.chance.durations.get(player).binding = 0;
      } else {
        rolls::tick(options.chance.actions.get(player).binding,
                    options.chance.durations.get(player).binding, freed, 3, 6,
                    1, options);
      }
    }
    if (freed || v.wrapper_gone) {
      protocol::end_effect(options.log, id,
                           freed ? protocol::End::Bound : protocol::End::BoundSilent,
                           a.wrapped_move);
      v.wrapped = 0;
      v.wrapper_gone = false;
      a.wrapped_move = Move::None;
      return std::nullopt;
    }
    // A faint clears the volatiles: the log's move is read first.
    const Move by = a.wrapped_move;
    assert(n != 0);
    passive_damage(battle, player, n, options);
    protocol::damage_bound(options.log, id, mon, by);
    protocol::faints(options.log);
    return outcome(battle);
  }

  case Stage::Uproar: {
    // Showdown's gen4 uproar condition (order 10, subOrder 11): the hidden
    // duration ticks first; at 0 it ends, else the upkeep. The decomp's
    // ENDTURN_UPROAR also ends it if the user couldn't move (util:1647).
    if (v.uproar == 0 || !alive()) return std::nullopt;
    const bool ended = --v.uproar == 0;
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).uproar,
                  options.chance.durations.get(player).attacking, ended, 2, 5, 1,
                  options);
    }
    if (ended) {
      v.multiple_turns = false;
      protocol::end_effect(options.log, id, protocol::End::Uproar);
    } else {
      protocol::start(options.log, id, protocol::Start::UproarUpkeep);
    }
    return std::nullopt;
  }

  case Stage::Timers:
    break;
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
      protocol::end_effect(options.log, id, protocol::End::Encore);
    }
  }

  // ENDTURN_LOCK_ON, TAUNT (util:1734-1748). Lock-On is on its user, as
  // Showdown; Charge ends in use_move.
  if (v.lock_on != 0 && --v.lock_on % 4 == 0) v.lock_on = 0;
  if (a.taunt != 0 && --a.taunt == 0) {
    protocol::end_effect(options.log, id, protocol::End::Taunt);
  }

  // ENDTURN_YAWN (util:1752): the Pokémon falls asleep the turn after,
  // unless it has a status, Vital Spirit or Insomnia, or an Uproar is on
  // (whose -fail Showdown prints).
  // Safeguard doesn't stop it, but Sleep Clause does (Showdown's yawn ends
  // with trySetStatus from the user, which the clause checks).
  // Log: End Yawn [silent] (its onEnd), then the sleep.
  if (v.yawn != 0 && --v.yawn == 0 && alive()) {
    protocol::end_effect(options.log, id, protocol::End::Yawn);
    if (can_status(battle, player, Secondary::Sleep) && !sleep_clause(battle, player)) {
      MoveState none{};
      inflict(battle, player, Secondary::Sleep, none, options);
    } else if (uproar(battle) && !mon.status.any() &&
               a.ability != Ability::VitalSpirit && a.ability != Ability::Insomnia) {
      uproar_stops(battle, player, options);
    }
  }

  // The decomp's ENDTURN_ITEMS2 (util:1501) cures here what the passes above
  // set; Showdown's berries wait for the Update after all the residuals
  // (end_of_turn), so one Lum Berry takes both Yawn's sleep and Thrash's
  // confusion.
  return std::nullopt;
}

// Whether the order of one stage on a Speed tie changes the state, computed
// by running it both ways on copies. Only Leech Seed (it heals or hurts the
// seeder: a capped heal, a faint between the two drains, Liquid Ooze) and
// the damage stages (both last Pokémon fainting there: the first to faint
// loses) can; the other stages touch their own Pokémon only. Two ends of
// the battle with the same Result are the same state, whatever HP the
// winner is left at (CLAUDE.md, "Chance").
constexpr bool same_state(const Battle& x, const Flow& fx, const Battle& y,
                          const Flow& fy) {
  if (fx || fy) return fx == fy;
  return x.sides[0] == y.sides[0] && x.sides[1] == y.sides[1];
}

// Whether `run(battle, options, first)` with P1 first and with P2 first, on
// copies, gives different states, or (with Chance) records different draws:
// an order that ends the battle the same way can still skip a draw the
// other records (a bind tick after the last Pokémon faints), and an
// unrecorded coin there would leave two keys covering one branch.
template <typename O, typename F>
constexpr bool orders_differ(const Battle& battle, const O& options, F run) {
  using C = std::conditional_t<detail::CHANCE<O>,
                               typename std::remove_cvref_t<O>::ChanceType,
                               NullChance>;
  Options<protocol::NullLog, C, NullCalc> ox{}, oy{};
  if constexpr (detail::CHANCE<O>) {
    ox.chance = options.chance;
    oy.chance = options.chance;
  }
  Battle x = battle;
  Battle y = battle;
  const Flow fx = run(x, ox, Player::P1);
  const Flow fy = run(y, oy, Player::P2);
  if (!same_state(x, fx, y, fy)) return true;
  if constexpr (detail::CHANCE<O>) {
    return !(ox.chance.actions == oy.chance.actions);
  }
  return false;
}

template <typename O>
constexpr bool stage_orders_differ(const Battle& battle, Stage stage,
                                   const O& options) {
  if (stage < Stage::LeechSeed || stage > Stage::Bind) return false;
  return orders_differ(battle, options, [stage](Battle& b, auto& o, Player p) {
    Flow f = battler_stage(b, p, stage, o);
    if (!f) f = battler_stage(b, foe(p), stage, o);
    return f;
  });
}

// stage_orders_differ without the copies. Within a stage each handler
// touches its own Pokémon, except through Leech Seed (the seeder's HP), the
// battle ending after a handler, and a faint's effects on the foe, of which
// only wrapper_gone is read here, by Bind. So:
// - Leech Seed: both are seeded and in (else one handler is a no-op).
// - Status, Nightmare, Curse: both hits are lethal and both Pokémon are
//   their side's last (the first to faint loses). Otherwise the faints, HP
//   or Result agree; the stages draw nothing.
// - Bind: both are bound and either hit is lethal: the other then ends
//   silently, or isn't reached and its tick goes unrecorded.
// Over-approximated where cheaper (mutual Leech Seed, Bind): a coin
// recorded where it needn't be is one extra chance node; one left
// unrecorded where it mattered would be a bug, which debug builds check
// against the copies.
template <typename O>
constexpr bool stage_order_matters(const Battle& battle, Stage stage,
                                   const O& options) {
  const auto last = [&](Player p) {
    const Side& side = battle.side(p);
    const Pokemon& active = side.get(1);
    for (const Pokemon& mon : side.pokemon) {
      if (&mon != &active && mon.species != Species::None && !mon.fainted()) return false;
    }
    return true;
  };
  const auto lethal = [&](Player p) {
    const uint32_t n = stage_damage(battle, p, stage);
    return n != 0 && n >= battle.side(p).get(1).hp;
  };
  const Volatiles& v1 = battle.sides[0].active.volatiles;
  const Volatiles& v2 = battle.sides[1].active.volatiles;
  const bool in = !battle.sides[0].get(1).fainted() && !battle.sides[1].get(1).fainted();
  bool matters = false;
  switch (stage) {
  case Stage::LeechSeed:
    matters = in && v1.leech_seed && v2.leech_seed;
    break;
  case Stage::Status:
  case Stage::Nightmare:
  case Stage::Curse:
    matters = lethal(Player::P1) && lethal(Player::P2) && last(Player::P1) &&
              last(Player::P2);
    break;
  case Stage::Bind:
    matters = in && v1.wrapped != 0 && v2.wrapped != 0 &&
              (lethal(Player::P1) || lethal(Player::P2));
    break;
  default:
    break;
  }
  assert(matters || !stage_orders_differ(battle, stage, options));
  (void)options;
  return matters;
}

// The per-Pokémon residuals with unequal Speeds: every stage of `first`'s,
// then every stage of the other's.
template <typename O>
constexpr Flow battler_pass(Battle& battle, Player first, O& options) {
  for (const Player p : {first, foe(first)}) {
    for (const Stage stage : STAGES) {
      if (Flow over = battler_stage(battle, p, stage, options)) return over;
    }
  }
  return std::nullopt;
}

// Quick Claw in the residual order, as Showdown: updateSpeed before the
// residuals (sim/battle.ts:2818) gives a holder 65535 Speed on a turn its
// roll succeeded (gen3 getActionSpeed, data/mods/gen3/scripts.ts:18), for
// every residual handler, Wish and the weather included. The decomp's
// ENDTURN_ORDER uses the turn's draw too (util:1209). `s1` and `s2` are the
// Speeds before the residuals, raised here if the roll succeeded. The roll
// is the turn's one draw; if the move order didn't need it, it is drawn
// here if it changes the comparison, and returned as pending: Wish and the
// weather use it, and keep_residual_quick_claw records it after them if the
// battler pass needs it.
struct ResidualSpeeds {
  bool pending;  // drawn here, not yet recorded
  bool proc;
  bool tie;      // a Speed tie with or without Quick Claw
};

template <typename O>
constexpr ResidualSpeeds residual_speeds(Battle& battle, uint32_t& s1, uint32_t& s2,
                                         O& options) {
  ResidualSpeeds r{false, false, false};
  const bool q1 = holds(battle.sides[0], HoldEffect::QuickClaw);
  const bool q2 = holds(battle.sides[1], HoldEffect::QuickClaw);
  if (!q1 && !q2) return r;
  const uint32_t t1 = q1 ? UINT32_MAX : s1;
  const uint32_t t2 = q2 ? UINT32_MAX : s2;
  const auto cmp = [](uint32_t x, uint32_t y) { return (x > y) - (x < y); };
  if (cmp(s1, s2) == cmp(t1, t2)) return r;
  r.tie = s1 == s2 || t1 == t2;
  const Optional drawn = battle.request.quick_claw;
  if (drawn == Optional::None) {
    r.pending = true;
    r.proc = rolls::quick_claw_draw(battle, options);
  } else {
    r.proc = drawn == Optional::True;
  }
  if (r.proc) {
    s1 = t1;
    s2 = t2;
  }
  return r;
}

// A pending draw is recorded only if the two orders give different states:
// both passes are run on copies. A Speed tie on either side would need the
// stage coins too, so it is recorded. Over-approximated: if both Pokémon
// draw in the pass (Shed Skin, Starf Berry, Yawn's sleep length), the
// copies take the draws in a different order and can differ only by that.
// An unrecorded draw stays out of the request, which a residual faint's
// request keeps past the update (replacement_order).
template <typename O>
constexpr void keep_residual_quick_claw(Battle& battle, const ResidualSpeeds& r,
                                        O& options) {
  if (!r.pending) return;
  const bool matters =
      r.tie || orders_differ(battle, options, [](Battle& b, auto& o, Player p) {
        return battler_pass(b, p, o);
      });
  if (matters) rolls::quick_claw_keep(battle, r.proc, options);
}

// Perish Song's count for p's active Pokémon, as Showdown (decided
// 2026-10-07): its gen4 condition (onResidualOrder 12,
// data/mods/gen4/moves.ts:1017) counts down in the residual pass, after the
// per-Pokémon residuals and in their order, so its faints are replaced with
// theirs. The decomp's HandleWishPerishSongOnTurnEnd (util:1825) runs it
// after the residual faints are replaced, with a request of its own. The
// count shows 3, 2, 1; at 0 the Pokémon faints. Both counts run before the
// battle can end: Showdown's fieldEvent skips faintMessages after a
// handler whose duration ends (sim/battle.ts:519), and the count's faint is
// its onEnd (data/moves.ts:13263), so two last Pokémon at 0 tie. Showdown runs Speed Boost, Truant's toggle and White Herb after it
// (order 27-29); that changes only the log. Log: Start perishN (perish0
// before the faint).
template <typename O>
constexpr void perish_count(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  Pokemon& mon = battle.side(player).get(1);
  if (!a.volatiles.perish_song || mon.fainted()) return;
  // Showdown's duration runs 4 to 0 from the move, so the count it prints
  // is the timer before this step.
  protocol::start_count(options.log, protocol::ident(battle, player), protocol::Start::Perish,
                        a.perish_song_timer);
  if (a.perish_song_timer != 0) {
    a.perish_song_timer--;
    return;
  }
  a.volatiles.perish_song = false;
  passive_damage(battle, player, mon.hp, options);
}

// A future attack landing on p's side, as Showdown's futuremove slot
// condition (data/conditions.ts:379; gen4 mod's order 11, so after every
// per-Pokémon residual and before Perish Song): its duration ticks, and at
// 0 the attack lands on whatever Pokémon is in, unless it has fainted (the
// attack is spent; Showdown only hints). The landing is trySpreadMoveHit
// with gen4's steps: a semi-invulnerable target is missed unless locked on, then the
// accuracy (Future Sight 90, Doom Desire 85) with the user's accuracy
// stage only if it is still in (stages clear at a switch or faint), its
// ability wherever it is (gen3 doesn't ignore a benched Pokémon's
// abilities), and the target's evasion and modifiers (staged_accuracy);
// Protect doesn't stop it (no protect flag), Wonder Guard lets '???'
// through. Then the stored damage, through a Substitute, with Focus Band
// (Action::future) but no Endure (survive(); gen4's Endure skips future
// moves, data/mods/gen4/moves.ts:434); Rage counts the hit, Bide
// the damage. Destiny Bond doesn't take the user (its onFaint skips future
// moves). The decomp (HandleWishPerishSongOnTurnEnd, util:1781) lands it
// after the residual faints are replaced, with a request of its own.
// Log: End move: M, then Miss (the user benched if it has left; [miss]
// on this update's last move line, as attrLastMove), or the damage.
template <typename O>
constexpr Flow future_attack(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  SideConditions& c = side.conditions;
  if (c.future_sight == 0 || --c.future_sight != 0) return std::nullopt;
  const Move move = side.future_sight_move;
  const uint32_t stored = side.future_sight_damage;
  const uint8_t position = c.future_user;
  side.future_sight_move = Move::None;
  side.future_sight_damage = 0;
  c.future_user = 0;
  Pokemon& target = side.get(1);
  if (target.fainted()) return std::nullopt;

  const Player user = foe(player);
  const Side& user_side = battle.side(user);
  const bool in = user_side.order[0] == position && !user_side.get(1).fainted();
  const ID id = protocol::ident(battle, player);
  const ID source = in ? protocol::ident(battle, user)
                       : protocol::benched(ID::init(user, position));
  protocol::end_effect(options.log, id, protocol::End::FutureMove, move);
  const ActivePokemon& t = side.active;
  // The user's Lock-On, while it's in, reaches a semi-invulnerable target
  // too (lockon's onSourceInvulnerability, as invulnerable()).
  const bool locked = in && locked_on(battle, user);
  bool lands = !t.volatiles.semi_invulnerable() || locked;
  if (lands && !locked) {
    const uint32_t calc = staged_accuracy(
        battle, player, get(move).accuracy, in ? user_side.active.boosts.acc : int8_t(0),
        user_side.pokemon[position - 1].ability, move == Move::DoomDesire);
    lands = calc >= 100 || rolls::future_hit(battle, user, calc, options);
  }
  if (!lands) {
    protocol::miss(options.log, source, id);
    return std::nullopt;
  }
  MoveState m{};
  m.move = move;
  m.chosen = move;
  m.type = Type::Mystery;
  m.slot = MoveState::NO_SLOT;
  const uint32_t d = survive(battle, user, m, stored, rolls::FUTURE, options);
  m.hp_dealt = hp_update(battle, player, d, m, options);
  // Its tryMoveHit records it in attackedBy, and both moves are on Mirror
  // Move's noMirror list: Mirror Move fails until the next attack. Unless
  // its user is out: the next turn drops attackedBy entries whose source
  // isn't active (sim/battle.ts:1707), so the attack before it counts again.
  if (in) {
    side.active.last_taken_move = Move::None;
    side.active.last_taken_from = 0;
  }
  ActivePokemon& a = side.active;
  if (a.substitute_hp == 0) a.volatiles.substitute = false;
  if (m.damaged) {
    protocol::damage(options.log, id, target);
    if (target.fainted()) faint(battle, player, options);
    rage(battle, user, m, options);
  }
  protocol::faints(options.log);
  // The hit loop's Update after its faintMessages (sim/battle-actions.ts:996)
  // ends the foe's Attract once its source has fainted.
  if (ActivePokemon& f = battle.side(user).active; target.fainted() && f.volatiles.infatuated) {
    protocol::end_effect(options.log, protocol::ident(battle, user), protocol::End::Attract);
    f.volatiles.infatuated = false;
  }
  return outcome(battle);
}

// The durations with no residual order, which Showdown runs after every
// other residual (Perish Song's included), for p's active Pokémon. Each
// lasts while its move renews it (ActivePokemon::renewed: Showdown's
// duration 2, which this pass brings to 1).
// - lockedmove (data/conditions.ts:253): at its end the lock goes, and with
//   trueDuration at 1 the user is confused (onEnd: -start confusion
//   [fatigue], a fresh 2-5; not over a confusion or Own Tempo, and
//   Safeguard doesn't stop a confusion from the user itself). Otherwise
//   onResidual: a sleeping user loses the lock silently, else trueDuration
//   counts down. The decomp's ENDTURN_THRASH (util:1673) counts the lock
//   down only on turns the move ran, and ends it if the user couldn't move.
// - rollout and iceball, furycutter: end silently.
// - Bide (gen3 mod; its duration 3 at the start): ends silently at 0, if its
//   third turn's action never came (a faint ended the actions).
template <typename O>
constexpr void lock_residuals(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  if (side.get(1).fainted()) return;
  if (v.thrash != 0) {
    // trueDuration becomes public at the second residual (Durations count
    // 2): the lock goes on only at 2, and ends in confusion only at 1. At
    // the first it is 2 or 3, and the lock goes on. Recorded once, with
    // 1/2 (True: the 2-turn length); the count is then 0, untracked.
    if constexpr (CHANCE<O>) {
      uint8_t& c = options.chance.durations.get(player).attacking;
      if (c == 2) {
        rolls::tick(options.chance.actions.get(player).thrash, c, v.thrash <= 1, 2, 3,
                    1, options);
        c = 0;
      }
    }
    if ((a.renewed & ActivePokemon::RENEWED_LOCK) && v.thrash >= 2) {
      if (side.get(1).status.asleep()) {
        v.multiple_turns = false;
        v.thrash = 0;
        if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 0;
      } else {
        v.thrash--;
        if constexpr (CHANCE<O>) {
          uint8_t& c = options.chance.durations.get(player).attacking;
          if (c != 0) c++;
        }
      }
    } else {
      const bool fatigue = v.thrash <= 1;
      v.multiple_turns = false;
      v.thrash = 0;
      if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 0;
      if (fatigue && v.confusion == 0 && a.ability != Ability::OwnTempo) {
        confuse(battle, player, options, protocol::Start::ConfusionFatigue);
      }
    }
  }
  if (a.rollout != 0 && !(a.renewed & ActivePokemon::RENEWED_LOCK)) {
    a.rollout = 0;
    v.multiple_turns = false;
  }
  if (a.fury_cutter != 0 && !(a.renewed & ActivePokemon::RENEWED_FURY_CUTTER)) {
    a.fury_cutter = 0;
  }
  a.renewed = 0;
  if (v.bide != 0 && --v.bide == 0) end_bide(battle, player, options);
}

// Disable's end, as Showdown's gen3 condition, which drops gen4's residual
// order (data/mods/gen3/moves.ts:210), so it runs with the unordered
// durations, after Perish Song (lock_residuals). The decomp's
// ENDTURN_DISABLE (util:1692) runs among the battler's other timers. It
// ends silently if the Pokémon no longer knows the move (public), else the
// hidden timer ticks. Log: End Disable.
template <typename O>
constexpr void disable_residual(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.disable_timer == 0 || battle.side(player).get(1).fainted()) return;
  const ID id = protocol::ident(battle, player);
  bool known = false;
  for (const MoveSlot& ms : a.moves) known |= ms.id == a.disabled_move;
  if (!known) {
    a.disabled_move = Move::None;
    a.disable_timer = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).disable = 0;
  } else {
    const bool ended = --a.disable_timer == 0;
    if (ended) {
      a.disabled_move = Move::None;
      protocol::end_effect(options.log, id, protocol::End::Disable);
    }
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).disable,
                  options.chance.durations.get(player).disable, ended, 3, 6,
                  1, options);
    }
  }
}

// HandleEndTurn_ContinueBattle (main:3932), then BattleTurnPassed's residuals
// (main:3956), the future attacks, Perish Song's count and
// HandleFaintedMonActions.
template <typename O> constexpr Flow end_of_turn(Battle& battle, O& options) {
  for (Side& side : battle.sides) {
    ActivePokemon& a = side.active;
    a.volatiles.flinched = false;
    // The decomp also cancels a sleeping Pokémon's multi-turn move here
    // (main:3944); Showdown keeps it until the move is aborted (see
    // inflict()), or for the Thrash family until the residuals
    // (lock_residuals).
  }
  // The order (see the section comment): ENDTURN_ORDER (util:1209) runs
  // GetWhoStrikesFirst with the chosen moves' priority; Showdown's residual
  // handlers have none. Showdown sorts every handler once, by the Speeds
  // before the residuals (with Quick Claw): a weather that ends or a Speed
  // Boost doesn't reorder them.
  uint32_t s1 = speed(battle, Player::P1);
  uint32_t s2 = speed(battle, Player::P2);
  const ResidualSpeeds rs = residual_speeds(battle, s1, s2, options);
  if (Flow over = field_residuals(battle, s1, s2, options)) return over;
  keep_residual_quick_claw(battle, rs, options);
  const Player faster = s1 > s2 ? Player::P1 : Player::P2;
  // A log-only order: on a tie, a coin per stage on these Speeds, not the
  // current ones (log_order), which a Speed Boost may have changed.
  const auto coin = [&] { return speed_order(battle, s1, s2); };
  if (s1 != s2) {
    if (Flow over = battler_pass(battle, faster, options)) return over;
  } else {
    for (const Stage stage : STAGES) {
      const Player first =
          stage_order_matters(battle, stage, options)
              ? rolls::residual_tie(battle, stage == Stage::LeechSeed, options)
              : coin();
      for (const Player p : {first, foe(first)}) {
        if (Flow over = battler_stage(battle, p, stage, options)) return over;
      }
    }
  }
  // The future attacks, by the Speed of the Pokémon each lands on: a coin
  // on a tie if both land this turn (always recorded; either could end the
  // battle).
  Player order = faster;
  if (s1 == s2) {
    const bool both = battle.sides[0].conditions.future_sight == 1 &&
                      battle.sides[1].conditions.future_sight == 1;
    order = both ? rolls::residual_tie(battle, false, options) : coin();
  }
  for (const Player p : {order, foe(order)}) {
    if (Flow over = future_attack(battle, p, options)) return over;
  }
  Player last = coin();
  for (const Player p : {last, foe(last)}) perish_count(battle, p, options);
  last = coin();
  for (const Player p : {last, foe(last)}) {
    lock_residuals(battle, p, options);
    disable_residual(battle, p, options);
  }
  protocol::faints(options.log);
  // White Herb's residual (order 29), the last.
  last = coin();
  for (const Player p : {last, foe(last)}) white_herb(battle, p, options);
  // Showdown's stall expires with the residuals, before a residual faint's
  // replacement: a turn without a successful Protect, Detect or Endure ends
  // the chain (protect()).
  for (Side& side : battle.sides) {
    ActivePokemon& a = side.active;
    if (a.stall != 0 && --a.stall == 0) a.protect_uses = 0;
  }
  if (Flow over = outcome(battle)) return over;
  // Showdown's gen3 Update after the residual action (sim/battle.ts:2915),
  // unless a replacement is due: a berry takes what came after the item
  // stage (Thrash's confusion, Yawn's sleep).
  if (!battle.sides[0].get(1).fainted() && !battle.sides[1].get(1).fainted()) {
    const Player first = log_order(battle);
    for (const Player p : {first, foe(first)}) item_cure(battle, p, options);
  }
  return faints(battle, Phase::FaintAfterResiduals);
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

// The end of update's messages: the Faints still queued, then the winner
// or a tie, or the next turn's number, then the terminating None.
// Result::start() is returned only by start() and next_turn().
template <typename O>
constexpr Result finish(const Battle& battle, Result result, O& options) {
  protocol::faints(options.log);
  switch (result.type()) {
  case Result::Win: protocol::win(options.log, Player::P1); break;
  case Result::Lose: protocol::win(options.log, Player::P2); break;
  case Result::Tie: protocol::tie(options.log); break;
  default:
    if (result == Result::start()) protocol::turn(options.log, battle.turn);
    break;
  }
  protocol::end(options.log);
  return checked(battle, result);
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
      if (battle.turn == 0) return finish(battle, start(battle, options), options);
      select(battle, Player::P1, c1);
      select(battle, Player::P2, c2);
      order(battle, options);
      // CheckFocusPunch_ClearVarsBeforeTurnStarts (main:4906): Focus Punch's
      // setup is only a message (bs1:3709), which the decomp skips for a
      // sleeping user (main:4915) and Showdown doesn't (its focuspunch
      // condition's onStart: SingleTurn Focus Punch, from beforeTurnMove
      // actions, sorted by Speed with a tie shuffle of their own).
      // TryClearRageStatuses follows Showdown instead (cancel()).
      const Player punch = log_order(battle);
      for (const Player p : {punch, foe(punch)}) {
        if (battle.request.choice[index(p)].type() == Choice::Move &&
            battle.request.move[index(p)] == Move::FocusPunch) {
          protocol::single_turn(options.log, protocol::ident(battle, p),
                                protocol::SingleTurn::FocusPunch);
        }
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
      if ((f = faints(battle, Phase::FaintAfterAction))) return finish(battle, *f, options);
    }
    if ((f = actions(battle, from, options))) return finish(battle, *f, options);
    [[fallthrough]];
  }
  case Phase::FaintAfterAction:
    if (phase == Phase::FaintAfterAction) {
      replace(battle, c1, c2, options);
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = faints(battle, phase))) return finish(battle, *f, options);
    }
    if ((f = end_of_turn(battle, options))) return finish(battle, *f, options);
    [[fallthrough]];
  case Phase::FaintAfterResiduals:
    if (phase == Phase::FaintAfterResiduals) {
      replace(battle, c1, c2, options);
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = faints(battle, phase))) return finish(battle, *f, options);
    }
    return finish(battle, next_turn(battle), options);
  }
  assert(false && "invalid phase");
  return Result::error();
}

} // namespace hoenn
