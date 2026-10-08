# rewrite: a libpkmn-style Gen III (ADV) engine in C++

This is an attempt to replace ADV-Simulator as birch's engine. It follows
libpkmn's design choices (a small POD battle, one `update` per turn, the
choice/result protocol, and the Chance and Calc options), but it's written in
C++ and doesn't chase every saved bit.

**It's a header-only C++ library, used directly by a C++ driver (birch).**
There is no C API and no compiled library: no `pkmn.h`, no `extern "C"`, no
`pkmn_gen3_*` names. birch adapts its search to this interface, not the
reverse.

**Be cautious, and don't push ahead.** While a design choice is still open,
don't offer to write, add or commit the thing it affects. Lay out the open
choices and their tradeoffs, give a recommendation, and wait for the user to
decide. Help the user be careful: point out assumptions and unverified
claims rather than smoothing over them.

**Singles only.** The engine is for 1v1 singles battles (Smogon ADV OU).
Skip the decomp's doubles-only branches (ally targeting, spread damage,
`gBattlersCount > 2`, Follow Me, Helping Hand) rather than carrying them, and
don't size or shape anything for them.

**Keep this file current and lean.** It is loaded every session. Record
decisions and findings where they belong (layout decisions and deviations in
`src/gen3/README.md`, details in header comments) and keep only the working
rules, the turn frame, the mechanics checklist and the status here. When code
changes, update what this file says about it.

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

## Priorities, in order

1. **Safety.** Week-long paid RL runs must not die or be quietly corrupted.
   - `update` never throws, allocates or recurses without a bound. Given
     valid input, every array index is in range by construction.
   - **Don't hold it wrong, as libpkmn** (decided). The caller passes only
     choices from `choices()` and only battles that are `valid()`. Debug
     asserts both; release checks neither, so bad input is UB there and
     correct input pays nothing for checks. `update` doesn't return
     `Result::Error` for bad input.
   - Every loop has a fixed bound. After `turn += 1`, `update` returns `Tie`
     once the turn reaches 1000 (libpkmn `gen1/mechanics.zig:1650`).
   - `Battle` is trivially copyable, with `static_assert`s on its size, and
     holds no pointers, strings or containers.
   - Debug and test builds run under ASan and UBSan with `assert` on, and
     that's where misuse and engine bugs are caught. Release builds have no
     asserts.
   - Assert liberally in mechanics, as libpkmn does: preconditions,
     invariants (`valid()`), unreachable branches, the input checks above.
2. **Speed.** Search copies and steps the battle millions of times.
   - A battle of roughly 700 B, copied with `memcpy`.
   - A 64-bit RNG seed inside the battle (SplitMix64, `common/rng.hpp`).
   - `constexpr` data tables, no virtual calls.
   - Log, Chance and Calc are template parameters whose null versions
     compile to nothing.
   - For reference: Oak/libpkmn Gen I runs about 76k MCTS iterations/s,
     ADV-Simulator about 19k.
3. **Correctness where competitive play reaches it.** Obscure interactions
   that the Smogon ADV teams in `teams/` never produce can wait.

**Data covers the full game; mechanics take the shortcuts.** `Battle`,
`Actions`, `Durations`, the tables and the enums leave room for every move,
ability, item and random draw (Spite, Present, Magnitude, Psywave…), so an
unwritten mechanic is a gap in `update`, not a missing field that changes the
layout.

Fuzzing matters less than in libpkmn: we promise no crashes and correct
competitive play, not cartridge-exact behaviour for every input.

## Mechanics references

- **Read Emerald, test against Showdown.** Write mechanics from the decomp
  (`pokeemerald/`); tests check behaviour against Showdown. Record
  disagreements in `src/gen3/README.md`.
- **Link battles are the default branch.** Where the decomp branches on
  `BATTLE_TYPE_LINK` (or Frontier and the like), take the link path. Where
  Showdown departs from it anyway, match Showdown and record the exception in
  `src/gen3/README.md`. Decide each branch on its own. Known exceptions: Knock
  Off (README decision 8).
- **Ignore libpkmn's `showdown` mode switch.** Take libpkmn's structure and
  conventions, and the mechanics from the decomp.
- **Useful decomp files:** `src/battle_main.c` (turn order),
  `src/battle_util.c` (end of turn, abilities, items),
  `src/battle_script_commands.c`, `data/battle_scripts_1.s`. Cite them as
  main:N, util:N, bsc:N, bs1:N.
- **ADV-Simulator** (`../ADV-Simulator`) shows what has to be handled and is
  a differential oracle in testing.
- **Don't write mechanics from Showdown's source.** Use it for Smogon's
  clauses and for the team format, which aren't cartridge mechanics.
  Exception, decided: the damage formula follows Showdown's arithmetic
  (`src/gen3/README.md`, "Damage").

## Interface (libpkmn's shape)

Keep libpkmn's shape, as C++ templates and plain functions. Names and
signatures are ours. birch still runs on ADV-Simulator, in two ways (both
under `../cpp/include/`):
- **Its C-style API** (`pkmn_gen3_*`): `search/mcts.h`, `search/data.h`,
  `search/poke-engine-evaluate.h`, `gen3/parse.h`, `util/`. Search calls
  update, choices, reseed and `randomize_hidden`. It keys chance nodes on the
  raw `Actions` bytes (`Obs`, sized `PKMN_GEN3_CHANCE_ACTIONS_SIZE`), and its
  damage-roll clamping writes override bytes 0 and 8.
- **Its C++ `::Battle` object API** (`get_stats()`, `team.active()`...):
  the encoders in `encode/`, `nn/`, `py/` and `train/`. These read
  ADV-Simulator's durations as 12 bytes per player; ours are 11.

- **`update(battle, c1, c2, options) -> Result`** runs one turn, or the rest
  of a turn after a mid-turn request. At turn 0 both players pass and
  `update` starts the battle (see [Turn phases](#turn-phases)).
- **`choices(battle, player, request, out) -> n`** lists the legal choices,
  at most 9.
- **Choice** is one byte, `{type: Pass | Move | Switch, data}`. A move's data
  is the move slot 1–4, or 0 for Struggle or a forced continuation (charging,
  rampage, Bide). A switch's data is a SLOT, 2–6. Pass's data is 0.
- **SLOT is order-dependent, as in libpkmn.** `Side::pokemon[6]` never moves;
  `Side::order[6]` holds 1-based indexes into it, by SLOT, so SLOT `s` is
  `pokemon[order[s - 1] - 1]` (`Side::get(s)`) and SLOT 1 is active. A switch
  to SLOT `s` swaps `order[0]` and `order[s - 1]`. Per-Pokémon data that
  Chance or the interface exposes is indexed by SLOT (`Durations.sleep`, Roar's
  target). The log's `ID` carries the original party position. "Move slot"
  (1–4) is a different thing.
- **Result** is one byte, `{type: None | Win | Lose | Tie | Error, p1, p2}`,
  where `p1` and `p2` are the Choice type each player must make next.
  `Error` isn't for bad input (see "Safety"); it's kept for a full log
  buffer (open, below). A faint
  replacement is `Switch` for one side and `Pass` for the other; a Baton Pass
  replacement is a mid-turn `Switch`.
- **Options** are `{log, chance, calc}`, each a real or a null type.
- **The log comes last**, after the mechanics. Search runs with the null log.
  The log is the source of public information for imperfect-information use.
  Keep the `Log` parameter so adding it changes no signatures. Open: what
  `update` does with a full buffer (libpkmn returns `Result::Error`).

### Chance

**Chance assumes perfect information** (open teams). So `Actions` may record
outcomes the cartridge never shows (a Focus Band that didn't activate).
Imperfect-information signals will come from the log.

The battle stores the **true hidden state**: sleep, confusion, Disable,
Encore, binding, rampage and Uproar lengths are rolled at infliction (ranges
from the decomp) and kept as remaining-turn counters. `Chance` keeps the
**public** side:

- **`Durations`**: per hidden effect, how many steps its counter has visibly
  taken (0 inactive, 1 just started, +1 per step). Six sleeps per side by
  SLOT, plus confusion, Disable, Encore, binding and attacking. They count
  counter steps, not move attempts: an Early Bird attempt adds 2.
- **`Actions`**: which random branch this update took, one `Action` per
  player plus a shared `Turn` (Quick Claw, speed ties). `chance.hpp`
  catalogues every reachable `Random()` site with its multiplicity.
- **`probability`**: the probability of the branch taken, a
  `Rational<double>`. Decided: double, so no overflow bounds have to be
  derived for integer fractions; past 2^53 it loses precision instead of
  failing.

**No RNG frame fidelity** (decided). The engine has to be good enough for RL
in OU, not frame-exact, so there's no libpkmn pending/commit: draw whenever
it's convenient, but a draw goes in `Actions` and `probability` only when its
outcome can matter (Quick Claw with no holder, Static on a paralyzed target).

**Hooks** (decided): one roll helper per catalogue row, in `mechanics.hpp`
(libpkmn's `Rolls`), does the Calc override, the RNG draw and the Chance
record, all in one place. It touches `actions`, `durations` and
`probability` directly under `if constexpr (Chance::enabled)`. The null
types are only `{ static constexpr bool enabled = false; }`. Writing an
`Actions` field that's already set asserts. `randomize_hidden` (resampling
the hidden counters from `Durations`, which birch's search calls) lives in
`chance.hpp`.

**The observation must determine the branch exactly. No hashing.** Search
keys chance-node children by `Actions`, so given the battle and both
choices, `Actions` must determine the `Result`, the resulting `Durations` and
all public state (only the true hidden counters may differ; search resamples
them from `Durations`, as Oak's `randomize_hidden_variables`). Every draw that
can affect public state gets its own field, sized for its multiplicity in one
update (a multi-hit move rolls crit and damage per hit). Overflowing a field
is a bug: assert on it.

Rejected: storing elapsed durations and re-rolling "wakes this turn?" each
turn (less faithful, and pushes probability bookkeeping into the engine).

### Calc

As libpkmn's `calc.zig`. **Overrides** are an `Actions`: a nonzero field
forces that branch, 0 rolls normally. Hidden rolls are in neither; to fix a
sleep length, set the counter in the battle. **`Summaries`** are on hold.

**`min_ko_roll(battle, player, slot, crit) -> 0..15, or 16`**: the smallest
damage-roll index (0 = 85%, 15 = 100%) at which one hit from that move slot
KOs the foe's active Pokémon, via the normal damage path without side
effects. Ignores multi-hit; fixed-damage moves return 0 or 16.

## Layout

Mirrors libpkmn's `src/` tree, all headers. Partly written:
`gen3/mechanics.hpp` (the frame and the hit backbone) and `gen3/calc.hpp`
(overrides only). Scenario tests: only the few in `check_frame`.

**Build** (CMake, tests and tools only): the library is the `INTERFACE`
target `pkmn::gen3` (include path `src/`, C++20). `cmake --preset debug`
(asserts, ASan, UBSan) or `release` (`-O3`, no asserts), then
`cmake --build --preset X` and `ctest --preset X`. Tests build with
`-Wall -Wextra -Wpedantic -Werror`:
- `headers`: every header compiles alone (so its `static_assert`s run), and
  all are linked into one binary (no duplicate definitions).
- `check_data`: `static_assert`s on the generated tables.
- `check_teams`: parses `PKMN_TEAMS_DIR` (default `../teams/adv`); every
  team must parse (`--expect NAME` would allow a named team to fail).
- `check_damage`: `damage.hpp` against Showdown 0.11.10, from the committed
  `tools/damage_cases.inc`. Regenerate it with `node
  tools/damage_oracle.cjs > tools/damage_cases.inc` (needs ADV-Simulator's
  pinned Showdown, or its path as the argument).
- `check_frame`: the turn frame (start, order, Quick Claw and speed-tie rolls
  with Chance and Calc, trapping, faint requests and resuming, the turn cap),
  a few step-3 scenarios (a forced plain hit and its probability, PP, an
  immunity, Focus Band, Struggle, Spikes, a replacement fainting to Spikes,
  a double KO), then a random self-play soak over the team files.

```
CMakeLists.txt, CMakePresets.json
src/common/
  data.hpp            Player, ID, Optional, Choice, Result
  rng.hpp             SplitMix64 RNG and its draw helpers
  options.hpp         Options<Log, Chance, Calc>
  rational.hpp        exact probabilities (libpkmn's Rational)
  protocol.hpp        log plumbing: Writer, Log, NullLog (messages last)
src/gen3/
  README.md           layout decisions (numbered), deviations, field map
  data.hpp            Status, Phase, Request, Pokemon, Volatiles,
                      ActivePokemon, Side, Field, Battle with valid() and
                      size asserts; calc_stats, hidden_power, max_pp
  data/               generated tables (tools/generate.py), plus the
                      hand-written natures.hpp and tables.hpp
  chance.hpp          Random() catalogue, draft Actions and Durations,
                      Chance and NullChance, randomize_hidden
  helpers.hpp         helpers::Pokemon -> Battle at turn 0 (libpkmn helpers)
  showdown.hpp        Showdown export parser -> helpers::Pokemon
  damage.hpp          Hit, crit_stage, can_crit, matchup, damage, apply_roll
  mechanics.hpp       update(), choices(), the roll helpers   (the frame)
  calc.hpp            Calc and NullCalc; min_ko_roll                (todo)
tools/
  generate.py         one-off data generator from pokeemerald/
  check_data.cpp      static_asserts on the generated tables
  check_teams.cpp     parses teams/adv/ and builds a battle from each
  check_damage.cpp    damage.hpp against damage_cases.inc
  check_frame.cpp     the turn frame and step 3, plus a self-play soak
  damage_oracle.cjs   writes damage_cases.inc from Showdown
```

`pokeemerald/` is a depth-1 clone of pret/pokeemerald on the local branch
`battle-only` (`a23e37a`), stripped to battle code, not pushed, ignored by
this repo.
`showdown/` is a depth-1 clone of smogon/pokemon-showdown master (`c046106`,
0.11.11), ignored by this repo; cite it by path. `check_damage` still uses
ADV-Simulator's pinned 0.11.10.

## Turn phases

From `gBattleMainFunc` (`battle_main.c`).

**Start (turn 0), as libpkmn.** A built battle is at turn 0 with no active
Pokémon (`Battle::valid()` enforces: no active Pokémon exactly at turn 0).
The first `update` (both Pass) switches in SLOT 1 on each side and ends at
turn 1 (libpkmn `gen1/mechanics.zig`, `start`). Emerald also runs
`TryDoEventsBeforeFirstTurn` here: switch-in abilities (Intimidate, Sand
Stream, Drizzle) and items, possibly in speed order with a tie draw, and the
first Quick Claw draw. Untraced.

A turn runs:

1. Selection. `gRandomTurnNumber` (Quick Claw) is drawn at the end of the
   previous turn (main:4013).
2. Order (`SetActionsAndBattlersTurnOrder`), then Focus Punch setup.
3. The actions (`RunTurnActionsFunctions`). Each action's script ends in
   `end`, which sets `B_ACTION_TRY_FINISH` (bsc:3957), which runs
   `HandleFaintedMonActions` (util:638) before the next action.
4. `BattleTurnPassed` (main:3956): `DoFieldEndTurnEffects`, then
   `DoBattlerEndTurnEffects`, then `HandleFaintedMonActions`, then
   `HandleWishPerishSongOnTurnEnd` (Future Sight, then Perish Song), with
   faint replacement after each Future Sight or Perish Song event.

`update` suspends wherever a Pokémon can faint, plus Baton Pass (`Phase` in
`data.hpp`):

| Phase | Who chooses | Resume at |
|---|---|---|
| `BatonPass` | user's side `Switch`, other `Pass` | finish Baton Pass's switch-in, then the remaining action |
| `FaintAfterAction` | either side or both | end of turn (the remaining action was cancelled) |
| `FaintAfterResiduals` | either side or both | Future Sight |
| `FaintAfterFutureSight` | either side or both | Perish Song |
| `FaintAfterPerish` | either side or both | the next turn |

`Request` holds the phase, the next action (Baton Pass only), who moves first
(decided once at selection, after the Quick Claw and speed-tie draws, which
must not be redrawn) and each player's queued action resolved at selection
(Encore, Choice Band and Struggle applied, as `gChosenMoveByBattler`), and
the turn's Quick Claw draw. Residual faints don't interrupt the residual
passes. Per-turn state isn't in `Battle` (README decision 2).

**The frame is one `switch (request.phase)`** (decided), whose cases fall
through in turn order, so the order is written once: `None` (select, order,
Focus Punch, actions from 0), `BatonPass` (finish the switch-in, actions from
`request.next`), `FaintAfterAction` (end of turn), `FaintAfterResiduals`
(Future Sight), `FaintAfterFutureSight` (Perish Song), `FaintAfterPerish`
(next turn). A resumed case first does its replacement. This relies on
`Phase` being declared in that order: `static_assert` it. Internal steps
return `std::optional<Result>`: empty continues, a value (a request or the
end of the battle) is returned.

## Tricky mechanics

The mechanics checklist. **Expand it as we go:** for each item record the
decomp source, the decision and the covering test. *(verify)* marks
unchecked claims.

- **Spikes** (`Cmd_switchineffects`, bsc:5180; `spikes()`, tested in
  `check_frame`). Up to 3 layers: 1/8, 1/6, 1/4 of max HP. Flying and
  Levitate are immune. Applies on every switch-in: voluntary, faint
  replacement, Baton Pass, Roar/Whirlwind drag-in. It comes before the
  switch-in abilities and items, which a Spikes faint skips
  (`tryfaintmon_spikes`, bs1:3394); a replacement that faints to it raises
  another request in the same phase. When
  both sides replace at once, `HandleFaintedMonActions` goes in battler-ID
  order, not speed order *(verify against Showdown)*.
- **Effect order.**
  - Turn order: switches, then priority, then Quick Claw (one u16 per turn,
    `gRandomTurnNumber`, serves both sides), then speed with a random tie.
    `GetWhoStrikesFirst` and `SetActionsAndBattlersTurnOrder` (main).
  - Two switches go in speed order with a random tie, as Showdown (decided;
    the decomp uses battler-ID order, so record it in the README). The order
    matters: the first switch-in's Intimidate hits the foe that's leaving.
  - Quick Claw is drawn lazily, at the first read where it can change the
    order, and kept in `Request` (the padding byte) so the end-of-turn order
    reuses it across a request (decided). Two holders share the one draw, so
    both proccing is a speed tie. *(verify against Showdown, which may roll
    per holder)*
  - End of turn re-runs `GetWhoStrikesFirst` (`ENDTURN_ORDER`, util:1209):
    an independent speed-tie draw, the same Quick Claw value, and the
    chosen moves' priority (`ignoreChosenMoves` is FALSE). *(verify)* Which
    move it reads after a mid-turn replacement: `moves[chosenMovePosition]`
    of whoever is active.
  - `DoFieldEndTurnEffects`: screens, Mist, Safeguard, Wish (util:1173),
    weather and so on. `DoBattlerEndTurnEffects`, in speed order: Ingrain,
    Leftovers, Leech Seed, poison, burn, Nightmare, Curse, binding, Uproar,
    rampage, Disable, Encore, Taunt, Yawn and so on.
  - Then faint replacement, Future Sight, Perish Song (see Turn phases).
  - Encode the order once, as a table or switch, not spread across handlers.
- **A faint "ends the turn"** (Gen IV differs). A Pokémon fainting during the
  actions is replaced at once and every queued action that turn is cancelled:
  `BattleScript_FaintedMonSendOutNew` (bs1:2883) ends with
  `cancelallactions` (bsc:5850), setting actions to
  `B_ACTION_CANCEL_PARTNER` = `B_ACTION_FINISHED`. So a faster Explosion, or
  a switch-in dying to Spikes, denies the foe's move. The replacement takes
  that turn's residuals. Protocol: `Switch` for the fainted side, `Pass` (or
  `Switch`) for the other.
  - *(verify)* A link double KO goes through
    `BattleScript_LinkHandleFaintedMonMultiple` (via `checkteamslost`,
    bsc:3582), which has no `cancelallactions`. If both faint on the first
    action, does the replacement use the fainted Pokémon's queued move?
    `HandleAction_UseMove` checks only absent flags copied at turn start.
- **Residual faints** are replaced after both residual passes, so the
  replacement skips that turn's residuals. Future Sight and Perish Song still
  follow.
- **The battle ends mid-residuals.** `checkteamslost` sets `gBattleOutcome`
  as soon as a side is out (bsc:3564–3577); the weather loop exits on it
  (bs1:3219) and `BattleTurnPassed` runs residuals only while it's 0
  (main:3961). If both last Pokémon would die to residuals, the faster dies
  first and its side loses. Audit Explosion, Self-Destruct, Destiny Bond,
  recoil and Perish Song the same way *(verify tie rules against Showdown)*.
- **Baton Pass** has its own switch-in (main:3158–3217, bsc:4648; README
  decision 5).
  - Passes: stat stages; confusion (with its counter), Focus Energy,
    Substitute and its HP, escape prevention, Curse, Leech Seed, Lock-On,
    Perish Song and its timer, Ingrain, Mud Sport, Water Sport.
  - Doesn't pass: the toxic counter, the rest of the disable struct, the
    last-move fields.
  - If the passer had locked on to the foe, the foe's lock resets to 2
    (main:3178–3186) *(verify against Showdown)*.
  - It doesn't cancel the foe's queued action: the request resumes at it.
- **Sleep Talk** picks a random usable move other than itself (a
  `move_slot` chance action); the ban list is `Extra::NoSleepTalk` plus
  `CheckMoveLimitations` ignoring PP. The called move uses no PP. Fails if
  nothing can be called. Only usable asleep; the sleep counter keeps ticking.
  Encore shouldn't redirect the called move *(verify)*. Whether the called
  move re-runs the attack canceler (a second sleep step) is unverified.
- **Roar and Whirlwind.** Priority −6. The replacement is a random
  non-fainted bench Pokémon (a chance action, no request). Fails with no
  bench, against Suction Cups, against Ingrain. The dragged-in Pokémon takes
  Spikes and doesn't act. Protect and Substitute interaction *(verify)*.
- **Sleep Clause and Freeze Clause** are Smogon rules, always on, so
  compile-time constants with no field. A sleep move fails if the foe already
  has a Pokémon put to sleep by this player; Rest (`Status::EXT`) doesn't
  count; Yawn does *(verify)*. Freeze Clause likewise (every freeze in
  singles comes from the foe).

To add when they come up: Pursuit on a switching target, Choice Band
locking, Focus Punch, Truant, Destiny Bond and Perish Song endgames,
Self-Destruct and Explosion halving Defense, Struggle, running out of PP.

## Plan

**Soften the nut:** build the shared structures first, so each effect
becomes a few lines. These are structures, not steps:

- **The turn frame:** the phases above as an explicit table, with suspend
  and resume settled once.
- **The hit backbone:** `BattleScript_EffectHit` (canceler, accuracy, PP,
  crit, damage, type calc, adjust, hit, secondary, faint check, `moveend`),
  with its entry points as helpers.
- **The dispatchers:** `moveend`'s cases, `AbilityBattleEffects`,
  `ItemBattleEffects`, the attack canceler and the end-of-turn order, each
  one ordered list.
- **The roll helpers:** one per catalogue row (Calc override, RNG, Chance
  report), as libpkmn's `Rolls` (see "Chance", Hooks).
- **The checks:** `Battle::valid()` asserted after every `update` in debug,
  plus soak runs.

**Plan steps, in order.** "Step N of the plan" and the `TODO(plan step N)`
markers in the code mean this list. Don't rush step 3 because "birch runs"
feels like progress; every later mechanic inherits the frame.

1. The pure calculation functions. **Done** except checking `calc_stats`
   against ADV-Simulator over `teams/`. `damage.hpp` follows Showdown's
   arithmetic, takes no `options` and never draws; a zero divisor is
   asserted (only corrupt stats reach it); the dynamic inputs are a `Hit`,
   which the hit backbone builds per action (its `MoveState` is on the
   stack, as libpkmn gen2's `State`). Details and the decomp comparison in
   `src/gen3/README.md`, "Damage".
2. The turn frame and roll helpers, with null Chance and Calc. **Done**
   for the frame: `update` falls through the phases, `choices`, selection
   (forced moves, Struggle, Encore), the order with the lazy Quick Claw and
   tie draws, plain switching, faint requests and the turn cap.
3. **Switching and plain damaging moves end to end**, including the turn-0
   start. **Done** (2026-10-07, branch `step3`): everything in scope below,
   with the decomp trace and the deviations in `src/gen3/README.md`, "Hit
   backbone". What it left for step 4 is marked `TODO(plan step 4)` in
   `mechanics.hpp`. Scenario tests are only the few in `check_frame`.
   - **In scope:**
     - The hit backbone for `Effect::Hit` (`BattleScript_EffectHit`): the
       attack canceler, PP (deducted from both the active and the party
       copy), accuracy, crit, damage via `damage.hpp`, the roll, the hit,
       the faint check, the `moveend` cases a plain hit reaches.
     - Struggle, which `choices()` already offers (recoil, no PP).
     - Roll helpers for accuracy, crit and the damage roll.
     - Fainting (`FaintClearSetData`, main:3264). Spikes on every switch-in.
     - The turn-0 start: trace `TryDoEventsBeforeFirstTurn` (main:3841).
     - Scenario tests for what this reaches (deferred; see Decided).
   - **Decided** (2026-10-07):
     - Effects not written yet are a no-op that still uses PP. Step 3 fills
       code in statically; testing comes later, so this stopgap isn't
       designed further.
     - Immunity before crit, as Showdown (README "Damage").
     - Switch-in abilities (Intimidate, Sand Stream, Drizzle, Trace) wait
       for step 4, with the weather residuals.
     - No birch adaptation in step 3: this is sim development, and the
       "done when birch runs" criterion is dropped for now.
     - Both last Pokémon fainting in one action is a `Tie`: the decomp ORs
       `B_OUTCOME_LOST` and `B_OUTCOME_WON` into `B_OUTCOME_DREW`
       (`Cmd_checkteamslost`, bsc:3563-3577), and Showdown's `checkWin`
       (`sim/battle.ts:2610`) picks a winner only after gen 4. This holds
       for Explosion too: `setatkhptozero` (bsc:6577) zeroes the user before
       the hit and `checkteamslost` runs after both faints (bs1:2831); in
       Showdown the user's faint is queued first (gen3 `scripts.ts:201`) and
       `checkWin` runs once on the whole queue.
4. Trace the hit backbone and the dispatchers, then add effects by group,
   starting with what `teams/` uses. The log comes last.

**Testing.**

- **Scenario tests** per tricky mechanic, RNG forced through Calc overrides,
  each checked against Showdown: Explosion ends the turn; Spikes sac cancels
  the foe's attack; a mid-turn replacement takes residuals; a residual
  faint's replacement takes none; the battle ends mid-residuals (faster dies,
  its side loses); double KO on the first action (does the replacement act,
  and in what order); Baton Pass resumes at the foe's move and the toxic
  counter restarts.
- **Matching Showdown** is the target; how to compare (scenario replays,
  outcome distributions) isn't designed.
- **Differential runs against ADV-Simulator** from one C++ driver,
  comparing outcome distributions over the same team pairs.
- **Soak runs** of millions of random self-play games under ASan/UBSan,
  checking invariants (HP in range, PP never negative, `choices()` non-empty
  while the battle isn't over, the turn cap).
- **A benchmark** against ADV-Simulator's 19k iterations/s.

## Status (2026-10-07)

**Done** (details in the header comments and `src/gen3/README.md`):

- Generated data tables and Showdown-style name tables
  (`tools/generate.py`), hand-written natures and misc tables.
- `common/`: `Player`, `Choice`, `Result`, the RNG, `Rational`, `Options`,
  the log plumbing.
- `gen3/data.hpp`: the full `Battle` draft (808 B, unpacked) with `valid()`,
  including the turn-0 rule; `calc_stats`, `hidden_power`, `max_pp`.
- `gen3/chance.hpp`: the `Random()` catalogue, draft `Actions` (101 B) and
  `Durations` (22 B).
- `gen3/helpers.hpp` and `gen3/showdown.hpp`: all 327 teams in `teams/adv/`
  parse and build.
- Decided: header-only C++ library with no C API; Chance's probability is a
  `double`; the damage formula follows Showdown's arithmetic.
- `gen3/damage.hpp`: crit stage, type matchup, damage before the roll, the
  roll. `check_damage` matches Showdown 0.11.10 on all 74 cases (148 with
  crits), covering every modifier it applies.

- The build: CMake presets for debug (ASan/UBSan) and release, with
  `headers`, `check_data`, `check_damage`, `check_teams` and `check_frame`
  passing in both, under g++ and clang.
- `gen3/mechanics.hpp`: the turn frame (plan step 2; see Plan steps).
  `chance.hpp`'s per-hit draws are `HitDraws`, since `damage.hpp` owns
  `Hit`.

- `gen3/mechanics.hpp`, plan step 3: the hit backbone for plain hits and
  `Effect::Recoil` (Struggle), the attack canceler, accuracy, PP, crit,
  the roll, Focus Band, fainting (`faint()`), Spikes on every switch-in, and
  the battle start traced (its abilities wait for step 4).

**Not started:** every other move effect (they only use PP), switch-in
abilities and items, the residuals, the ability and item dispatchers,
`min_ko_roll`, most scenario tests. Only the damage formula has been
compared with Showdown; nothing with ADV-Simulator, and `calc_stats` only
through the damage cases. birch still runs on ADV-Simulator and must be
adapted (see Interface).

**Open:**

- Check the `Actions` and `Durations` drafts against the `Battle` layout.
- `Item::KnockedOff` and a `Move` sentinel for `MOVE_UNAVAILABLE` in the
  generator; bounds of `bide_damage` and `future_sight_damage` (u32 for now).
- The per-`Effect` behaviour (hand-written with the mechanics).
- A coverage report of the effects, abilities and hold effects `teams/` uses.
- Trace the battle start (`TryDoEventsBeforeFirstTurn`): switch-in
  abilities and items before turn 1, possibly in speed order with a tie draw.
- Team parsing enforces only Species Clause; the other Smogon ADV clauses
  aren't checked.

**Caveats:**

- Deoxys-Attack and -Defense stats are hard-coded in the generator from
  memory; check them against pokefirered.
- The generator parses C with regexes that assume the decomp's formatting
  (designated-initializer blocks start with `{` and a newline; gender ratio
  reimplements `PERCENT_FEMALE`). A reformatted decomp could break it
  silently.
- `HoldEffect` `param` means something different per effect; read the decomp
  per effect.
- birch's `.gitignore` lists `rewrite/pokeemerald/` but not `rewrite/`;
  decide whether birch ignores this repo or tracks it as a submodule.
