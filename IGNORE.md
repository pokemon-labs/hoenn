## Strategy: replace Emerald's battle scripts with direct code

Emerald drives battles with a bytecode VM: scripts in
`data/battle_scripts_*.s`, a program counter (`gBattlescriptCurrInstr`), one
`Cmd_*` handler per opcode in `src/battle_script_commands.c`, and a stack of
script pointers for `call`/`return`. It exists so the GBA can pause mid-turn
for text and animation. We don't port it.

- **Write each mechanic as ordinary functions, as libpkmn does.** Read a
  script as the spec for the order of checks, state changes and RNG calls.
  Drop presentation-only opcodes (`waitmessage`, `waitanimation` and so on).
- **A script VM would break the safety rules:** its state is a program counter
  and a stack of pointers, and `Battle` holds no pointers.
- **Suspend only at real decisions** (Baton Pass and faint replacements).
  `Battle.request` records where `update` resumes. Keep the turn phases
  explicit and few, so nothing needed to resume lives in a C++ stack frame.
- **Shared tails become helpers.** Entry points that scripts jump into
  (`BattleScript_HitFromAccCheck` and the like) become functions several
  effects call.

The scripts themselves are easy (615 labels, explicit branches). The hard
part is control flow hidden in C: 313 direct assignments to
`gBattlescriptCurrInstr` (each handler picks its successor), 91
`BattleScriptPush` calls injecting subroutines, re-entrant state machines
(`moveend`, `gBattleMainFunc`), global control flags (`gHitMarker`,
`gMoveResultFlags`), and `various` with 27 subcommands.

**Approach, one mechanic at a time, limited to the competitive pool:**

1. Trace the effect's script path. Read every `Cmd_*` handler it reaches,
   noting where each can send the program counter and which global flags it
   reads or sets.
2. Rewrite the path as one direct function that cites the decomp lines.
3. Optionally (not built): a tool that decodes the script bytes and dumps
   every branch target and called script per label. It can't translate (the
   semantics are in the C handlers), but it gives a checklist and groups the
   effects by backbone, so it's worth building before step 4 of the plan.

Write mechanics analytically: understand the whole mechanic from the decomp,
then write one code path. Don't grow code test-by-test; tests check the
result, they don't drive the design.



**Is realizability enough for "good enough for search and RL"?** (asked
2026-10-08; answer recorded, nothing decided). No, but it's the right core.
It checks only that Showdown's outcome is in our support. Search, unlike RL,
doesn't average bugs away: it steers into them and exploits them. Gaps, most
important for search first:

1. **Support, not probability.** A wrong rate (a 30% secondary instead of
   10%, a skewed sleep length) passes. Search weights chance nodes by it.
   Compare branch frequencies with `probability`, ours and Showdown's.
2. **Damage hides in the 16 rolls.** An error of a few points usually still
   matches some roll, at the wrong index, which moves KO thresholds.
   `check_damage` covers only 74 fixed cases, not in-battle inputs.
   *(unverified: no example produced)*
3. **Random branch search** can't tell a rare branch from a bug. Fix for 2
   and 3: the trace script records Showdown's public draws (roll, crit,
   secondaries) by wrapping its PRNG, tagged by call site, and the checker
   forces them through Calc. Hidden counters stay sampled. This would
   revise the "search at random" decision.
4. **One direction only.** Branches we have and Showdown doesn't (a move
   that works where it should fail) are what search exploits. A statistical
   version of the reverse test is cheaper than replaying into Showdown.
5. **Random choices miss the states search steers to** (Perish traps, Baton
   Pass chains, Substitute stalls, berry thresholds, endgames). Add traces
   with one side driven by a cheap policy.
6. **The log hides state** (PP, toxic counter, Protect's count, last-move
   fields, Counter's damage, Truant). Keep the state guard alongside the
   log comparison rather than replacing it; it found the Sleep Talk PP bug.
7. **`randomize_hidden`'s distribution is untested.** Realizability passes
   a resampler that's too loose. Over self-play, compare the true counter
   given `Durations` with what `randomize_hidden` samples.

Suggested order: 1, 3, 6, 7, 5, 4.

Gap 1, our side: done in `check_chance` (2026-10-08, see the chance-key
test above). Showdown's side: wrap `random`, `randomChance`, `sample` and `shuffle` to
multiply a per-update probability. Caveat (the user's): Showdown's
probability is per draw path, ours per `Actions` key, and many paths map to
one observable outcome: a `shuffle` of tied speeds whose orders look alike,
draws we skip because they can't matter, two rolls giving the same damage.
So compare at the level of the observed outcome (the log chunk), by
frequency over many seeds from one state, not path by path.
