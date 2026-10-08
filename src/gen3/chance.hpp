#pragma once

#include <cassert>
#include <cstdint>
#include <iterator>
#include <type_traits>

#include "../common/data.hpp"
#include "../common/rational.hpp"
#include "../common/rng.hpp"
#include "data.hpp"

namespace pkmn::gen3 {

// Durations. These are findings from the decomp (pokeemerald, branch
// battle-only); the struct follows the Actions catalogue below.
//
// Durations keeps the *public* side of each hidden duration: how many steps
// the counter has visibly taken. The battle keeps the true counter. Search
// resamples the true counter from Durations before each iteration. Every roll
// is exactly uniform (Random() & 3, & 1 or % 4), so after k ticks with the
// effect still running, the initial roll N is uniform over the values with
// N - k >= 1, and the remaining counter is N - k. (Durations stores k + 1;
// see the struct.)
//
// Paths: bsc = src/battle_script_commands.c, util = src/battle_util.c,
// main = src/battle_main.c.
//
// Effect      Roll  Set at                          Ticks
// ----------  ----  ------------------------------  ---------------------------
// Sleep       2-5   SetMoveEffect (bsc:2481), Yawn  each move attempt that
//                   residual (util:1758)            reaches CANCELER_ASLEEP
// Confusion   2-5   SetMoveEffect (bsc:2541)        each move attempt that
//                                                   reaches CANCELER_CONFUSED
// Disable     2-5   Cmd_disablelastusedattack       every end of turn,
//                   (bsc:8007)                      including the turn it's set
// Encore      3-6   Cmd_trysetencore (bsc:8039)     every end of turn,
//                                                   including the turn it's set
// Binding     3-6   SetMoveEffect (bsc:2618)        every end of turn,
//                                                   including the turn it's set
// Uproar      2-5   SetMoveEffect (bsc:2573)        every end of turn,
//                                                   from the first use
// Thrash      2-3   SetMoveEffect MOVE_EFFECT_THRASH every end of turn,
//                   (bsc:2860)                      from the first use
//
// Sleep (util:2006, CANCELER_ASLEEP).
// - Stored per Pokémon, by SLOT (six per side). The counter is in status1,
//   which persists through switching, and Gen III doesn't reset it.
// - The counter is checked before the move. When it reaches 0 the Pokémon
//   wakes and acts that same turn, so N gives N - 1 turns of being unable to
//   move.
// - Early Bird takes 2 per tick (wakes at attempt ceil(N / 2)). Durations
//   counts counter steps, not attempts, so an Early Bird attempt adds 2 and
//   the posterior above holds unchanged. This relies on perfect information:
//   the battle's ability is the true one.
// - Rest sets 3 (Cmd_trysetrest, bsc:6779). That's public, so a sleep with
//   Status::EXT is never resampled.
// - No tick on turns the Pokémon doesn't attempt a move: it switches, faints
//   first, or was dragged in or brought in mid-turn.
// - Uproar wakes sleepers outright (UproarWakeUpCheck and ENDTURN_UPROAR);
//   that's public.
//
// Confusion (util:2143, CANCELER_CONFUSED).
// - It snaps out when the counter hits 0 and acts that turn, so 1-4 turns of
//   being confused.
// - It only ticks if the attack canceler gets that far. These run first, in
//   order (util:1971): sleep, freeze, Truant, recharge, flinch, choosing a
//   Disabled move, Taunt, Imprison. Any of them stopping the move means no
//   confusion tick that turn. Waking up does not stop it (the canceler
//   resumes after the wake message), so the wake turn does tick.
// - Baton Pass keeps STATUS2_CONFUSION with its counter (main:3175), so the
//   Durations entry moves to the incoming Pokémon. A normal switch clears it.
// - Confusion from the end of Thrash is a fresh 2-5 roll.
//
// Disable (util:1706). It also ends if the Pokémon no longer has the move.
//
// Encore (util:1723). It also ends if the encored move's PP hits 0 or the
// Pokémon no longer has the move.
//
// Binding (util:1591).
// - Tracked on the victim's side.
// - The decrement comes before the damage, so N gives N - 1 = 2-5 turns of
//   chip damage, then a "freed" message on the Nth end of turn.
// - It ends early if the binder switches out (main:3199) or the victim uses
//   Rapid Spin (bsc:8826). The decomp's Baton Pass doesn't pass it;
//   Showdown's does (partiallytrapped has no noCopy), and this follows it.
//
// Uproar (util:1647). 2-5 turns in total. It ends early if the user is unable
// to move (WasUnableToUseMove).
//
// Thrash, Petal Dance and Outrage (util:1673).
// - 2-3 turns in total, then confusion.
// - The lock is only set if the first use passes the accuracy check
//   (BattleScript_EffectRampage). A miss on the first turn starts no lock.
// - It ends early if the user is unable to move.
// - Uproar and the Thrash family both lock the user into its move, so they
//   can't overlap. They can share one "attacking" slot; the public locked
//   move says which one it is.
//
// Nothing to track:
// - Fixed lengths, known from public events: Rest, Bide (2, Cmd_setbide, so
//   unlike Gen I it's not "attacking"), Taunt (2), Yawn (2), Charge,
//   Lock-On/Mind Reader, Perish Song, Future Sight/Doom Desire, Wish,
//   Reflect, Light Screen, Safeguard, Mist, weather from moves (5), Rollout,
//   recharge, Truant, the Toxic counter, Stockpile, Protect's consecutive-use
//   count.
// - Re-rolled every turn, so nothing to remember: thawing from freeze (1 in
//   5), Shed Skin (1 in 3), full paralysis, infatuation, confusion self-hit.

// Actions. The catalogue below lists every Random() site in battle
// code that singles Smogon ADV play can reach, with its multiplicity: the
// most times it can fire in one update. The struct follows it.
//
// Actions has two jobs. As an observation, it is the exact key for a chance
// node's children. Given the battle before the update and both choices, it
// must determine the Result, Durations and all public state. Hashing is out
// (see CLAUDE.md). As a specification (Calc overrides), a nonzero field
// forces that branch. Both jobs use the same struct with the same fields, per
// hit included: anything worth observing is worth forcing, and the reverse.
// Hidden rolls are in neither. To fix a sleep length, set the counter in the
// battle, which holds the true value.
//
// Rules that follow:
// - Draw only when the outcome can change state. Static on a target that is
//   already paralyzed, King's Rock after the target has moved, and Focus
//   Band on a non-lethal hit draw nothing. This doesn't change any
//   distribution; it keeps irrelevant draws out of the key and the
//   probability. The decomp often draws first and checks later (Static);
//   reorder the checks.
// - The decomp's rejection loops (Effect Spore, Sleep Talk, Roar, Starf
//   Berry) are uniform over the accepted values. Draw that uniform directly,
//   with no loop.
// - Hidden rolls (the Durations table above) are never observed when they
//   are rolled. Their *ending* is observed, at the tick where the stored
//   counter says the effect ends or continues.
//
// Out of scope: doubles targeting (util:181-233, 3017, 3838-3875), running
// (util:434-466), obedience (util:3945-4019), Pickup, catching, Metronome
// and Mirror Move's random pick (singles has one candidate), Conversion and
// Conversion 2, OHKO moves (OHKO Clause), and Roar's level check (bsc:7172,
// which only draws when the user is lower level; Smogon is all level 100).
// Present, Magnitude, Psywave and Spite are listed but low priority.
//
// Notation: P is the player whose action makes the draw. The defender's
// Focus Band and contact abilities count toward the attacker's hit.
//
// Turn-level draws (one per update, shared, not per player)
// ---------------------------------------------------------
// Site                    Outcome                     Mult  Notes
// Quick Claw              gRandomTurnNumber (u16)     1     main:4013, read in
//                         < 0xFFFF * 20 / 100                GetWhoStrikesFirst.
//                         (1/5 here, as Showdown)            ONE draw per turn
//                                                            serves both sides,
//                                                            so two holders
//                                                            proc together. Also
//                                                            applies to the end
//                                                            of turn order.
// Speed tie, move order   1/2                         1     main:4728/4745 via
//                                                            SetActionsAnd-
//                                                            BattlersTurnOrder.
//                                                            Battle start
//                                                            (main:3858) uses
//                                                            the same draw.
// Speed tie, end of turn  1/2                         1     ENDTURN_ORDER
//                                                            (util:1209) calls
//                                                            GetWhoStrikesFirst
//                                                            again: an
//                                                            independent re-roll.
//
// Per player, per move execution (at most one per player per update; Sleep
// Talk's called move is part of the same execution)
// ------------------------------------------------------------------------
// Site                    Outcome                     Mult  Notes
// Thaw                    1/5 thaw                    1     CANCELER_FROZEN
//                                                            (util:2048)
// Confusion self-hit      1/2                         1     util:2148. On a
//                                                            self-hit, also one
//                                                            damage roll and
//                                                            P's own Focus Band
//                                                            (adjustnormal-
//                                                            damage2, bsc:1705,
//                                                            1720).
// Full paralysis          1/4                         1     util:2175
// Infatuation             1/2                         1     util:2190
// Sleep Talk slot         uniform over usable slots   1     bsc:8268
// Protect/Detect/Endure   sProtectSuccessRates/65536  1     bsc:6514
// Accuracy                acc/100 after stages        1     bsc:1176. Once per
//                                                            move, multi-hit
//                                                            included.
// Multi-hit count         2,3,4,5: 3/8,3/8,1/8,1/8    1     bsc:7147, two draws
//                                                            folded into one
//                                                            outcome
// Crit                    1/n, n by crit stage        6     bsc:1282, per hit
//                                                            (Beat Up: one
//                                                            per healthy party
//                                                            member, up to 6)
// Damage roll             85-100, 1/16 each           6     bsc:1662, per hit
//                                                            (Beat Up: as
//                                                            Showdown; the
//                                                            decomp has none)
// Focus Band (defender)   10/100, lethal hits only    6     bsc:1677 and 5878
//                                                            (fixed damage),
//                                                            per hit
// Contact ability         Static, Flame Body, Poison  5     util:2774-2844,
//   (defender's)          Point, Cute Charm 1/3.             MOVEEND_ON_DAMAGE_
//                         Effect Spore 1/10, then            ABILITIES, which
//                         sleep/poison/paralysis             the multi-hit loop
//                         at 1/3 each                        runs on every hit
//                                                            (moveendto
//                                                            MOVEEND_NEXT_
//                                                            TARGET)
// King's Rock             10/100                      6     util:3763. Per hit,
//                                                            plus once more
//                                                            after the multi-hit
//                                                            loop (moveendfrom
//                                                            IMMUNITY_ABILITIES
//                                                            re-runs case 12).
//                                                            At most one can
//                                                            matter: a flinch
//                                                            is set once.
// Secondary effect        chance/100 (x2 Serene       1     bsc:2923, once,
//                         Grace)                             after a multi-hit
//                                                            loop too
// Tri Attack's status     burn/freeze/par, 1/3 each   1     bsc:2601
// Roar/Whirlwind target   uniform over the bench      1     bsc:7346
// Spite                   2-5 PP, 1/4 each            1     bsc:8333
// Present, Magnitude,     per decomp                  1     bsc:8615, 8672,
//   Psywave                                                  7936
//
// Per player, end of turn
// -----------------------
// Site                    Outcome                     Mult  Notes
// Future Sight / Doom     accuracy, damage roll,      1     BattleScript_Mon-
//   Desire hit            target's Focus Band                TookFutureAttack.
//                                                            P is the user's
//                                                            side. No crit.
// Shed Skin               1/3                         1     util:2607
// Starf Berry stat        uniform over non-maxed      1     util:3462
//                         stats (Atk-SpD; Showdown
//                         samples the same five)
//
// Per player, hidden durations ending (no draw here, but observed)
// ---------------------------------------------------------------
// Sleep, confusion, Disable, Encore, binding, Uproar and Thrash: at most one
// end-or-continue observation each per player per update, since only one of
// P's Pokémon can tick in an update. (Verify that Sleep Talk's called move
// doesn't re-run the attack canceler and tick twice.)
//
// So the most per player in one update is one move execution with up to six
// hits (five for the multi-hit moves, six for Beat Up), one confusion
// self-hit instead of the move, and one Future Sight hit.

// One damaging hit. A multi-hit move fills hits[0..n-1].
//
// A confusion self-hit shares hits[0] (damage and focus_band only) with the
// move's first hit. That's safe: a self-hit replaces the move, so the two
// never happen in the same update, and Action::confusion == True says which
// one hits[0] holds. The key stays exact.
struct HitDraws {
  Optional crit;       // bsc:1282
  uint8_t damage;      // the roll as a percentage, 85-100 (bsc:1662, 1705)
  Optional focus_band; // the defender's; drawn only on a lethal hit
                       // (bsc:1677, 1720, 5878)
  Optional contact;    // the defender's Static, Flame Body, Poison Point,
                       // Cute Charm or Effect Spore (util:2774-2844). Per hit:
                       // a mid-loop burn changes later damage, and a mid-loop
                       // sleep ends the loop.

  friend constexpr bool operator==(const HitDraws&, const HitDraws&) = default;
};

// The draws that belong to one player's action in an update. Every field is
// 0 when nothing was drawn; otherwise it is the outcome.
struct Action {
  // Before the move (AtkCanceller_UnableToUseMove, util:1971).
  Optional thaw;        // util:2048, True = thawed
  Optional confusion;   // util:2148, True = hit itself
  Optional paralysis;   // util:2175, True = fully paralyzed
  Optional infatuation; // util:2190, True = immobilized

  // The move.
  //
  // spore, tri_attack and variable are sparse: each picks from a small set
  // and is rarely filled. They are separate for clarity for now. They could
  // be combined into one detail byte whose meaning depends on the move or
  // ability.
  uint8_t move_slot;     // Sleep Talk's pick, 1-4 (bsc:8268)
  Optional protect;      // Protect, Detect or Endure succeeding (bsc:6514)
  Optional hit;          // accuracy (bsc:1176), once per move
  uint8_t multi_hit;     // the hit count, 2-5 (bsc:7147)
  HitDraws hits[6];       // MAX_HITS
  uint8_t spore;         // Effect Spore's status: 1 sleep, 2 poison,
                         // 3 paralysis
  Optional kings_rock;   // True = flinched. A flinch is set at most once and
                         // which draw set it doesn't change state.
  Optional secondary;    // the move's secondary effect (bsc:2923)
  uint8_t tri_attack;    // 1 burn, 2 freeze, 3 paralysis (bsc:2601)
  uint8_t switch_target; // the SLOT Roar or Whirlwind drags in, 2-6
                         // (bsc:7346)
  uint8_t spite;         // PP removed, 2-5 (bsc:8333)
  uint8_t variable;      // Present, Magnitude or Psywave's outcome, by move

  // End of turn.
  Optional future_hit; // Future Sight or Doom Desire's accuracy, on the
                       // user's side
  HitDraws future;       // crit stays None: future attacks don't crit
  Optional shed_skin;  // util:2607
  uint8_t starf;       // the stat Starf Berry raises, 1-5: Atk, Def, Spe,
                       // SpA, SpD (util:3462)

  // Hidden durations at their tick: True = ended, False = continued.
  Optional sleep;
  Optional confusion_end;
  Optional disable;
  Optional encore;
  Optional binding;
  Optional uproar;
  Optional thrash;

  friend constexpr bool operator==(const Action&, const Action&) = default;
};

// Draws for the whole turn, each at most once per update.
struct Turn {
  Optional quick_claw;  // 1/5 (Showdown's); one draw serves both
                        // sides (main:4013)
  uint8_t speed_tie;    // move order: 1 = P1 first, 2 = P2 first
  uint8_t residual_tie; // ENDTURN_ORDER's independent re-roll (util:1209),
                        // same encoding

  friend constexpr bool operator==(const Turn&, const Turn&) = default;
};

// The chance branch an update took (Chance's observation), or the branch to
// force (Calc's overrides). One struct serves both: anything worth observing
// is worth forcing, and the reverse. Hidden rolls are in neither; to fix a
// sleep length, set the counter in the battle.
struct Actions {
  Turn turn;
  Action p1;
  Action p2;

  constexpr Action& get(Player p) { return p == Player::P1 ? p1 : p2; }
  constexpr const Action& get(Player p) const {
    return p == Player::P1 ? p1 : p2;
  }

  friend constexpr bool operator==(const Actions&, const Actions&) = default;
};

static_assert(sizeof(HitDraws) == 4);
// The most hits one move execution draws for (Beat Up's six).
inline constexpr uint8_t MAX_HITS = std::size(Action{}.hits);
static_assert(MAX_HITS == 6);
static_assert(sizeof(Action) == 53);
static_assert(sizeof(Actions) == 3 + 2 * 53);
static_assert(std::is_trivial_v<Actions> && std::is_standard_layout_v<Actions>);
static_assert(Actions{} == Actions{});

// The public side of each hidden duration (see the findings at the top),
// with libpkmn's convention: 0 means inactive, 1 means the effect has just
// started (no steps yet), and each step adds 1. Ending resets it to 0. The
// battle holds the true remaining counter; search resamples it from these
// counts.
//
// At a count of c (so c - 1 steps) the original roll N is uniform over
// [max(lo, c), hi] and the remaining counter is N - (c - 1). Ranges below are
// the counts an active effect can show.
struct Durations {
  struct Side {
    // By SLOT, as libpkmn: sleep[0] is always the active Pokémon's. status1
    // persists through switching, so each Pokémon keeps its own count, and a
    // switch to SLOT s swaps sleep[0] and sleep[s - 1] as it swaps order.
    // Steps at each move attempt that reaches CANCELER_ASLEEP: 1, or 2 with
    // Early Bird. Rest's sleep (Status::EXT) is public and never resampled,
    // but its count is kept the same way.
    uint8_t sleep[6];  // 1-5 (roll 2-5)
    // Volatiles on the active Pokémon. A switch clears them, except that
    // Baton Pass keeps confusion and binding with their counts.
    uint8_t confusion; // 1-5 (roll 2-5), steps at CANCELER_CONFUSED
    uint8_t disable;   // 1-5 (roll 2-5), steps every end of turn
    uint8_t encore;    // 1-6 (roll 3-6), steps every end of turn
    uint8_t binding;   // 1-6 (roll 3-6), on the victim's side
    uint8_t attacking; // Uproar 1-5 (roll 2-5) or Thrash 1-3 (roll 2-3). They
                       // can't overlap, and the public locked move says which.

    friend constexpr bool operator==(const Side&, const Side&) = default;
  };

  Side p1;
  Side p2;

  constexpr Side& get(Player p) { return p == Player::P1 ? p1 : p2; }
  constexpr const Side& get(Player p) const {
    return p == Player::P1 ? p1 : p2;
  }

  // A switch on p's side to SLOT `slot` (a switch action, a faint
  // replacement or Baton Pass), as libpkmn's Chance.switched: the sleep
  // counts swap with the SLOTs, the active Pokémon's volatile counts clear,
  // and so does the foe's binding, since p was the binder (main:3199).
  // Baton Pass keeps confusion and, as Showdown, binding (baton_pass_in).
  constexpr void switched(Player p, uint8_t slot, bool baton = false) {
    assert(slot >= 2 && slot <= 6);
    Side& d = get(p);
    const uint8_t out = d.sleep[0];
    d.sleep[0] = d.sleep[slot - 1];
    d.sleep[slot - 1] = out;
    if (!baton) {
      d.confusion = 0;
      d.binding = 0;
    }
    d.disable = 0;
    d.encore = 0;
    d.attacking = 0;
    get(foe(p)).binding = 0;
  }

  friend constexpr bool operator==(const Durations&, const Durations&) = default;
};

static_assert(sizeof(Durations) == 22);
static_assert(std::is_trivial_v<Durations> &&
              std::is_standard_layout_v<Durations>);

// Chance, as libpkmn's Chance(Rational): the probability of the branch an
// update took, the branch itself (Actions) and the public durations.
//
// update() resets `probability` and `actions` when it starts; `durations`
// carry over between updates, so the caller keeps one Chance per battle (and
// copies it along with the battle). The roll helpers in mechanics.hpp write
// these fields directly behind `if constexpr (enabled)` (CLAUDE.md, "Chance",
// Hooks); there are no per-site methods.
template <typename R = Rational<double>> struct Chance {
  static constexpr bool enabled = true;

  R probability;
  Actions actions;
  Durations durations;

  constexpr void reset() {
    probability.reset();
    actions = {};
  }
};

// Search's default: no observation, no cost.
struct NullChance {
  static constexpr bool enabled = false;
};

static_assert(std::is_empty_v<NullChance>);

namespace detail {
// The remaining counter of a hidden effect seen at count c (c - 1 steps
// taken) whose roll is uniform on [lo, hi]: the roll is uniform on
// [max(lo, c), hi], and the counter is the roll minus the steps.
constexpr uint8_t resample(RNG& rng, uint8_t c, uint8_t lo, uint8_t hi) {
  assert(c >= 1 && c <= hi && lo <= hi);
  const uint8_t min = c > lo ? c : lo;
  const uint8_t roll = uint8_t(min + rng.range(uint32_t(hi - min + 1)));
  return uint8_t(roll - (c - 1));
}
} // namespace detail

// Resamples every true hidden counter from its public count, as Oak's
// randomize_hidden_variables, drawing from the battle's RNG. Search calls it
// on a copy of the root before each iteration. A counter whose effect isn't
// active in the battle, or whose count is 0 (untracked), is left alone, and
// Rest's sleep (Status::EXT) is public. The ranges are the table at the top.
constexpr void randomize_hidden(Battle& battle, const Durations& durations) {
  for (const Player p : {Player::P1, Player::P2}) {
    Side& side = battle.side(p);
    const Durations::Side& d = durations.get(p);
    for (uint8_t s = 1; s <= 6; s++) {
      Pokemon& mon = side.get(s);
      const uint8_t c = d.sleep[s - 1];
      if (c != 0 && mon.status.asleep() && !mon.status.self_inflicted()) {
        mon.status = Status::slp(detail::resample(
            battle.rng, c, Status::MIN_SLEEP, Status::MAX_SLEEP));
      }
    }
    if (battle.turn == 0) continue;
    ActivePokemon& a = side.active;
    Volatiles& v = a.volatiles;
    if (d.confusion != 0 && v.confusion != 0) {
      v.confusion = detail::resample(battle.rng, d.confusion, 2, 5);
    }
    if (d.disable != 0 && a.disable_timer != 0) {
      a.disable_timer = detail::resample(battle.rng, d.disable, 2, 5);
    }
    if (d.encore != 0 && a.encore_timer != 0) {
      a.encore_timer = detail::resample(battle.rng, d.encore, 3, 6);
    }
    if (d.binding != 0 && v.wrapped != 0) {
      v.wrapped = detail::resample(battle.rng, d.binding, 3, 6);
    }
    if (d.attacking != 0) {
      assert(v.uproar == 0 || v.thrash == 0);
      if (v.uproar != 0) {
        v.uproar = detail::resample(battle.rng, d.attacking, 2, 5);
      } else if (v.thrash != 0) {
        v.thrash = detail::resample(battle.rng, d.attacking, 2, 3);
      }
    }
  }
}

// The battle with what Chance's observation doesn't determine taken out: the
// RNG seed, and every true hidden counter that randomize_hidden resamples
// (or that an infliction rolls) set to 1 if active. What's left, with the
// Durations, is the state that the choices and the Actions must determine
// (CLAUDE.md, "Chance"; tools/check_chance.cpp tests it). Keep this list the
// same as randomize_hidden's.
constexpr void mask_hidden(Battle& battle) {
  battle.rng = {};
  for (Side& side : battle.sides) {
    for (Pokemon& mon : side.pokemon) {
      if (mon.status.asleep() && !mon.status.self_inflicted()) {
        mon.status = Status::slp(1);
      }
    }
    ActivePokemon& a = side.active;
    Volatiles& v = a.volatiles;
    // No pointers to the members: packing will make them bitfields.
    const auto mask = [](uint8_t c) { return uint8_t(c != 0); };
    v.confusion = mask(v.confusion);
    v.wrapped = mask(v.wrapped);
    v.uproar = mask(v.uproar);
    v.thrash = mask(v.thrash);
    a.disable_timer = mask(a.disable_timer);
    a.encore_timer = mask(a.encore_timer);
  }
}

static_assert([] {
  // Seen asleep at count 4 (three steps): the roll was 4 or 5, so 1 or 2
  // turns remain.
  RNG rng{1};
  for (int i = 0; i < 100; i++) {
    const uint8_t r = detail::resample(rng, 4, 2, 5);
    if (r < 1 || r > 2) return false;
  }
  // Just started: the full range.
  bool seen[7] = {};
  for (int i = 0; i < 200; i++) seen[detail::resample(rng, 1, 3, 6)] = true;
  return !seen[2] && seen[3] && seen[4] && seen[5] && seen[6];
}());
static_assert([] {
  Durations d{};
  d.p1.sleep[0] = 3;
  d.p1.sleep[3] = 1;
  d.p1.confusion = 2;
  d.p2.binding = 4;
  d.switched(Player::P1, 4);
  return d.p1.sleep[0] == 1 && d.p1.sleep[3] == 3 && d.p1.confusion == 0 &&
         d.p2.binding == 0;
}());

} // namespace pkmn::gen3
