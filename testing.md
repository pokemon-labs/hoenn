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
| Teams | `../teams/adv/*.txt` (real Smogon teams) | generated, kept only if gen3ou's validator accepts | generated, no legality check |
| Showdown format | `gen3ou` | `gen3ou` | `gen3customgame` |
| Strictness | highest | middle | lowest |
| Built | yes (via `all.txt`) | no | no |

The two random environments use generators, not the step 5 group files
(`tools/teams/group*.txt`), which stay in git history. The banned moves
those files carried for `soak` and `check_chance` only (OHKO moves,
Swagger, Assist) are then reached by the truly random environment, whose
`gen3customgame` allows them.

### Generators (not built)

One seeded Node script writes teams as Showdown export text, so every
consumer (`soak`, `check_chance`, the trace script) reads files as now.
Each file starts with a comment giving the mode, the seed and the Showdown
commit, so a generated team keeps its provenance.

- **Random OU-legal.** Per slot: a species uniform over those gen3ou allows,
  four moves from its learnset, one of its abilities, an item, random EVs
  (multiples of 4, at most 510), a nature, level 100. Species Clause holds.
  The team goes through gen3ou's validator; rejected teams are redrawn, up
  to a cap. Uniform rather than usage-weighted, for coverage.
  `gen3randombattle` won't do: its levels vary by species and its sets
  are curated.
- **Truly random.** As Showdown's `ExhaustiveRunner`
  (`showdown/sim/tools/exhaustive-runner.ts`), which libpkmn's fuzz uses:
  cycle through pools of every species, move, ability and item so each
  appears, with no legality check. Level 100.
  - Before the first run, audit what this can reach that OU never does
    (unverified which of these the engine handles): ability/species pairs
    that don't exist, Follow Me and Helping Hand (unwritten, so a no-op
    using PP; check what Showdown's singles does), Quick Claw (the trace
    script bans it, Showdown's gen3 doesn't log it), and whatever
    `gen3customgame` allows that gen3ou doesn't.

## Team input

**Decided (2026-10-15):** tests take any number of team files, so the OU
dump needn't be copied into `all.txt`, and each team keeps its source.

Now: `soak`, `check_chance`, `check_teams` and `coverage` take several
files; `showdown_traces.cjs --teams FILE` and `bench` take one;
`protocol_survey.cjs` hard-codes `all.txt`. The ctest `check_teams` globs
`../teams/adv/*.txt`, so it parses every team twice (once from
`all.txt`).

Decided (2026-10-15): every tool that takes teams takes a list of team
files as positional arguments (one file is a list of one) and pools every
team in them. Same behaviour in every tool.

Proposed:

- A team is identified by (file, index in the file, title). The trace's
  game record carries both teams' identities, and every failure report
  prints them.
- `all.txt` is deleted. It lives in the birch repo (`../teams/adv/`), and
  birch's `README.md`, `teams/adv/README.md` and `docs/HANDOFF.md` mention
  it, so that's a separate birch commit.
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

## Suites

Built and in ctest (both presets): `headers`, `check_data`, `check_teams`,
`check_damage`, `check_frame`, a 100-game `soak`, `check_chance` and
`check_chance_unmasked` (must fail). These are the fast suites; they run
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
- **Probability calibration:** per pair, each key's free count fits its
  probability (Chernoff, 1e-10), the keys' sum is at most 1, and held-out
  draws landing on unfound keys fit 1 − sum. Over the run, held-out hits
  are binned by key probability (powers of 2, plus "unseen") and each
  bin's z must be under 6. The per-pair tests alone are too weak (they
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
  aren't allowed as known failures. The test (not yet changed) compares the whole
  state, so it reports 1-2 collisions a run where a residual KO ends the
  battle and the samples differ only in whether the winner's own residual
  ran first.
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

- `bench` (release, null log, Chance on, Calc off): about 3.2M updates/s
  on the OU teams; `tools/libpkmn_bench.zig` about 9.0M/s for libpkmn's
  Gen I; ADV-Simulator's update about 1.4 µs. None is like for like.
  Not a pass/fail suite.
- `coverage`: which moves on the given teams have a written mechanic
  (100% on `teams/adv/`). Not a pass/fail suite.

## Pass criteria (proposed)

"Matched" is a `check_showdown` game reproduced to its end, state and log,
within the cap. Every bar is per run.

| | OU | Random OU-legal | Truly random |
|---|---|---|---|
| `check_showdown` games | 10,000 | 10,000 | 10,000 |
| try cap | 1M | 1M | 1M |
| matched | ≥ 98.5% | ≥ 97% | ≥ 90% |
| log-only mismatches | count | reported, not counted | reported, not counted |
| choice-set mismatches | 0 | 0 | 0 |
| `soak` (debug) games | 100,000 | 100,000 | 100,000 |
| `soak` failures | 0 | 0 | 0 |
| `check_chance` games (128 samples) | 1,000 | 1,000 | 1,000 |
| collisions | 0 | 0 | 0 |
| calibration | every bin \|z\| < 6 | same | same |

Rationale:

- **OU at 98.5%.** Measured: 99.2% on 1,000 games (seed 8, 2026-10-08),
  too few to place a bar 4 standard deviations below (±0.28). Every
  failure there is a rare branch the random search missed (Beat Up, Rock
  Blast), not a known engine bug. Re-set the bar from a 10,000-game
  baseline. It should rise toward 99.9% once Showdown's draws are forced
  (limitation 3 below), which would leave only real mismatches.
- **Random OU-legal at 97%, state only.** Unmeasured. The group files ran
  98.9% to 99.98% (10,000 games each), but they're hand-built around a few
  moves; uniform random sets will reach more rare-branch moves and
  untested item and ability pairs. Log-only mismatches are reported, not
  counted, to lower the bar for now; may tighten later.
- **Truly random at 90%, state only.** Unmeasured, and it reaches
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
smogon/pokemon-showdown at a pinned commit (now `c046106`, 0.11.11);
reporting uses master. That means the pin must live in the repo as a file
the jobs read (now it's only prose in CLAUDE.md, and `showdown/` is an
ignored depth-1 clone), and a job clones and builds Showdown before
running.

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

- **Search** (not a test): birch clamps damage rolls by writing override
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

**An inconsistency.** The Metronome pick is forced because a low
probability defeated the search, yet Beat Up and Rock Blast games fail in
the same test for the same reason (six exact rolls are about 4e-8 per
try) and aren't forced. It's a special case added when group 8 needed it,
not a principle. Their draws are just as public: `-crit` and `-hitcount`
are logged, and each roll follows from the HP change. The principled
version is limitation 3: force every public draw Showdown made, recorded
by wrapping its PRNG, and search only the hidden ones. Then the Metronome
special case goes.

**The risk.** A forced test trusts the override path. If an override is
applied where the free draw isn't, a forced test can pass while the free
engine is wrong. The cover is that `check_chance` compares forced against
free on the same key, and `soak`, `check_chance`'s free samples and
`check_showdown`'s search all run unforced. Keep at least one unforced
suite for everything a forced one covers.

## Known engine gaps (observed, unpatched)

Where we differ from Showdown and haven't fixed it. A gap that a
Showdown change closes gets crossed off here.

- **Encore on a lock Metronome called** (about 30 of 10,000 group 8
  games). Showdown's Encore swaps the locked move for Metronome, runs it
  as `[from] lockedmove` without PP, and the lock's handlers keep going.
  Left as a gap (the user, 2026-10-15); comment in `use_move`.
- **New `check_chance` collision, group 3** (2026-10-15, undiagnosed):
  `check_chance --replay 15040563541741120241 tools/teams/group3.txt`. A
  Pelipper mirror, turn 111: P1 first, Toxic hits, p2's Surf crits; p2's
  new toxic ticks in one sample and not the other, with the same Actions.
  Suspects: the new `item_cure` pass after the residuals, or the residual
  order on a Speed tie.
- **Endure and a future attack** landing the same turn: Endure doesn't
  save the target (no `Turn` at the residuals; Showdown's would).
- **Toxic stage across a double replacement:** Showdown keeps the
  survivor's old toxic stage (it lives on the status); ours resets in
  `enter()`. Needs a per-Pokémon counter (layout). 2 group 2 games.
- **Recycle after Knock Off:** Showdown's `itemKnockedOff` still blocks
  Trick and Thief after the recycled item comes back; ours doesn't.
- **Leftovers order on an exact Speed tie** (7 group 8 games). The coin is
  a fair 50/50 per stage (verified); probably few state-matching tries,
  unconfirmed.
- **Log only:** Limber's cure against Attract's `-end` (Update's speed
  order, 1 game); Leech Seed (Showdown draws accuracy before the
  Substitute and already-seeded checks, so it can log a miss where we
  fail; state the same).
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
   Calc; hidden counters stay sampled. This would revise the "random
   branch search" decision.
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
the foe's move); differential runs against ADV-Simulator.

## Later: an OU expected-failure list

Recorded, not adopted (the user chose percentages for now, 2026-10-15). A
percentage lets a new failure hide behind an old one. The alternative for
OU: a committed list of known failing games (seed, game, reason); the run
fails on any game not listed, and on a listed game that now passes, so the
list stays current. It needs fixed seeds and stable failures, which means
limitation 3 fixed first: until then, any change to the number of tries a
game needs reshuffles which rare branches fail.

## Decisions

Decided (2026-10-15): percentages for now; fresh seeds; team files as a
pooled list, the same in every tool; generators for the two random
environments; Showdown itself (pinned for gating, master for reporting),
not `@pkmn/sim`; terminal states compared by `Result` and probability;
the figures above as the first bars.

Open:

1. **Where the jobs run, and how often** (deferred by the user).
2. **Re-setting the bars** from measured 10,000-game baselines.
