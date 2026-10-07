#pragma once

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

} // namespace pkmn::gen3
