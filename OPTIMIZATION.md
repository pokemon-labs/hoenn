# Optimization

Ideas for making `update` faster, from reading the code (2026-10-16). None
is implemented or profiled yet; the ranking is judgement, not measurement.
Figures are in `TESTING.md`, "Others" (about 1.95M updates/s with the null
log and Chance on, 1.63M/s with the log; libpkmn's Gen I 8.5M/s and 8.0M/s).
One update is about 520 ns.

**First step, before any change:** `perf record` on `bench` (release), and
count how often a turn ends on an exact Speed tie. Ideas 1 and 2 are certain
waste; 3 and 4 could be larger, and only a profile can tell.

## 1. The residual Speed-tie copies

`end_of_turn` calls `stage_order_matters` for each stage on an exact Speed
tie. For Leech Seed, Status, Nightmare, Curse and Bind it copies the battle
twice (stack copies, not heap: the no-allocation rule holds), runs the
stage both ways with the null options, and compares the sides with
`same_state`. That is 10 battle copies, 20 stage runs and 10 comparisons on
every tied turn, even when neither Pokémon has the stage's effect. Ties are
common in OU mirrors.

**Why it exists:** Chance, not the mechanics. A draw is recorded in
`Actions` only if it changes the state (decided 2026-10-08; CLAUDE.md,
"Chance"). The copies decide whether the stage's coin is recorded
(`rolls::residual_tie`) or not (`log_order`).

**With Chance and Calc off it is pure waste, and skipping it is exact.**
Without a Calc override, both branches draw the same `rng.chance(1, 2)`, so
`if constexpr (CHANCE<O> || CALC<O>)` around the test changes no game.
`residual_quick_claw` looks the same (one branch through
`rolls::quick_claw`, the other `chance(1, 5)` directly); check that the
helper draws exactly `chance(1, 5)` without an override.

**With Chance on (what `bench` measures), logic can replace the copies.**
Within one stage each handler touches its own Pokémon, except through three
paths: the battle ending partway (`outcome()` after each damaging handler),
Leech Seed acting on the seeder's HP, and `faint()`'s effects on the foe
(it clears escape prevention, sets `wrapper_gone`, reverts a Castform; of
these only `wrapper_gone` is read in these stages, by Bind). Damage amounts
come from each Pokémon's own max HP, toxic counter and status.

| Stage | Order matters only if… | Frequency |
|---|---|---|
| Status, Nightmare, Curse | both handlers deal damage, both hits are lethal, and both Pokémon are their side's last (the first to faint loses). Otherwise the HP, faints or `Result` agree, and terminal states with the same `Result` are the same state. | rare; a cheap test |
| Bind | the same, or each Pokémon binds the other and a hit would KO (the KO sets the other's `wrapper_gone`: a silent end instead of damage) | mutual binding: very rare |
| Leech Seed | each Pokémon has seeded the other (else one handler is a no-op); then the heal cap, the drain capped at current HP, Liquid Ooze and a faint between the drains interact. Exact: a small calculation on the two HPs; or treat it as mattering. | Grass can't be seeded: very rare |

Treating a rare case as "matters" is safe: `Actions` still determines the
state (the chance-key test holds) and search gets one extra chance node;
`residual_quick_claw` already over-approximates this way. Only "doesn't
matter" when it does is a bug.

Care points:
- Compute the damage once, in one function (`stage_damage(battle, p,
  stage)`) that both the handlers and the rule use, so they can't drift
  (toxic's increment before its damage, `fraction()`'s minimum of 1).
- Bind's "freed" reads the true hidden counter, as the copies do.
- `same_state` compares only `sides`, not the Chance durations; the rule
  inherits that, it doesn't add it.
- Debug check: keep `stage_order_matters` in debug builds and assert that
  when it says "matters", the rule does too. Soak, `check_frame` and
  `check_chance` then test the rule.

**Not `residual_quick_claw`.** It compares whole Pokémon-major passes, where
stages couple: Uproar ending before or after the other's Yawn (sleep or
not), and Leech Seed's heal before or after the seeder's Leftovers (both
capped; common in OU). It runs only when someone holds Quick Claw and the
roll flips the order, so keep its copies.

**Open decision:** (a) the Chance-off skip (exact, no decision needed
beyond doing it); (b) a cheap gate, skipping the copies when the stage
can't act for both Pokémon or no hit is lethal (exact, removes nearly all
copies); (c) the full rule above with the debug check. Recommended: (a) and
(b), then (c) only if a profile still shows the copies.

## 2. `same_state` compares field by field

The defaulted `operator==` on `Side` compares member by member. The structs
look padding-free (the chance key already relies on that for `Action`), so a
`memcmp` per side would do, guarded by
`static_assert(std::has_unique_object_representations_v<Side>)`. Matters
only while idea 1 keeps copies.

## 3. Instruction cache and inlining (unverified)

One 8,400-line template instantiated in one translation unit; `execute`
dispatches to about 70 move functions, and GCC may inline many into one
huge `update`. If the profile shows front-end stalls, marking the rare move
functions cold or `noinline` (Transform, Sketch, Metronome, Bide…) keeps
the hot path compact. Could be the largest win, or nothing.

## 4. Build settings (no code changes)

- Profile-guided optimisation (`-fprofile-use`, trained on `bench`):
  typically 10–20% on branchy code.
- `-march=native` if the training machines are known.
- One Clang vs GCC 15 comparison.

## 5. Small hot-path items (probably 1–3% each)

- `staged_accuracy` does double multiplies and divides on every accuracy
  check, even at stage 0 with no modifiers; return the base accuracy early
  when nothing changes it.
- `side.get(1)` goes through `order[0]` and a 42-byte stride on every call,
  dozens per move; the compiler likely can't cache it across calls that may
  write `order`.
- `effective_weather` loops both sides for Cloud Nine and Air Lock, from
  damage, accuracy and the residuals; once per action would do.
- `execute`'s dispatch is a chain of `if`s; plain hits return first, so
  only status moves walk it. A `switch` on `Effect` is tidier, probably not
  faster.

## 6. The log's cost

The log costs hoenn 16% and libpkmn 6% (about 200 ns and 7 ns per update).
Not looked into; the profile of `bench --log` is the place to start.

## Probably not worth it

- **Data tables:** moves about 3.7 KB, species about 6 KB, type chart
  324 B; they fit in L1/L2, and the classifiers (`sound`, `plain_hit`,
  `targets_user`) compile to cheap switches.
- **The null log** costs nothing: every message is behind
  `if constexpr (L::enabled)`.
- **Chance bookkeeping:** a 153 B `Actions` reset per update and two double
  multiplies per draw.
- **Packing the battle** (`Volatiles` bools to bits): a copy is about 20 ns
  against 520 ns per update, and bitfields slow every access. Only matters
  if search stores many battles.
- **Damage arithmetic** is integer; its one 64-bit division is negligible.
