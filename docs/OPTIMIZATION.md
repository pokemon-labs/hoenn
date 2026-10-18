# Optimization

Where `update`'s time goes and what to try next. Started from reading the
code (2026-10-16); since 2026-10-17 from profiles of `bench`.

## Status (2026-10-17)

Three changes are in (`2c9c697`, `eb72135`, `eeca4b4`), together about
+22% with the log on and +20% with the null log, Chance on:

| Commit | Change | `bench --log` | `bench` |
|---|---|---|---|
| `main` before (`d0892a3`) | | 1.625M/s | 1.944M/s |
| `2c9c697` | residual tie coins by a rule, no battle copies (idea 1) | +3.7% | +2.9% |
| `eb72135` | `battler_stage` inlined with a constant stage | +7.8% | +10.0% |
| `eeca4b4` | `item_cure`, `white_herb`, `ability_immunity`: check inline, body out of line | +9.3% | +6.1% |

Medians of 8 rounds, the four builds round-robin, pinned to one core,
30,000 battles on `teams/ou`, an AMD Ryzen 7 5825U on AC power in
performance mode (battery saver makes the numbers meaningless). Every step
was faster than the one before in at least 7 of 8 rounds. `bench --log` is
the configuration RL uses (`RL.md`: the log on, no Chance or Calc). The
figures in `TESTING.md`, "Others" predate these changes.

**Behaviour is unchanged.** A scratch tool (not committed) played the same
seeded games on the old and new headers and hashed, after every update, the
choices offered, the `Result`, the log bytes, the battle bytes, and
`Actions`, `Durations` and the probability: 20,000 OU games and 10,000 group
games matched in both configurations, and a planted change (Leftovers 1/15)
flagged every game. The one intended difference is idea 1's
over-approximation (below), which no sampled game reached. Worth committing
as `tools/check_same.cpp` for future work: build it against two trees and
diff the outputs.

## How to measure

- `bench` and `bench --log` in the release preset, pinned (`taskset -c N`),
  several alternating runs per build; a single run varies by 2-4%, and a
  loaded or power-saving machine by far more. Compare the update counts too:
  they must match if the change keeps every draw.
- A profile: a release build with `-g -fno-omit-frame-pointer` (same speed),
  then `perf record -F 20000` and `perf annotate -l` on the hot symbols.
  `perf` needs `kernel.perf_event_paranoid` at 1 or less (set it with sudo
  in a real terminal; it resets at reboot). Zen 3 has no top-down metrics;
  `perf stat -e cycles,instructions,branch-misses,L1-icache-load-misses`
  stands in.
- How often a turn ends on an exact Speed tie: 2.0% of turns on `teams/ou`,
  5.3% on `tools/teams` (random choices; OU mirrors would tie more).

## The profile now (`bench --log`, after `eeca4b4`)

About 2,240 cycles and 4,940 instructions per update, IPC 2.2, about 14
branch misses and 19 L1 instruction-cache misses. It's bound by the
instructions executed, not by the front end or the caches.

The pattern behind both inlining wins: a small function called many times
per update that usually does nothing spends most of its time on its entry,
exit and a `switch` dispatch (`battler_stage`: 53% entry and exit, 12% the
stage switch; `item_cure`: 54% entry and exit; `ability_immunity`: 38% its
ability switch). A predictable check at the call site removes it. Both gains
came out larger than the measured overhead (about 10% against 7-8%, 9%
against 4-5%), probably because the common path then has no calls and keeps
its registers (unverified).

What's left on top: `actions`, `hit`, `moveend`, `damage`, `cancel`,
`execute` (the move itself, 25-30% together), the residuals (`end_of_turn`,
`battler_pass`, `field_residuals`, `item_end_turn`), `log_order` with
`speed` and `speeds` (about 6%), `arrive` and `leave` (switching; half of
`leave` is `a = ActivePokemon{}`, which is needed), and `bench`'s own loop
with `choices()` inlined (about 8%).

## Next

### A. `log_order` only where there are two things to order (decision needed)

`log_order` computes both Speeds in full, and draws a coin on a tie, to
order two log lines. `white_herbs` and the end-of-turn `item_cure` call it
every time, though nearly always neither Pokémon has a White Herb or a cure
to run. Two call sites already guard it (`p1 && p2 ? log_order(battle) :
...`: Forecast in `weather_change`, and Damp's holder in `hit`); the others
would follow. Estimated 3-5%.

**The catch:** the coin on a tie is unrecorded, and skipping it changes the
RNG stream, so every seed plays a different game from the first skipped tie
on. The state and the log are still right (CLAUDE.md allows a draw wherever
it's convenient), but the equivalence check above stops applying, and
anything that pins a seed to an exact game would need regenerating. Check
first whether `check_frame`'s scenarios or the Showdown fixture depend on
hoenn's own stream.

The exact alternative: compute both Speeds once per action and reuse them,
keeping every draw. Smaller and fiddlier (a Speed can change mid-action).

### B. PGO (a build change, no code)

Profile-guided optimisation: build with `-fprofile-generate`, run a
training workload, rebuild with `-fprofile-use -fprofile-partial-training`.
Measured on `eeca4b4`, trained on `bench --log` (seed 1), timed on seed 2:

| Configuration | Release | PGO | |
|---|---|---|---|
| `bench --log`, `teams/ou` | 1.966M/s | 2.366M/s | +20.3% |
| `bench --log`, `tools/teams` (not trained on) | 1.945M/s | 2.314M/s | +19.0% |
| `bench`, null log and Chance on (not trained on) | 2.319M/s | 2.269M/s | -2.2% |

Same games (update counts match). Care points:
- Generate and use in **one build directory**: the profile's file name
  comes from the object's path, and a different directory finds no profile
  (GCC says so with `-Wmissing-profile`; don't silence it).
- It speeds up only what it trained on: the Chance-on instantiation is
  different code and got slower. Train on every configuration that matters.
- A profile is tied to the source and the compiler: regenerate it in the
  build, don't commit it.
- Open: where it lives. The build RL loads is the Python module
  (`python/CMakeLists.txt` on `pybind`), so a script there that generates,
  trains and uses is the natural place; train on `bench --log` until real
  rollouts exist, then on those.
- Not tried: `-march=native` (if the training machines are known), Clang.

### C. Small exact items (about 1-2% each)

- `speed()`: skip the stage-ratio division when the Speed stage is 0 (most
  of the time); the division is a real integer divide.
- `staged_accuracy`: double multiplies and divides on every accuracy check;
  return the base accuracy early when stage and modifiers are neutral.
- `effective_weather` loops both sides for Cloud Nine and Air Lock from
  damage, accuracy, `speed` and the residuals; once per action would do.
- More of the inline-check pattern: look for other small helpers whose
  annotate shows mostly entry and exit (`white_herbs`, `use_item`'s callers,
  `outcome`).

### D. `choices()`

About 8% of `bench` is its loop, mostly `choices()` inlined. RL calls
`choices()` every step too, so it counts there; not looked into.

## Done and ruled out

**1. The residual Speed-tie copies** (`2c9c697`). On an exact tie,
`end_of_turn` copied the battle twice per stage (10 copies, 20 stage runs)
to decide whether the stage's coin is recorded (`rolls::residual_tie`) or
only orders the log. `stage_order_matters` now decides by a rule:
- Leech Seed: both seeded and in.
- Status, Nightmare, Curse: both hits lethal and both Pokémon their side's
  last.
- Bind: both bound and either hit lethal (the other then ends silently, or
  isn't reached and its tick goes unrecorded).

`stage_damage` gives the handlers and the rule one damage computation. The
rule over-approximates for mutual Leech Seed and mutual Bind: there it may
record a coin (one extra chance node, `probability` halved) where the copies
didn't; the state, log and draws are the same. Debug builds keep the copies
(`stage_orders_differ`) and assert the rule never misses a case they catch.

**Kept: `keep_residual_quick_claw`'s copies.** It compares whole
Pokémon-major passes, where stages couple (Uproar ending before or after the
other's Yawn; Leech Seed's heal before or after the seeder's Leftovers). It
runs only when someone holds Quick Claw and the roll flips the order. It's
the last user of `same_state` outside debug.

**2. `same_state` by `memcmp`.** Moot: only the Quick Claw path and debug
use it now.

**3. Instruction cache, cold or `noinline` rare moves.** Ruled out: L1
instruction-cache misses per update were about 7 (`bench`) and 14
(`bench --log`) before the inlining, 11 and 19 after, against 2,200+
cycles. The inlining added 25 KB of code (6%) and still won; more inlining
should watch that count.

**Inlining note.** The three changes add the codebase's first
`[[gnu::always_inline]]`; GCC and Clang honour it, and it changes no
meaning.

## Probably not worth it

- **Data tables:** moves about 3.7 KB, species about 6 KB, type chart
  324 B; they fit in L1/L2, and the classifiers (`sound`, `plain_hit`,
  `targets_user`) compile to cheap switches.
- **The null log** costs nothing: every message is behind
  `if constexpr (L::enabled)`. The real log's writers are about 1% each at
  most.
- **Chance bookkeeping:** a 153 B `Actions` reset per update and two double
  multiplies per draw. RL runs without Chance; the Python wrapper on
  `pybind` builds `Chance<>` with the log, and needn't.
- **Packing the battle** (`Volatiles` bools to bits): a copy is about 20 ns
  against about 500 ns per update, and bitfields slow every access. Only
  matters if search stores many battles.
- **Damage arithmetic** is integer; its one 64-bit division is negligible.
