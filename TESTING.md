# Testing

The spec for the engine's tests: what each suite checks, the environments
it runs in and what passing means. Written before most of it is built
(2026-10-15), so it doubles as the plan; each section says what exists.
Figures marked *proposed* are first guesses, to revise after a measured
baseline.

## What "passing" means

The engine passes when every suite meets its bar in every environment, on
a run whose seeds, engine commit and Showdown commit are recorded. Three
kinds of check, in priority order (CLAUDE.md, "Priorities"):

1. **Safety:** no crash, no `valid()` failure, no sanitizer report, no
   choice-set mismatch. Zero tolerance in every environment.
2. **Chance keys:** `(c1, c2, Actions)` determines the result. Zero
   collisions in every environment: a collision is an engine bug whatever
   the team.
3. **Matching Showdown:** the share of Showdown games we reproduce to the
   end. This is the only bar that relaxes from one environment to the next.

## Environments

| | OU | Random OU-legal | Truly random |
|---|---|---|---|
| Teams | `teams/ou/*.txt` (real Smogon teams) | generated, kept only if gen3ou's validator accepts | generated, no legality at all |
| Showdown format | `gen3ou` | `gen3ou` | `gen3customgame` + the two clause mods |
| Strictness | highest | middle | lowest |
| Built | yes | yes | yes |

The two random environments use generators, not the step 5 group files
(`tools/teams/group*.txt`), which stay in git history. The banned moves
those files carried for `soak` and `check_chance` only (OHKO moves,
Swagger, Assist) are then reached by the truly random environment, whose
`gen3customgame` allows them.

### Generators

One seeded Node script writes teams as Showdown export text, so every
consumer (`soak`, `check_chance`, the trace script) reads files as now.
Each file starts with a comment giving the mode, the seed and the Showdown
commit, so a generated team keeps its provenance.

- **Random OU-legal** (`tools/gen_teams.cjs --mode ou-legal`; decided
  2026-10-16). Uniform rather than usage-weighted, for coverage.
  `gen3randombattle` won't do: its levels vary by species and its sets are
  curated.
  - Per slot: a species uniform over the 366 gen3ou allows that have an
    allowed ability (374 less Sandshrew and the like, whose only ability is
    Sand Veil or Soundproof), distinct within the team; an ability uniform
    over its allowed ones; up to 4 distinct moves uniform over those
    Showdown's `checkCanLearn` accepts and gen3ou doesn't ban (Smeargle:
    every move; bare Hidden Power, typed by the IVs); IVs uniform 0-31;
    508 EVs in random multiples of 4, at most 252 per stat; a nature
    uniform; level 100. Happiness is written as 0 with Frustration
    (Showdown's import), so the parser's Frustration-and-Return deviation
    can't arise.
  - Bans are filtered before drawing, so they don't thin any species. A
    set `validateSet` still rejects (egg and event combinations, the OHKO
    clause) redraws its moves; then `validateTeam` (the Baton Pass clauses)
    redraws the team, about 0.7% of teams. 2,000 teams take 6 s. On
    re-import every team passes `validateTeam`; Showdown only fills in
    names and single-gender species' genders.
  - **Items** (decided): the 59 of Showdown's gen3 items that map to an
    engine effect item and gen3ou allows (not Quick Claw, Bright Powder or
    Lax Incense), or none 10% of the time. The list is read from
    `names.hpp`. Not the 42 that parse as `Other`: the state handles them,
    but the formatter names `Other` and `Mail` as "", so Trick, Knock Off,
    Thief, Covet and Recycle would mismatch the log until `Other` carries
    its identity. (The draft's "they don't parse" and "the engine's 74"
    were wrong: every decomp item parses, and 9 engine items are invalid
    or unobtainable in Showdown's gen3.)
  - **Teams** (decided): `run_env.py` generates a fresh pool per run from
    the run seed (`generate` in `criteria.toml`, 2,000 teams) into the run
    directory as `teams.txt`, and all three suites read it. The file
    starts with a comment giving the mode, the seed and the Showdown
    commit; both parsers skip text before the first `===`.
  - **Order** (decided): the generator, then a 1,000-game exploratory run
    to rank the gaps by frequency, fix them, then the 10,000-game run that
    sets the bar.
  - Known gaps it will reach: the unwritten items (Leppa, the flavor
    berries, King's Rock, Shell Bell; the last two can act on most
    hits), and Follow Me and Helping Hand, which Showdown's singles logs
    as a failure (lines unchecked) where the engine only uses PP (state
    the same, log only).
- **Truly random** (`tools/gen_teams.cjs --mode random`; decided
  2026-10-16). No legality at all: every field is drawn independently and
  uniformly, so it fuzzes rather than covers. Not Showdown's
  `ExhaustiveRunner`, which cycles pools for coverage.
  - Per team, 1-6 Pokémon. Per Pokémon: a species over every gen 3 one
    both sims name (389: Castform's battle-only formes left out), repeats
    allowed; an ability over all 76, whatever the species; 1-4 distinct
    moves over all 353 but Struggle (bare Hidden Power, typed by the IVs);
    an item over the 104 both name (every decomp item but Mail and Friend
    Ball, `Other` included), or none 10% of the time; level 1-100; IVs
    0-31; EVs 0-255 each, no total; a nature; happiness 0-255 (not 255
    with Frustration: export drops 255, and the two parsers' defaults then
    differ); gender M, F or unstated (Showdown's then picks, and the trace
    carries it), whatever the species.
  - Where it stops short (decided): distinct moves, since Showdown keys
    Disable, Encore, Mimic and Sketch by move ID and the engine by slot,
    and level 100 at most, the engine's layout.
  - No validation anywhere (decided): `helpers::check_pokemon` checks only
    the engine's bounds (IDs, level, IVs, EVs 0-255, a packed and distinct
    moveset, an ability), and the parser has no Species Clause. The other
    environments are validated by the trace script's gen3ou validator.
    Showdown's battle takes any gender a set gives and clamps EVs to 0-255
    with no total (`sim/pokemon.ts:340,397`).
  - Showdown plays `gen3customgame@@@Sleep Clause Mod,Freeze Clause Mod`
    (`showdown_traces.cjs --format custom`): the engine always applies the
    two clauses, Custom Game has neither. It has no Endless Battle Clause,
    which the engine doesn't have either; the turn limit isn't part of it
    (`sim/battle.ts:1842`).
  - `check_showdown` compares items as engine items, so two `Other` items
    match (Showdown names them, the engine doesn't).
  - Reached here and nowhere else: foreign abilities (Truant, Wonder
    Guard, Forecast off Castform), teams under 6, level differences (OHKO
    moves, Seismic Toss, Night Shade, Psywave), the Uber species and
    Deoxys formes (whose stats the generator took from memory), the OU
    bans (OHKO moves, Swagger, Assist, evasion moves and items, Sand Veil,
    Soundproof), Quick Claw (unlogged in Showdown's gen3; unverified that
    `check_showdown` matches its order) and the `Other` items, whose log
    lines mismatch (log only, uncounted here).

## Team input

**Decided (2026-10-15):** tests take any number of team files, so the OU
dump needn't be copied into `all.txt`, and each team keeps its source.

Decided (2026-10-15): every tool that takes teams takes a list of team
files as positional arguments (one file is a list of one) and pools every
team in them. Same behaviour in every tool. Built (2026-10-15); the
trace's game record carries each team's `file` beside its index there.

**The OU teams are in this repo** (the user, 2026-10-15): `teams/ou/`,
the Smogon dump files with their README. The tools and ctest take every `*.txt` there,
whatever its name, so a file can be added or renamed without touching
them. Fixed in the copy, so Showdown's gen3ou validator accepts them: 13
Pokémon with no EVs line got `EVs: 1 HP` (Showdown's own advice for 0 EVs;
no stat changes), and the nickname `(snowlight nxc)`, which Showdown's
importer read as a species, lost its parentheses. The 7 teams that break
today's gen3ou rules (Quick Claw, Soundproof, the Baton Pass clauses)
were removed (the user, 2026-10-15): 320 teams, all valid.

Proposed:

- A team is identified by (file, index in the file, title). The trace's
  game record carries both teams' identities, and every failure report
  prints them.
- Some teams appear in more than one file (e.g. "DDSala OffCune
  PursuitMeta RestLax SDHera"). Pooled as given, they're drawn twice as
  often; no dedupe.
- The game-to-team pairing depends on the order of the loaded teams, so
  old traces' seeds won't give the same pairs afterwards. Fresh seeds make
  that moot (below).

## Seeds

**Decided (2026-10-15):** runs use fresh seeds, so repeated runs find new
cases. As libpkmn's fuzz (`src/test/fuzz.zig:53`): a seed passed on the
command line is used, otherwise one is drawn from the OS; it is printed at
the start and in the summary, and a failing game replays alone from it
(`soak --replay`, `check_chance --replay`, `check_showdown --game`).

`check_showdown`'s branch search is already seeded from `--seed` and the
game number (`tools/check_showdown.cpp:637`), so a run is reproducible
from its trace seed and its search seed.

With fresh seeds, a bar on a rate fluctuates from run to run. A game-match
rate of 99% over 10,000 games has a standard deviation of 0.1 points, so
each bar sits at least 4 standard deviations below the measured rate:
a run that fails is a regression, or a new case worth a look, not bad
luck. A failing game found this way becomes a committed fixture.

### The fixture

`tools/fixtures/showdown.jsonl` holds one Showdown game per bug fixed from
the step 6-8 runs (40 games, 2026-10-17), replayed by ctest's
`check_showdown_fixture` at `--seed 1`. Each game is cut one step after
the step that failed (the next step gives the request check), and its
header's `fixture` field names the bug, the commit that fixed it and the
run it came from. Replayed at the commit before its fix, each game fails,
and the games for later fixes fail with it. Games are renumbered within
the file, and the search seed depends on the game number, so a renumbered
game must be checked again. The worst step needs about 15,000 of the
1,000,000 tries.

Adding one: take the failing game from the run's traces, keep the header
(drop `p1.file` and `p2.file`, add `fixture`) and the steps through one
past the failing step, renumber it, and check that it fails before the fix
and passes after. Truant games stay out while Truant is a known gap. The
8 failures of the two early OU runs are left out: their traces were
deleted.

## Suites

Built and in ctest (both presets): `headers`, `check_data`, `check_teams`,
`check_damage`, `check_frame`, a 100-game `soak`, `check_chance`,
`check_chance_unmasked` (must fail) and `check_showdown_fixture`. These are the fast suites; they run
on every change.

The three below are the environment suites. Their run times on this
machine, 16 threads (2026-10-15, groups 1-4): 10,000 games of
`check_showdown` take 5-10 minutes (traces 3-4 minutes on 4 Node
processes, the check 1-7); `soak` runs about 13,000 updates/s per debug
thread (1,000 games in 6 s); `check_chance` takes about 3.5 minutes per 100
games at 128 samples on one thread.

### soak

Random self-play under ASan and UBSan (debug), `valid()` and the
probability checked after every update. Fails on any crash or failed
check.

### check_chance, the chance-key test

The user's spec (2026-10-08). At each state, sample many (RNG seed,
`randomize_hidden`) pairs, run `update` for each choice pair, and map
`(c1, c2, Actions)` to the result: the battle under `mask_hidden` (no seed,
hidden counters set to 1 if active), the `Durations`, the `Result` and the
probability. Two samples with the same key must give the same result. It
finds collisions only among branches the samples reach; mutating the
damage-roll or the sleep-end record is caught.

- **Forced replays:** each key found is replayed with its `Actions` forced
  through Calc, so a key the free samples saw once is still compared.
  About 0.1-0.3% of keys stay uncompared: the hidden ends (sleep,
  confusion, binding) aren't forceable, so their replays can land
  elsewhere.
- **Probability calibration:** per pair, each key expected at least once
  in the free samples has a count that fits its probability (Chernoff,
  1e-10), the keys' sum is at most 1, and held-out
  draws landing on unfound keys fit 1 − sum. Over the run, held-out hits
  are binned by key probability (powers of 2, plus "unseen") and each
  bin's count must have a tail chance of at least 1e-7: a normal tail
  with the exact variance when that is at least 100, else Poisson
  (decided 2026-10-16; a z-test called a 2% event in a bin expecting
  0.02 hits z 7.0). Keys expected less than once are left to the bins
  (decided 2026-10-16): Beat Up's ~3e7 keys repeat some unlikely key by
  chance. Diagnosed on a five-hit key at 4.6e-8 seen 2 of 128 times
  (`check_chance --samples 128 --replay 7066941967204212297
  teams/ou/*.txt`, turn 9): the hits' crits and rolls were independent
  and uniform, and over 2M rounds of 128 the old test fired 204 times
  against 182 predicted for correct probabilities. Crits drawn at 1/16
  but reported at 1/4 or 1/32 still fail the bins (p < 1e-8). The per-pair tests alone are too weak (they
  missed rolls reported at 1/17). Because samples go through
  `randomize_hidden`, this checks it against the reported probabilities
  (consistency, not truth). Mutations caught: crits at 1/4, rolls at
  1/17, crit yes/no swapped.
- **Mask ablation** (`--unmask`, `check_chance_unmasked`): the seed
  collides in every game, sleep in 19 of 50, Encore in 4 of 50, confusion
  in 30 of 30 mirror games of the one Confuse Ray team. Binding, Uproar,
  Thrash and Disable weren't ablated when this was measured (2026-10-08,
  before their moves were written); redo it.
- **Terminal states** (decided 2026-10-15, as the engine's rule in
  CLAUDE.md, "Chance"): when the battle ends, two samples with the same key
  must agree on the `Result` and the probability only. Search never
  expands a terminal node. The bar stays strict (0 collisions); these
  aren't allowed as known failures. Built (2026-10-15): the three
  collisions of the last group run (groups 1, 2 and 3) were all of this
  kind (both samples `Lose`, differing in the winner's HP and toxic
  counter) and no longer report.
- **Which probabilities are checked.** Our own only. Two samples with one
  key must report the same probability, and the calibration checks that
  each key's reported probability matches how often our RNG lands on it.
  Nothing compares them with Showdown's: Showdown reports no
  probabilities, and its side of limitation 1 below is unbuilt.

### check_showdown, the realizability test

The user's design (2026-10-07). `tools/showdown_traces.cjs` plays seeded
Showdown games with random choices and writes JSONL traces (teams, seed,
each side's choice set, the chosen pair, the log chunk);
`tools/check_showdown.cpp` replays them. At every state our `choices()`
must equal Showdown's set, and some branch of ours must produce
Showdown's log: `update` rerun from fresh seeds and `randomize_hidden`
until one matches, up to a try cap. **Never copy Showdown's hidden
counters:** correct `Durations` must make the true value samplable, and
the test checks exactly that. A state guard (HP, status, PP, items,
boosts, main volatiles, Substitute HP, Protect's count, side conditions,
weather, residual timing, future attacks) runs alongside the log
comparison, since the log hides some state.

- It assumes the omniscient log identifies the resulting state.
  Exceptions: draws with no effect, and Quick Claw, which Showdown's gen3
  doesn't log (`data/mods/gen3/items.ts:301`), so the traces ban it.
- Decided (2026-10-08): random branch search, no inference code; compare
  log lines; pin `showdown/` (0.11.11, `c046106`); offline trace files.
- Not in ctest (traces aren't committed) and doesn't build under clang
  (`json.hpp`'s incomplete `std::pair`, a `-Wbitwise-instead-of-logical`
  at the team setup).

### Others

- `bench` (release; `--log` for the real log): updates/s on the OU teams.
  2026-10-16, `47a807a` plus `--log`, 3 × 100,000 battles, GCC 15.2, idle
  machine; `tools/libpkmn_bench.zig` for libpkmn's Gen I (zig 0.16), with
  `libpkmn_bench_options.zig` or `libpkmn_bench_log_options.zig`:

  | | null log, Chance on, Calc off | log, Chance and Calc off |
  |---|---|---|
  | hoenn | 1.95M/s | 1.63M/s (−16%) |
  | libpkmn Gen I | 8.5M/s | 8.0M/s (−6%) |

  The log mode turns Chance off too, so its cost is the log's net of
  Chance's. `9adccd4` gives about 2.0M with the null log on the same day,
  so its recorded 0.94M was a bad measurement, not the engine. 10,000-battle
  runs vary by about 6%; compare commits on the same day, interleaved.
  ADV-Simulator's update about 1.4 µs. None is like for like.
  Not a pass/fail suite.
- `coverage`: which moves on the given teams have a written mechanic
  (100% on `teams/ou/`). Not a pass/fail suite.

## Pass criteria (proposed)

"Matched" is a `check_showdown` game reproduced to its end, state and log,
within the cap. Every bar is per run.

**Multi-hit failures are allowed** (decided 2026-10-15, every
environment): a game whose first unmatched step is a multi-hit move
(Effect::MultiHit, Beat Up, Double Kick, Triple Kick, Twineedle,
Bonemerang, or one of these called by Metronome, Sleep Talk, Mirror
Move or Assist) counts as neither matched nor failed: it leaves the
denominator, and the summary reports how many. Built (2026-10-15): a
failing step is multi-hit when any `|move|` line in Showdown's log of it
names such a move (a called move has its own line), whichever side used
it. Seed 8's 8 failures (1,000 OU games) are all classified so. The cost: a real
multi-hit bug past the first hit would hide here, so `soak` and
`check_chance` are its only cover.

| | OU | Random OU-legal | Truly random |
|---|---|---|---|
| `check_showdown` games | 10,000 | 10,000 | 10,000 |
| try cap | 1M | 1M | 1M |
| matched | ≥ 99.9% | ≥ 99.8% | ≥ 90% |
| log-only mismatches | count | count | reported, not counted |
| choice-set mismatches | 0 | 0 | 0 |
| `soak` (debug) games | 100,000 | 100,000 | 100,000 |
| `soak` failures | 0 | 0 | 0 |
| `check_chance` games (128 samples) | 1,000 | 1,000 | 1,000 |
| collisions | 0 | 0 | 0 |
| calibration | every bin tail ≥ 1e-7 | same | same |

Rationale:

- **OU at 99.9%** (the user, 2026-10-16). Measured: 9943 of 9946
  counted games (99.97%, seed 8981393492580689921, 2026-10-16); the bar
  is about 4 standard deviations (±0.017) below it. Not rerun at 99.9%.
- **Random OU-legal at 99.8%, log-only counted** (the user, 2026-10-16).
  Measured: 9898 of 9913 counted games (99.85%, seed
  16645649204763325269, engine `d344f32`), 9 of the 15 the Truant gap
  (Known gaps) and 6 Showdown divergences then fixed. The Truant gap
  alone, about 9 per 10,000 games (99.91%, ±0.03), puts the bar about 4
  standard deviations under; a Poisson(9) count passes it 99.9% of the
  time. Against the raw 99.85% it would be only about 1.3 below, so the
  bar assumes the 6 stay fixed.
- **Truly random at 90%, state only.** The log is still compared, since
  it pins the branch where the state guard can't (Conversion 2's type):
  Custom Game is a debug format, so Showdown shows exact HP on every
  public line (`sim/battle.ts:227`), and `check_showdown` formats ours so
  when the trace's tier is Custom Game. A log-only mismatch still ends
  that game's check, so the `Other` items' names (log lines "") hide
  whatever follows them. The first run (2026-10-16) "matched" 1,000 of
  1,000 by stopping at step 0 on the HP; a run with `--no-log` matched
  974 of 998, and the one failure diagnosed (Conversion 2) was a wrong
  branch the log would have ruled out. The bar itself is unmeasured, and the environment reaches
  interactions CLAUDE.md's priority 3 says can wait.
- **The same safety and chance bars everywhere.** A crash or a collision is
  an engine bug whatever produced the team.
- **Not sized for CI.** At these figures one environment takes about 15
  minutes on this machine's 16 threads, likely more than GitHub's hosted
  runners allow. Deliberate: CI comes later.

## CI

Not built; where and how often jobs run is deferred.

**Every suite runs without Claude:** one command per environment, a
nonzero exit on failure, and a summary that says which bar failed. Nothing
needs reading by hand to know pass or fail.

**Gating and reporting.** Both kinds of job run the same suites and pass
or fail. The difference is what a failure marks. A gating job runs on our
pushes and pull requests, so its failure is the red X on our commit: our
change broke something. A reporting job runs on a schedule against
Showdown master; its failure marks only that scheduled run, since the
cause is a Showdown change, not ours. It prompts a look: bump the pin,
cross off a gap Showdown closed, or record a new one.

**Decided (2026-10-15):** gating against the pinned Showdown; a
reporting job against master, defined but not hooked up to Showdown's
pushes yet.

**Decided (2026-10-15): Showdown itself, not `@pkmn/sim`.** Gating uses
smogon/pokemon-showdown at a pinned commit; reporting uses master. The pin
is a file in the repo holding the full commit hash (now
`c046106cbe075931b1ff8d8b800ff5be47a85f96`, 0.11.11), which the runner
reads to check out and build Showdown. Bumping Showdown is a one-line
commit. Built (2026-10-15): `SHOWDOWN_COMMIT`. The runner keeps each
commit it tests fetched and built in `.showdown/<hash>/` (ignored), and
points the `showdown` symlink at the pinned one, so the scripts' default
and the `showdown/...` citations follow the pin.

For comparison, libpkmn (`~/oak/cpp/extern/engine/.github/workflows/`):
`test.yml` gates on push and pull request against a pinned `@pkmn/sim`
(0.9.31); `fuzz.yml` runs every 6 hours, 15 minutes of fuzzing from a
fresh seed (replaying a failing seed and uploading its logs), then 15
minutes of integration against Showdown, still the pinned version. It has
no job against master.

## Calc in tests

Calc is easy to misread, so here is what it is and where tests use it.

**What it is.** `Calc` holds `overrides`, an `Actions` value. When `update`
reaches a random draw, its roll helper (`mechanics.hpp`, one per
catalogue row) checks the matching override field: nonzero forces that
outcome, 0 draws normally. Hidden counters (sleep, confusion, binding
lengths) aren't overrides; a test sets them in the battle instead.
`NullCalc` compiles all of this away. `Summaries` and `min_ko_roll` aren't
built.

**Where it's used, and why.**

- **Search** (not a test): clamping damage rolls by writing override
  bytes.
- **`check_frame`:** a scenario test needs one specific outcome (a crit, a
  Quick Claw proc) to exercise its mechanic. Forcing it makes the test
  deterministic; otherwise it would hunt for a seed that happens to give
  it, and break whenever the draw order changes.
- **`check_chance`'s forced replays:** the test needs two samples with the
  same key to compare. A rare key often turns up once among the free
  samples, so nothing would check it. Replaying the key's `Actions` as
  overrides, from a new seed and new hidden values, gives the second
  sample. It also tests Calc itself: an override that's ignored or applied
  at the wrong site lands elsewhere or collides.
- **`check_showdown`, Metronome only:** the random search must hit
  Showdown's exact branch, and a game with two Metronome picks (1/336
  each) plus damage rolls runs past the try cap. The pick is public (it's
  on Showdown's `[from] Metronome` line), so forcing it hides nothing the
  test is for: given that pick, our engine must still produce Showdown's
  log, with every other draw searched. What it skips is whether we could
  draw that pick at all. A forced pick Metronome can't call asserts
  (`mechanics.hpp`, `metronome_can_call`), but only in debug, and
  `check_showdown` runs in release; the pick set is otherwise checked only
  by the unforced games.

**Multi-hit moves aren't forced.** The Metronome pick is forced because a low
probability defeated the search, yet Beat Up and Rock Blast games fail in
the same test for the same reason (six exact rolls are about 4e-8 per
try) and aren't forced. It's a special case added when group 8 needed it,
not a principle. Their draws are just as public: `-crit` and `-hitcount`
are logged, and each roll follows from the HP change. The principled
version would force every public draw Showdown made, recorded by wrapping
its PRNG (limitation 3). Rejected (the user, 2026-10-15): too much
machinery around Showdown's source. Instead a game lost to a multi-hit
move is allowed in every environment (Pass criteria).

**The risk.** A forced test trusts the override path. If an override is
applied where the free draw isn't, a forced test can pass while the free
engine is wrong. The cover is that `check_chance` compares forced against
free on the same key, and `soak`, `check_chance`'s free samples and
`check_showdown`'s search all run unforced. Keep at least one unforced
suite for everything a forced one covers.

## Known engine gaps (observed, unpatched)

Where we differ from Showdown and haven't fixed it. A gap that a
Showdown change closes gets crossed off here.

- **Log-only mismatches, truly random** (2026-10-17, one or two
  each, undiagnosed; 5 of the 10,000-game run's 25, the other 20 the
  weather order, fixed): Shell Bell's heal against the attacker's faint,
  Wonder Guard's `-immune` without `[from]`, Bide's `-end` against a
  faint, Truant's `cant` against paralysis.

- **Gaining Truant: Trace, Transform, Skill Swap** (1 of the random
  OU-legal exploratory run; 6 of the 10,000-game run and 9 of the rerun,
  2026-10-16; left as
  a gap by decision). Showdown's `truantTurn` is a Pokemon property that
  only Truant's own onSwitchIn resets, so a Pokémon that gains Truant
  carries the value from its last Truant stint and may loaf or act on its
  first turn by that history. We follow the cartridge (the counter cleared
  at switch-in), which would need a per-party bit to change. 10 of the 16
  state failures of the truly random 10,000-game run (2026-10-17).

- **Fake Out against Focus Punch** (2 of the first OU 10,000-game run,
  2026-10-16). Showdown runs Focus Punch's lost focus before the flinch:
  `|move|...|Focus Punch||[still]`, then `|cant|...|Focus Punch|Focus
  Punch`, and the PP is spent. We stop at the flinch and keep the PP.

- **Encore on a lock Metronome called** (about 30 of 10,000 group 8
  games). Showdown's Encore swaps the locked move for Metronome, runs it
  as `[from] lockedmove` without PP, and the lock's handlers keep going.
  Left as a gap (the user, 2026-10-15); comment in `use_move`.
- **Endure and a future attack** landing the same turn: Endure doesn't
  save the target (no `Turn` at the residuals; Showdown's would).
- **Toxic stage across a double replacement:** Showdown keeps the
  survivor's old toxic stage (it lives on the status); ours resets in
  `enter()`. Needs a per-Pokémon counter (layout). 2 group 2 games, 1
  of the first OU 10,000-game run.
- **Recycle after Knock Off:** Showdown's `itemKnockedOff` still blocks
  Trick and Thief after the recycled item comes back; ours doesn't.
- **Leftovers order on an exact Speed tie** (7 group 8 games). The coin is
  a fair 50/50 per stage (verified); probably few state-matching tries,
  unconfirmed.
- **Log only:** Limber's cure against Attract's `-end` (Update's speed
  order, 1 game); Leech Seed (Showdown draws accuracy before the
  Substitute and already-seeded checks, so it can log a miss where we
  fail; state the same).
- **check_chance collision, undiagnosed** (2026-10-15, before the
  terminal-state rule; not terminal): `check_chance --replay
  15040563541741120241 tools/teams/group3.txt` (Pelipper mirror, turn 111:
  P1 first, Toxic hits, P2's Surf crits; P2's fresh toxic ticks in one
  sample and not the other, same `Actions`). Suspects: the end-of-turn
  `item_cure` pass, the residual Speed-tie order.
- **Unexamined:** 2 crit + crit + freeze Porygon2 games and 1 crit + freeze
  + Lum game in group 8 (rare branches?).
- **Still open from group 6:** Grudge's `-activate` when the KO ends the
  battle; Focus Punch against confusion.

## Known test limitations

From "Is realizability enough for search and RL?" (2026-10-08; nothing
decided). Realizability checks only that Showdown's outcome is in our
support. Search, unlike RL, doesn't average bugs away: it steers into
them. Most important for search first:

1. **Support, not probability.** A wrong rate (a 30% secondary instead of
   10%) passes. Our side is done (`check_chance`'s calibration). Showdown's
   side: wrap `random`, `randomChance`, `sample` and `shuffle` to multiply
   a per-update probability, and compare by frequency of the observed
   log chunk over many seeds from one state, since many Showdown draw
   paths map to one of our keys (the user's caveat).
2. **Damage hides in the 16 rolls.** An error of a few points still
   matches some roll, at the wrong index, which moves KO thresholds.
   `check_damage` covers 74 fixed cases. *(unverified: no example)*
3. **Random branch search can't tell a rare branch from a bug.** Every
   current OU failure is of this kind. Fix for 2 and 3: the trace script
   records Showdown's public draws (roll, crit, secondaries) by wrapping
   its PRNG, tagged by call site, and the checker forces them through
   Calc; hidden counters stay sampled. Rejected 2026-10-15; multi-hit
   failures are allowed instead (Pass criteria).
4. **One direction only.** Branches we have and Showdown doesn't are what
   search exploits. A statistical reverse test is cheaper than replaying
   into Showdown.
5. **Random choices miss the states search steers to** (Perish traps,
   Baton Pass chains, Substitute stalls, berry thresholds, endgames). Add
   traces with one side driven by a cheap policy.
6. **The log hides state** (PP, toxic counter, Protect's count, last-move
   fields, Counter's damage, Truant). Keep the state guard.
7. **`randomize_hidden`'s distribution is untested.** Over self-play,
   compare the true counter given `Durations` with what it samples.

Suggested order: 1, 3, 6, 7, 5, 4.

Also not built: scenario tests per tricky mechanic (Explosion ends the
turn, a Spikes sac cancels the foe's attack, a mid-turn replacement takes
residuals, a residual faint's replacement takes none, the battle ending
mid-residuals, a double KO on the first action, Baton Pass resuming at
the foe's move).

## Later: an OU expected-failure list

Recorded, not adopted (the user chose percentages for now, 2026-10-15). A
percentage lets a new failure hide behind an old one. The alternative for
OU: a committed list of known failing games (seed, game, reason); the run
fails on any game not listed, and on a listed game that now passes, so the
list stays current. It needs fixed seeds and stable failures, which means
limitation 3 fixed first: until then, any change to the number of tries a
game needs reshuffles which rare branches fail.

## Implementation

Order (agreed 2026-10-15): shared pieces first, then each environment in
turn, each measured before its bars are fixed.

1. Team input as a pooled list of files in every tool. Done (2026-10-15).
2. `check_chance`'s terminal-state comparison. Done (2026-10-15).
3. `check_showdown`'s multi-hit classification. Done (2026-10-15).
4. The criteria file (`tools/criteria.toml`) and the Showdown pin file
   (`SHOWDOWN_COMMIT`). Done (2026-10-15), OU only.
5. The runner, `python3 tools/run_env.py ENV` (`--seed`, `--jobs`,
   `--scale` for a smoke run, `--master` or `--showdown-commit` for the
   reporting run). Done (2026-10-15). It builds both presets, shards each
   suite over `--jobs` (all cores), derives every shard's seed from the
   run seed, pools the shards' counts (check_chance's calibration bins
   included) and checks them against the bars. Output, a summary with
   replay commands and the failing shards' traces, goes to
   `build/runs/ENV-TIME-SEED/`. A 1% smoke run takes about 15 s. Node
   trace processes run at most one per GB free at the start
   (`--trace-jobs`): each grows past 350 MB, and 16 at once exhausted this
   7 GB machine (the OOM killer took the user's browser, 2026-10-15).
   The calibration bar was a |z| under 6, wrong for bins expecting far
   below one hit; now a tail chance (Probability calibration, above).
   Fixed with it (2026-10-16): the runner's regex dropped the bins
   2^-1 to 2^-9 (padded labels), and check_chance left out empty bins
   with expectations under 0.5, which biased the pooled deep bins.
6. OU: a 10,000-game run to measure the real match rate (98.5% is a guess
   from 1,000 games), then set the bar a safe margin under it. Measured
   (2026-10-16, seed 8981393492580689921, 11.5 minutes): 9943 of 9946
   counted games (99.97%), 54 multi-hit games left out, 0 choice-set
   mismatches; soak 100,000 games clean; check_chance 0 collisions, but
   the probabilities fail (the z-test and a Beat Up false positive, both
   fixed 2026-10-16; Probability calibration). An earlier run
   (14 of 16 shards, the rest OOM-killed) matched 8709 of 8714. The
   unmatched: Fake Out against Focus Punch (2), the toxic stage across a
   double replacement (1), 5 undiagnosed. Bar set to 99.9% (2026-10-16).
7. Random OU-legal: the generator, a measured run, its bar. Generator
   done (2026-10-16). Exploratory run (2026-10-16, seed
   3112126714767284292, 1,000 games): 405 of 987 matched (41%), 582
   unmatched (86 by the log alone); soak 10,000 and check_chance clean.
   By the first known gap in Showdown's log at the failure: Shell Bell 104,
   the flavor berries 389 (Figy 92, Wiki 82, Mago 78, Aguav 74, Iapapa
   63), Helping Hand 68, Leppa 1; 20 others, mostly White Herb (8: fired
   before the move, or not at all) and Focus Energy (5), the rest single. With the items and
   Helping Hand's log written, the same seed matches 967 of 988 (97.9%):
   21 unmatched (6 by the log alone), White Herb 9, Focus Energy 6, 6
   single (an HP difference twice, a PP one, a drain heal, boosts, a
   request). Psych Up (it copied Focus Energy; gen3 inherits the gen5
   mod's stages-only onHit) and White Herb (it acted on Intimidate's drop;
   Showdown's gen3 checks it before the ability's Start) fixed: 987 of 989
   (99.80%), and OU at the 6's seed, 1,000 games, 996 of 996.
   10,000-game run (engine `36ed3cc`, seed 17554437607677572831): 9823 of
   9898 (99.24%), 2 choice-set mismatches, 2 chance collisions; fixed in
   `d344f32`. Rerun (seed 16645649204763325269): 9898 of 9913 (99.85%), 1
   choice-set mismatch, soak and check_chance clean; 9 the Truant gap, 6
   fixed after (Knock Off on a fainted Sticky Hold, Brick Break into an
   immune target, Imprison and Hidden Power, Rage on Present's heal, the
   cures after Yawn waiting for the Update), which then match, as do
   shards 0 and 11 whole. Bar set to 99.8% (2026-10-16).
8. Truly random: the audit (Generators), the generator, a measured run,
   its bar. Audit, generator and runner done (2026-10-16, Generators);
   `random` in `tools/criteria.toml`. Exploratory run (seed
   13342887709897526592, 1,000 games): check_showdown 950 of 953 on the
   state (99.69%), 47 multi-hit games left out, 40 unmatched (37 by the
   log alone, at least some the `Other` items' names; unclassified), 0
   choice-set mismatches; soak 10,000 clean; check_chance 4 collisions and
   1 sum failure. Paused there by the user (2026-10-16). Fixed
   (2026-10-17): the chance failures (an unrecorded residual Quick Claw
   draw kept in the request past the update; an unrecorded tie coin
   whose two orders ended the battle alike but recorded different draws,
   so `orders_differ` now compares the recorded Actions too), Focus Punch
   against Present's heal (the heal's Hit event breaks focus), and two
   checker errors: Castform's forme read from a non-Forecast Castform's
   type, and the `Other` and `Mail` items' empty names in the log, which
   now match any Showdown item the engine takes as the same (the
   state's rule). The same seed then matches 950 of 953 with 2 log-only
   (the weather order, Known gaps). 10,000-game run (seed
   7867203914769733344, engine `1cf48b7` plus the chance fixes only):
   9626 of 9642 on the state (99.83%), 358 multi-hit games left out, 0
   choice-set mismatches; soak 100,000 and check_chance clean. Its 370
   failures replayed with every fix: 329 match, 25 by the log alone
   (Known gaps), 16 on the state, unclassified (mostly a PP one off,
   Truant-like; some HP, Torment, Rage, Perish Song, a future attack).
   The 16 diagnosed (2026-10-17): 10 the Truant gap (Trace 8, Transform
   1, Skill Swap 1; `closest try` often names the foe, but the one that
   differs is the Pokémon that gained Truant), 1 Fake Out against Focus
   Punch (Known gaps), and two engine bugs, fixed: Stomp, Astonish,
   Needle Arm and Extrasensory never missed a minimized target (gen5's
   mod drops Minimize's onAccuracy, and gen3 inherits that), 4 games;
   Perish Song started on a Soundproof user (gen7's Soundproof drops the
   user check; the decomp skips it too), 1. All 5 then match.
   Of the 25 log-only, 20 were the weather's damage order with a Quick
   Claw holder (every one had one): Showdown's `updateSpeed` applies the
   turn's roll before all the residuals, Wish and the weather included.
   Fixed: the residuals draw the roll first if the move order didn't,
   order Wish and the weather by it, and record it only if the battler
   pass needs it (`residual_speeds`). All 20 then match; a 10% run (seed
   3573881441079700720) matched 960 of 960, log included, chance clean.
   5 log-only remain (Known gaps).
   The 90% bar is not re-set.
9. CI, later: a gating workflow, then the reporting one.

## Decisions

Decided (2026-10-15): percentages for now; multi-hit failures allowed;
no PRNG wrapping; fresh seeds; team files as a
pooled list, the same in every tool; generators for the two random
environments; Showdown itself (pinned for gating, master for reporting),
not `@pkmn/sim`; terminal states compared by `Result` and probability;
the figures above as the first bars.

Open:

1. **Where the jobs run, and how often** (deferred by the user).
2. **Re-setting the bars** from measured 10,000-game baselines.
