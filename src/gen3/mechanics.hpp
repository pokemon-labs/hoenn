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

// gRandomTurnNumber < 0xFFFF * param / 100 (main:4653, 4687).
inline constexpr uint32_t QUICK_CLAW = 0xFFFF * 20 / 100;
static_assert(get(Item::QuickClaw).param == 20 && QUICK_CLAW == 13107);

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
                        : battle.rng.chance(QUICK_CLAW, 65536);

  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.turn.quick_claw, optional(proc));
    observe(options, proc ? QUICK_CLAW : 65536 - QUICK_CLAW, 65536);
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

// The Pokémon in SLOT 1 becomes active: Cmd_switchindataupdate (bsc:4621)
// and SwitchInClearSetData (main:3152), as README decision 4. Then
// switchineffects (bsc:5180).
constexpr void switch_in(Battle& battle, Player player) {
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

  // TODO(plan step 3): switchineffects: Spikes (bsc:5239), then the switch-in
  // abilities and items.
  if (a.ability == Ability::Truant) a.truant = true; // bsc:5258
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

  // TODO(plan step 3): Pursuit on the switching target (bs1:3088), and
  // switchoutabilities (Natural Cure).
  ActivePokemon& f = battle.side(foe(player)).active;
  f.volatiles.infatuated = false;
  f.volatiles.wrapped = 0;
  f.wrapped_move = Move::None;
  f.volatiles.escape_prevention = false;
  f.volatiles.lock_on = 0;

  std::swap(side.order[0], side.order[slot - 1]);
  if constexpr (CHANCE<O>) options.chance.durations.switched(player, slot);
  switch_in(battle, player);
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
// is WON | LOST). Who loses when both last Pokémon faint in one action is
// still to verify against Showdown (CLAUDE.md, "Tricky mechanics").
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
// fainted.
//
// TODO(plan step 3): fainting itself (FaintClearSetData, main:3264, and the
// status clear) belongs where HP reaches 0. switch_to clears the foe's state
// only at the replacement, so a residual pass between the two would still
// see it.
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
template <typename O> constexpr Result start(Battle& battle, O&) {
  switch_in(battle, Player::P1);
  switch_in(battle, Player::P2);
  // TODO: TryDoEventsBeforeFirstTurn (main:3841), untraced: switch-in
  // weather, abilities and items in speed order (CLAUDE.md, "Open").
  turn_cleanup(battle);
  battle.turn = 1;
  return Result::start();
}

// HandleAction_UseMove (util:78) and the move's script.
// TODO(plan step 3): the hit backbone. Until then a move does nothing.
template <typename O> constexpr void use_move(Battle&, Player, O&) {}

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
      use_move(battle, p, options);
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
    if (phase == Phase::FaintAfterAction) replace(battle, c1, c2, options);
    if ((f = end_of_turn(battle, options))) return checked(battle, *f);
    [[fallthrough]];
  case Phase::FaintAfterResiduals:
    if (phase == Phase::FaintAfterResiduals) replace(battle, c1, c2, options);
    if ((f = future_sight(battle, options))) return checked(battle, *f);
    [[fallthrough]];
  case Phase::FaintAfterFutureSight:
    if (phase == Phase::FaintAfterFutureSight) replace(battle, c1, c2, options);
    if ((f = perish_song(battle, options))) return checked(battle, *f);
    [[fallthrough]];
  case Phase::FaintAfterPerish:
    if (phase == Phase::FaintAfterPerish) replace(battle, c1, c2, options);
    return checked(battle, next_turn(battle));
  }
  assert(false && "invalid phase");
  return Result::error();
}

} // namespace pkmn::gen3
