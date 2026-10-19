# Hoenn: a libpkmn-style Gen III (ADV) engine in C++

A header-only Gen III singles engine (Smogon ADV OU) for MCTS and
policy-gradient RL, in `namespace hoenn`, used directly by a C++ driver (no
C API, no compiled library). It follows libpkmn's design: a small POD battle,
one `update` per turn, the choice/result protocol, and the Log, Chance and
Calc options. MCTS needs Chance (`Durations` to resample hidden state,
`Actions` to key chance nodes) and a small, cheap-to-copy state; policy
gradient needs the log, which it tokenizes.

**Be cautious, and don't push ahead.** While a design choice is still open,
don't offer to write, add or commit the thing it affects. Lay out the open
choices and their tradeoffs, give a recommendation, and wait for the user to
decide. Point out assumptions and unverified claims rather than smoothing
over them. (Following Showdown is not an open choice.)

**Singles only.** Skip the decomp's doubles-only branches (ally targeting,
spread damage, `gBattlersCount > 2`) and don't size anything for them.

**Keep this file lean**: working rules, the interface, a short status.
Mechanics, the turn frame, the protocol and the deviations are in
`docs/ENGINE.md`; tests, bars and known gaps in `docs/TESTING.md`;
performance in `docs/OPTIMIZATION.md`; RL plans in `docs/RL.md`; details in
header comments; history in git.

## Priorities, in order

1. **Safety.** Week-long paid RL runs must not die or be quietly corrupted.
   - `update` never throws, allocates or recurses without a bound. Every loop
     has a fixed bound. Given valid input, every index is in range by
     construction.
   - **Don't hold it wrong, as libpkmn.** The caller passes only choices from
     `choices()` and battles that are `valid()`. Debug asserts both; release
     checks neither (bad input is UB). `update` doesn't return
     `Result::Error` for bad input.
   - `Battle` stays trivially copyable, with no pointers, strings or
     containers, and its size is `static_assert`ed.
   - Assert liberally in mechanics: preconditions, invariants, unreachable
     branches. Debug and test builds run under ASan and UBSan with asserts.
   - The turn cap: `update` returns `Tie` once `turn` reaches 1001
     (`MAX_TURNS`), so turn 1000 is played, as Showdown
     (`sim/battle.ts:1843`).
2. **Speed.** Search copies and steps the battle millions of times; RL steps
   it with the log on. Null options compile to nothing.
3. **Correctness where competitive play reaches it.** Obscure interactions
   the OU teams never produce can wait.

## Mechanics references

- **Showdown is the target; the decomp is the skeleton.** Mechanics are
  direct functions in the decomp's shape (its order of checks, state changes
  and draws; no script VM or event system) that behave as Showdown's gen3.
  Where they differ, follow Showdown without asking: verify it in
  `showdown/` (cite file:line), comment the difference at the code, and add
  a visible one to `docs/ENGINE.md`, "Showdown and the cartridge disagree".
- **Write mechanics analytically:** understand the whole mechanic, then write
  one code path. Tests check the result; they don't drive the design.
- **Link battles are the default branch** where the decomp branches on
  `BATTLE_TYPE_LINK`; exceptions are in `docs/ENGINE.md`.
- Take libpkmn's structure and conventions only; ignore its `showdown` mode.
- **Decomp citations:** main:N (`src/battle_main.c`), util:N
  (`src/battle_util.c`), bsc:N (`src/battle_script_commands.c`), bs1:N
  (`data/battle_scripts_1.s`), at pret/pokeemerald `731ad5b`, which is
  outside the repo. `showdown/` is a symlink to the pinned checkout
  (`tools/criteria.toml`, `[showdown]`); `tools/run_env.py` keeps it built
  in `.showdown/<hash>/`. Cite both by path.

## Interface

- **`update(battle, c1, c2, options) -> Result`** runs one turn, or the rest
  of one after a mid-turn request. At turn 0 both pass to start the battle.
- **`choices(battle, player, request, out) -> n`**, at most 9.
- **Choice** is one byte, `{Pass | Move | Switch, data}`. Move data is the
  move slot 1–4, or 0 for Struggle or a forced continuation; Switch data is a
  SLOT 2–6.
- **SLOT is order-dependent, as libpkmn.** `Side::pokemon[6]` never moves;
  SLOT `s` is `pokemon[order[s - 1] - 1]` (`Side::get(s)`), SLOT 1 is
  active, and a switch swaps `order[0]` and `order[s - 1]`. Per-Pokémon
  data in Chance and the interface is by SLOT; the log's `ID` is the party
  position. A move slot (1–4) is a different thing.
- **Result** is one byte, `{None | Win | Lose | Tie | Error, p1, p2}`, with
  the Choice type each player makes next: a faint replacement is `Switch`
  for one side and `Pass` for the other; Baton Pass's is a mid-turn
  `Switch`.
- **Options** are `{log, chance, calc}`, each a real or a null type.
- **The log** is Showdown's omniscient stream in two layers: `update` writes
  libpkmn-style binary opcodes, and `format.hpp` turns them into Showdown's
  text line for line, `|split|` triplets included (`docs/ENGINE.md`,
  "Protocol").

### Chance

Chance assumes perfect information (open teams). The battle holds the true
hidden counters (sleep, confusion, Disable, Encore, binding, rampage,
Uproar), rolled at infliction. Chance holds the public side: `Durations`
(steps each counter has visibly taken), `Actions` (the random branch taken)
and `probability`. `chance.hpp` catalogues every reachable draw.

- **No RNG frame fidelity.** Draw wherever convenient, but record a draw in
  `Actions` and `probability` only when its outcome can change the
  perfect-information state. A draw that changes only the log is made but
  not recorded. A terminal state is the same whatever HP the winner has.
- **`Actions` determines the branch exactly, with no hashing.** Given the
  battle and both choices, it determines the `Result`, `Durations` and all
  public state; only true hidden counters may differ (`randomize_hidden`
  resamples them). Every draw that can affect public state has its own
  field, sized for its multiplicity in one update; overflowing or
  rewriting a field asserts.
- **Hooks:** one roll helper per catalogue row in `effects.hpp` does the
  Calc override, the draw and the Chance record, under
  `if constexpr (Chance::enabled)`. The null types are only
  `{ static constexpr bool enabled = false; }`.
- **Calc** (`calc.hpp`): overrides are an `Actions`; nonzero forces that
  branch. Hidden rolls are set in the battle instead.

## Build and tests

CMake, for tests and tools only; the library is the `INTERFACE` target
`hoenn` (C++20). `cmake --preset debug` (asserts, ASan, UBSan) or `release`,
then `cmake --build --preset X` and `ctest --preset X`. Tests build with
`-Wall -Wextra -Wpedantic -Werror`. Targets: `headers`, `check_data`,
`check_teams`, `check_damage`, `soak`, `check_chance`, `check_frame`,
`check_showdown_fixture`; the full environments run with
`python3 tools/run_env.py ENV`. Regenerate the damage cases with
`node tools/damage_oracle.cjs > tools/damage_cases.inc`. Everything else
(suites, bars, CI) is in `docs/TESTING.md`.

`teams/ou/` holds the Smogon OU teams; `tools/teams/` the synthetic group
teams; `tools/fixtures/showdown.jsonl` one committed game per fixed failure.
`src/data/` was generated once from the decomp and is edited by hand now.

## Status (2026-10-18)

The engine is complete: every move, item and the log are written. All three
environments pass their bars (OU 99.96%, OU-legal 99.89%, random 99.92% at
619f1e4, with 4 fixes since). Work is fixing what the tests find and speed:
three optimizations (2026-10-17) gave about +22% on `bench --log`; PGO and
the next candidates are in `docs/OPTIMIZATION.md`.

**Open:**
- A Rollout checker artifact (2 games, needs a decision) and OU game 15
  (toxic, unconfirmed); `docs/TESTING.md`, step 8.
- `build/runs/classify.py`'s KNOWN list is out of date.
- `MAX_LOG_SIZE` is a placeholder 2048 until the bound is derived (the debug
  `Writer` asserts it).
- A `Move` sentinel for `MOVE_UNAVAILABLE` in `src/data/`.
- Skipping the damage roll when every roll KOs (fewer chance nodes, but it
  changes the `Actions` key).

**Deferred:** `min_ko_roll` (spec in `calc.hpp`), Calc's `Summaries`,
explicit state tracking from the log, and the search adaptation.
