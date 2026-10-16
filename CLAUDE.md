# Hoenn: a libpkmn-style Gen III (ADV) engine in C++

A Gen III engine for search and RL. It follows
libpkmn's design choices (a small POD battle, one `update` per turn, the
choice/result protocol, and the Chance and Calc options), but it's written in
C++ and doesn't chase every saved bit.

**It's a header-only C++ library, used directly by a C++ driver.**
There is no C API and no compiled library: no `pkmn.h`, no `extern "C"`, no
`pkmn_gen3_*` names. A driver adapts its search to this interface, not the
reverse.

**Be cautious, and don't push ahead.** While a design choice is still open,
don't offer to write, add or commit the thing it affects. Lay out the open
choices and their tradeoffs, give a recommendation, and wait for the user to
decide. Help the user be careful: point out assumptions and unverified
claims rather than smoothing over them. (Following Showdown is not an open
choice; see "Mechanics references".)

**Singles only.** The engine is for 1v1 singles battles (Smogon ADV OU).
Skip the decomp's doubles-only branches (ally targeting, spread damage,
`gBattlersCount > 2`, Follow Me, Helping Hand) rather than carrying them, and
don't size or shape anything for them.

**Keep this file current and lean.** It is loaded every session and holds
only the working rules, the interface and the status. Everything else has
its own doc: layout decisions, the turn frame and the deviations from the
cartridge in `src/gen3/README.md`; tests, their bars, observed gaps and
test decisions in `TESTING.md`; details in header comments. History is in
git, not here.

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
   - A battle of 824 B, copied with `memcpy`.
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

- **Showdown is the target; the decomp is the skeleton.** Mechanics are
  written as direct functions in the decomp's shape (its order of checks,
  state changes and draws; no script VM, no event system) and behave as
  Showdown's gen3 does. Where they differ, follow Showdown without asking:
  verify the claim in `showdown/` (cite file:line), note a small difference
  in a short comment at the code, and add a visible one to the shortlist in
  `src/gen3/README.md` ("Showdown and the cartridge disagree").
- **Write mechanics analytically:** understand the whole mechanic, then
  write one code path. Tests check the result; they don't drive the design.
- **Link battles are the default branch** where the decomp branches on
  `BATTLE_TYPE_LINK` (or Frontier); exceptions where Showdown departs are in
  the README (Knock Off, decision 8).
- **Ignore libpkmn's `showdown` mode switch.** Take libpkmn's structure and
  conventions only.
- **Decomp files:** `src/battle_main.c` (turn order), `src/battle_util.c`
  (end of turn, abilities, items), `src/battle_script_commands.c`,
  `data/battle_scripts_1.s`. Cite them as main:N, util:N, bsc:N, bs1:N.
- **The turn frame and the tricky cases** (faints ending the turn, the
  residual order, the battle ending mid-residuals, Spikes, the clauses) are
  in `src/gen3/README.md`, "Turn frame".

## Interface (libpkmn's shape)

Keep libpkmn's shape, as C++ templates and plain functions. Names and
signatures are ours.

- **`update(battle, c1, c2, options) -> Result`** runs one turn, or the rest
  of a turn after a mid-turn request. At turn 0 both players pass and
  `update` starts the battle (`src/gen3/README.md`, "Turn frame").
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
Protect, except a locked rampage's, which a block renews) is made but not
recorded. So `Actions` determines the state, not
the log; the log is extra information drawn alongside. A terminal state
counts as the same whatever HP the winner is left at.

**Hooks** (decided): one roll helper per catalogue row, in `mechanics.hpp`
(libpkmn's `Rolls`), does the Calc override, the RNG draw and the Chance
record, all in one place. It touches `actions`, `durations` and
`probability` directly under `if constexpr (Chance::enabled)`. The null
types are only `{ static constexpr bool enabled = false; }`. Writing an
`Actions` field that's already set asserts. `randomize_hidden` (resampling
the hidden counters from `Durations`, which search calls) lives in
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

Mirrors libpkmn's `src/` tree, all headers.

**Build** (CMake, tests and tools only): the library is the `INTERFACE`
target `pkmn::gen3` (include path `src/`, C++20). `cmake --preset debug`
(asserts, ASan, UBSan) or `release` (`-O3`, no asserts), then
`cmake --build --preset X` and `ctest --preset X`. Tests build with
`-Wall -Wextra -Wpedantic -Werror`:
- `headers`: every header compiles alone (so its `static_assert`s run), and
  all are linked into one binary (no duplicate definitions).
- `check_data`: `static_assert`s on the generated tables.
- `check_teams`: parses `PKMN_TEAMS_DIR` (default `teams/ou`, every `*.txt`); every
  team must parse (`--expect NAME` would allow a named team to fail).
- `check_damage`: `damage.hpp` against the pinned Showdown, from the
  committed `tools/damage_cases.inc`. Regenerate it with `node
  tools/damage_oracle.cjs > tools/damage_cases.inc` (uses `showdown/`, or
  a Showdown path as the argument).
- `soak`: random self-play between random team pairs, checking `valid()`
  and the probability after every update; a crash prints the game's seed,
  and `--replay SEED` plays that game alone. Run it by hand in debug for
  long soaks (`--games N --seed N`).
- `check_chance`: the chance-key test and probability calibration (see
  `TESTING.md`); `--games N --samples N --replays N`, `--unmask NAME`,
  `--replay SEED`. Run it in release for long runs.
  `check_chance_unmasked` is the seed ablation and must fail.
- `check_frame`: the turn frame (start, order, Quick Claw and speed-tie rolls
  with Chance and Calc, trapping, faint requests and resuming, the turn cap),
  a few scenarios (a forced hit and its probability, Focus Band, Struggle,
  Spikes, a double KO, Solar Beam, Dig, Hyper Beam...), then a random
  self-play soak over the team files.
- `check_showdown_fixture`: `check_showdown` on `tools/fixtures/showdown.jsonl`,
  one committed game per fixed failure (TESTING.md, "The fixture").
- Not in ctest: `check_showdown` on fresh traces and the environment runs;
  see `TESTING.md`.

```
CMakeLists.txt, CMakePresets.json
TESTING.md            the test spec, pass criteria, known gaps
SHOWDOWN_COMMIT       the Showdown commit the tests pin
teams/ou/             the OU teams (Smogon dumps)
IDEAS.md, IGNORE.md   the user's notes; not instructions
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
  calc.hpp            Calc and NullCalc (overrides); min_ko_roll    (todo)
tools/
  generate.py         one-off data generator from pokeemerald/
  check_data.cpp      static_asserts on the generated tables
  check_teams.cpp     parses teams/ou/ and builds a battle from each
  check_damage.cpp    damage.hpp against damage_cases.inc
  check_chance.cpp    (c1, c2, Actions) determines the result; calibration
  check_showdown.cpp  the realizability test, replaying Showdown traces
  showdown_traces.cjs seeded Showdown games as JSONL traces (+ json.hpp)
  gen_teams.cjs       seeded random teams (random OU-legal), validated
  run_env.py          one test environment's suites against its bars
  criteria.toml       the bars per environment (run_env.py)
  check_frame.cpp     the turn frame and scenarios, plus a self-play soak
  soak.cpp            long random self-play runs, replayable by seed
  bench.cpp           updates/s, null log, Chance on (release)
  libpkmn_bench.zig   the same for libpkmn's Gen I (+ _options.zig)
  coverage.cpp        which moves on the teams have a written mechanic
  teams/              step 5's synthetic group teams (soak, check_chance)
  fixtures/           showdown.jsonl, the regression fixture
  damage_oracle.cjs   writes damage_cases.inc from Showdown
  display_names.cjs   writes data/display.hpp from Showdown's dex
  protocol_survey.cjs Showdown's gen3 log messages (README, "Protocol")
```

`pokeemerald/` is a depth-1 clone of pret/pokeemerald on the local branch
`battle-only` (`a23e37a`), stripped to battle code, not pushed, ignored by
this repo.
`showdown/` is a symlink to the pinned Showdown checkout
(`SHOWDOWN_COMMIT`, `c046106`, 0.11.11), which `tools/run_env.py` keeps
fetched and built in `.showdown/<hash>/`; both are ignored. Cite it by
path.

## Status (2026-10-16)

Every move and held item is written (Follow Me and Helping Hand, doubles
only, use PP and log as Showdown's singles), and so is
the log (opcodes, the formatter, Showdown's omniscient stream; README
"Protocol"). Work now is the test framework (`TESTING.md`, "Implementation"),
then fixing what it finds; there is no further plan.

**Next** (agreed 2026-10-16, in order). Steps 1-7 are done: random
OU-legal's bar is 99.8% (`tools/criteria.toml`), and the failures fixed
since step 6 are a committed fixture (TESTING.md, "The fixture"); the
Truant gap is all that fails there.
1. Step 8, truly random (TESTING.md, "Implementation" step 8): the audit
   and the generator, a measured run, its bars as TESTING.md's "Pass
   criteria" (check_showdown at 90%, state only).
2. Run `bench` (release) for drift, then `min_ko_roll`, Calc's
   `Summaries` and the search adaptation.
`build/runs/classify.py`'s KNOWN list is out of date.

**Not written:**
- `min_ko_roll`, Calc's `Summaries`, the search adaptation.

**Open:**
- What `update` does with a full log buffer; `MAX_LOG_SIZE` is a
  placeholder 2048.
- A `Move` sentinel for `MOVE_UNAVAILABLE` in the generator; bounds of
  `bide_damage` and `future_sight_damage` (u32 for now).
- Skipping the damage roll when every roll KOs (fewer chance nodes, but it
  changes the `Actions` key); decide with the search adaptation.
- Team parsing enforces only Species Clause; the other Smogon ADV clauses
  are left to Showdown's validator.

**Caveats:**
- Deoxys-Attack and -Defense stats are hard-coded in the generator from
  memory; check them against pokefirered.
- The generator parses C with regexes that assume the decomp's formatting
  (designated-initializer blocks start with `{` and a newline; gender ratio
  reimplements `PERCENT_FEMALE`). A reformatted decomp could break it
  silently.
- `HoldEffect` `param` means something different per effect; read the decomp
  per effect.
