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
     once the turn reaches 1001 (`MAX_TURNS`): turn 1000 is played, as
     Showdown (`sim/battle.ts:1843`); libpkmn ties at 1000.
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
  (`pokeemerald/`); tests check behaviour against Showdown.
- **Where they differ visibly, follow Showdown** (decided 2026-10-07; so
  far crit after immunity, Quick Claw's 1/5, the damage arithmetic). The
  log will mirror Showdown's, so matching it pays. Note a small difference
  in a short comment at the code, not the README; the README is for layout
  decisions and big deviations. Check `showdown/` when writing a mechanic
  whose Showdown version might differ.
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
- **Where they disagree:** the shortlist in `src/gen3/README.md`,
  "Mechanics" ("Showdown and the cartridge disagree").

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
- **The log is Showdown's omniscient log, built in two layers, as libpkmn**
  (decided):
  - `update` writes compact binary opcodes (libpkmn's `protocol` ArgTypes,
    same values) into the `Log` buffer, for speed. It formats no text. The
    gen3 inventory, the hybrid `[from]` encoding and what's open are in
    `src/gen3/README.md`, "Protocol" (decided 2026-10-07).
  - A separate formatter turns that buffer into text that matches
    Showdown's protocol line for line.
  - The engine writes only the omniscient stream, including hidden
    information as `|split|pN` triplets (Showdown's `Battle.addSplit`,
    `sim/battle.ts:3081`: the marker, the full-information line, then the
    public line). Each player's private view comes from Showdown's own
    `splitRegex` and `extractChannelMessages` (`sim/battle.ts:33-59`), not
    from the engine. So mirror the raw log with its `|split|` triplets, not
    channel -1's output, which has already dropped the public lines.

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
"Matter" means the perfect-information state (decided 2026-10-08): a draw
that changes only the log (a tied order of log lines, an accuracy roll into
Protect) is made but not recorded. So `Actions` determines the state, not
the log; the log is extra information drawn alongside. A terminal state
counts as the same whatever HP the winner is left at.

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
- `soak`: random self-play between random team pairs, checking `valid()`
  and the probability after every update; a crash prints the game's seed,
  and `--replay SEED` plays that game alone. Run it by hand in debug for
  long soaks (`--games N --seed N`).
- `check_chance`: the chance-key test and probability calibration (see
  `testing.md`); `--games N --samples N --replays N`, `--unmask NAME`,
  `--replay SEED`. Run it in release for long runs.
  `check_chance_unmasked` is the seed ablation and must fail.
- `check_frame`: the turn frame (start, order, Quick Claw and speed-tie rolls
  with Chance and Calc, trapping, faint requests and resuming, the turn cap),
  a few step-3 scenarios (a forced plain hit and its probability, PP, an
  immunity, Focus Band, Struggle, Spikes, a replacement fainting to Spikes,
  a double KO), some move scenarios (Solar Beam, Dig, Hyper Beam...), then
  a random self-play soak over the team files.

```
CMakeLists.txt, CMakePresets.json
testing.md            the test spec, pass criteria, known gaps
src/common/
  data.hpp            Player, ID, Optional, Choice, Result
  rng.hpp             SplitMix64 RNG and its draw helpers
  options.hpp         Options<Log, Chance, Calc>
  rational.hpp        exact probabilities (libpkmn's Rational)
  protocol.hpp        log plumbing: ArgType, Writer, Log, FaintQueue
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
  protocol.hpp        the gen3 log messages (pkmn::protocol)
  format.hpp          binary log -> Showdown's text lines
  calc.hpp            Calc and NullCalc; min_ko_roll                (todo)
tools/
  generate.py         one-off data generator from pokeemerald/
  check_data.cpp      static_asserts on the generated tables
  check_teams.cpp     parses teams/adv/ and builds a battle from each
  check_damage.cpp    damage.hpp against damage_cases.inc
  check_chance.cpp    (c1, c2, Actions) determines the result; calibration
  check_frame.cpp     the turn frame and step 3, plus a self-play soak
  soak.cpp            long random self-play runs, replayable by seed
  bench.cpp           updates/s, null log, Chance on (release)
  libpkmn_bench.zig   the same for libpkmn's Gen I (+ _options.zig)
  coverage.cpp        which moves on the teams have a written mechanic
  damage_oracle.cjs   writes damage_cases.inc from Showdown
  display_names.cjs   writes data/display.hpp from Showdown's dex
  protocol_survey.cjs Showdown's gen3 log messages (README, "Protocol")
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
   faint replacement after each Future Sight or Perish Song event. Both
   follow Showdown instead (Perish Song decided 2026-10-07; Future Sight
   2026-10-09): they run at the end of the residual pass (future attacks,
   then Perish Song), before the residual faints are replaced.

`update` suspends wherever a Pokémon can faint, plus Baton Pass (`Phase` in
`data.hpp`):

| Phase | Who chooses | Resume at |
|---|---|---|
| `BatonPass` | user's side `Switch`, other `Pass` | finish Baton Pass's switch-in, then the remaining action |
| `FaintAfterAction` | either side or both | end of turn (the remaining action was cancelled) |
| `FaintAfterResiduals` | either side or both | the next turn |

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
  both sides replace at once, both enter, then each takes Spikes and its
  switch-in effects in speed order, as Showdown (README, "Switch-in
  effects"); the decomp uses battler-ID order.
- **Effect order.**
  - Turn order: switches, then priority, then Quick Claw (one u16 per turn,
    `gRandomTurnNumber`, serves both sides), then speed with a random tie.
    `GetWhoStrikesFirst` and `SetActionsAndBattlersTurnOrder` (main).
  - Two switches go in speed order with Quick Claw and a random tie, as
    Showdown (decided; README). The order matters: the first switch-in's
    Intimidate hits the foe that's leaving.
  - Quick Claw is drawn lazily, at the first read where it can change the
    order, and kept in `Request` (the padding byte) so the end-of-turn order
    reuses it across a request (decided). Two holders share the one draw, so
    both proccing is a speed tie. Showdown agrees, and its 1/5 is used.
  - End of turn: the decomp re-runs `GetWhoStrikesFirst` (`ENDTURN_ORDER`,
    util:1209) with the chosen moves' priority and the turn's Quick Claw.
    Showdown orders residual handlers by order, Speed (with Quick Claw),
    then subOrder, and the code follows it (found by `check_showdown`,
    2026-10-08). Every gen3 per-Pokémon residual has order 10, so with
    unequal Speeds the faster Pokémon takes all of them first (from gen 5
    the orders differ and it is effect by effect); on a Speed tie they
    interleave by subOrder with a coin per stage (`end_of_turn`).
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
  replacement skips that turn's residuals (Future Sight and Perish Song
  included: they are in the pass).
- **The battle ends mid-residuals.** `checkteamslost` sets `gBattleOutcome`
  as soon as a side is out (bsc:3564–3577); the weather loop exits on it
  (bs1:3219) and `BattleTurnPassed` runs residuals only while it's 0
  (main:3961). If both last Pokémon would die to residuals, the faster dies
  first and its side loses. Showdown agrees: `fieldEvent` checks after
  every handler (`sim/battle.ts:569`); the decomp's Leech Seed script has
  no `checkteamslost`, and the code follows Showdown there (tested in
  `check_frame`). Audit Explosion, Self-Destruct, Destiny Bond, recoil and
  Perish Song the same way *(verify tie rules against Showdown)*. Sand,
  hail and Perish Song hurt both first, so both last fainting is a tie
  (Showdown; README shortlist).
- **Baton Pass** (`baton_pass`, `baton_pass_in`, tested in `check_frame`)
  has its own switch-in (main:3158–3217, bsc:4648; README decision 5).
  - Passes: stat stages; confusion (with its counter), Focus Energy,
    Substitute and its HP, escape prevention, Curse, Leech Seed, Perish
    Song and its timer, Ingrain, Mud Sport, Water Sport.
  - Follows Showdown (verified, `copyVolatileFrom` and `noCopy`) where it
    differs: binding passes; the passer's Lock-On on the foe keeps its
    count (the decomp resets it to 2); the foe's Lock-On on the passer is
    lost. None of these moves is on `teams/`. Showdown also passes Charge
    (TODO when Charge is written).
  - Doesn't pass: the toxic counter, the rest of the disable struct, the
    last-move fields. Fails with no bench; trapping doesn't stop it.
  - It doesn't cancel the foe's queued action: the request resumes at it.
    A recipient that faints to Spikes ends the actions, as any faint.
- **Sleep Talk** (`sleep_talk`, tested), as Showdown's gen3: only asleep;
  fails under a Choice Band lock or Encore (the cartridge agrees); picks
  uniformly among the slots `sleep_talk_banned` allows (a `move_slot` chance
  action) and fails if the pick has no PP. The cartridge also leaves out
  disabled, taunted, tormented and imprisoned moves and calls 0-PP moves.
  The called move uses no PP and doesn't re-run the canceler (the decomp's
  tracker stays at its end; Showdown's useMove skips BeforeMove).
  Sleep Talk and Snore turns go back on the counter at switch-out
  (`sleep_skipped`, Showdown's gen3 `skippedTime`, a real cartridge quirk).
- **Roar and Whirlwind** (`roar`, `drag`, tested in `check_frame`).
  Priority −6, accuracy 100 (evasion can make it miss), blocked by Protect
  (Showdown's gen3 agrees on both; master's base data doesn't, its gen4
  mod restores them). The replacement is a uniform pick among the
  non-fainted bench (`switch_target`, no request). Fails with no bench,
  against Suction Cups, Ingrain or a semi-invulnerable target; Soundproof
  stops Roar. As Showdown: no level check, through a Substitute, and the
  drag comes after the whole action. The dragged-in Pokémon takes Spikes
  and its switch-in effects, and the dragged-out one's queued move is
  cancelled.
- **Sleep Clause and Freeze Clause** are Smogon rules, always on, so
  compile-time constants with no field (Gen 3 OU has Sleep Clause Mod and
  Freeze Clause Mod). A foe's sleep fails while a living Pokémon on the
  target's side is asleep from the foe (`sleep_clause`); Rest
  (`Status::EXT`) doesn't count. It stops sleep moves after the accuracy
  check, Yawn's sleep and Effect Spore's sleep (verified in Showdown's
  `sleepclausemod`, tested). Freeze Clause likewise.

To add when they come up: Choice Band
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
   starting with what `teams/` uses. The log comes last (see Interface:
   opcodes, a text formatter, Showdown's omniscient log).
5. The rest of the moves (decided 2026-10-08), after the log: 97 left
   (99 unwritten, less Follow Me and Helping Hand). `tools/coverage` is
   100% on `teams/adv/`, so each group needs synthetic teams carrying its
   moves for `check_showdown`. Groups, in order:
   1. Hits with a simple extra: Stomp, Astonish, Needle Arm, Extrasensory,
      Ancient Power, Silver Wind, Twineedle, Double Kick, Bonemerang,
      Triple Kick, Facade, Revenge, Smelling Salt, Weather Ball, Low Kick,
      Secret Power, Pay Day, Jump Kick, Hi Jump Kick.
   2. Fixed and variable damage, OHKOs: Sonic Boom, Dragon Rage, Super
      Fang, Psywave, Present, Magnitude, Fissure, Horn Drill, Guillotine,
      Sheer Cold.
   3. Charge, semi-invulnerable, recharge: Fly, Dig, Dive, Bounce, Razor
      Wind, Skull Bash, Sky Attack, Hyper Beam.
   4. Locked moves (done; no `WasUnableToUseMove` bit, README decision
      2): Thrash, Petal Dance, Outrage, Uproar, Rollout, Ice Ball, Fury
      Cutter, Rage, Bide.
   5. Trapping and delayed damage: Bind, Wrap, Clamp, Fire Spin,
      Whirlpool, Sand Tomb, Future Sight, Doom Desire, Stockpile, Spit Up,
      Swallow.
   6. Volatiles: Swagger, Flatter, Teeter Dance, Attract, Disable, Torment,
      Spite, Foresight, Odor Sleuth, Lock-On, Mind Reader, Focus Energy,
      Charge, Ingrain, Grudge.
   7. Side and field: Safeguard, Mist, Mud Sport, Water Sport, Magic Coat,
      Snatch.
   8. Copying, calling, swapping: Metronome, Mirror Move, Assist, Nature
      Power, Sketch, Transform, Trick, Role Play, Skill Swap, Recycle,
      Conversion, Conversion 2, Camouflage, Dream Eater, Teleport, Splash.

**Testing** is specified in `testing.md`: the environments (OU, random
OU-legal, truly random), the suites, the pass criteria, how Calc is used in
tests, the known engine gaps and the test limitations. Record test
decisions and newly observed gaps there, not here.

## Status (2026-10-08)

**Done** (details in the header comments and `src/gen3/README.md`):

- Generated data tables and Showdown-style name tables
  (`tools/generate.py`), hand-written natures and misc tables.
- `common/`: `Player`, `Choice`, `Result`, the RNG, `Rational`, `Options`,
  the log plumbing.
- `gen3/data.hpp`: the full `Battle` draft (824 B, unpacked) with `valid()`,
  including the turn-0 rule; `calc_stats`, `hidden_power`, `max_pp`.
- `gen3/chance.hpp`: the `Random()` catalogue, draft `Actions` (121 B) and
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

- Plan step 4, begun (branch `step4`): `tools/soak` (random self-play,
  replayable per-game seeds, a 100-game `ctest` case); status and flinch
  secondaries, secondary stat drops and Meteor Mash's raise, Tri Attack,
  Hidden Power, Earthquake, Thunder, Sky Uppercut, Double-Edge, Explosion,
  Seismic Toss, Night Shade, Rapid Spin and Brick Break. `tools/coverage`
  counts what's handled (see "Next session" for the current numbers).

**Not started:** every other move effect (they only use PP), switch-in
abilities and items, the berries and the Uproar, Thrash and Yawn
residuals, the ability and item dispatchers,
`min_ko_roll`, most scenario tests. Only the damage formula has been
compared with Showdown; nothing with ADV-Simulator, and `calc_stats` only
through the damage cases. birch still runs on ADV-Simulator and must be
adapted (see Interface).

**Next session** (step 4, on `main`; `step3` and `step4` were merged
2026-10-08; ask before each open choice):

- **Decisions for the user**, each blocking what follows it:
  - Decided (2026-10-07): confusion's self-hit follows Showdown (typeless
    40 power via `getDamage`, gen4 `conditions.ts:77`). Both roll 85-100
    (decomp: `adjustnormaldamage2`, bsc:1705); they differ in that the
    decomp applies Reflect and Normal type items to the self-hit and
    blocks Focus Band under the user's Substitute. A self-hit cancels the
    move, so it reuses `hits[0]` (`damage`, `focus_band`), keyed by
    `confusion` = True: no new field.
  - Decided (2026-10-07): accuracy follows Showdown (gen3 `scripts.ts:319`:
    accuracy stage then evasion stage as exact fractions, then
    `randomChance(acc, 100)`), replacing the decomp's combined integer
    ratios.
  - Checked: Brick Break breaks screens into a Ghost type in both the
    decomp and Showdown (gen3 `scripts.ts:367`, `TryHit` before the
    immunity result).
  - Decided (2026-10-07): the per-turn `ProtectStruct` lives on `update`'s
    stack (README decision 2), conditional on the field check. The check
    found one exception: `WasUnableToUseMove`, read by the Uproar and
    Thrash residuals after a `FaintAfterAction` request (README decision 2).
    Settled in step 5 group 4: Showdown's locks don't read it, so it isn't
    kept. Protect, Endure, Counter,
    Mirror Coat, Magic Coat and Snatch are unblocked.
  - Skipping the damage roll when every roll KOs (fewer chance nodes, but
    it changes the `Actions` key). Can wait for the birch adaptation.
- Done (2026-10-07): accuracy switched to Showdown's arithmetic; confusion
  (the canceler, the self-hit, Confuse Ray's `Effect::Confuse` and the
  `ConfuseHit` secondary), covered by `check_frame`. Still to write:
  Swagger, Flatter and Thrash's end. Only 2 slots in `teams/` confuse.
- Done (2026-10-07): the primary status moves (Toxic, Poison Powder,
  Thunder Wave, Stun Spore, Glare, Will-O-Wisp; `status_move`).
  `detail::handled()` is what `tools/coverage` counts: 66% of move slots
  (92% of damaging ones). Gap: Synchronize (24 holders in `teams/`) doesn't
  pass the status back yet; it belongs to `moveend`'s ability dispatcher.
- Done (2026-10-07): the end of turn as straight-line code (decided over a
  table): `field_residuals` (side timers, Wish, weather and sand/hail
  damage) and `battler_stage` per Pokémon in the residual order
  (Ingrain, Rain Dish, Shed Skin, Speed Boost, Truant's toggle, Leftovers,
  Leech Seed, poison, toxic, burn, Nightmare, Curse, binding, Disable,
  Encore, Lock-On, Charge, Taunt; ties since reworked, see 2026-10-08).
  `soak` now builds in
  release. About 3.5% of random soak games reach the turn cap: Struggle
  against Leftovers stalls, once every move is out of PP.
- Decided (2026-10-07): the dispatchers are one plain function per
  decomp mode (`item_cure`, `item_end_turn`, then `ability_on_damage`,
  `synchronize`...), called at the decomp's sites. Where the decomp's
  `ITEMEFFECT_MOVE_END` cures only the first Pokémon, follow Showdown (both).
- Done (2026-10-07): held items. Status berries and White Herb after every
  move and at end of turn; Leftovers, Sitrus and the pinch berries (Salac,
  Liechi, Petaya, Lansat, Starf) at end of turn only, as on the cartridge
  and in Showdown's gen3 mod. Not yet: Leppa, the flavor berries, King's
  Rock, Shell Bell.
- Done (2026-10-07): `moveend`'s abilities: Synchronize (armed by any
  poison, toxic, paralysis or burn that takes hold, `MoveState::synchronize`),
  Color Change, Rough Skin, Effect Spore (its sleep waits for Sleep
  Clause), Poison Point, Static, Flame Body, Cute Charm, the immunity cures.
  Following Showdown, Safeguard blocks the ability-inflicted statuses.
- Done (2026-10-07): the absorbing abilities (`absorbed()`, from
  `JumpIfMoveFailed` after a passed accuracy check, in the hit and status
  paths; Flash Fire takes Will-O-Wisp). Destiny Bond's setup there is still
  a TODO.
- Done (2026-10-07, following Showdown, verified in its source): switch-in
  abilities (Sand Stream, Drizzle, Drought, Intimidate, Trace), White Herb
  and Natural Cure; simultaneous switch-ins arrive in speed order. Fixed:
  two switch actions now count Quick Claw (gen3 Showdown's
  `getActionSpeed`); the old code and test said Showdown ignored it.
- Done (2026-10-07, following Showdown): sleep. Sleep moves, Rest, Yawn,
  Snore, Sleep Talk, Effect Spore's sleep, Sleep Clause, `sleep_skipped`
  (`Battle` is now 816 B). `tools/coverage`: 69% of move slots.
- Done (2026-10-07, the decomp's scripts; Showdown agrees on the state):
  the stat moves. The one-stat raises, Cosmic Power, Bulk Up, Calm Mind,
  Dragon Dance, Minimize, Defense Curl, Belly Drum and both Curses
  (`stat_up_move`, `stat_pair_move`, `belly_drum`, `curse`). A raise at +6
  marks the move MISSED, as the decomp. Ghost Curse's Protect check waits
  for Protect. `tools/coverage`: 75% of move slots. New mechanics mark
  their log sites with `// Log: <ArgType>` comments.
- Done (2026-10-07): Roar and Whirlwind (see the checklist) and
  Soundproof's block in the canceler. `tools/coverage`: 78%.
- Done (2026-10-07): Substitute (the decomp and Showdown agree; it ends a
  binding on the user).
- Done (2026-10-07): Protect, Detect and Endure. The per-turn state is a
  `Turn` local of `actions()`, reached through `MoveState::turn` (decided;
  README decision 2). Protect is part of `accuracy()`, as the decomp's
  accuracy check; the consecutive-use chance stays at 1/8 after the
  fourth, as Showdown. Showdown's protect flag matches the decomp's except
  Counter and Mirror Coat (take Showdown's when written). Roar fixed:
  accuracy 100 and Protect, as Showdown's gen3. `tools/coverage`: 83%.
- Done (2026-10-07): the Spikes move and the healing moves (Recover, Slack
  Off, Soft-Boiled, Milk Drink, Morning Sun, Synthesis, Moonlight); the
  decomp and Showdown agree. `tools/coverage`: 87%.
- Done (2026-10-07): Focus Punch (`Turn::hit`, set in `hp_update`; a
  0-HP hit breaks focus too, as Showdown). `tools/coverage`: 89% of move
  slots, 95% of damaging ones.
- Done (2026-10-07): Baton Pass (see the checklist), with the frame's
  `BatonPass` request and resume. `tools/coverage`: 91% of move slots.
- Done (2026-10-07): the Wish move (its landing was already in
  `field_residuals`); the decomp and Showdown agree. 92%.
- Done (2026-10-07): the Leech Seed move (the decomp's check order, as
  the status moves). Open, for the log: Showdown draws accuracy before
  the Substitute and already-statused checks, so it can log a miss where
  this fails; the state is the same. 92% (14502 of 15696 slots).
- Done (2026-10-07): Refresh, Heal Bell and Aromatherapy (the decomp and
  Showdown agree). Fixed: every end of sleep now goes through `wake()`,
  which also clears `sleep_skipped`; a cure other than waking used to
  leave it for the next sleep. 94%.
- Done (2026-10-07): Counter and Mirror Coat (`Turn::physical` and
  `special`, set in `hp_update`). Protect blocks them, as Showdown. Both
  count Hidden Power as physical whatever its type (Showdown's gen3 says
  so explicitly; an older note here said otherwise). 95%.
- Done (2026-10-07): Absorb, Mega Drain and Giga Drain, as Showdown
  where it differs (a Substitute's half rounds up; no drain without
  effect).
- Done (2026-10-07): Taunt (the selection, canceler and residual were
  already there; power 0 matches Showdown's Status on every move).
- Done (2026-10-07): Rain Dance, Sunny Day, Sandstorm and Hail (the decomp
  and Showdown agree). 97%.
- Done (2026-10-07): Endeavor (the decomp and Showdown agree).
- Done (2026-10-07): Beat Up, as Showdown (README "Damage"); `Action::hits`
  grew to 6 (`MAX_HITS`, decided). The survival step is `survive()`.
- Done (2026-10-07): Knock Off, as Showdown (README decision 8), with
  `Item::KnockedOff` now in the generator. 98%.
- Done (2026-10-07): Perish Song, as Showdown: the count runs in the
  residual pass (`perish_count`), so `Phase::FaintAfterPerish` is gone; the
  user's Soundproof doesn't protect it; a semi-invulnerable foe is missed.
- Done (2026-10-07): the one-stat drops (Screech, Metal Sound...), Tickle
  (through a Substitute: the decomp and Showdown's gen3 `bypasssub` agree;
  fixed 2026-10-08), Superpower and Overheat, Haze, Psych Up (copies Focus
  Energy and misses a semi-invulnerable target, as Showdown).
- Done (2026-10-07): Reflect, Light Screen, Return, Frustration, Flail and
  Reversal (the decomp and Showdown agree; power at least 1).
- **Gate (decided 2026-10-07):** once every move on `teams/adv/` is
  written, start the realizability test (`testing.md`).
- Done (2026-10-07): Mean Look (Protect stops it: the decomp and Showdown,
  whose gen5 mod adds the flag, agree; fixed 2026-10-08; the protect flags
  match on every teams/adv/ move but Counter and Mirror Coat). 99%
  (15598 of 15696 slots).
- Done (2026-10-08): `check_chance`, the user's chance-key test (see
  `testing.md`): 0 collisions over 500 games at 128 samples.
- Done (2026-10-08): Thief and Covet (the decomp and Showdown agree;
  Showdown doesn't keep Enigma Berry and Mail out).
- Done (2026-10-08): Encore, as Showdown's gen3 fail list. `last_move`
  now follows Showdown's lastMove: a move stopped before use (paralysis,
  sleep, flinch, a confusion self-hit) leaves it as it was, where the
  decomp writes MOVE_UNAVAILABLE.
- Done (2026-10-08): Solar Beam, the first charge move (the decomp and
  Showdown agree). Following Showdown's onMoveAborted, sleep and freeze now
  cancel a multi-turn move in the canceler, where the decomp waits for the
  end of the turn (main:3944) or keeps it.
- Done (2026-10-08): Destiny Bond (`faint_target`; the decomp and
  Showdown agree in singles).
- Done (2026-10-08): the 2-5 hit moves (`Effect::MultiHit`: Rock Blast,
  Arm Thrust, Bullet Seed...), as Showdown's gen3 loop: contact abilities
  per hit, and no stop after Endure or Focus Band (the decomp stops).
  Effect Spore's status moved into `HitDraws` (per hit), so `Actions` grew
  to 121 B.
- Done (2026-10-08): Mimic (the decomp and Showdown agree).
- Done (2026-10-08): the last single-slot moves: Water Spout (and
  Eruption), Gust and Twister, Fake Out, Pain Split, Memento, Imprison,
  Nightmare, all checked against Showdown (Memento and Pain Split follow
  it where the decomp differs). `tools/coverage`: 100% of the 7848 move
  slots on `teams/adv/`.
- Fixed (2026-10-08), found by 200k-game soaks: Mimic copying Hidden
  Power (every `Pokemon` now holds its own `hidden_power` and
  `hidden_power_type`, in what was padding); sleep or freeze landing on a
  charging Pokémon left a forced choice with no lock (now, as Showdown, the
  lock stays until the move is aborted). `soak` now reports the game seed
  on a release segfault.
- **Resume here (2026-10-08).** The realizability harness, with a
  temporary state guard (decided with the user; the log comparison
  replaces it later): `tools/showdown_traces.cjs` (seeded Showdown 0.11.11
  games over `teams/adv/all.txt`, teams through the gen3ou validator, JSONL
  traces) and `tools/check_showdown.cpp` (+ `tools/json.hpp`; `--why`,
  `--game`, `--tries`; random branch search; guard: HP, status, PP, items,
  boosts, main volatiles, Substitute HP, Protect's `stall` duration, side
  conditions, weather, residual timing). Not in ctest (traces aren't
  committed). Run: `node tools/showdown_traces.cjs --games 1000 --seed 7 >
  t.jsonl; build/release/check_showdown --quiet t.jsonl`. **Every game
  matches to the end** on seeds 7 and 8 (2000 games), one only past the
  1M-try cap (game 816 of seed 7: two crits, a 10% drop and two rolls; it
  matched at 2.03M). Fixed via it, all following Showdown (README
  shortlist): Pressure on field-wide moves and on Sleep Talk itself
  (Showdown's `'self'`; the decomp's DEPENDS takes 2 PP), Truant at the
  battle start, Thief's Choice Band lock, Speed rounding, end-of-turn
  order by Speed, sand/hail and Perish Song double KOs tie, Pursuit on a
  switching target, the turn cap (turn 1000 is played), Protect's chain
  ending after any turn without a success (`ActivePokemon::stall`), and
  Endure at +4 (likely a Showdown data bug, followed anyway). Committed
  (`a53cd5b`).
- **The log, group 1** (2026-10-08, committed): plumbing and opcodes
  (`common/protocol.hpp`), the gen3 messages (`gen3/protocol.hpp`), the
  formatter (`gen3/format.hpp`), display names from Showdown's dex
  (`tools/display_names.cjs` -> `data/display.hpp`), and in `update`:
  `move` (`[from]` Sleep Talk, Pursuit, lockedmove), `switch`, `drag`,
  `cant`, `faint` (queued, as Showdown's faintMessages), `turn`, `win`,
  `tie`, every `-damage` and `-heal`. Decisions in README "Protocol".
  `check_showdown` now compares the log (`LOGGED`, both lines of each
  `|split|` triplet) whenever the state guard passes; the move line's
  `[still]` and `[miss]` were normalized away until group 2
  (`MOVE_ATTRIBUTES`). The traces carry nicknames (`sets[].name`), so
  regenerate old ones. Result: 184 of 200 games (seed 7) match to the
  end, every failure by the log alone. Split traces by game to run in
  parallel (`scratch/split.py N prefix traces...`); since 2026-10-15 the
  user allows all 16 threads (was 4).
- Done (2026-10-08), the 16 group-1 failures of seed 7, following Showdown:
  - **Residual ties, one coin per stage** (`Stage`, `battler_stage`,
    `stage_order_matters`, `rolls::residual_tie`): 11 failures were exact
    Speed ties. With unequal Speeds the order stays Pokémon-major; on a
    tie the handlers interleave by Showdown's subOrder, a coin per stage.
    A coin is recorded only where running the stage both ways on copies
    gives different states (Leech Seed: bits 0-1 of `Turn::residual_tie`;
    the damage stages, where it ends the battle: bits 2-3); Wish's, the
    weather's, Perish Song's and the other stages' coins are unrecorded
    (`log_order`). `Actions` stays 121 B.
  - **Quick Claw in the residual order** (`residual_quick_claw`), as
    Showdown (`updateSpeed`, gen3 `getActionSpeed`: a holder whose roll
    succeeded has 65535). The turn's one roll; if the move order didn't
    draw it, it is drawn only if it changes the comparison and recorded
    only if the two whole passes, run on copies, give different states
    (decided with the user). Over-approximated when both Pokémon draw in
    the pass. Wish and the weather still order by plain Speed (log only).
  - **The canceler in Showdown's `BeforeMove` order** (game 27): recharge,
    sleep, freeze, Truant, flinch, Disable, Imprison, confusion, Attract,
    paralysis, Taunt.
  - Games 8 and 74 (Beat Up) match with a higher cap (10.5M and 14M
    tries): rare branches, not bugs. Game 13 (a 4-hit Rock Blast with a
    crit on hit 1, about 2e-7 per try) doesn't match in 20M tries;
    undiagnosed, bad luck at maybe 2%. This is limitation 3 in `testing.md`.
  - Result: 197 of 200 games (seed 7) match to the end at the 1M cap.
- Done (2026-10-08), **group 2, the hit results**: `-fail`, `-miss`,
  `-immune`, the move line's `[still]` and `[miss]` (`MOVE_ATTRIBUTES`
  on), and the `-activate` of what stops a move (Protect, Safeguard, Mist,
  Suction Cups, Ingrain; `check_showdown`'s `ACTIVATED`). Every move aimed
  at the foe goes through `try_hit`, Showdown's gen3 `tryMoveHit` order
  (README "Protocol"); then each move's own checks in Showdown's order
  (`status_stop`, `drops()` for Clear Body, Hyper Cutter and Mist).
  Soundproof moved from the canceler into `try_hit`. Fixed on the way,
  following Showdown: Tickle through a Substitute, Protect stopping Mean
  Look, Psych Up missing Fly/Dig/Dive, Flash Fire's Will-O-Wisp
  exceptions, `[still]` on a sun Solar Beam. Results (state guard plus
  groups 1 and 2, 1M-try cap): seed 7 197 of 200, seed 8 990 of 1000.
- **Diagnosed (2026-10-08), group 2's failures:**
  - Beat Up (seed 7 games 8, 74; seed 8 games 504, 703, 837, 946, 958)
    and Rock Blast (seed 7 game 13; seed 8 games 32, 403, 690): rare
    branches, not bugs (`testing.md`, limitation 3). The old "closest try" was the first
    state-matching try's log difference, which misled; `check_showdown`
    now reports the longest matching log prefix, and with `--why` the
    longest over every try. Every one matches 4-6 hit lines first; game
    8's six exact rolls are about 4e-8 per try.
  - Seed 8 games 572 and 873 were double replacements after Explosion,
    not voluntary switches: Showdown orders the instaswitch actions by the
    fainted Pokémon, whose handlers don't run (not active), so the fainted
    Exeggutor's Chlorophyll didn't count. Fixed (`replacement_order`).
- **Group 3, decided (2026-10-08):** four steps, each checked on seeds 7
  and 8 before the next: 3a the rest of the hit results, 3b major status,
  3c boosts, 3d volatiles (README "Protocol"). Each writes the item and
  ability forms of its types too; the reason enums cover the whole gen3
  inventory; `Boost` carries a direction bit (Showdown prints `-unboost`
  at -6 even for a raise, `sim/battle.ts:2049`) and an item source.
- Done (2026-10-08): group 3a (`-crit`, `-supereffective`, `-resisted`,
  `-hitcount`, `-prepare`, `-anim`). Seed 7 197 of 200, seed 8 992 of
  1000 (groups 1, 2, 3a and the state guard); every failure is a Beat Up
  or Rock Blast rare branch.
- Done (2026-10-08): group 3b (`-status`, `-curestatus` with a bench
  ident for Heal Bell, `-cureteam`, Heal Bell's `-activate`). Same
  results: seed 7 197 of 200, seed 8 992 of 1000 (groups 1, 2, 3a, 3b and
  the guard). A mutation (`[msg]` broken) fails 29 of 50 games, so the
  comparison sees them. Log markers left for 3d: Shed Skin's and the
  immunity abilities' `-activate`, Nightmare's and Yawn's `-end`.
- Done (2026-10-08): group 3c (`-boost`, `-unboost` with 0 at the cap,
  `-setboost`, `-copyboost`, `-clearallboost`, `-clearnegativeboost`;
  `lower_stats` for primary drops). Same results: seed 7 197 of 200, seed
  8 992 of 1000 (groups 1-3c and the guard), every failure a Beat Up or
  Rock Blast rare branch. The comparison caught Curse's order (spe, atk,
  def). Checked: the decomp's MISSED on a +6 raise changes only
  Conversion 2's last-landed fields, which Showdown doesn't keep.
  Following Showdown (the user, 2026-10-08): the stat berries are eaten
  at +6, Lansat under Focus Energy, Starf with every stat at +6
  (`data/items.ts:3392`, 3267, 6003; the decomp keeps them).
- Done (2026-10-08): group 3d (`-start`, `-end`, every `-activate`,
  `-singleturn`, `-singlemove`, `-sethp`, `-fieldactivate`; README
  "Protocol"). Same results: seed 7 197 of 200, seed 8 992 of 1000
  (groups 1-3 and the guard), every failure a Beat Up or Rock Blast rare
  branch. Found by the comparison: Flash Fire's silent `-end` at
  switch-out (not at a faint: fixed 2026-10-08, step 5 group 1). A mutation (two 3d strings) fails 43 of 200.
- Done (2026-10-08): group 4 (`-sidestart`, `-sideend`, `-weather`;
  README "Protocol"). Same results: seed 7 197 of 200, seed 8 992 of
  1000 (groups 1-4 and the guard), every failure a Beat Up or Rock Blast
  rare branch. A mutation (two group 4 strings) fails 179 of 200. Two
  sides' screens ending in one turn print in a random, unrecorded order.
- Done (2026-10-08): group 5 (`-item`, `-enditem`, `-ability`; README
  "Protocol"). Same results: seed 7 197 of 200, seed 8 992 of 1000
  (groups 1-5 and the guard), every failure a Beat Up or Rock Blast rare
  branch. A mutation (`[eat]` and Pressure's `[silent]`) fails 41 of 50.
- **Resume here (2026-10-15):** plan step 5, group 8 committed (all 16 moves).
  Layout decided
  with the user: `Action` gained `metronome` and `assist` (`MovePick`,
  bytewise u16; Actions 141 B), Conversion's type goes in `variable` (+1);
  `ActivePokemon` gained `hidden_power`/`hidden_power_type` (Transform
  copies them; still 824 B). Following Showdown: Conversion 2 reads
  `last_used_move`/`last_used_type` (lastMoveUsed, set in `cancel()`;
  replaced the decomp's write-only landed fields); Mirror Move reads
  `last_taken_move` as attackedBy (`MoveState::attacked`, set when
  `try_hit` passes), kept only if `mirrors()` (knows Mirror Move or
  Metronome), whose roll `try_hit` then records; called moves go through
  `call_move` (Sleep Talk too; `MoveState::from` is the `[from]`). New log
  messages: `-transform`, `-nothing`, Activate Sketch and Skill Swap,
  ItemFrom::Recycle. Assist is banned in gen3ou: `tools/teams/group8_assist.txt`
  is for soak/check_chance only. Deviation: Recycle fails after Knock Off.
  Done: debug ctest passes; 300-game debug soaks on both files clean.
  `check_showdown` on `scratch/g8/t1-4.jsonl` (seeds 801-804, 50 games
  each): 40/200 at first, 196/200 after these fixes (2026-10-15), all
  following Showdown: the guard compares the active (transformed) species;
  Sketch is gen4's (fails into a Substitute, keeps the slot's PP Ups, logs
  `move: Mimic`; `Activate::Sketch` removed); an immunity ability cures
  after every switch-in (`arrive`: Ditto's Limber, a returning Insomnia,
  Trace); Lum Berry eats as soon as a status lands (`inflict`, Showdown's
  onAfterSetStatus; not for a Synchronize holder, whose Lum waits as
  before); a Pokémon leaving clears the foe's `last_taken_move`
  (clearVolatile drops the source's lastMove, so Mirror Move fails).
  `check_showdown` now forces Metronome's pick from Showdown's `[from]
  Metronome` line through Calc (two picks plus rolls exceeded the cap; only
  the pick is forced). Later, as Showdown: Synchronize passes back only
  the status its holder took (`MoveState::synchronized`); multi-hit moves
  recompute effectiveness per hit; `mirrors()` also counts Mimic, Sketch and
  Transform; Bide's start rolls accuracy against the user's own stages; the
  contact abilities come after the self drops, Knock Off and Thief.
  `check_showdown` skips a stale choicelock without a Choice Band.
  **10k results (2026-10-15, seeds 701-704, 1M cap, `scratch/run_all.sh`,
  output in `scratch/all/`)**: group 1 9943, 2 9915 (before the
  Synchronize fix; recheck in `scratch/all/g2b/`), 3 9998, 4 9982, 5 9996,
  6 9947, 7 9988, 8 9718; teams/adv seed 8 992/1000. Soaks clean.
  Group 2 recheck after the Synchronize fix: 9974.
  check_chance: 1 collision each in groups 1, 2, 4 and 8. Read from the
  reports (not replayed): all four look like the known winner-HP false
  positive (a residual KO ends the battle; samples differ only in whether
  the winner's own residual ran first).
  **Group 8 fixes (2026-10-15, after 283ac28; UNCOMMITTED, ctest debug and
  release pass; no soak or check_chance yet)**, all as Showdown: a Role
  Play failing in its own TryHit still counts as an attack (gotAttacked);
  Hustle cuts Bide's start accuracy (Physical); the Choice Band lock
  outlives its band until the holder's next move (cancel()) or the turn's
  end (turn_cleanup), and any move locks (Struggle, uncalled moves), so a
  band Tricked back keeps the lock; Pursuit KOing a switcher prints Natural
  Cure's cure before the faint and Attract's -end before the switch; Lum
  eats before Rest's heal; a Fire hit thaws before Color Change
  (`defrost()`); Beat Up is '???' (`move_type`: no Color Change, nothing
  for Conversion 2); Mirror Move's entry survives the attacker leaving and
  returning in the same turn (`ActivePokemon::last_taken_from`, in former
  padding; still 124 B), is pruned at the turn's end, and needs the
  attacker's last_move; a landing future attack clears it; no faint flush
  after a multi-hit loop (gen3 prints faints after AfterMove); no Charge
  -end for a user that fainted; Bide's -end after a confusion self-hit;
  a Baton Pass recipient's berries/White Herb run before arrive(); an
  item_cure pass after the residuals (Showdown's Update; Thrash's fatigue
  confusion vs Lum); Recycle works on a knocked-off hand (Showdown's
  itemKnockedOff then still blocks Trick/Thief; ours doesn't).
  `check_showdown`: an all-Imprisoned Pokémon (hidden-disabled moves,
  every choice becomes Struggle) counts as forced.
  Group 8 on the same 10k traces (`scratch/g8r/s0-13.jsonl`, 14 shards):
  9718 -> 9890. Of the 110 left: 69 multi-hit moves via Metronome (rare
  branches), 30 Encore + a Metronome-called multi-turn move (Showdown's
  Encore OverrideAction swaps the locked move for Metronome, run as
  `[from] lockedmove`, no PP, with the lock's handlers still going; left
  as a known gap, the user's call 2026-10-15), 7 Leftovers order on an exact Speed tie
  (the coin is a fair 50/50 per stage, verified; probably few
  state-matching tries, unconfirmed), 2 crit+crit+freeze Porygon2 (rare?),
  1 crit+freeze+Lum, 1 Limber cure vs Attract -end order (Update speed
  order, log-only).
  **Partial rerun with these fixes (2026-10-15, `scratch/run_all16.sh`,
  output in `scratch/all16/`; stopped after group 4 to start the test
  framework):** check_showdown groups 1-4 unchanged (9943, 9974, 9998,
  9982); soaks clean; check_chance 1 collision in groups 1 and 2 (the
  winner-HP pattern, from the reports) and a **new one in group 3**, not
  terminal: `check_chance --replay 15040563541741120241
  tools/teams/group3.txt` (Pelipper mirror, turn 111: P1 first, Toxic hits,
  p2's Surf crits; p2's fresh toxic ticks in one sample and not the other,
  same Actions). Undiagnosed; suspect the new item_cure pass or the
  residual Speed-tie order. Groups 5-8 and the baseline not rerun.
  **Next:** the test framework, specified in `testing.md` (steps 1-3 of
  its Implementation done 2026-10-15; the group 3 collision was the
  winner-HP false positive after all); finish the rerun under it. Still open from group 6: Grudge's -activate
  when the KO ends the battle; Focus Punch vs confusion.
  Group 7 done (2026-10-15), as Showdown's gen3: Mist and Safeguard
  (`screen`), Mud Sport and Water Sport (gen4 volatiles on the user, in
  `self_volatile`), Magic Coat (bounces in `try_hit` after the roll and
  Protect) and Snatch (`snatched`, right after the PP), both per-turn flags
  in `Turn`, the moved move run as Sleep Talk's call (`call_back`,
  `[from]`); neither fails when used last, and only the steal takes
  Pressure's PP (gen4 Snatch drops mustpressure). Also fixed, as Showdown:
  Safeguard's -activate when it blocks Synchronize; Pursuit on a switching
  target skips the canceler (useMove runs no BeforeMove); a stat drop's
  roll is recorded even when Mist, an ability or -6 blocks it (the hit
  still lands in `last_taken_move`; check_chance found it).
  - Results (`tools/teams/group7.txt`): `check_showdown` 200/200 on seeds
    771-774 and 200/200 on 781-784; teams/adv 597/600 (the known rare
    branches); check_chance 0 collisions; 1000-game debug soak clean. Not
    done: the 10k run, `check_frame` scenarios.
  Group 6 done (2026-10-15), all 15 moves, as Showdown's gen3:
  Swagger and Flatter (`swagger`), Teeter Dance (`confuse_move`), Attract,
  Disable (+1 turn if the target already moved; Durations count starts at
  2, or 1, over a 3-6 roll; it ends in `disable_residual`, after Perish
  Song, since gen3 drops its residual order), Torment, Spite
  (`rolls::spite`, logs the roll), Foresight and Odor Sleuth, Lock-On and
  Mind Reader (on the user with the target's position, README decision
  9), Focus Energy, Ingrain and Charge (`self_volatile`; Charge ends after
  the next move, in `use_move`), Grudge (`faint_target`, logged via the
  faint queue). Also fixed: Cute Charm attracts even when its holder
  faints to the hit (Showdown). Swagger is banned in Showdown's gen3ou:
  only in `tools/teams/group6_swagger.txt` (soak, check_chance).
  - Results: `check_showdown` on `tools/teams/group6.txt` seed 761 (traces
    `scratch/g6/s0-3.jsonl`) 198/200 at the 1M cap; the 2 failures (Speed
    tie plus two crits) match at 1.1M and 3.4M tries. teams/adv seed 8
    597/600 (the known rare branches). Debug ctest and a 500-game soak
    clean. Not done: the 10k run, `check_frame` scenarios.
  - Group 5, as Showdown: binding (`Effect::Trap` in `hit()`, 3-6 turns;
    a binder leaving sets `Volatiles::wrapper_gone`: the victim is no
    longer trapped and the binding ends at its next residual, silently
    unless the counter runs out then); Future Sight and Doom Desire
    (`future_sight_move`: typeless, damage rolled at the use;
    `future_attack` lands in the residual pass after the battler stages,
    accuracy drawn then, `-miss` from a benched user via
    `protocol::benched`); Stockpile, Spit Up, Swallow (silent failures,
    Spit Up's layers go at AfterMove). `Phase::FaintAfterFutureSight` is
    gone. `Battle` is 824 B. `check_showdown` now compares each side's
    pending future attack (turns, damage) from the traces.
  - Also fixed: a failed Baton Pass is the last move (Encore can lock it).
  - Results (`tools/teams/group5.txt`): seed 751 200/200 (3 games need a
    2M-try cap), seed 752 999/1000; teams/adv seed 8 992/1000 (unchanged);
    soak 5000 debug games clean; check_chance 0 collisions. The one failure
    is older and log-only: Pursuit KOing a switching Natural Cure holder
    (Showdown prints its silent -curestatus before the faint). Not done:
    the 10k run, `check_frame` scenarios.
  - Known gap: Endure doesn't save a Pokémon from a future attack landing
    the same turn (no `Turn` at the residuals; Showdown would).
  Groups 1-4 done:
  - Group 4 (2026-10-09, the user's bar 95%): Showdown's lockedmove,
    uproar, rollout/iceball, furycutter, rage and gen3 bide. Locks renew
    through `ActivePokemon::renewed` and run out in `lock_residuals` (after
    Perish Song) or the Uproar stage; the canceler's stops call
    `abort_move` (Showdown's MoveAborted: only a two-turn charge and Bide
    end); locked turns use no PP; Thrash's trueDuration is read and
    recorded at the second residual, where it shows (reading it at the
    restart broke the search after a mid-turn KO). Also, as Showdown:
    Uproar wakes only the Pokémon it hits (onTryHit), its sleep `-fail`
    comes before Sleep Clause, Rage's boost is at onHit per hit (`rage()`),
    Rapid Spin's self effect comes before the contact ability. Results
    (`tools/teams/group4.txt`): seed 701 200/200, seed 702 1000/1000;
    soak clean; check_chance one winner-HP false positive. teams/adv seed
    8: 992/1000, the same 8 Beat Up and Rock Blast rare-branch games as
    before. Not done: the 10k `run_group.sh 4`, `check_frame` scenarios.
  - Group 1: 10k games 9936 matched. Group 2 (2026-10-09): 9956; OHKO
    moves only in `tools/teams/group2_ohko.txt` (soak, check_chance).
  - Group 3 (2026-10-09): Fly, Dig, Dive, Bounce, Razor Wind, Skull Bash,
    Sky Attack share Solar Beam's two-turn path (`two_turn`, in `hit()`);
    Hyper Beam and kin set `recharge`. As Showdown: the charge expires with
    the next turn if its move never ran (`recharge_timer` doubles as its
    timer), Razor Wind's high crit, Whirlpool hits Dive. 10k games:
    9998 matched (the 2 failures match at a 30M-try cap:
    rare branches); soak and check_chance clean. The teams/adv baseline,
    rerun after group 4: 992/1000. `check_frame` covers Dig, Skull Bash, the charge expiry
    and Hyper Beam.
  - Fixed during group 3, as Showdown: the Morning Sun family at full HP
    prints no -fail; Fake Out counts move actions (`move_actions`,
    Showdown's activeMoveActions), not turns; in a double replacement, a
    first arrival that faints to Spikes cancels the second's switch-in
    effects (`arrive_both`). `check_showdown` skips `stall` once the
    battle has ended (the 3 Perish Song games).
  - Checked: the group 2 Whiscash/Magnitude failures are rare branches
    (all match at a 30M-try cap), not a Magnitude bug. Known gap: in that
    double replacement Showdown also keeps the survivor's old toxic stage
    (its stage lives on the status and persists across switches; ours is
    in `ActivePokemon`, reset by `enter()`). Fixing it needs a per-Pokémon
    counter (layout); 2 group-2 games. Unfixed.
  - check_chance's terminal-state comparison flags winner-HP-only
    differences (1-2 per run, false positives; the user declined changing
    the test). Deferred: the log buffer bound (`MAX_LOG_SIZE` is a
    placeholder 2048).
- `check_showdown` doesn't build under clang (`json.hpp`'s incomplete
  `std::pair` and a `-Wbitwise-instead-of-logical` at the team setup);
  g++ builds it. Predates group 3d.
- Speed, for reference: about 1.1M `update`/s in the release soak (random
  choices, Chance and Calc on, one thread). Not yet compared like for like
  with ADV-Simulator (its 19k is MCTS iterations/s) or libpkmn.
- The fix to one team in `../teams/adv/` (Leer Mence's Forretress lost its
  `IVs: 3 Atk`) is uncommitted in the birch repo.

**Open:**

- Check the `Actions` and `Durations` drafts against the `Battle` layout.
- A `Move` sentinel for `MOVE_UNAVAILABLE` in the
  generator; bounds of `bide_damage` and `future_sight_damage` (u32 for now).
- The per-`Effect` behaviour (hand-written with the mechanics).
- Extend `tools/coverage` to the abilities and hold effects `teams/` uses.
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
