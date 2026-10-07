# rewrite: a libpkmn-style Gen III (ADV) engine in C++

This is an attempt to replace ADV-Simulator as birch's engine. It follows
libpkmn's design choices (a small POD battle, one `update` per turn, the
choice/result protocol, and the Chance and Calc options), but it's written in
C++ and doesn't chase every saved bit.

## Priorities, in order

1. **Safety.** Week-long paid RL runs must not die or be quietly corrupted.
   - `update` never throws, never allocates and never recurses without a
     bound. Every array index is in range by construction or is checked.
   - Bad input (an illegal choice or a corrupt state) returns
     `Result::Error`. It never crashes and never causes UB.
   - Every loop has a fixed bound. A hard turn cap ends the battle as a tie.
   - `Battle` is `std::is_trivially_copyable` and is checked with
     `static_assert`s on its size. It holds no pointers, strings or
     containers.
   - Debug and test builds run under ASan and UBSan, with `assert` turned on.
     Release builds have no asserts, which makes the guarantees above
     load-bearing.
   - Use runtime `assert`s liberally in mechanics functions, as libpkmn
     does. Assert preconditions (a move slot is 1–4, a Pokémon isn't
     fainted), invariants (HP is in range, `Status::valid()`) and branches
     that should be unreachable. They document what each function assumes,
     and soak runs turn them into a check on every step. They are not input
     validation: anything a caller can get wrong must still return
     `Result::Error` in release builds.
2. **Speed.** Search copies and steps the battle millions of times.
   - The target is a battle of roughly 700 B that is copied with `memcpy`.
   - The RNG is a 64-bit seed stored inside the battle, as in Showdown and
     libpkmn. There is no mt19937.
   - Move, species, item and ability data come from `constexpr` tables.
   - There are no virtual calls.
   - Logging, Chance and Calc are template parameters (the C++ equivalent of
     libpkmn's comptime `Options`). Their null versions compile to nothing.
   - For reference, Oak/libpkmn Gen I runs about 76k MCTS iterations/s and
     ADV-Simulator about 19k.
3. **Correctness where competitive play reaches it.** See
   [Tricky mechanics](#tricky-mechanics). Obscure interactions that the
   Smogon ADV teams in `teams/` never produce can wait.

Fuzz testing matters much less here than in libpkmn. We don't promise
cartridge-exact behaviour for every input; we promise no crashes and correct
competitive play.

## Mechanics references

- **The Emerald C decompilation (pokeemerald)** is the authority. The useful
  files are `src/battle_main.c` (turn order), `src/battle_util.c` (end-of-turn
  effects, abilities, items), `src/battle_script_commands.c`, and
  `data/battle_scripts_1.s`. Where it helps, cite the decomp function in a
  comment.
- **ADV-Simulator** (`../ADV-Simulator`) has broad coverage. Use it to see
  what has to be handled, and as a differential oracle in testing.
- **Don't use Showdown's source.** Use Smogon's rule definitions only for the
  clauses, which aren't cartridge mechanics.

Write the mechanics analytically: read the decomp, understand the whole
mechanic, then write one direct code path for it. Don't grow the code
test-by-test to make cases pass. That produces bloated, special-cased code.
Tests check the result; they don't drive the design.

## API (must match libpkmn)

birch's search (`cpp/include/search/mcts.h`) already calls the `pkmn_gen3_*`
C API that ADV-Simulator's `src/pkmn/gen3.hpp` provides. This engine exposes
the same API, so it can be swapped in.

- **`update(battle, c1, c2, options) -> Result`** runs one turn, or the rest
  of a turn after a mid-turn request.
- **`choices(battle, player, request, out) -> n`** lists the legal choices,
  at most 9.
- **Choice** is one byte, `{type: Pass | Move | Switch, data}`.
  - A move's data is the slot, 1–4. Data 0 means Struggle, or a forced
    continuation such as a charging move, a rampage or Bide.
  - A switch's data is the team slot. Teams don't reorder, as in
    ADV-Simulator's API.
  - Pass's data is 0.
- **Result** is one byte, `{type: None | Win | Lose | Tie | Error, p1, p2}`.
  `p1` and `p2` are the Choice type each player must make next. A faint
  replacement is `Switch` for one side and `Pass` for the other. A Baton Pass
  replacement is a mid-turn `Switch`.
- **Options** take the form `{log, chance, calc}`. Each is either a real type
  or a null type.

### Chance: hidden state lives in the battle, observations live outside it

As in libpkmn, the battle stores the **true hidden state**. A sleep's length
is rolled when the sleep is inflicted and kept as a remaining-turns counter.
The same goes for confusion, Disable, Encore, binding moves, rampages and
Uproar. The decomp sets the counters at infliction; take the ranges from it.

`Chance` keeps the **public** information:

- **`Durations`** says how many turns each hidden effect has visibly lasted
  for each side. There are six sleep slots per side, plus confusion,
  Disable, Encore, binding and attacking.
- **`Actions`** has fixed per-player slots that record which random branch
  this update took. It covers the damage roll, hit, crit, secondary effect,
  speed tie, Quick Claw, full paralysis, confusion self-hit, infatuation,
  multi-hit count, the move-slot draw (Sleep Talk), the Roar target, ability
  and item procs, and observations of hidden durations ending or continuing.
- **`probability`** is the rational probability of the branch taken.

Search resamples the hidden counters from `Durations` before each
iteration, as Oak's `randomize_hidden_variables` does.

We rejected the alternative, which is to store elapsed durations in the
battle and re-roll "does it wake this turn?" each turn. It is less faithful,
and it pushes probability bookkeeping into the engine.

### Calc

Calc follows libpkmn's `calc.zig`:

- **Overrides** force the damage roll (85–100) and crit for each side. The
  forced values show up in `Actions`.
- **`Summaries`** report the base and final damage of each attack.

### Extra: the minimum KO roll

`min_ko_roll(battle, player, slot, crit) -> 0..15, or 16 if no roll KOs`
returns the smallest damage-roll index at which one hit from the move in that
slot KOs the foe's active Pokémon in the current state. The index is 0 for
85% damage up to 15 for 100%. It uses the normal damage path, without side
effects. It ignores multi-hit moves; for fixed-damage moves it returns 0 or
16.

## Layout

Mirror libpkmn's `src/` tree:

```
rewrite/
  include/pkmn.h            C ABI (pkmn_gen3_*), as libpkmn's src/include/pkmn.h
  src/common/
    data.hpp                Player, ID, Choice, Result
    rng.hpp                 64-bit PRNG on the in-battle seed
    options.hpp             Options<Log, Chance, Calc>, null types
    rational.hpp            exact probabilities for Chance
    protocol.hpp            optional binary log (off in hot builds)
  src/gen3/
    README.md               decisions and deviations, like libpkmn's gen*/README.md
    data.hpp                Battle, Side, Pokemon, ActivePokemon, Volatiles,
                            Conditions, Field and size static_asserts
    data/                   generated constexpr tables: species, moves, types,
                            items, abilities
    mechanics.hpp           update(), choices(), turn order, moves, residuals
    helpers.hpp             building a battle from teams, for tests and tools
    chance.hpp              Actions, Durations, Chance
    calc.hpp                Overrides, Summaries, min_ko_roll
    test.cpp                scenario tests
  tools/                    data generators (from the decomp or ADV-Sim tables)
```

### Data layout (draft)

This starts from libpkmn's Gen II layout and adds what ADV-Simulator tracks.
Moves and species are 16 bits. Items and abilities are 8 bits; items are
remapped to held items only.

- **`Battle`** holds:
  - `sides[2]`
  - `rng: u64`
  - `turn: u16`
  - `field`: the weather and its turns
  - `request`: what resumes a turn after an interruption, such as a Baton
    Pass or faint replacement
- **`Side`** holds:
  - `pokemon[6]` and `active`
  - `conditions`: Spikes layers, Reflect, Light Screen, Safeguard, Mist,
    Wish and Future Sight turns
  - the Wish HP and Future Sight damage
  - the last move used, and the last move that hit this side (for Mirror
    Move)
  - the physical and special damage taken this turn (for Counter and Mirror
    Coat), and Bide's damage
- **`Pokemon`** holds:
  - stats, moves (`{id, pp, pp_ups}`), HP, species, status, types and level
  - the item, the previous item and the Knocked Off flag
  - the ability, happiness, gender, Hidden Power type and power, and
    position
- **`ActivePokemon`** holds:
  - volatiles (u128)
  - the stats, moves, species, types and ability, which Transform, Forecast,
    Trace and Skill Swap can change
  - boosts (7 × i4)

Don't get clever with unions or bit packing yet. Get it correct and
reasonably small first.

## Tricky mechanics

This is a starting list. **Expand and refine it as we go.** For each item,
record the decomp function it comes from, what was decided, and the test
that covers it. Statements marked *(verify)* haven't been checked against
the decomp yet.

- **Spikes.** There are up to 3 layers, doing 1/8, 1/6 and 1/4 of max HP.
  - Flying types and Levitate users are immune.
  - Spikes apply on every switch-in: a voluntary switch, a faint
    replacement, Baton Pass, and a Roar or Whirlwind drag-in.
  - Order relative to switch-in abilities such as Intimidate *(verify)*.
  - When both sides replace at once, the speed order decides who switches in
    first.
- **Effect order.**
  - Turn order:
    - switches before moves
    - then priority
    - then Quick Claw
    - then speed, with a random tiebreak
  - `GetWhoStrikesFirst` and `SetActionsAndBattlersTurnOrder` are in
    `battle_main.c`.
  - End-of-turn effects come in two passes:
    - `DoFieldEndTurnEffects`: screens, Mist, Safeguard, Wish, weather
      damage and so on
    - `DoBattlerEndTurnEffects`: Ingrain, Leftovers, Leech Seed, poison,
      burn, Nightmare, Curse, binding, Uproar, rampage, Disable, Encore,
      Taunt, Yawn and so on, in speed order
  - Then Future Sight, then Perish Song *(verify the exact sequence)*.
  - Encode the order once, as a table or a switch, not spread across move
    handlers.
- **The turn ending on a KO.** If a Pokémon faints before it acts, its
  queued move is skipped.
  - End-of-turn residuals still run.
  - The replacement comes after the end of the turn, through the result
    protocol: `Switch` for the fainted side and `Pass` for the other.
  - When both sides faint, both switch.
  - The battle ends when one side has no Pokémon left, or in a tie if both
    sides run out at once *(verify the tie and Self-Destruct rules against
    Smogon ADV)*.
- **Sleep Talk.** It picks a random usable move other than itself. The
  decomp's ban list covers two-turn moves, Focus Punch, Bide, Uproar,
  Metronome, Assist, Mirror Move and so on *(verify the exact list)*.
  - The pick is a `move_slot` chance action.
  - The called move doesn't use PP.
  - It fails if no move can be called.
  - Interactions with Choice Band and Encore: ADV-Sim's `ef95175` stops
    Encore from redirecting the called move.
  - It is only usable while asleep. The sleep counter keeps ticking.
- **Roar and Whirlwind.** Priority −6.
  - The replacement is a random non-fainted bench Pokémon. It is a chance
    action, not a player choice, so no request is made.
  - It fails if there is no bench, against Suction Cups, and against
    Ingrain.
  - The dragged-in Pokémon takes Spikes and doesn't act that turn.
  - Interaction with Protect and Substitute *(verify)*.
- **Sleep Clause and Freeze Clause.** These are Smogon rule mods, not
  cartridge mechanics, so take them from the Smogon ADV ruleset, not the
  decomp.
  - Store a per-Pokémon flag that says whether its sleep was inflicted by the
    foe (as opposed to self-inflicted by Rest).
  - Sleep Clause: a sleep move fails if the foe already has a Pokémon put to
    sleep by the other player. Rest doesn't count. Yawn counts *(verify)*.
  - Freeze Clause works the same way for freezes.
  - Both are switchable options at battle start.

Add these to the list when they come up: Baton Pass (what passes, and the
mid-turn request), Pursuit on a switching target, Choice Band locking, Focus
Punch, Truant, Destiny Bond and Perish Song endgames, Self-Destruct and
Explosion halving Defense, Struggle, and running out of PP.

## Plan

1. **Skeleton.**
   - Put the libpkmn structure in place:
     - the `common/` types
     - the `gen3/data.hpp` layout with its `static_assert`s
     - `Options` and the null types
     - the `Chance` and `Calc` shapes
     - the `pkmn.h` C ABI
     - an `update` that handles switching and plain damaging moves
       end-to-end
     - `choices()`
   - Also generate the data tables.
   - Done when birch's `mcts.h` builds and runs a search against it.
2. **Mechanics.**
   - Work in this order:
     1. Fill in turn order, damage, status, residuals and switching from the
        decomp, using ADV-Sim to check coverage.
     2. Add every move, ability and item that appears in `teams/`.
     3. Add the rest of the competitive pool.
   - Each mechanic is one analytical code path with a decomp citation.
3. **Testing.**
   - **Scenario tests** for each tricky mechanic, with the RNG forced through
     Chance and Calc overrides.
   - **Differential runs against ADV-Simulator** through the shared
     `pkmn_gen3_*` API. Compare outcome distributions over the same team
     pairs, not exact streams, because the RNGs differ.
   - **A soak run.** Many millions of random self-play games under ASan and
     UBSan, checking invariants: HP is in range, PP is never negative,
     `choices()` is never empty while the battle isn't over, and the turn cap
     is respected.
   - **A benchmark** against ADV-Simulator's 19k iterations/s.

## Status and caveats (2026-10-06)

**Done:**
- `pokeemerald/` is a depth-1 clone of pret/pokeemerald. It sits on the local
  branch `battle-only` (`a23e37a`), which strips everything unrelated to
  battle mechanics, and it isn't pushed. This repo ignores it, and
  `tools/generate.py` expects it at this path.
- `src/gen3/data/*.hpp` is generated by `tools/generate.py`.
  `src/gen3/README.md` lists the weird cases: the Hidden Power split, the
  Deoxys forms, species order, Struggle, type-effectiveness order, and the
  `Mail`/`Other` items.
- `tools/check_data.cpp` has compile-time spot checks. Nothing else is tested,
  and there's no build system yet.

**Next:** `src/common/`. In rough order, before battle data and mechanics:
1. `common/`: `Player`, `ID`, `Choice`, `Result`, `Options` and the null
   types
2. the RNG: a u64 seed, with draw helpers that mirror the decomp's
   `Random() % n` and `& mask` shapes so Chance probabilities come out exact
3. `rational.hpp`
4. a `pkmn.h` stub matching what birch's `cpp/include/search/mcts.h` calls
5. pure calculation functions: stats, stat stages, crit stages, base damage,
   type application in `ORDER`
6. a coverage report that parses `teams/` into our enums, counts the effects,
   abilities and hold effects in use, and diffs the tables against
   ADV-Sim's
7. the build: release and ASan/UBSan presets, with the generator in the
   build
8. the `Actions`, `Durations` and `Summaries` shapes

**Caveats:**
- **Deoxys forms.** The Attack and Defense stats are hard-coded in the
  generator from memory. Check them against pokefirered.
- **Generator parsing.** The generator parses C with regexes that assume the
  decomp's formatting. Designated-initializer blocks must start with `{` and
  a newline. Gender ratio reimplements `PERCENT_FEMALE` as
  `min(254, pct * 255 / 100)` instead of reading the macro. A reformatted
  decomp could break it silently in places the asserts don't cover.
- **Species IDs differ from the decomp.** `Species` uses National Dex order,
  so it doesn't match `SPECIES_*`. Anything that reads decomp data keyed by
  species ID (learnsets, Frontier sets) needs a mapping.
- **Hidden Power.**
  - Mimic copying Hidden Power must retype the move to the user's type.
  - Metronome can call the generic stub.
  - The stub's data type is the decomp's Normal and means nothing.
- **Struggle's data says Normal**, but mechanics must skip type calculation
  for it: no effectiveness and no STAB.
- **`HoldEffect` `param` means something different per effect** (a percentage,
  an HP amount, a stat and so on). Read the decomp per effect before using
  it.
- **Calling moves.** Sleep Talk's legality also needs
  `CheckMoveLimitations`, ignoring PP. That's mechanics, not data.
- **Unverified claims.** The *(verify)* items in
  [Tricky mechanics](#tricky-mechanics) haven't been checked against the
  decomp.
- **The birch repo.** birch's `.gitignore` lists `rewrite/pokeemerald/` but
  not `rewrite/`. Now that this is its own repo, decide whether birch should
  ignore it or track it as a submodule.
