#pragma once

#include <cassert>
#include <bit>
#include <cstdint>
#include <iterator>
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

// ---------------------------------------------------------------------------
// The turn's state
// ---------------------------------------------------------------------------

// MOVE_EFFECT_* bytes this code applies, in the decomp's order.
enum class Secondary : uint8_t {
  None, Sleep, Poison, Burn, Freeze, Paralysis, Toxic, Confusion, Flinch,
  TriAttack,
  // MOVE_EFFECT_*_MINUS_1 on the target, in Boosts order, then ATK_PLUS_1 and
  // DEF_PLUS_1 on the user.
  AtkDown, DefDown, SpeDown, SpaDown, SpdDown, AccDown, AtkUp, DefUp,
  // MOVE_EFFECT_ALL_STATS_UP on the user (Ancient Power, Silver Wind).
  AllUp,
};

// The per-turn ProtectStruct (ENGINE.md decision 2): a local of actions(),
// reached through MoveState::turn, so it starts empty each turn and after Baton
// Pass. Indexed by player.
struct Turn {
  bool protect[2]; // ProtectStruct.protected: Protect or Detect worked
  bool endure[2];  // ProtectStruct.endured: Endure worked
  // A damaging move (or Present's heal) hit the Pokémon itself: Showdown's
  // lostFocus, which Focus Punch reads (the decomp's also needs HP lost).
  bool hit[2];
  // ProtectStruct.physicalDmg and specialDmg: the HP the foe's last physical or
  // special move took from the Pokémon itself this turn (Counter, Mirror Coat).
  uint16_t physical[2];
  uint16_t special[2];
  // Showdown's magiccoat and snatch volatiles: used this turn and not yet
  // spent.
  bool magic_coat[2];
  bool snatch[2];
};

// One move's execution, on the stack (as libpkmn gen2's State): the
// decomp's per-action globals and the gHitMarker bits this code reads,
// cleared between actions (HandleAction_ActionFinished, util:661).
struct MoveState {
  // The move, and how the action came to use it.
  Move move;     // gCurrentMove
  Move chosen;   // gChosenMove: the move, or the caller (Sleep Talk, Snatch...)
  Move from;     // the move that called this one, if `called`
  Type type;     // as used: Weather Ball's, Hidden Power's (move_type)
  uint8_t slot;  // gCurrMovePos, 0-3, or NO_SLOT
  bool self;     // aimed at the user (MOVE_TARGET_USER)
  bool first;    // the target acts after this, so a flinch can stop it
  bool no_pp;    // HITMARKER_NO_PPDEDUCT: Struggle, a locked or called move
  bool locked;   // the forced turn of a locked move ([from] lockedmove)
  bool called;   // run by another move (call_move): no canceler, no PP
  bool pursuing; // Pursuit on a switching target (pursues): power x2, sure hit
  bool release;  // Bide's release (cancel): no move line, no PP

  // What has happened so far.
  uint8_t result;    // gMoveResultFlags (MoveResult)
  bool obeys;        // HITMARKER_OBEYS: past the attack canceler
  bool attackstring; // HITMARKER_ATTACKSTRING_PRINTED
  bool unable;       // HITMARKER_UNABLE_TO_USE_MOVE
  bool attacked;     // past the accuracy check and TryHit (Showdown's gotAttacked)
  bool damaged;      // TARGET_TURN_DAMAGED: the target itself lost HP
  uint16_t hp_dealt; // gHpDealt: what the target or its Substitute lost
  uint16_t total;    // what the target itself lost over every hit (Shell Bell)

  // Left for moveend, or for after it.
  Secondary synchronize; // the status that arms Synchronize (bsc:2503), or None
  Player synchronized;   // who took it
  Item stolen;           // Thief's item, handed over at MOVEEND_CHANGED_ITEMS
  bool grudge;           // the target fainted under Grudge (faint_target)
  uint16_t bonded_hp;    // the user's HP before Destiny Bond took it, for Shell Bell
  bool faints;           // Explosion, Memento: the user faints after moveend
  bool delegated;        // it called a move (call_move), whose moveend ends the action
  bool drag;             // Roar worked: the drag follows the action
  bool pass;             // Baton Pass worked: the recipient is asked for after it

  Turn* turn; // this turn's ProtectStruct; null for a future attack's landing
};

// How a move aimed at the foe meets it, for try_hit.
struct Aim {
  // A natural immunity (the type chart with Levitate, or Leech Seed's Grass):
  // -immune whether the roll hits or not.
  bool immune = false;
  // Showdown's accuracy `true`: no roll (the decomp's
  // NO_ACC_CALC_CHECK_LOCK_ON).
  bool sure = false;
  // Whether a hit that gets past TryHit changes the state; if not, the roll is
  // log-only.
  bool matters = true;
  // The target's Wonder Guard stops the move (its TryHit, after the others').
  bool wonder_guard = false;
};

enum class Leave : uint8_t { Switch, BatonPass, Drag };

// DoBattlerEndTurnEffects (util:1460), split by Showdown's onResidualSubOrder
// (order 10), so that a Speed tie can interleave them: Ingrain 1; Rain Dish,
// Shed Skin, Speed Boost 3; Leftovers and the berries 4; Leech Seed 5; poison
// and burn 6; Nightmare 7; Curse 8; binding 9; Uproar 11; then Encore, Taunt,
// Yawn, Lock-On and Charge, which touch only their own Pokémon and share a
// stage (Truant's toggle shares the ability stage). Disable has no gen3 order
// (lock_residuals).
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

// The flow the effects call back into, defined after them.
template <typename O> constexpr void faint(Battle& battle, Player player, O& options);
template <typename O>
constexpr void faint_target(Battle& battle, Player player, Move used, bool& grudge,
                            uint16_t& bonded_hp, O& options);
template <typename O> constexpr void arrive(Battle& battle, Player player, O& options);
template <typename O>
constexpr void leave(Battle& battle, Player player, uint8_t slot, O& options,
                     Leave how = Leave::Switch);
template <typename O>
constexpr bool try_hit(Battle& battle, Player player, MoveState& m, Aim aim, O& options);
template <typename O>
constexpr void strike(Battle& battle, Player player, MoveState& m, Hit h, uint32_t fixed,
                      uint8_t i, O& options);
template <typename O>
constexpr void execute(Battle& battle, Player player, MoveState& m, O& options);

} // namespace detail

} // namespace hoenn

#include "effects.hpp"

namespace hoenn {

namespace detail {

// ---------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------

// What GetWhoStrikesFirst reads, taken at one moment so a lazy draw later sees
// the same inputs.
struct Speeds {
  uint32_t speed[2];
  int8_t priority[2];
  bool quick_claw[2];
};

// The players' chosen moves (Move::None for a switch). Quick Claw counts for
// every action, switches too (gen3 getActionSpeed,
// data/mods/gen3/scripts.ts:18), but not at the battle start.
constexpr Speeds speeds(const Battle& battle, Move m1, Move m2,
                        bool quick_claw) {
  Speeds s{};
  const Move moves[2] = {m1, m2};
  for (const Player p : {Player::P1, Player::P2}) {
    const uint8_t i = index(p);
    s.speed[i] = speed(battle, p);
    s.priority[i] = moves[i] == Move::None ? int8_t(0) : get(moves[i]).priority;
    s.quick_claw[i] =
        quick_claw && holds(battle.side(p), HoldEffect::QuickClaw);
  }
  return s;
}

// GetWhoStrikesFirst (main:4595): priority, then Speed with Quick Claw as the
// top Speed, then a tie draw. Quick Claw is drawn only if it would change the
// comparison; two that proc tie.
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

// SetActionsAndBattlersTurnOrder (main:4756): a switch before a move, two moves
// by GetWhoStrikesFirst, two switches by Speed with Quick Claw, as Showdown
// (the decomp: battler-ID order).
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

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

// The move slots p can select, as a mask: CheckMoveLimitations (util:1082).
// Choice Band ignores MOVE_UNAVAILABLE for now (CLAUDE.md, "Open").
constexpr uint8_t usable_moves(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  const ActivePokemon& a = side.active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  const bool choice_band = holds(side, HoldEffect::ChoiceBand);
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const Move m = a.moves[i].id;
    if (m == Move::None || a.moves[i].pp == 0) continue;
    if (m == a.volatiles.disabled_move) continue;
    if (a.volatiles.torment && m == a.volatiles.last_move) continue;
    if (a.volatiles.taunt != 0 && get(m).bp == 0) continue;
    if (imprisoned(f, m)) continue;
    if (a.volatiles.encore_timer != 0 && a.volatiles.encored_move != m) continue;
    if (choice_band && a.volatiles.choice_move != Move::None && a.volatiles.choice_move != m) {
      continue;
    }
    mask |= uint8_t(1 << i);
  }
  return mask;
}

// Whether p's active Pokémon can't switch out (main:4232-4247). Shadow Tag
// traps Shadow Tag, as on the cartridge (unverified against Showdown).
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

// Records p's choice as gChosenMoveByBattler (main:4129, util:78): a forced
// move is the locked move, else Move(0) is Struggle.
constexpr void select(Battle& battle, Player player, Choice choice) {
  Request& r = battle.request;
  const ActivePokemon& a = battle.side(player).active;
  const uint8_t i = index(player);
  r.choice[i] = choice;
  if (choice.type() == Choice::Switch) {
    r.move[i] = Move::None;
  } else if (forced(a)) {
    assert(choice.data() == 0);
    r.move[i] = a.volatiles.locked_move;
  } else if (choice.data() == 0) {
    r.move[i] = Move::Struggle;
  } else {
    assert(a.volatiles.encored_move == Move::None ||
           choice.data() - 1 == a.volatiles.encored_slot);
    r.move[i] = a.moves[choice.data() - 1].id;
  }
}

// ---------------------------------------------------------------------------
// Switching
// ---------------------------------------------------------------------------

// TurnValuesCleanUp(FALSE) (main:4866), minus the per-turn struct (ENGINE.md
// decision 2); it runs before turn 1 too (main:3909).
constexpr void turn_cleanup(Battle& battle) {
  for (const Player p : {Player::P1, Player::P2}) {
    Side& side = battle.side(p);
    ActivePokemon& a = side.active;
    if (a.volatiles.first_turn != 0) a.volatiles.first_turn--;
    // A charge whose second turn never came ends with the turn, as Showdown's
    // twoturnmove (duration 2); the decomp keeps the lock.
    Volatiles& v = a.volatiles;
    if ((v.kind == Volatiles::Kind::Recharge || v.charging()) && --v.lock == 0) end_kind(v);
    if (a.volatiles.substitute_hp == 0) a.volatiles.substitute = false;
    // Showdown's endTurn drops an attackedBy entry whose source isn't active
    // (sim/battle.ts:1707).
    if (a.volatiles.last_taken_from != battle.side(foe(p)).order[0]) {
      a.volatiles.last_taken_move = Move::None;
      a.volatiles.last_taken_from = 0;
    }
    // Showdown's choicelock onDisableMove.
    if (slot_of(a, a.volatiles.choice_move) == NO_SLOT || !holds(side, HoldEffect::ChoiceBand)) {
      a.volatiles.choice_move = Move::None;
    }
  }
}

// p's active Pokémon has just reached 0 HP: cleareffectsonfaint (bsc), then
// FaintClearSetData (main:3264). It stays in the slot until its replacement
// comes in, keeping its species, stats, moves and ability (and, as the decomp,
// gLockedMoves); its other state goes, and so does what the foe had from it.
// gBideDmg goes too, unlike the decomp: only a Biding active reads it, and
// this ends Bide, so it was dead, and a lethal hit's size leaves no trace.
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
  a.stats = kept.stats;
  for (uint8_t i = 0; i < 4; i++) a.moves[i] = kept.moves[i];
  a.species = kept.species;
  a.volatiles.locked_move = kept.volatiles.locked_move;
  a.types[0] = get(a.species).types[0];
  a.types[1] = get(a.species).types[1];
  a.ability = kept.ability;
  a.volatiles.first_turn = 2;
  // Showdown's faintMessages runs the ability's End after the faint line.
  if (a.ability == Ability::CloudNine || a.ability == Ability::AirLock) {
    forecast(battle, foe(player), effective_weather(battle), options,
             protocol::ident(battle, player));
  }

  ActivePokemon& f = battle.side(foe(player)).active;
  f.volatiles.escape_prevention = false;
  // The foe's Attract ends when the replacement comes in (run_switch), its
  // binding at its residual (Stage::Bind), as Showdown's.
  if (f.volatiles.wrapped != 0) f.volatiles.wrapper_gone = true;

  if constexpr (CHANCE<O>) {
    Durations::Side& d = options.chance.durations.get(player);
    d.sleep[0] = d.confusion = d.disable = d.encore = d.binding = d.attacking = 0;
  }
}

// tryfaintmon BS_TARGET after p's move hit the foe (bsc:2965): a foe at 0 HP
// faints, and its Destiny Bond takes p unless p has fainted already (bs1:3379).
// Its Grudge, as Showdown's onFaint, empties the user's last move (`used`),
// unless the user fainted first (Explosion); the PP goes at the end of moveend.
// `bonded_hp` keeps the user's HP for Shell Bell's log.
template <typename O>
constexpr void faint_target(Battle& battle, Player player, Move used, bool& grudge,
                            uint16_t& bonded_hp, O& options) {
  const Player defender = foe(player);
  if (!battle.side(defender).get(1).fainted()) return;
  const Volatiles::Kind kind = battle.side(defender).active.volatiles.kind;
  const bool bond = kind == Volatiles::Kind::DestinyBond;
  if (bond) protocol::faint(options.log, protocol::ident(battle, defender), true);
  if (kind == Volatiles::Kind::Grudge &&
      get(used).effect != Effect::Explosion) {
    const ActivePokemon& a = battle.side(player).active;
    if (const uint8_t s = slot_of(a, used); s != NO_SLOT && a.moves[s].pp != 0) {
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

// ---------------------------------------------------------------------------
// Switch-ins, as Showdown's gen3 switchIn and runSwitch
// (sim/battle-actions.ts:197, 228): the Pokémon lands with its switch (Spikes,
// then after turn 0 a weather ability), and its runSwitch, one SwitchIn event
// for every Pokémon that landed together, runs the other switch-in abilities
// by priority, then the Update. The decomp runs Intimidate and Trace after the
// action (util:1945).
// ---------------------------------------------------------------------------

// The Pokémon in SLOT 1 becomes active: Cmd_switchindataupdate (bsc:4621) and
// SwitchInClearSetData (main:3152), as ENGINE.md decision 4.
constexpr void enter(Battle& battle, Player player) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  assert(mon.species != Species::None && !mon.fainted());
  // A knocked-off Pokémon loses an item it gained since (bsc:4641; Showdown's
  // gen3 switchIn, sim/battle-actions.ts:163).
  if ((side.knocked_off & (1 << (side.order[0] - 1))) && mon.item != Item::None) {
    mon.item = Item::KnockedOff;
  }

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
  a.volatiles.first_turn = 2; // main:3220, decision 14
}

// switchineffects (bsc:5180) as Showdown's switchIn after the switch line:
// Spikes, then after turn 0 the weather ability (AfterEntryHazard,
// sim/battle-actions.ts:204). Returns whether the Pokémon stands: a Spikes
// faint skips the rest and its runSwitch.
template <typename O>
constexpr bool land(Battle& battle, Player player, O& options) {
  if (spikes(battle, player, options)) {
    protocol::faints(options.log);
    return false;
  }
  if (battle.turn != 0) weather_ability(battle, player, options);
  return true;
}

// runSwitch's SwitchIn event (sim/battle-actions.ts:237) for `first` and, with
// `both`, its foe: each priority's handlers in that order, then at priority 0
// Truant and every White Herb (onAnySwitchIn). Then the Update after the
// action (sim/battle.ts:2971; none after the switch itself): the foe's Attract
// ends if its source is gone (data/moves.ts:736), an immunity ability cures a
// status from before it or a traced one, and the berries act.
template <typename O>
constexpr void run_switch(Battle& battle, Player first, bool both, O& options) {
  const Player order[2] = {first, foe(first)};
  const uint8_t n = both ? 2 : 1;
  // The event's handlers are found before any runs: a traced Truant has none.
  bool truant[2] = {};
  for (uint8_t i = 0; i < n; i++) {
    truant[i] = battle.side(order[i]).active.ability == Ability::Truant;
  }
  for (const uint8_t priority : {3, 2, 1}) {
    for (uint8_t i = 0; i < n; i++) {
      if (!battle.side(order[i]).get(1).fainted()) {
        ability_switch_in(battle, order[i], priority, options);
      }
    }
  }
  for (uint8_t i = 0; i < n; i++) {
    // bsc:5258; at the battle start Truant acts on turn 1 (Showdown agrees).
    if (truant[i]) battle.side(order[i]).active.volatiles.truant = battle.turn != 0;
  }
  white_herbs(battle, options);
  for (uint8_t i = 0; i < n; i++) {
    Side& side = battle.side(order[i]);
    Volatiles& v = battle.side(foe(order[i])).active.volatiles;
    // Unless the source came back first (a switch-in that faints to Spikes
    // runs no Update).
    if (v.infatuated && side.attract_source != side.order[0]) {
      v.infatuated = false;
      protocol::end_effect(options.log, protocol::ident(battle, foe(order[i])),
                           protocol::End::Attract);
    }
    side.attract_source = 0;
  }
  ability_immunity(battle, options);
  const Player f = log_order(battle);
  for (const Player p : {f, foe(f)}) {
    if (!battle.side(p).get(1).fainted()) item_cure(battle, p, options);
  }
}

// One Pokémon's switch-in: it lands, then its runSwitch.
template <typename O>
constexpr void arrive(Battle& battle, Player player, O& options) {
  if (land(battle, player, options)) run_switch(battle, player, false, options);
}

// Whether two weather abilities' order can change the result.
constexpr bool weathers_differ(Ability a, Ability b) {
  const Weather w1 = ability_weather(a);
  const Weather w2 = ability_weather(b);
  return w1 != Weather::None && w2 != Weather::None && w1 != w2;
}

// p's active Pokémon leaves for SLOT `slot`, which enters: a switch or a faint
// replacement, Baton Pass or Roar's drag (`how`). The foe loses what this
// Pokémon was the source of (main:3158-3199; ENGINE.md decision 9), except, for
// Baton Pass, its escape prevention and Lock-On (main:3158).
template <typename O>
constexpr void leave(Battle& battle, Player player, uint8_t slot, O& options,
                     Leave how) {
  Side& side = battle.side(player);
  const bool baton = how == Leave::BatonPass;
  assert(slot >= 2 && slot <= 6);
  assert(side.get(slot).species != Species::None && !side.get(slot).fainted());

  // switchoutabilities (bsc:9590): Natural Cure, as Showdown's onSwitchOut.
  Pokemon& out = side.get(1);
  if (side.active.ability == Ability::NaturalCure && out.status.any()) {
    protocol::cure_status(options.log, protocol::ident(battle, player), out.status,
                          protocol::Cure::NaturalCure);
    if (out.status.asleep()) wake(battle, player, options);
    out.status = Status::none();
    if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 0;
  }
  // Showdown's switchIn runs the ability's End: Flash Fire's boost ends.
  if (side.active.volatiles.flash_fire) {
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::FlashFire);
  }
  if (side.active.ability == Ability::CloudNine || side.active.ability == Ability::AirLock) {
    weather_change(battle, options, player);
  }
  // Showdown's gen3 skippedTime (ENGINE.md, field map): the Sleep Talk and
  // Snore turns go back on the sleep counter.
  if (const uint8_t k = side.active.volatiles.status_counter; k != 0 && out.status.asleep()) {
    assert(out.status.sleep() + k <= Status::MAX_SLEEP);
    out.status.bits = uint8_t(out.status.bits + k);
    if constexpr (CHANCE<O>) {
      uint8_t& count = options.chance.durations.get(player).sleep[0];
      assert(count == 0 || count > k);
      if (count != 0) count = uint8_t(count - k);
    }
  }
  ActivePokemon& f = battle.side(foe(player)).active;
  if (f.volatiles.infatuated && side.attract_source == 0) side.attract_source = side.order[0];
  // The binder leaves: Showdown's partiallytrapped stays until its residual, no
  // longer trapping (Stage::Bind). Baton Pass too: the recipient isn't the
  // source.
  if (f.volatiles.wrapped != 0) f.volatiles.wrapper_gone = true;
  if (!baton) f.volatiles.escape_prevention = false;

  std::swap(side.order[0], side.order[slot - 1]);
  if constexpr (CHANCE<O>) options.chance.durations.switched(player, slot, baton);
  enter(battle, player);
  protocol::switched(options.log,
                     how == Leave::Drag ? protocol::ArgType::Drag : protocol::ArgType::Switch,
                     protocol::ident(battle, player), side.get(1),
                     baton ? protocol::SwitchFrom::BatonPass
                           : protocol::SwitchFrom::None);
}

// ---------------------------------------------------------------------------
// Faints and the end of the battle
// ---------------------------------------------------------------------------

// HandleFaintedMonActions (util:1880) at `phase`: the end of the battle, or a
// replacement request for each side whose active Pokémon fainted.
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

// The order two replacements enter in: Showdown's instaswitch actions
// (sim/side.ts:1012), by the fainted Pokémon's stored Speed, since an inactive
// Pokémon has no handlers. Quick Claw counts only if the turn already drew it,
// so no draw made here is reused unrecorded. Each lands with its switch, so a
// tie is recorded where the order can change the result (`matters`: Spikes,
// or two weathers), and is an unrecorded coin otherwise.
template <typename O>
constexpr Player replacement_order(Battle& battle, bool matters, O& options) {
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
  if (matters) return rolls::speed_tie(battle, options);
  return battle.rng.chance(1, 2) ? Player::P1 : Player::P2;
}

// The replacements a faint request asked for, with one still waiting from
// the request before (Request::next). Each enters and lands in turn, and the
// battle can end between them; then one runSwitch for those standing (the
// runSwitch actions follow the instaswitches, sim/battle-queue.ts:177). A first
// replacement that faints to Spikes is asked for again at once, with the
// other's still queued (data/mods/gen3/scripts.ts:16), as is a second while the
// first's runSwitch waits.
template <typename O>
constexpr Flow replace(Battle& battle, Choice c1, Choice c2, O& options) {
  Request& r = battle.request;
  Choice choices[2] = {c1, c2};
  bool landed[2] = {false, false};
  uint8_t pending = 0; // 1 + the player whose switch waits
  if (r.next != 0) {
    const uint8_t i = r.next >= Request::LANDED ? r.next - Request::LANDED : r.next - Request::PENDING;
    assert(i < 2 && choices[i].type() == Choice::Pass);
    if (r.next >= Request::LANDED) {
      landed[i] = true;
    } else {
      choices[i] = r.choice[i];
      pending = uint8_t(i + 1);
    }
  }
  const Phase phase = r.phase;
  r.phase = Phase::None;
  r.next = 0;
  const bool s1 = choices[0].type() == Choice::Switch;
  const bool s2 = choices[1].type() == Choice::Switch;
  Player first = s1 ? Player::P1 : Player::P2;
  if (pending) {
    // The new choice goes before the queued one (commitChoices,
    // sim/battle.ts:3076).
    first = foe(Player(pending - 1));
  } else if (s1 && s2) {
    const bool matters =
        battle.sides[0].conditions.spikes != 0 || battle.sides[1].conditions.spikes != 0 ||
        weathers_differ(battle.sides[0].get(choices[0].data()).ability,
                        battle.sides[1].get(choices[1].data()).ability);
    first = replacement_order(battle, matters, options);
  }
  for (const Player p : {first, foe(first)}) {
    const Choice c = choices[index(p)];
    assert((c.type() == Choice::Switch) == battle.side(p).get(1).fainted());
    if (c.type() != Choice::Switch) continue;
    leave(battle, p, c.data(), options);
    if (land(battle, p, options)) {
      landed[index(p)] = true;
      continue;
    }
    if (Flow over = outcome(battle)) return over;
    const uint8_t q = index(foe(p));
    const bool waits = p == first && choices[q].type() == Choice::Switch;
    if (!waits && !landed[q]) break;
    r.phase = phase;
    r.next = uint8_t((waits ? Request::PENDING : Request::LANDED) + q);
    if (waits) r.choice[q] = choices[q];
    return Result::init(Result::None, p == Player::P1 ? Choice::Switch : Choice::Pass,
                        p == Player::P2 ? Choice::Switch : Choice::Pass);
  }
  if (landed[0] && landed[1]) {
    run_switch(battle, log_order(battle), true, options);
  } else if (landed[0] || landed[1]) {
    run_switch(battle, landed[0] ? Player::P1 : Player::P2, false, options);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// The battle's start
// ---------------------------------------------------------------------------

// Turn 0, as libpkmn's start: both leads come in, then one runSwitch for both
// in Speed order without Quick Claw, as Showdown; the order can change the
// result only with two weathers. The decomp's TryDoEventsBeforeFirstTurn
// (main:3841) runs each kind of switch-in effect for both in turn.
template <typename O> constexpr Result start(Battle& battle, O& options) {
  for (const Player p : {Player::P1, Player::P2}) {
    enter(battle, p);
    protocol::switched(options.log, protocol::ArgType::Switch,
                       protocol::ident(battle, p), battle.side(p).get(1));
  }
  const Player first =
      weathers_differ(battle.sides[0].active.ability, battle.sides[1].active.ability)
          ? detail::first(battle, speeds(battle, Move::None, Move::None, false), options)
          : log_order(battle);
  run_switch(battle, first, true, options);
  turn_cleanup(battle);
  battle.turn = 1;
  return Result::start();
}

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------

// Showdown's |move| line (data/mods/gen3/scripts.ts:165), once past the
// canceler: [from] the caller for a called move, [from] Pursuit on a switching
// target, [from] lockedmove for a forced turn.
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

// The attack canceler: AtkCanceler_UnableToUseMove (util:1987), then the
// rest of Cmd_attackcanceler (bsc:915). Returns whether the move goes
// ahead; the first case that stops it ends the check. The cases come in
// the order of Showdown's BeforeMove priorities (recharge 11, sleep and
// freeze 10, Truant 9, flinch 8, Disable 7, Imprison 4, confusion 3,
// Attract 2, paralysis 1, Taunt 0), which the decomp shuffles (recharge
// after sleep, Taunt before Imprison, paralysis before Attract).
template <typename O>
constexpr bool cancel(Battle& battle, Player player, MoveState& m, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  const ID id = protocol::ident(battle, player);
  assert(!mon.fainted());
  const auto go = [&] {
    log_move(battle, player, m, options);
    v.last_used_move = m.move;
    v.last_used_type = m.type;
    m.obeys = true;
    return true;
  };
  // A called move: its caller's canceler ran. Pursuit on a switching target:
  // Showdown's useMove runs no BeforeMove handler.
  if (m.called || m.pursuing) return go();
  const auto stop = [&](protocol::Cant reason) {
    protocol::cant(options.log, id, reason, m.move);
    abort_move(battle, player, options);
    m.unable = true;
    return false;
  };
  // Showdown's rage onBeforeMove (priority 100) ends Rage at any move attempt;
  // the decomp's waits for a turn without it. CANCELER_FLAGS ends Destiny Bond
  // and Grudge.
  if (v.kind <= Volatiles::Kind::Grudge) end_kind(v);

  // CANCELER_RECHARGE
  if (v.kind == Volatiles::Kind::Recharge) {
    end_kind(v);
    return stop(protocol::Cant::Recharge);
  }

  // CANCELER_ASLEEP: the counter ticks; Snore and Sleep Talk go on while
  // asleep. Uproar wakes only the Pokémon it hits, as Showdown's (hit()),
  // not everyone at the canceler (UproarWakeUpCheck, bsc:6801).
  if (mon.status.asleep()) {
    const uint8_t by = a.ability == Ability::EarlyBird ? 2 : 1;
    const bool rest = mon.status.self_inflicted();
    const Status before = mon.status;
    const bool woke = !mon.status.tick_sleep(by);
    if (woke) protocol::cure_status(options.log, id, before);
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).sleep,
                  options.chance.durations.get(player).sleep[0], woke,
                  rest ? Status::REST_SLEEP : Status::MIN_SLEEP,
                  rest ? Status::REST_SLEEP : Status::MAX_SLEEP, by, options);
    }
    // Showdown's gen3 skippedTime counts the Sleep Talk and Snore turns
    // (ENGINE.md, field map).
    if (woke) {
      wake(battle, player, options);
    } else if (m.move != Move::Snore && m.move != Move::SleepTalk) {
      v.status_counter = 0;
      return stop(protocol::Cant::Sleep);
    } else {
      protocol::cant(options.log, id, protocol::Cant::Sleep, m.move);
      v.status_counter++;
    }
  }

  // CANCELER_FROZEN: it thaws with 1/5. A thawing move (Effect::ThawHit)
  // goes on frozen, and CANCELER_THAW thaws it below. No attack string.
  const bool thaw_hit = get(m.move).effect == Effect::ThawHit;
  if (mon.status.frozen()) {
    if (rolls::thaw(battle, player, options)) {
      protocol::cure_status(options.log, id, mon.status);
      mon.status = Status::none();
    } else if (!thaw_hit) {
      protocol::cant(options.log, id, protocol::Cant::Freeze, m.move);
      abort_move(battle, player, options);
      return false;
    }
  }

  // CANCELER_TRUANT; the counter toggles at the end of each turn.
  if (a.ability == Ability::Truant && v.truant) {
    m.result |= MoveResult::MISSED;
    return stop(protocol::Cant::Truant);
  }

  // CANCELER_FLINCH
  if (v.flinched) {
    v.flinched = false;
    return stop(protocol::Cant::Flinch);
  }

  // CANCELER_DISABLED, CANCELER_IMPRISONED
  if (v.disabled_move != Move::None && v.disabled_move == m.move) {
    return stop(protocol::Cant::Disable);
  }
  if (imprisoned(battle.side(foe(player)).active, m.move)) return stop(protocol::Cant::Imprison);

  // CANCELER_CONFUSED: the hidden counter ticks; at 0 the Pokémon snaps out,
  // else it hits itself with 1/2.
  if (v.confusion != 0) {
    v.confusion--;
    const bool ended = v.confusion == 0;
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).confusion_end,
                  options.chance.durations.get(player).confusion, ended, 2, 5, 1, options);
    }
    if (ended) {
      protocol::end_effect(options.log, id, protocol::End::Confusion);
    } else {
      protocol::activate(options.log, id, protocol::Activate::Confusion);
      if (rolls::confusion(battle, player, options)) {
        hurt_itself(battle, player, m, options);
        return false;
      }
    }
  }

  // CANCELER_IN_LOVE: Showdown's -activate comes before the draw.
  if (v.infatuated) {
    protocol::activate_with(options.log, id, protocol::Activate::Attract, Move::None,
                            protocol::ident(battle, foe(player)));
    if (rolls::infatuation(battle, player, options)) return stop(protocol::Cant::Attract);
  }

  // CANCELER_PARALYZED (its script cancels a multi-turn move, bs1:3773).
  if (mon.status.paralyzed() && rolls::paralysis(battle, player, options)) {
    return stop(protocol::Cant::Paralysis);
  }

  // Showdown's choicelock onBeforeMove: a lock whose band has gone ends here
  // (or at the end of the turn), so a band Tricked back this turn keeps it.
  if (!holds(side, HoldEffect::ChoiceBand)) v.choice_move = Move::None;

  // CANCELER_TAUNTED
  if (v.taunt != 0 && get(m.move).bp == 0) return stop(protocol::Cant::Taunt);

  // CANCELER_BIDE, as Showdown's gen3 onBeforeMove: at 2 only -activate; at 1
  // it ends, and fails if nothing took HP from it, else releases (bide()).
  if (v.kind == Volatiles::Kind::Bide) {
    assert(m.move == Move::Bide && v.lock <= 2);
    if (v.lock == 2) {
      protocol::activate(options.log, id, protocol::Activate::Bide);
      m.unable = true;
      return false;
    }
    protocol::end_effect(options.log, id, protocol::End::Bide);
    if (v.bide_damage == 0) {
      protocol::fail(options.log, id);
      abort_move(battle, player, options);
      m.unable = true;
      return false;
    }
    m.release = true;
    m.obeys = true;
    return true;
  }

  // CANCELER_THAW (Showdown's frz onModifyMove, before the move line).
  if (mon.status.frozen()) {
    assert(thaw_hit);
    protocol::cure_status(options.log, id, mon.status, protocol::Cure::Move, false, m.move);
    mon.status = Status::none();
  }

  // No PP left (bs1:3556): Spite or Pressure took the last after selection.
  if (m.slot != NO_SLOT && a.moves[m.slot].pp == 0 &&
      m.move != Move::Struggle && !v.multiple_turns()) {
    protocol::cant(options.log, id, protocol::Cant::PP, m.move);
    m.attackstring = true;
    m.result |= MoveResult::MISSED;
    return false;
  }

  // Soundproof (ABILITYEFFECT_MOVES_BLOCK, util:2645) comes here in the decomp;
  // Showdown's is a TryHit handler (try_hit).
  return go();
}

// ---------------------------------------------------------------------------
// TryHit, moveend and the hit
// ---------------------------------------------------------------------------

// Showdown's gen3 tryMoveHit (data/mods/gen3/scripts.ts:263) from
// Invulnerability through TryHit, for a move aimed at the foe: an out-of-reach
// target is missed with no roll; then the roll (-immune for a natural immunity
// on a miss, else -miss); then TryHit's handlers by priority (Protect, Magic
// Coat, the target's ability); then a natural immunity's -immune. Returns
// whether the move goes on to its own checks. The decomp checks most failures
// before the roll; the state is the same, the messages are Showdown's. The roll
// is recorded only where its outcome can change the state.
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
  // A lower-level OHKO fails before the roll (Showdown's gen3; Cmd_tryKO,
  // bsc:7490).
  const bool ohko = get(m.move).effect == Effect::Ohko;
  if (ohko && battle.side(player).get(1).level < battle.side(defender).get(1).level) {
    protocol::immune(options.log, target, protocol::Immune::OHKO);
    m.result |= MoveResult::DOESNT_AFFECT_FOE;
    return false;
  }
  const bool protect = protected_from(m, player);
  const bool soundproof = t.ability == Ability::Soundproof && sound(m.move);
  // Sturdy stops OHKO moves (onTryHit, after Protect's).
  const bool sturdy = ohko && t.ability == Ability::Sturdy;
  const Absorb absorb = absorbs(battle, player, m);
  const bool bounce = bounces(player, m);
  const bool blocked = protect || soundproof || sturdy || absorb != Absorb::None;
  // Protect renews a locked rampage.
  const bool renews = protect && get(m.move).effect == Effect::Rampage && m.locked;

  bool hits = true;
  if (const uint32_t calc = aim.sure ? 100 : accuracy(battle, player, m); calc < 100) {
    assert(calc > 0);
    // Protect, Sturdy and a heal-less absorb leave a miss's state; Soundproof
    // and Flash Fire set no flag, so moveend records the move as landed and
    // their roll matters.
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
    if (renews) lock_renew(battle, player);
    return false;
  }
  if (bounce) {
    m.turn->magic_coat[index(defender)] = false;
    call_back(battle, defender, m.move, Move::MagicCoat, m, options);
    m.result |= MoveResult::FAILED;
    return false;
  }
  // Soundproof sets no result flag (bs1:4158); Sturdy sets MISSED (bsc:7490).
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

// Cmd_moveend (bsc:4210) with moveendall: its cases in the decomp's order.
template <typename O>
constexpr void moveend(Battle& battle, Player player, MoveState& m, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Side& target_side = m.self ? side : battle.side(foe(player));
  ActivePokemon& t = target_side.active;

  // MOVEEND_RAGE, DEFROST and ON_DAMAGE_ABILITIES run in the hit (rage(),
  // contact()), as Showdown's onHit and DamagingHit.

  // MOVEEND_SYNCHRONIZE_TARGET, IMMUNITY_ABILITIES, SYNCHRONIZE_ATTACKER (no
  // self-targeting move arms Synchronize).
  if (!m.self) synchronize(battle, foe(player), m, options);
  ability_immunity(battle, options);
  synchronize(battle, player, m, options);

  // MOVEEND_CHANGED_ITEMS (Thief and Covet) before the Choice Band lock, as
  // Showdown's (gen4 items.ts onAfterMove), so Thief locks into a stolen band;
  // the decomp's comes after.
  if (m.stolen != Item::None) side.get(1).item = m.stolen;

  // MOVEEND_CHOICE_MOVE, as Showdown's: any move locks, even Struggle or a
  // called move; a working Baton Pass doesn't.
  if (m.obeys && holds(side, HoldEffect::ChoiceBand) && a.volatiles.choice_move == Move::None &&
      !(m.chosen == Move::BatonPass && !(m.result & MoveResult::FAILED))) {
    a.volatiles.choice_move = m.chosen;
  }

  // MOVEEND_ATTACKER_VISIBLE, which ends a semi-invulnerable turn when the
  // move failed or was stopped, has nothing to do: the charge turn returns
  // before either, and the second turn and the canceler end the charge.
  assert(!((m.result & MoveResult::NO_EFFECT) || m.unable) ||
         !a.volatiles.semi_invulnerable());

  // Showdown's AfterMove comes before the item cures.
  after_move(battle, player, m, options);

  // MOVEEND_ITEM_EFFECTS_ALL: both Pokémon, as Showdown's onUpdate (the decomp
  // stops at the first, util:3740).
  for (const Player p : {Player::P1, Player::P2}) {
    if (!battle.side(p).get(1).fainted()) item_cure(battle, p, options);
  }

  // MOVEEND_KINGSROCK_SHELLBELL: King's Rock for a single hit (the multi-hit
  // loops and Beat Up roll it per hit), then Shell Bell, as Showdown's
  // (data/items.ts:5656): an eighth of what the move took from the target
  // itself over all its hits. Not for Bide's release. A user that Destiny Bond
  // took heals in the log only.
  const Effect effect = get(m.move).effect;
  if (!multi_hit(effect) && effect != Effect::BeatUp) flinch_item(battle, player, m, 0, options);
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
    if (s.active.volatiles.substitute_hp == 0) s.active.volatiles.substitute = false;
  }

  // MOVEEND_UPDATE_LAST_MOVES: gChosenMove for the last move.
  // A Baton Pass that worked leaves it to the recipient.
  if (get(m.chosen).effect != Effect::BatonPass || (m.result & MoveResult::FAILED)) {
    // As Showdown's lastMove, a move stopped before it was used, or Bide's
    // release, leaves the last move as it was; the decomp writes
    // MOVE_UNAVAILABLE.
    if (m.obeys && !m.release) a.volatiles.last_move = m.chosen;
  }

  // MOVEEND_MIRROR_MOVE, as Showdown's attackedBy.
  if (m.attacked && !m.self && mirrors(t)) {
    t.volatiles.last_taken_move = m.move;
    t.volatiles.last_taken_from = side.order[0];
  }

  // White Herb, in Showdown's AfterMove (skipped by a move stopped before it
  // ran).
  if (!m.unable) white_herbs(battle, options);

  // Grudge takes the move's PP after the cures, so a Leppa Berry waits.
  const uint8_t s = m.grudge ? slot_of(a, m.chosen) : NO_SLOT;
  if (s != NO_SLOT) deduct_pp(battle, player, s, a.moves[s].pp);
}

// A hit's own failures before its accuracy check: Snore while awake (bs1:1257),
// Fake Out after the first action out (bsc:6786; Showdown counts move actions),
// Spit Up with no Stockpile (bs1:2154, silent), Focus Punch after the user was
// hit (bs1:2260; Turn::hit).
template <typename O>
constexpr bool fails_early(Battle& battle, Player player, MoveState& m, O& options) {
  const Effect effect = get(m.move).effect;
  const ActivePokemon& a = battle.side(player).active;
  if (effect == Effect::FocusPunch && m.turn->hit[index(player)]) {
    m.unable = true; // Showdown's beforeMoveCallback: no AfterMove
    protocol::last_still(options.log);
    protocol::cant(options.log, protocol::ident(battle, player), protocol::Cant::FocusPunch);
    pp_reduce(battle, player, m);
    return true;
  }
  if ((effect == Effect::Snore && !battle.side(player).get(1).status.asleep()) ||
      (effect == Effect::FakeOut && a.volatiles.move_actions > 1) ||
      (effect == Effect::SpitUp && a.volatiles.stockpile == 0)) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    m.result |= MoveResult::FAILED;
    return true;
  }
  return false;
}

// A two-turn move (bs1:1903, 778, 1811, 1064, 1973). The first turn charges
// (bs1:795): PP with no attack string, the lock (bsc:2571), Skull Bash's
// Defense, Fly's and the others' semi-invulnerability; Solar Beam in sun fires
// at once. The second turn ends the charge and hits with no PP. Returns whether
// this turn only charged.
template <typename O>
constexpr bool charges(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  Volatiles& v = a.volatiles;
  const Effect effect = get(m.move).effect;
  const ID id = protocol::ident(battle, player);
  if (v.charging()) {
    end_kind(v);
    m.no_pp = true;
    return false;
  }
  if (effect == Effect::SolarBeam && effective_weather(battle) == Weather::Sun) {
    // Showdown still prepares, then plays the attack as -anim.
    protocol::prepare(options.log, id, m.move);
    protocol::last_still(options.log);
    protocol::anim(options.log, id, m.move, protocol::ident(battle, foe(player)));
    return false;
  }
  protocol::last_still(options.log);
  protocol::prepare(options.log, id, m.move);
  pp_reduce(battle, player, m);
  if (effect == Effect::SkullBash) protocol::boost(options.log, id, DEF, raise(a, DEF, 1, m), false);
  using K = Volatiles::Kind;
  assert(v.kind == K::None);
  v.kind = effect != Effect::SemiInvulnerable ? K::Charge
           : m.move == Move::Dig              ? K::Underground
           : m.move == Move::Dive             ? K::Underwater
                                              : K::OnAir;
  assert(v.kind != K::OnAir || m.move == Move::Fly || m.move == Move::Bounce);
  v.lock = 2;
  v.locked_move = m.move;
  return true;
}

// Whether hit h reaches the foe: typecalc's immunities first (as Showdown,
// ENGINE.md "Damage"), then attackstring, ppreduce and try_hit, then Brick
// Break's screens (bsc:9883; not into an immune target, as Showdown) and the
// immunity's message. A Jump Kick that doesn't reach crashes.
template <typename O>
constexpr bool lands(Battle& battle, Player player, MoveState& m, const Hit& h, O& options) {
  const Effect effect = get(m.move).effect;
  const Player defender = foe(player);
  Side& foe_side = battle.side(defender);
  m.result |= matchup(battle, player, h);
  // Showdown's Wonder Guard names itself even where a type immunity also stops
  // the move, but not where only Dream Eater's does (a target awake or behind a
  // Substitute).
  const bool wonder_guard = (m.result & MoveResult::NO_EFFECT) &&
                            foe_side.active.ability == Ability::WonderGuard;
  if (effect == Effect::DreamEater &&
      (!foe_side.get(1).status.asleep() || foe_side.active.volatiles.substitute)) {
    m.result |= MoveResult::DOESNT_AFFECT_FOE;
  }
  const bool affects = !(m.result & MoveResult::NO_EFFECT);
  SideConditions& screens = foe_side.conditions;
  const bool breaks = effect == Effect::BrickBreak && affects &&
                      (screens.reflect != 0 || screens.light_screen != 0);
  m.attackstring = true;
  pp_reduce(battle, player, m);
  if (!try_hit(battle, player, m,
               {.immune = (m.result & MoveResult::DOESNT_AFFECT_FOE) != 0,
                .matters = affects,
                .wonder_guard = wonder_guard},
               options)) {
    if (effect == Effect::RecoilIfMiss) crash(battle, player, m, h, options);
    return false;
  }
  if (breaks) {
    if (screens.reflect != 0) {
      protocol::side_end(options.log, defender, protocol::SideCondition::Reflect);
    }
    if (screens.light_screen != 0) {
      protocol::side_end(options.log, defender, protocol::SideCondition::LightScreen);
    }
    screens.reflect = 0;
    screens.light_screen = 0;
  }
  if (!affects) {
    const ID id = protocol::ident(battle, defender);
    if (wonder_guard) {
      protocol::immune(options.log, id, protocol::Immune::Ability, Ability::WonderGuard);
    } else {
      protocol::immune(options.log, id);
    }
    if (effect == Effect::RecoilIfMiss) crash(battle, player, m, h, options);
    return false;
  }
  return true;
}

// One strike: crit, roll (none for fixed damage), survival and HP, with
// Showdown's modifyDamage messages.
template <typename O>
constexpr void strike(Battle& battle, Player player, MoveState& m, Hit h, uint32_t fixed,
                      uint8_t i, O& options) {
  uint32_t d = fixed;
  if (d == 0) {
    const bool crits = can_crit(battle, player);
    const ActivePokemon& t = battle.side(foe(player)).active;
    const uint32_t cap = t.volatiles.substitute && t.volatiles.substitute_hp != 0
                             ? t.volatiles.substitute_hp
                             : battle.side(foe(player)).get(1).hp;
    d = rolls::hit_damage(battle, player, foe(player), i, h, crits,
                          crits ? crit_stage(battle, player, m.move) : uint8_t(0), cap, options);
    log_modifiers(battle, player, m, h.crit, options);
  }
  d = survive(battle, player, m, d, i, options);
  m.hp_dealt = hp_update(battle, foe(player), d, m, options);
  log_hit(battle, player, m, options);
}

// The multi-hit loop (bs1:604, 839, 1075, 1384), as Showdown's gen3
// (data/mods/gen3/scripts.ts:393): 2-5, 2 or 3 hits while both stand and the
// user is awake, each with what a hit sets off and the item cures. Triple
// Kick's hits are 10, 20, 30, each checking accuracy. Showdown hits on through
// Endure; the decomp stops (bs1:638).
template <typename O>
constexpr void multi_hit_strikes(Battle& battle, Player player, MoveState& m, const Hit& h,
                                 O& options) {
  const Effect effect = get(m.move).effect;
  const Pokemon& user = battle.side(player).get(1);
  const Pokemon& target = battle.side(foe(player)).get(1);
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
    // Color Change may have changed the target's type.
    if (i > 0) {
      constexpr uint8_t eff = MoveResult::SUPER_EFFECTIVE | MoveResult::NOT_VERY_EFFECTIVE;
      m.result = uint8_t((m.result & ~eff) | (matchup(battle, player, hi) & eff));
    }
    m.damaged = false;
    strike(battle, player, m, hi, 0, i, options);
    rage(battle, player, m, options);
    if (effect == Effect::Twineedle) {
      secondary(battle, player, m, options, i);
      synchronize(battle, foe(player), m, options);
    }
    flinch_item(battle, player, m, i, options);
    contact(battle, player, m, i, options);
    for (const Player p : {Player::P1, Player::P2}) {
      if (!battle.side(p).get(1).fainted()) item_cure(battle, p, options);
    }
    damaged |= m.damaged;
    hits++;
  }
  if (effect == Effect::TripleKick) rolls::kicks(player, hits, options);
  if (effect == Effect::MultiHit) {
    const bool stopped = target.fainted() || user.fainted() ||
                         (user.status.asleep() && m.chosen != Move::SleepTalk);
    assert(hits == n || stopped);
    rolls::multi_hits(player, hits, stopped, options);
  }
  m.damaged = damaged;
  // The faints wait for the end of runMove, after AfterMove.
  protocol::hit_count(options.log, protocol::ident(battle, foe(player)), hits);
}

// BattleScript_EffectHit (bs1:236) and the scripts built on it, after the
// canceler: the move's own failures, the charge, the power, the accuracy check
// and the damage, then what follows a hit, in Showdown's order.
template <typename O>
constexpr void hit(Battle& battle, Player player, MoveState& m, O& options) {
  const Effect effect = get(m.move).effect;
  if (fails_early(battle, player, m, options)) return;
  if (two_turn(effect) && charges(battle, player, m, options)) return;
  const uint32_t counter = counter_damage(battle, player, m);
  if (counter == 0 && (effect == Effect::Counter || effect == Effect::MirrorCoat ||
                       effect == Effect::Endeavor)) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    m.result |= MoveResult::FAILED;
    // Endeavor's onTry prints -fail; Counter and Mirror Coat fail silently.
    if (effect == Effect::Endeavor) protocol::fail(options.log, protocol::ident(battle, player));
    return;
  }
  if (effect == Effect::Explosion && !explodes(battle, player, m, options)) return;

  Hit h = base_hit(battle, player, m);
  // Present's, Magnitude's or Psywave's draw. Magnitude's is drawn and logged
  // before the accuracy check (bs1:1683), and recorded once the move lands.
  uint8_t variable = 0;
  if (effect == Effect::Magnitude) {
    variable = rolls::variable(battle, player, effect, options);
    h.power = MAGNITUDE[variable - 4].power;
    protocol::activate_magnitude(options.log, protocol::ident(battle, player), variable);
  }
  if (!lands(battle, player, m, h, options)) return;

  Side& side = battle.side(player);
  const Player defender = foe(player);
  // Uproar's onTryHit wakes the target, then the user (the decomp, everyone at
  // the canceler).
  if (effect == Effect::Uproar) {
    for (const Player p : {defender, player}) {
      Pokemon& mon = battle.side(p).get(1);
      if (!mon.status.asleep()) continue;
      protocol::cure_status(options.log, protocol::ident(battle, p), mon.status);
      mon.status = Status::none();
      wake(battle, p, options);
    }
  }
  // The power callbacks that count the hit: Rollout doubles per hit (bsc:7896),
  // and after Defense Curl, locking the user below 5 hits; Fury Cutter starts
  // its count.
  if (effect == Effect::Rollout) {
    ActivePokemon& a = side.active;
    Volatiles& v = a.volatiles;
    const uint8_t count = v.kind == Volatiles::Kind::Rollout ? v.lock : 0;
    h.power = uint16_t(get(m.move).bp << count);
    if (!side.get(1).status.asleep()) {
      assert(v.kind == Volatiles::Kind::None || v.kind == Volatiles::Kind::Rollout);
      v.kind = Volatiles::Kind::Rollout;
      v.lock = uint8_t(count + 1);
      v.locked_move = m.move;
      if (v.lock < 5) v.renewed_lock = true;
    }
    if (a.volatiles.defense_curl) h.power = uint16_t(h.power * 2);
  } else if (effect == Effect::FuryCutter) {
    ActivePokemon& a = side.active;
    if (a.volatiles.fury_cutter == 0) {
      a.volatiles.fury_cutter = 1;
      a.volatiles.renewed_fury_cutter = true;
    }
    h.power = uint16_t(std::min(get(m.move).bp << (a.volatiles.fury_cutter - 1), 160));
  }
  if (effect == Effect::Magnitude) rolls::variable_matters(player, effect, variable, options);
  if (effect == Effect::Psywave || effect == Effect::Present) {
    variable = rolls::variable(battle, player, effect, options);
    rolls::variable_matters(player, effect, variable, options);
  }
  if (effect == Effect::Present) {
    if (present_heals(battle, player, m, variable, options)) return;
    h.power = variable == 2 ? 40 : variable == 3 ? 80 : 120;
  }

  if (multi_hit(effect)) {
    multi_hit_strikes(battle, player, m, h, options);
  } else {
    strike(battle, player, m, h,
           counter != 0 ? counter : fixed_damage(battle, player, effect, variable), 0, options);
    // -ohko after a hit on a Substitute; the KO's comes after the contact
    // ability.
    if (effect == Effect::Ohko && !m.damaged) protocol::ohko(options.log);
  }
  on_hit(battle, player, m, options);
  if (!multi_hit(effect)) rage(battle, player, m, options);
  self_effects(battle, player, m, options);
  if (effect != Effect::Twineedle) secondary(battle, player, m, options);
  after_hit(battle, player, m, options);
  // Superpower and Overheat's drops on the user (bsc:2839, 2895), whatever its
  // abilities or Mist.
  if (effect == Effect::Superpower || effect == Effect::Overheat) {
    const ID id = protocol::ident(battle, player);
    const auto drop = [&](uint8_t stat, int8_t n) {
      if (const int8_t lost = lower(side.active, stat, n)) {
        protocol::boost(options.log, id, stat, lost, true);
      }
    };
    if (effect == Effect::Superpower) {
      drop(ATK, 1);
      drop(DEF, 1);
    } else {
      drop(SPA, 2);
    }
  }
  if (!multi_hit(effect)) contact(battle, player, m, 0, options);
  if (effect == Effect::Ohko && m.damaged && battle.side(defender).get(1).fainted()) {
    protocol::ohko(options.log);
  }
  recoil(battle, player, m, options);
  faint_target(battle, player, m.chosen, m.grudge, m.bonded_hp, options);
}

// ---------------------------------------------------------------------------
// The move frame and the actions
// ---------------------------------------------------------------------------

template <typename O>
constexpr void script(Battle& battle, Player player, MoveState& m, O& options);

// One use of m.move, the frame every move runs through: the setup, the
// canceler, the shared opening, the script, moveend, then Bide's end or the
// user's faint.
template <typename O>
constexpr void execute(Battle& battle, Player player, MoveState& m, O& options) {
  assert(m.move != Move::None);
  if (m.chosen == Move::None) m.chosen = m.move;
  m.self = self_aimed(battle.side(player).active, m.move);
  m.type = move_type(battle, m.move);
  const Effect effect = get(m.move).effect;

  if (!cancel(battle, player, m, options)) return moveend(battle, player, m, options);
  // Every script but the hits' opens with attackstring and ppreduce, after
  // which Snatch can steal the move.
  if (!plain_hit(effect) && !m.release) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    if (snatched(battle, player, m, options)) return moveend(battle, player, m, options);
  }
  script(battle, player, m, options);
  // A move that calls another ends with the called move's moveend.
  if (m.delegated) return;
  moveend(battle, player, m, options);
  if (m.release && battle.side(player).active.volatiles.kind == Volatiles::Kind::Bide) {
    end_bide(battle, player, options);
  }
  // Explosion and Memento: the user, at 0 HP since the move, faints now.
  if (m.faints) faint(battle, player, options);
}

// The script for m's effect: each effect once, with its parameters.
template <typename O>
constexpr void script(Battle& battle, Player player, MoveState& m, O& options) {
  const auto log = [&] { return protocol::ident(battle, player); };
  switch (get(m.move).effect) {
  // The major statuses, Rest and Yawn.
  case Effect::Poison: return status_move(battle, player, m, Secondary::Poison, options);
  case Effect::Toxic: return status_move(battle, player, m, Secondary::Toxic, options);
  case Effect::Paralyze: return status_move(battle, player, m, Secondary::Paralysis, options);
  case Effect::WillOWisp: return status_move(battle, player, m, Secondary::Burn, options);
  case Effect::Sleep: return status_move(battle, player, m, Secondary::Sleep, options);
  case Effect::Rest: return rest(battle, player, m, options);
  case Effect::Yawn: return yawn(battle, player, m, options);
  case Effect::Confuse:
  case Effect::TeeterDance: return confuse_move(battle, player, m, options);
  case Effect::Swagger: return swagger(battle, player, m, {ATK, 2}, options);
  case Effect::Flatter: return swagger(battle, player, m, {SPA, 1}, options);
  case Effect::Attract: return attract(battle, player, m, options);
  case Effect::Nightmare: return nightmare(battle, player, m, options);
  case Effect::Curse: return curse(battle, player, m, options);
  case Effect::LeechSeed: return leech_seed(battle, player, m, options);

  // Stat stages: setstatchanger in each script (bs1:475-533, 927-975), and
  // the two-stat moves (bs1:2687-2800).
  case Effect::AttackUp: return stat_up_move(battle, player, m, {ATK, 1}, options);
  case Effect::AttackUp2: return stat_up_move(battle, player, m, {ATK, 2}, options);
  case Effect::DefenseUp: return stat_up_move(battle, player, m, {DEF, 1}, options);
  case Effect::DefenseUp2: return stat_up_move(battle, player, m, {DEF, 2}, options);
  case Effect::SpeedUp2: return stat_up_move(battle, player, m, {SPE, 2}, options);
  case Effect::SpecialAttackUp: return stat_up_move(battle, player, m, {SPA, 1}, options);
  case Effect::SpecialAttackUp2: return stat_up_move(battle, player, m, {SPA, 2}, options);
  case Effect::SpecialDefenseUp2: return stat_up_move(battle, player, m, {SPD, 2}, options);
  case Effect::EvasionUp: return stat_up_move(battle, player, m, {EVA, 1}, options);
  case Effect::Minimize: return stat_up_move(battle, player, m, {EVA, 1}, options);
  case Effect::DefenseCurl: return stat_up_move(battle, player, m, {DEF, 1}, options);
  case Effect::CosmicPower: return stat_pair_move(battle, player, m, DEF, SPD, options);
  case Effect::BulkUp: return stat_pair_move(battle, player, m, ATK, DEF, options);
  case Effect::CalmMind: return stat_pair_move(battle, player, m, SPA, SPD, options);
  case Effect::DragonDance: return stat_pair_move(battle, player, m, ATK, SPE, options);
  case Effect::BellyDrum: return belly_drum(battle, player, m, options);
  case Effect::AttackDown: return stat_down_move(battle, player, m, {ATK, 1}, options);
  case Effect::AttackDown2: return stat_down_move(battle, player, m, {ATK, 2}, options);
  case Effect::DefenseDown: return stat_down_move(battle, player, m, {DEF, 1}, options);
  case Effect::DefenseDown2: return stat_down_move(battle, player, m, {DEF, 2}, options);
  case Effect::SpeedDown: return stat_down_move(battle, player, m, {SPE, 1}, options);
  case Effect::SpeedDown2: return stat_down_move(battle, player, m, {SPE, 2}, options);
  case Effect::SpecialDefenseDown2: return stat_down_move(battle, player, m, {SPD, 2}, options);
  case Effect::AccuracyDown: return stat_down_move(battle, player, m, {ACC, 1}, options);
  case Effect::EvasionDown: return stat_down_move(battle, player, m, {EVA, 1}, options);
  case Effect::Tickle: return tickle(battle, player, m, options);
  case Effect::Memento: return memento(battle, player, m, options);
  case Effect::Haze: return haze(battle, options);
  case Effect::PsychUp: return psych_up(battle, player, m, options);

  // HP.
  case Effect::RestoreHp:
  case Effect::Softboiled: return heal_move(battle, player, false, options);
  case Effect::MorningSun:
  case Effect::Synthesis:
  case Effect::Moonlight: return heal_move(battle, player, true, options);
  case Effect::Substitute: return substitute(battle, player, options);
  case Effect::PainSplit: return pain_split(battle, player, m, options);
  case Effect::Wish: return wish(battle, player, m, options);
  case Effect::Refresh: return refresh(battle, player, m, options);
  case Effect::HealBell: return heal_bell(battle, player, m, options);
  case Effect::Stockpile: return stockpile(battle, player, m, options);
  case Effect::Swallow: return swallow(battle, player, m, options);

  // The field and the sides.
  case Effect::RainDance: return weather_move(battle, player, m, Weather::Rain, options);
  case Effect::SunnyDay: return weather_move(battle, player, m, Weather::Sun, options);
  case Effect::Sandstorm: return weather_move(battle, player, m, Weather::Sandstorm, options);
  case Effect::Hail: return weather_move(battle, player, m, Weather::Hail, options);
  case Effect::Reflect:
  case Effect::LightScreen:
  case Effect::Mist:
  case Effect::Safeguard: return screen(battle, player, m, options);
  case Effect::Spikes: return spikes_move(battle, player, m, options);
  case Effect::PerishSong: return perish_song(battle, player, m, options);
  case Effect::FutureSight: return future_sight_move(battle, player, m, options);
  case Effect::BeatUp: return beat_up(battle, player, m, options);

  // Volatiles on the user or the foe.
  case Effect::Protect:
  case Effect::Endure: return protect(battle, player, m, options);
  case Effect::FocusEnergy:
  case Effect::Ingrain:
  case Effect::Charge:
  case Effect::MudSport:
  case Effect::WaterSport: return self_volatile(battle, player, m, options);
  // Destiny Bond (bsc:8279) and Grudge never fail; the user's next move clears
  // them (cancel()).
  case Effect::DestinyBond:
    assert(battle.side(player).active.volatiles.kind == Volatiles::Kind::None);
    battle.side(player).active.volatiles.kind = Volatiles::Kind::DestinyBond;
    return protocol::single_move(options.log, log(), protocol::SingleMove::DestinyBond);
  case Effect::Grudge:
    assert(battle.side(player).active.volatiles.kind == Volatiles::Kind::None);
    battle.side(player).active.volatiles.kind = Volatiles::Kind::Grudge;
    return protocol::single_move(options.log, log(), protocol::SingleMove::Grudge);
  case Effect::MagicCoat:
  case Effect::Snatch: return coat_or_snatch(battle, player, m, options);
  case Effect::Bide: return bide(battle, player, m, options);
  case Effect::MeanLook: return mean_look(battle, player, m, options);
  case Effect::Taunt: return taunt(battle, player, m, options);
  case Effect::Encore: return encore(battle, player, m, options);
  case Effect::Disable: return disable(battle, player, m, options);
  case Effect::Torment: return torment(battle, player, m, options);
  case Effect::Imprison: return imprison(battle, player, m, options);
  case Effect::Spite: return spite(battle, player, m, options);
  case Effect::Foresight: return foresight(battle, player, m, options);
  case Effect::LockOn: return lock_on(battle, player, m, options);

  // Switching.
  case Effect::Roar: return roar(battle, player, m, options);
  case Effect::BatonPass: return baton_pass(battle, player, m, options);

  // Copying, calling and swapping.
  case Effect::Transform: return transform(battle, player, m, options);
  case Effect::Mimic: return mimic(battle, player, m, options);
  case Effect::Sketch: return sketch(battle, player, m, options);
  case Effect::SleepTalk: return sleep_talk(battle, player, m, options);
  case Effect::MirrorMove: return mirror_move(battle, player, m, options);
  // Metronome: a uniform pick among METRONOME_MOVES.
  case Effect::Metronome:
    return call_move(battle, player, m, rolls::metronome(battle, player, options),
                     NO_SLOT, options);
  case Effect::Assist: return assist(battle, player, m, options);
  case Effect::NaturePower: return nature_power(battle, player, m, options);
  case Effect::Camouflage: return camouflage(battle, player, m, options);
  case Effect::Conversion:
  case Effect::Conversion2: return conversion(battle, player, m, options);
  case Effect::RolePlay:
  case Effect::SkillSwap: return role_play(battle, player, m, options);
  case Effect::Trick: return trick(battle, player, m, options);
  case Effect::Recycle: return recycle(battle, player, m, options);

  // -nothing is Splash's onTryHit; Teleport fails silently in a trainer battle
  // (gen7's onTry).
  case Effect::Splash: return protocol::nothing(options.log);
  case Effect::Teleport: m.result |= MoveResult::FAILED; return;
  // Doubles-only: they use PP. Helping Hand with no ally prints -notarget.
  case Effect::FollowMe: return;
  case Effect::HelpingHand: return protocol::no_target(options.log, log());

  default:
    assert(plain_hit(get(m.move).effect));
    return hit(battle, player, m, options);
  }
}

// HandleAction_UseMove (util:78): the move and slot the action uses, then its
// script. Struggle uses no PP; a forced move is the locked one; an Encore that
// began after selection replaces the chosen move, unless its move was
// overwritten, which ends it.
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
  if (a.volatiles.move_actions < 2) a.volatiles.move_actions++;

  MoveState m{};
  m.slot = NO_SLOT;
  if (data == 0 && r.move[i] == Move::Struggle) {
    m.move = Move::Struggle;
    m.no_pp = true;
  } else if (forced(a)) {
    // gCurrMovePos keeps the slot chosen when the lock began (unverified). A
    // locked move uses no PP (Showdown's runMove skips deductPP). Known gap:
    // Showdown's Encore on a lock Metronome called runs Metronome instead.
    m.move = a.volatiles.locked_move;
    m.locked = true;
    m.no_pp = true;
    m.slot = slot_of(a, m.move);
  } else if (a.volatiles.encored_move != Move::None &&
             a.moves[a.volatiles.encored_slot].id == a.volatiles.encored_move) {
    m.move = a.volatiles.encored_move;
    m.slot = a.volatiles.encored_slot;
  } else if (a.volatiles.encored_move != Move::None) {
    m.slot = a.volatiles.encored_slot;
    m.move = a.moves[m.slot].id;
    a.volatiles.encored_move = Move::None;
    a.volatiles.encored_slot = 0;
    a.volatiles.encore_timer = 0;
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
  execute(battle, player, m, options);
  assert(!(m.drag && m.pass));
  return m.drag ? After::Drag : m.pass ? After::Pass : After::None;
}

// Whether `player`, acting after the foe's switch, Pursues first: it chose
// Pursuit and isn't fainted, asleep, frozen or loafing. Showdown's gen3
// (data/mods/gen3/moves.ts:486) and the decomp (bsc:8723) agree.
constexpr bool pursues(const Battle& battle, Player player) {
  const Request& r = battle.request;
  const uint8_t i = index(player);
  const Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  return r.choice[i].type() == Choice::Move && r.move[i] == Move::Pursuit &&
         !mon.fainted() && !mon.status.asleep() && !mon.status.frozen() &&
         !(side.active.ability == Ability::Truant && side.active.volatiles.truant);
}

// Pursuit on `p`'s switching Pokémon (bs1:3088; Showdown's gen3
// onFoeBeforeSwitchOut): the foe's move runs as useMove (no canceler, double
// power, the full moveend) and uses up its action. Its faints print before the
// switch, which goes on, without a win check until the switch is done
// (runPursuitActivation, sim/battle-actions.ts:62; resolveFaints,
// sim/battle.ts:2627).
template <typename O>
constexpr void pursuit(Battle& battle, Player p, Turn& turn, O& options) {
  protocol::activate(options.log, protocol::ident(battle, p), protocol::Activate::Pursuit);
  use_move(battle, foe(p), false, turn, options, true);
  protocol::faints(options.log);
}

// RunTurnActionsFunctions (main:4937) from action `from`; a faint ends the
// actions (HandleAction_TryFinish, util:638). Roar's drag comes after its
// action and before the faint check, as Showdown's runAction
// (sim/battle.ts:2825); a dragged-out Pokémon's move is cancelled. A working
// Baton Pass asks for the recipient after the faint check and resumes at the
// next action. The switch still happens after Pursuit's KO
// (sim/battle-actions.ts:109); there is no Pursuit on Baton Pass, Roar or a
// replacement.
template <typename O>
constexpr Flow actions(Battle& battle, uint8_t from, O& options) {
  Turn turn{};
  bool pursued = false;
  for (uint8_t i = from; i < Request::END_OF_ACTIONS; i++) {
    const Player p = i == 0 ? battle.request.first : foe(battle.request.first);
    const Choice c = battle.request.choice[index(p)];
    if (pursued) continue; // the pursuer's action was its Pursuit
    After after = After::None;
    if (c.type() == Choice::Switch) {
      pursued = i == 0 && pursues(battle, foe(p));
      if (pursued) pursuit(battle, p, turn, options);
      // A pursuer that fainted (Rough Skin) and lost ends the battle before
      // the queued runSwitch (sim/battle.ts:2892).
      leave(battle, p, c.data(), options);
      if (land(battle, p, options) && !lost(battle.side(foe(p)))) {
        run_switch(battle, p, false, options);
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
// End of turn (BattleTurnPassed, main:3956): DoFieldEndTurnEffects (util:1181),
// then DoBattlerEndTurnEffects (util:1460), in Showdown's residual order:
// onResidualOrder, Speed, onResidualSubOrder, then a random order among exact
// ties (comparePriority, sim/battle.ts:408). The side timers, Wish (7) and the
// weather (8) come first, the per-Pokémon handlers (10), then Perish Song (12).
//
// With unequal Speeds the per-Pokémon handlers are Pokémon-major, as the
// decomp's; on a tie they interleave by Stage, with a coin per stage, recorded
// only where it changes the state (stage_order_matters). The decomp rolls one
// order for the pass (util:1209).
//
// The battle ends at the first residual KO that empties a side (Showdown's
// fieldEvent, sim/battle.ts:569), except that sand and hail hurt both first,
// and both Perish counts run first: there both last Pokémon fainting ties.
// Residual faints are replaced after both passes.
// ---------------------------------------------------------------------------

// f(std::integral_constant<Stage, s>) for each stage in order, until one ends
// the battle.
template <typename F> constexpr Flow each_stage(F f) {
  return [&]<std::size_t... I>(std::index_sequence<I...>) {
    Flow over;
    (void)((over = f(std::integral_constant<Stage, STAGES[I]>{})) || ...);
    return over;
  }(std::make_index_sequence<std::size(STAGES)>{});
}

// Whether two orders of one stage end in the same state. Only Leech Seed and
// the damage stages can differ; two ends with the same Result are the same,
// whatever HP the winner keeps (CLAUDE.md, "Chance").
constexpr bool same_state(const Battle& x, const Flow& fx, const Battle& y,
                          const Flow& fy) {
  if (fx || fy) return fx == fy;
  return x.sides[0] == y.sides[0] && x.sides[1] == y.sides[1];
}

// Whether `run` with P1 first and with P2 first, on copies, differs in the
// state or (with Chance) in the draws recorded: one order can skip a draw the
// other records, which would leave two keys for one branch.
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

// stage_orders_differ without the copies. Each handler touches its own Pokémon,
// except through the seeder's HP, the battle ending, and wrapper_gone. So it
// matters when:
// - Leech Seed: both are seeded and in.
// - Status, Nightmare, Curse: both hits are lethal and both are their side's
//   last.
// - Bind: both are bound and either hit is lethal.
// Over-approximated where cheaper: an extra coin is one extra chance node, a
// missing one a bug, which debug builds check against the copies.
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

// The per-Pokémon residuals with unequal Speeds: every stage of `first`'s, then
// the other's.
template <typename O>
constexpr Flow battler_pass(Battle& battle, Player first, O& options) {
  for (const Player p : {first, foe(first)}) {
    const Flow over = each_stage([&](auto s) {
      return battler_stage(battle, p, decltype(s)::value, options);
    });
    if (over) return over;
  }
  return std::nullopt;
}

// Quick Claw in the residual order, as Showdown: updateSpeed
// (sim/battle.ts:2818) gives a successful holder 65535 Speed for every residual
// handler (data/mods/gen3/scripts.ts:18); the decomp's ENDTURN_ORDER also uses
// the turn's draw (util:1209). If the move order didn't draw it, it is drawn
// here when it changes the comparison, and returned as pending for
// keep_residual_quick_claw.
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

// Whether p's pass may draw other than a Yawn's sleep length: Shed Skin with a
// status, or a Starf or confusion berry at its threshold (the stat, the
// confusion's length). Over-approximated: the berry's heal, nature and Own
// Tempo checks are ignored.
constexpr bool pass_draws(const Battle& battle, Player p) {
  const Side& side = battle.side(p);
  const Pokemon& mon = side.get(1);
  if (side.active.ability == Ability::ShedSkin && mon.status.any()) return true;
  const ItemData item = item_data(mon.item);
  switch (item.effect) {
  case HoldEffect::RandomStatUp: return mon.hp <= mon.stats.hp / item.param;
  case HoldEffect::ConfuseSpicy:
  case HoldEffect::ConfuseDry:
  case HoldEffect::ConfuseSweet:
  case HoldEffect::ConfuseBitter:
  case HoldEffect::ConfuseSour: return mon.hp <= mon.stats.hp / 2;
  default: return false;
  }
}

// Whether the order of the two per-Pokémon passes can change the state or the
// draws recorded: through Leech Seed, binding, an Uproar ending before the
// other's Yawn, both passes drawing (each takes the other's values in the
// other order; two Yawns alone only swap hidden sleep lengths), or a faint
// ending the battle or a Cloud Nine.
// Over-approximated: the damage ignores the pass's heals.
constexpr bool pass_order_matters(const Battle& battle) {
  const Side& s1 = battle.sides[0];
  const Side& s2 = battle.sides[1];
  if (s1.get(1).fainted() || s2.get(1).fainted()) return false;
  const Volatiles& v1 = s1.active.volatiles;
  const Volatiles& v2 = s2.active.volatiles;
  if (v1.leech_seed || v2.leech_seed || v1.wrapped != 0 || v2.wrapped != 0) return true;
  if (uproar(battle) && (v1.yawn != 0 || v2.yawn != 0)) return true;
  const bool o1 = pass_draws(battle, Player::P1);
  const bool o2 = pass_draws(battle, Player::P2);
  if ((o1 || v1.yawn == 1) && (o2 || v2.yawn == 1) && (o1 || o2)) return true;
  for (const Player p : {Player::P1, Player::P2}) {
    uint32_t damage = 0;
    for (const Stage stage : {Stage::Status, Stage::Nightmare, Stage::Curse}) {
      damage += stage_damage(battle, p, stage);
    }
    if (damage >= battle.side(p).get(1).hp) return true;
  }
  return false;
}

// Records a pending residual Quick Claw draw if the order needs it: a Speed tie
// either way, or pass_order_matters. An unrecorded draw stays out of the
// request (replacement_order).
template <typename O>
constexpr void keep_residual_quick_claw(Battle& battle, const ResidualSpeeds& r,
                                        O& options) {
  if (!r.pending) return;
  const bool matters = r.tie || pass_order_matters(battle);
  // The copies can also differ in two Yawns' hidden sleep lengths, drawn in the
  // other order, which the key needn't tell apart.
  assert(matters || (battle.sides[0].active.volatiles.yawn == 1 &&
                     battle.sides[1].active.volatiles.yawn == 1) ||
         !orders_differ(battle, options, [](Battle& b, auto& o, Player p) {
           return battler_pass(b, p, o);
         }));
  if (matters) rolls::quick_claw_keep(battle, r.proc, options);
}

// HandleEndTurn_ContinueBattle (main:3932), then BattleTurnPassed's residuals
// (main:3956), the future attacks, Perish Song and HandleFaintedMonActions.
template <typename O> constexpr Flow end_of_turn(Battle& battle, O& options) {
  // The decomp also cancels a sleeping Pokémon's multi-turn move here
  // (main:3944); Showdown keeps it until the move is aborted, or the residuals
  // for the Thrash family.
  for (Side& side : battle.sides) side.active.volatiles.flinched = false;
  // ENDTURN_ORDER (util:1209) uses the chosen moves' priority; Showdown's
  // residuals have none, and are sorted once by the Speeds before them (with
  // Quick Claw).
  uint32_t s1 = speed(battle, Player::P1);
  uint32_t s2 = speed(battle, Player::P2);
  const ResidualSpeeds rs = residual_speeds(battle, s1, s2, options);
  if (Flow over = field_residuals(battle, s1, s2, options)) return over;
  keep_residual_quick_claw(battle, rs, options);
  const Player faster = s1 > s2 ? Player::P1 : Player::P2;
  // A log-only order: a coin on a tie of these Speeds, not the current ones
  // (log_order).
  const auto coin = [&] { return speed_order(battle, s1, s2); };
  if (s1 != s2) {
    if (Flow over = battler_pass(battle, faster, options)) return over;
  } else {
    const Flow over = each_stage([&](auto s) -> Flow {
      constexpr Stage stage = decltype(s)::value;
      const Player first =
          stage_order_matters(battle, stage, options)
              ? rolls::residual_tie(battle, stage == Stage::LeechSeed, options)
              : coin();
      for (const Player p : {first, foe(first)}) {
        if (Flow f = battler_stage(battle, p, stage, options)) return f;
      }
      return std::nullopt;
    });
    if (over) return over;
  }
  // The future attacks, by the Speed of the Pokémon each lands on, with a
  // recorded coin on a tie: the first can KO the second's user, which then
  // loses its accuracy stage.
  Player order = faster;
  if (s1 == s2) {
    const bool both = battle.sides[0].conditions.future_sight == 1 &&
                      battle.sides[1].conditions.future_sight == 1;
    order = both ? rolls::residual_tie(battle, false, options) : coin();
  }
  for (const Player p : {order, foe(order)}) {
    future_attack(battle, p, options);
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
  // Showdown's stall expires with the residuals, before any replacement
  // (protect()).
  for (Side& side : battle.sides) {
    ActivePokemon& a = side.active;
    if (a.volatiles.stall != 0 && --a.volatiles.stall == 0) a.volatiles.protect_uses = 0;
  }
  if (Flow over = outcome(battle)) return over;
  // Showdown's gen3 Update after the residuals (sim/battle.ts:2915), unless a
  // replacement is due: a berry takes Thrash's confusion or Yawn's sleep.
  if (!battle.sides[0].get(1).fainted() && !battle.sides[1].get(1).fainted()) {
    const Player first = log_order(battle);
    for (const Player p : {first, foe(first)}) item_cure(battle, p, options);
  }
  return faints(battle, Phase::FaintAfterResiduals);
}

// ---------------------------------------------------------------------------
// Requests and update
// ---------------------------------------------------------------------------

// The rest of BattleTurnPassed (main:3974-4013): the next turn, or a tie at the
// turn cap.
constexpr Result next_turn(Battle& battle) {
  turn_cleanup(battle);
  battle.request = Request{};
  battle.turn++;
  if (battle.turn >= Battle::MAX_TURNS) {
    return Result::init(Result::Tie, Choice::Pass, Choice::Pass);
  }
  return Result::start();
}

// The player whose Baton Pass waits for its recipient.
constexpr Player baton_passer(const Request& r) { return r.next == 1 ? r.first : foe(r.first); }

// The type of choice p must make now, from the request.
constexpr Choice::Type requested(const Battle& battle, Player player) {
  const Request& r = battle.request;
  switch (r.phase) {
  case Phase::None:
    return battle.turn == 0 ? Choice::Pass : Choice::Move;
  case Phase::BatonPass: return baton_passer(r) == player ? Choice::Switch : Choice::Pass;
  default:
    // A replacement that waits (Request::next) passes.
    if (r.next >= Request::PENDING && r.next < Request::LANDED &&
        index(player) == r.next - Request::PENDING) {
      return Choice::Pass;
    }
    return battle.side(player).get(1).fainted() ? Choice::Switch
                                                : Choice::Pass;
  }
}

constexpr Result checked(const Battle& battle, Result result) {
  assert(battle.valid());
  (void)battle;
  return result;
}

// The end of update's messages: the queued Faints, the winner, a tie or the
// next turn, then None. Only start() and next_turn() return Result::start().
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

// The legal choices for `player` given its request, written to `out`; returns
// how many (at least 1).
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
      // message (bs1:3709), which the decomp skips for a sleeping user and
      // Showdown doesn't (beforeTurnMove, by Speed with its own tie shuffle).
      const Player punch = log_order(battle);
      for (const Player p : {punch, foe(punch)}) {
        if (focusing(battle, p)) {
          protocol::single_turn(options.log, protocol::ident(battle, p),
                                protocol::SingleTurn::FocusPunch);
        }
      }
    } else {
      // Finish Baton Pass's switch-in. A recipient that faints to Spikes ends
      // the actions.
      Request& r = battle.request;
      const Player passer = baton_passer(r);
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
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = replace(battle, c1, c2, options)) || (f = faints(battle, phase))) {
        return finish(battle, *f, options);
      }
    }
    if ((f = end_of_turn(battle, options))) return finish(battle, *f, options);
    [[fallthrough]];
  case Phase::FaintAfterResiduals:
    if (phase == Phase::FaintAfterResiduals) {
      // A replacement can faint to Spikes: replace it before going on.
      if ((f = replace(battle, c1, c2, options)) || (f = faints(battle, phase))) {
        return finish(battle, *f, options);
      }
    }
    return finish(battle, next_turn(battle), options);
  }
  assert(false && "invalid phase");
  return Result::error();
}

} // namespace hoenn
