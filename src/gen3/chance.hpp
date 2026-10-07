#pragma once

#include <cstdint>
#include <type_traits>

namespace pkmn::gen3 {

// TODO: Durations. These are findings from the decomp (pokeemerald, branch
// battle-only); nothing below is implemented yet.
//
// Durations keeps the *public* side of each hidden duration: how many times
// the counter has visibly ticked. The battle keeps the true counter. Search
// resamples the true counter from Durations before each iteration. Every roll
// is exactly uniform (Random() & 3, & 1 or % 4), so after k ticks with the
// effect still running, the initial roll N is uniform over the values with
// N - k >= 1, and the remaining counter is N - k.
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
// - Stored per Pokémon (six slots per side). The counter is in status1, which
//   persists through switching, and Gen III doesn't reset it.
// - The counter is checked before the move. When it reaches 0 the Pokémon
//   wakes and acts that same turn, so N gives N - 1 turns of being unable to
//   move.
// - Early Bird takes 2 per tick (wakes at attempt ceil(N / 2)). Dodrio,
//   Natu/Xatu, Ledian and the Seedot line may or may not have it, so the
//   ability can be hidden. Resampling the ability and the sleep counter has
//   to stay jointly consistent with what was seen.
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
//   Rapid Spin (bsc:8826). Baton Pass doesn't pass it.
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
//                         = 13107/65536                      ONE draw per turn
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
// Crit                    1/n, n by crit stage        5     bsc:1282, per hit
// Damage roll             85-100, 1/16 each           5     bsc:1662, per hit
// Focus Band (defender)   10/100, lethal hits only    5     bsc:1677 and 5878
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
//                         stats (Atk-SpD, Acc, Eva)
//
// Per player, hidden durations ending (no draw here, but observed)
// ---------------------------------------------------------------
// Sleep, confusion, Disable, Encore, binding, Uproar and Thrash: at most one
// end-or-continue observation each per player per update, since only one of
// P's Pokémon can tick in an update. (Verify that Sleep Talk's called move
// doesn't re-run the attack canceler and tick twice.)
//
// So the most per player in one update is one move execution with up to five
// hits, one confusion self-hit instead of the move, and one Future Sight hit.

// libpkmn's Optional(bool). None means no draw happened (observation) or
// roll normally (specification).
enum class Optional : uint8_t { None = 0, True = 1, False = 2 };

// One damaging hit. A multi-hit move fills hits[0..n-1]. A confusion
// self-hit uses hits[0] (damage and focus_band only); Action::confusion says
// which it is. The two can't happen in the same update.
struct Hit {
  Optional crit;       // bsc:1282
  uint8_t damage;      // the roll as a percentage, 85-100 (bsc:1662, 1705)
  Optional focus_band; // the defender's; drawn only on a lethal hit
                       // (bsc:1677, 1720, 5878)
  Optional contact;    // the defender's Static, Flame Body, Poison Point,
                       // Cute Charm or Effect Spore (util:2774-2844). Per hit:
                       // a mid-loop burn changes later damage, and a mid-loop
                       // sleep ends the loop.

  friend constexpr bool operator==(const Hit&, const Hit&) = default;
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
  uint8_t move_slot;     // Sleep Talk's pick, 1-4 (bsc:8268)
  Optional protect;      // Protect, Detect or Endure succeeding (bsc:6514)
  Optional hit;          // accuracy (bsc:1176), once per move
  uint8_t multi_hit;     // the hit count, 2-5 (bsc:7147)
  Hit hits[5];
  uint8_t spore;         // Effect Spore's status: 1 sleep, 2 poison,
                         // 3 paralysis
  Optional kings_rock;   // True = flinched. A flinch is set at most once and
                         // which draw set it doesn't change state.
  Optional secondary;    // the move's secondary effect (bsc:2923)
  uint8_t tri_attack;    // 1 burn, 2 freeze, 3 paralysis (bsc:2601)
  uint8_t switch_target; // Roar or Whirlwind's team slot, 1-6 (bsc:7346)
  uint8_t spite;         // PP removed, 2-5 (bsc:8333)
  uint8_t variable;      // Present, Magnitude or Psywave's outcome, by move

  // End of turn.
  Optional future_hit; // Future Sight or Doom Desire's accuracy, on the
                       // user's side
  Hit future;          // crit stays None: future attacks don't crit
  Optional shed_skin;  // util:2607
  uint8_t starf;       // the stat Starf Berry raises, 1-7 (util:3462)

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
  Optional quick_claw;  // gRandomTurnNumber < 13107; one draw serves both
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

  friend constexpr bool operator==(const Actions&, const Actions&) = default;
};

static_assert(sizeof(Hit) == 4);
static_assert(sizeof(Action) == 49);
static_assert(sizeof(Actions) == 3 + 2 * 49);
static_assert(std::is_trivial_v<Actions> && std::is_standard_layout_v<Actions>);
static_assert(Actions{} == Actions{});

} // namespace pkmn::gen3
