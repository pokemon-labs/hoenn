# rewrite: a libpkmn-style Gen III (ADV) engine in C++

This is an attempt to replace ADV-Simulator as birch's engine. It follows
libpkmn's design choices (a small POD battle, one `update` per turn, the
choice/result protocol, and the Chance and Calc options), but it's written in
C++ and doesn't chase every saved bit.

**Be cautious, and don't push ahead.** While a design choice is still open,
don't offer to write, add or commit the thing it affects. Lay out the open
choices and their tradeoffs, give a recommendation, and wait for the user to
decide. Help the user be careful: point out assumptions and unverified
claims rather than smoothing over them.

**Singles only.** The engine is for 1v1 singles battles (Smogon ADV OU).
Doubles get no consideration: skip the decomp's doubles-only branches
(ally targeting, spread damage, `gBattlersCount > 2`, Follow Me, Helping
Hand) rather than carrying them, and don't size or shape anything for them.

## Strategy: replace Emerald's battle scripts with direct code (candidate)

Emerald drives battles with a bytecode VM. Scripts in
`data/battle_scripts_*.s` are byte streams of opcodes. `gBattlescriptCurrInstr`
is the program counter, and `gBattleScriptingCommandsTable` dispatches one
opcode per frame to a `Cmd_*` handler in `src/battle_script_commands.c`.
`call` and `return` use a separate fixed stack of script pointers. The VM
exists so the GBA can pause mid-turn for text and animations. We have no
frames, so we don't port it.

- **Write each mechanic as ordinary functions, as libpkmn does.** Read a
  script as the spec for order of operations: which checks run, which state
  changes happen and when RNG is called, in that sequence. Drop the
  presentation-only opcodes (`waitmessage`, `waitanimation`, and so on).
- **A script VM would break the safety rules.** Its state is a program
  counter and a return stack of pointers, and `Battle` holds no pointers.
- **Suspend only at real decisions.** These are Baton Pass and faint
  replacements. `Battle.request` records where `update` resumes. Keep the
  turn phases explicit and few, so nothing needed to resume lives in a C++
  stack frame.
- **Shared tails become helpers.** Scripts jump into each other's middles
  (`BattleScript_HitFromAccCheck` and the like). Those entry points become
  functions that several effects call.

**Why the translation is hand work, not codegen.** The scripts themselves
are easy: 615 labels, explicit branches, matched `call`/`return`. The hard
part is the control flow hidden in C:

- 313 places in C assign `gBattlescriptCurrInstr` directly. Each of the 247
  `Cmd_*` handlers chooses its own successor, sometimes a script that the
  bytecode never names.
- 91 calls to `BattleScriptPush` from C inject subroutine calls (can't-attack
  paths, ability and item triggers).
- Re-entrant state machines: `moveend` (`moveendState`) runs one case, can
  call out to a script, then resumes at the next case. `gBattleMainFunc`
  sequences the turn the same way.
- Global flags used as control signals: `gHitMarker` and `gMoveResultFlags`
  carry decisions like "move failed" or "skip damage" between distant
  instructions.
- `various` is one opcode with 27 subcommands.

**Approach: one mechanic at a time, limited to the competitive pool.**

1. Trace the move effect's script path. Read every `Cmd_*` handler it
   reaches, and note where each one can send the program counter and which
   global flags it reads or sets.
2. Rewrite the path as one direct function that cites the decomp lines.
3. Optionally (not built yet): a small tool that decodes the script bytes
   and dumps, per label, every branch target and called script. It would
   give a checklist and catch missed branches. It can't do the translation,
   because the semantics live in the C handlers.

## Priorities, in order

1. **Safety.** Week-long paid RL runs must not die or be quietly corrupted.
   - `update` never throws, never allocates and never recurses without a
     bound. Every array index is in range by construction or is checked.
   - Bad input (an illegal choice or a corrupt state) returns
     `Result::Error`. It never crashes and never causes UB.
   - Every loop has a fixed bound. A hard turn cap ends the battle as a tie:
     after `turn += 1`, `update` returns `Tie` once the turn reaches 1000
     (as libpkmn's `gen1/mechanics.zig:1650`).
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

**Data covers the full game; mechanics take the shortcuts.** The scope cut
in priority 3 applies only to the mechanics in `update`. The data reflects
the full game (singles): `Battle`, `Actions`, `Durations`, the generated
tables and the enums all leave room for every move, ability, item and
random draw. That holds even for ones the competitive pool barely reaches,
like Spite, Present, Magnitude and Psywave. A mechanic that isn't written
yet is then a gap in `update`, which is easy to fill later. It isn't a
missing field that changes the layout and everything built on it.

Fuzz testing matters much less here than in libpkmn. We don't promise
cartridge-exact behaviour for every input; we promise no crashes and correct
competitive play.

## Mechanics references

- **Read Emerald, test against Showdown.** Showdown's ADV very nearly
  matches the cartridge, and the decomp is far easier to read precisely, so
  the decomp is what we write mechanics from. Testing against the decomp
  directly would need a lot of infrastructure first, so tests check behaviour
  against Showdown instead. Where the two disagree, record it in
  `src/gen3/README.md`.
- **Ignore libpkmn's modes.** libpkmn has a `showdown` option that switches
  between cartridge and Showdown behaviour. That distinction doesn't exist
  here: take libpkmn's structure and conventions, and the mechanics from the
  decomp.
- **The Emerald C decompilation (pokeemerald)** is the authority. The useful
  files are `src/battle_main.c` (turn order), `src/battle_util.c` (end-of-turn
  effects, abilities, items), `src/battle_script_commands.c`, and
  `data/battle_scripts_1.s`. Where it helps, cite the decomp function in a
  comment.
- **ADV-Simulator** (`../ADV-Simulator`) has broad coverage. Use it to see
  what has to be handled, and as a differential oracle in testing.
- **Don't write mechanics from Showdown's source.** Use Smogon's rule
  definitions only for the clauses, which aren't cartridge mechanics.
  Showdown is the test target, not the spec.

Write the mechanics analytically: read the decomp, understand the whole
mechanic, then write one direct code path for it. Don't grow the code
test-by-test to make cases pass. That produces bloated, special-cased code.
Tests check the result; they don't drive the design.

## API (must match libpkmn)

birch's search (`cpp/include/search/mcts.h`) calls a `pkmn_gen3_*` C API.
This engine provides the functions it calls, so it can be swapped in. Only
the function names and signatures are fixed; the struct sizes and byte
layouts behind them are our own, derived from Emerald.

- **`update(battle, c1, c2, options) -> Result`** runs one turn, or the rest
  of a turn after a mid-turn request.
- **`choices(battle, player, request, out) -> n`** lists the legal choices,
  at most 9.
- **Choice** is one byte, `{type: Pass | Move | Switch, data}`.
  - A move's data is the slot, 1–4. Data 0 means Struggle, or a forced
    continuation such as a charging move, a rampage or Bide.
  - A switch's data is a SLOT, 2–6.
- **SLOT is order-dependent, as in libpkmn. Get this right.** SLOT 1 is the
  active Pokémon and SLOT 6 the last. `Side::pokemon[6]` never moves;
  `Side::order[6]` holds 1-based indexes into it, by SLOT. So the Pokémon in
  SLOT `s` is `pokemon[order[s - 1] - 1]`, and the active one is
  `pokemon[order[0] - 1]`. A switch to SLOT `s` swaps `order[0]` and
  `order[s - 1]`: one byte pair, not whole `Pokemon` structs. Switching to
  the same SLOT twice is a round trip. Anything per-Pokémon that Chance or
  the API exposes is indexed by SLOT (`Durations.sleep`, Roar's
  `switch_target`). A Pokémon's original party position, `order[s - 1]`, is
  what the log's `ID` carries. "Move slot" (1–4) is a different thing.
  - Pass's data is 0.
- **Result** is one byte, `{type: None | Win | Lose | Tie | Error, p1, p2}`.
  `p1` and `p2` are the Choice type each player must make next. A faint
  replacement is `Switch` for one side and `Pass` for the other. A Baton Pass
  replacement is a mid-turn `Switch`.
- **Options** take the form `{log, chance, calc}`. Each is either a real type
  or a null type.
- **The log comes last.** Search always runs with the null log, so the log
  isn't needed for the skeleton or the mechanics. We will support it, as the
  last simulator code we write, after the mechanics (plan step 2). It is the
  source of public information for imperfect-information use: what a player
  actually sees, as on Showdown. Until then, keep the `Log` template
  parameter and the null type so adding it later changes no signatures.

### Chance: hidden state lives in the battle, observations live outside it

**Chance assumes perfect information** (open teams: both players know every
species, move, item, ability and stat). "Public" below means public in that
setting. So `Actions` can record outcomes the cartridge never shows, such as
a Focus Band that didn't activate or a contact ability that didn't proc,
because both players already know the item or ability. Don't design Chance
for imperfect information. Imperfect-information public signals will come
from the log, and `Durations` is the perfect-information version of the
bookkeeping a log reader would do.

As in libpkmn, the battle stores the **true hidden state**. A sleep's length
is rolled when the sleep is inflicted and kept as a remaining-turns counter.
The same goes for confusion, Disable, Encore, binding moves, rampages and
Uproar. The decomp sets the counters at infliction; take the ranges from it.

`Chance` keeps the **public** information:

- **`Durations`** counts, for each hidden effect, how many steps its counter
  has visibly taken, with libpkmn's convention: 0 is inactive, 1 is just
  started (no steps yet), and each step adds 1. There are six sleeps per
  side, by SLOT (`sleep[0]` is always the active Pokémon's, swapped along
  with `order`), plus confusion, Disable, Encore, binding and attacking.
- **`Actions`** records which random branch this update took: one `Action`
  per player, plus a shared `Turn` for draws that serve both sides (Quick
  Claw, the two speed ties). `chance.hpp` catalogues every reachable
  `Random()` site, with its multiplicity, and maps it to a field.
- **`probability`** is the rational probability of the branch taken.

**The observation must determine the branch exactly. No hashing.** Search
keys a chance node's children by `Actions` and replays them as the same
branch. So, given the battle before the update and both choices, `Actions`
must determine the `Result`, the resulting `Durations` and every public part
of the resulting battle. The only exception is the true values of the hidden
counters, which search resamples from `Durations` anyway. Two different
branches must never produce equal `Actions`. A hash can collide, and a
collision silently desyncs the battle from the search tree, which breaks the
safety priority. The same applies to everything Chance observes. So:

- Every random draw that can affect public state gets its own field.
- Size each field for the draw's **multiplicity**: the most times it can
  happen for one player in one update, not just once. For example, a
  multi-hit move rolls the crit and the damage on every hit. Enumerate the
  multiplicity per `Random()` site from the decomp.
- If a draw could happen more times than its fields hold, that's a bug.
  Assert on it, and size the fields so it can't happen.

Search resamples the hidden counters from `Durations` before each
iteration, as Oak's `randomize_hidden_variables` does.

We rejected the alternative, which is to store elapsed durations in the
battle and re-roll "does it wake this turn?" each turn. It is less faithful,
and it pushes probability bookkeeping into the engine.

### Calc

Calc follows libpkmn's `calc.zig`:

- **Overrides** are an `Actions`: the same struct Chance observes. A nonzero
  field forces that branch; 0 rolls normally. Anything worth observing is
  worth forcing, and the reverse. Hidden rolls are in neither; to fix a
  sleep length, set the counter in the battle.
- **`Summaries`** (libpkmn: per-player base and final damage, and whether
  HP capped it, so equivalent damage rolls can be merged) are on hold.

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
    options.hpp             Options<Log, Chance, Calc>
    rational.hpp            exact probabilities for Chance
    protocol.hpp            binary log: Writer and Log now, messages last
  src/gen3/
    README.md               decisions and deviations, like libpkmn's gen*/README.md
    data.hpp                Battle, Side, Pokemon, ActivePokemon, Volatiles,
                            Conditions, Field and size static_asserts
    data/                   generated constexpr tables: species, moves, types,
                            items, abilities
    mechanics.hpp           update(), choices(), turn order, moves, residuals
    helpers.hpp             building a battle from teams, for tests and tools
    chance.hpp              Actions, Durations, Chance
    calc.hpp                Overrides, min_ko_roll (Summaries on hold)
    test.cpp                scenario tests
  tools/                    data generators (from the decomp)
```

### Data layout (draft)

This starts from libpkmn's Gen II layout and adds what Gen III needs.
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
  - `pokemon[6]` (never reordered), `order[6]` (see SLOT) and `active`
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
  - the ability, happiness, gender, and Hidden Power type and power. (No
    `position`: with an `order` array, the index into `pokemon` is the
    original position.)
- **`ActivePokemon`** holds:
  - volatiles (u128)
  - the stats, moves, species, types and ability, which Transform, Forecast,
    Trace and Skill Swap can change
  - boosts (7 × i4)

Don't get clever with unions or bit packing yet. Get it correct and
reasonably small first.

#### Audit of the draft against the decomp (2026-10-07)

The draft above hasn't been revised yet. These findings apply to it.

Sources checked: `BattlePokemon`, status2 and status3, `DisableStruct`,
`ProtectStruct`, `SideTimer`, `gWishFutureKnock`, `gBattleStruct`, the
`gLast*` arrays, `SwitchInClearSetData` (main:3152) and `TurnValuesCleanUp`
(main:4857).

- **Bug: the toxic counter resets on switch.** The bad-poison residual
  (util:1533) bumps status1 in `gBattleMons` but never writes it back to the
  party. Every status1 write-back happens on a status change. Switch-in
  reloads status1 from the party, so the counter comes back as it was at
  infliction (0). `Status::toxic` in `gen3/data.hpp` says the opposite; move
  it to `ActivePokemon`. The sleep counter does persist, because the canceler
  writes it back (util:2259).
- **Wrong in the draft:**
  - **Wish HP.** Wish heals the recipient's max HP / 2 when it lands
    (`Cmd_trywish`). Store only the counter, plus the wisher for the log.
  - **The previous item** (Recycle) is per battler position: `usedHeldItems`
    is set by `Cmd_removeitem` and cleared only at battle start. Put it on
    `Side`.
  - **The last move used and the last move that hit** are per battler and
    cleared on switch-in. Put them on `ActivePokemon`, along with Bide's
    damage.
  - **`Pokemon` types.** Switch-in recomputes the types and the ability from
    the species (`Cmd_switchindataupdate`). Drop the types from `Pokemon`.
  - **Boosts as i4** contradicts "no bit packing"; use `int8_t[7]`.
- **Missing:**
  - Weather's permanent-vs-temporary flag (abilities set permanent weather).
  - The Future Sight / Doom Desire move, for the log.
  - On `ActivePokemon`, everything `SwitchInClearSetData` clears:
    - moves: last, last hit by, last hit type, locked, Choice Band, disabled
      (with timer), encored (with slot and timer), Mimic's slots
    - counters: toxic; the hidden confusion, Uproar, Thrash and wrap;
      Taunt, Perish Song, Lock-On, Yawn, Charge, Rollout, Fury Cutter,
      Stockpile, Protect uses, Substitute HP, Bide, Truant, `isFirstTurn`,
      recharge
    - flags: every status2 and status3 flag, plus Flash Fire, a pending
      Trace, and who is binding it
  - Per-turn state that `TurnValuesCleanUp` zeroes: protected, endured, the
    physical and special damage taken (Counter, Mirror Coat, Focus Punch).
  - What `request` holds: the phase, and the other player's queued action,
    already resolved at selection.
- **PP rule.** PP spent is written back to the party unless the user is
  transformed or the slot was replaced by Mimic (`Cmd_ppreduce`,
  `MOVE_IS_PERMANENT`). Transform gives 5 PP per copied move.
- **Left out on purpose:** `gLastLandedMoves` (AI only), `hpOnSwitchout`,
  side-timer battler IDs, Follow Me, Helping Hand, Transform's personality,
  and `gBattleMonForms` (a sprite; Forecast changes only the types).
- **Size.** About 2 × 365 + 20 ≈ 750 B, a little over the 700 B target.
  The levers are u16 move IDs, and keeping active moves only while
  transformed.
- **Open decisions** (recommended answer first):
  1. Move the toxic counter to `ActivePokemon`, which shrinks `Status` to
     1 byte. Yes.
  2. A per-turn struct inside `Battle`, cleared at turn start, so it survives
     a Baton Pass request. Yes.
  3. `request` holds the phase and the queued action. Yes.
  4. Volatile flags as C++ bitfields, a raw integer with masks, or `bool`s.
     Bitfields.

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
    - then Quick Claw. One u16 per turn (`gRandomTurnNumber`) serves both
      sides, so two holders activate together.
    - then speed, with a random tiebreak
  - End-of-turn order calls `GetWhoStrikesFirst` again (`ENDTURN_ORDER`), so
    a speed tie is re-rolled independently there, and Quick Claw applies to
    it too.
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
  - Interactions with Choice Band and Encore: Encore shouldn't redirect the
    called move *(verify)*.
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
  - Both are always on: every format we play has them. They can be a
    compile-time constant, with no field in `Battle`.
  - Sleep Clause needs to know whether a sleep was inflicted by the foe or
    self-inflicted by Rest. `Status::EXT` already records that.
  - Sleep Clause: a sleep move fails if the foe already has a Pokémon put to
    sleep by the other player. Rest doesn't count. Yawn counts *(verify)*.
  - Freeze Clause works the same way for freezes. In singles every freeze
    comes from the foe, so it needs no extra flag.

Add these to the list when they come up: Baton Pass (what passes, and the
mid-turn request), Pursuit on a switching target, Choice Band locking, Focus
Punch, Truant, Destiny Bond and Perish Song endgames, Self-Destruct and
Explosion halving Defense, Struggle, and running out of PP.

## Plan

**Approach: soften the nut.** Most move effects are variations on a few
shared structures. Build those first, so each effect becomes a few lines:

1. **The turn frame.** `gBattleMainFunc`'s phases as an explicit table:
   selection, order, actions, fainted Pokémon, field end of turn, battler
   end of turn, Wish, Perish Song and Future Sight, then replacements.
   Settle suspend and resume (`request`) here, once.
2. **The hit backbone.** `BattleScript_EffectHit`: canceler, accuracy, PP,
   crit, damage, type calc, adjust, hit, secondary, faint check, `moveend`.
   Its entry points (`HitFromAccCheck` and so on) become helpers.
3. **The dispatchers.** `moveend`'s cases, `AbilityBattleEffects`,
   `ItemBattleEffects`, the attack canceler and the end-of-turn order, each
   as one ordered list. An ability or item is then one entry.
4. **The roll helpers.** One per catalogue row: Calc override, RNG, Chance
   report, as libpkmn's `Rolls`. The hook interface falls out of these.
5. **The checks.** `Battle::valid()`, asserted after every `update` in debug,
   plus soak runs.

Two ways to check our work exist before any Showdown infrastructure:
Smogon's damage calculator for the pure damage functions, and the script
decoder (strategy step 3). The decoder is worth building early: it groups
the effects by backbone and gives a checklist of branches.

Readiness (2026-10-07): the data layer is solid. The mechanics are barely
started: no script path has been traced end to end. There's no turn-phase
design, no hooks, no build or tests, and no battle builder; scenario tests
need a crude one early. Suggested order:

1. Settle the layout decisions, then write `data.hpp`.
2. The build, ASan and a test runner, with a crude battle builder.
3. The pure calculation functions, checked against the damage calculator.
4. The turn frame and roll helpers, with null Chance.
5. Step 1 below: switching and plain damaging moves end to end.
6. Trace the hit backbone and the dispatchers. Then add effects by the
   decoder's groups, starting with `teams/`.

Don't rush step 5 because "birch runs" feels like progress. Every later
mechanic inherits a rough frame.

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
        decomp.
     2. Add every move, ability and item that appears in `teams/`.
     3. Add the rest of the competitive pool.
     4. Last, the log (`protocol.hpp`).
   - Each mechanic is one analytical code path with a decomp citation.
3. **Testing.**
   - **Scenario tests** for each tricky mechanic, with the RNG forced through
     Chance and Calc overrides.
   - **Matching Showdown** is the correctness target for mechanics. How to
     compare (scenario replays, outcome distributions) isn't designed yet.
   - **Differential runs against ADV-Simulator** through the shared
     `pkmn_gen3_*` API. Compare outcome distributions over the same team
     pairs, not exact streams, because the RNGs differ.
   - **A soak run.** Many millions of random self-play games under ASan and
     UBSan, checking invariants: HP is in range, PP is never negative,
     `choices()` is never empty while the battle isn't over, and the turn cap
     is respected.
   - **A benchmark** against ADV-Simulator's 19k iterations/s.

## Status and caveats (2026-10-07)

**Done:**
- `pokeemerald/` is a depth-1 clone of pret/pokeemerald. It sits on the local
  branch `battle-only` (`a23e37a`), which strips everything unrelated to
  battle mechanics, and it isn't pushed. This repo ignores it, and
  `tools/generate.py` expects it at this path.
- `src/gen3/data/` has the generated tables (`tools/generate.py`) and the
  hand-written `natures.hpp` and `tables.hpp`.
  `src/gen3/README.md` lists the weird cases: the Hidden Power split, the
  Deoxys forms, species order, Struggle, type-effectiveness order, and the
  `Mail`/`Other` items.
- `src/gen3/data.hpp` has `Status`: a 2-byte struct holding libpkmn's
  status byte (hidden sleep counter, `EXT` for Rest or bad poison) and the
  Toxic counter, which persists through switching.
- `src/common/data.hpp` has `Player`, `ID`, `Choice` and `Result`, with
  libpkmn's byte layouts.
- `src/common/rng.hpp` has the RNG: SplitMix64 on a u64 seed, with
  `range(n)` and `chance(num, den)` helpers that are loop-free. The header
  maps each decomp `Random()` shape to a helper.
- `src/common/rational.hpp` ports libpkmn's `Rational(T)`. It reduces lazily, and `T` is
  `uint64_t`, `u128` or `double`. Chance will use `double`, as libpkmn's C
  ABI does. Its tests are libpkmn's, run as `static_assert`s and also run
  under ASan and UBSan. It differs from libpkmn in these ways:
  - **Errors.** libpkmn returns `error.Overflow`. Here integer `update`,
    `add` and `mul` return `false`, and `cmp` returns an empty
    `std::optional`. Only tests use `cmp`, `add` and `mul` (libpkmn's engine
    never calls them), so that shape stays. `double` never fails, but past 2^53 it drifts instead,
    as libpkmn's does.
  - **The coprime check.** libpkmn asserts that `p` and `q` are coprime when
    both are comptime-known. Here that check is a separate overload,
    `update<P, Q>()`, which uses a `static_assert`. The runtime
    `update(p, q)` doesn't check.
  - **Arguments.** `add`, `mul` and `cmp` take their argument by value, so
    they reduce a copy, not the caller's rational.
  - **Bounded gcd.** Every gcd loop has a fixed bound. The `double` gcd
    converts to `uint64_t` below 2^64. Above that it runs Euclid with an exact
    remainder built from halving, which works at compile time, where libpkmn
    uses `@mod`.
- `src/common/options.hpp` has `Options<Log, Chance, Calc>` and libpkmn's
  `options(log, chance, calc)` helper. The members are
  `[[no_unique_address]]`, so null types take no space.
- `src/common/protocol.hpp` has the log plumbing from libpkmn's
  `protocol.zig`: `Writer`, `NullWriter`, `Log<W>`, `FixedLog`, `NullLog`
  and `null_log`. The message methods come last.
  - The driver owns the buffer, and `Writer` only points into it
    (`{buffer, len, pos}`). So it lives in `Options`, never in `Battle`.
  - Each write is all or nothing and returns `false` when the bytes don't
    fit; that's libpkmn's `error.NoSpaceLeft`. **Open:** what `update` does
    with a full buffer (libpkmn returns `Result::Error`). Decide when the log
    is written.
- `src/gen3/chance.hpp` has:
  - the decomp findings on every hidden duration (rolls, what makes each
    step, Baton Pass, Early Bird, Rest)
  - a catalogue of every reachable `Random()` site with its multiplicity per
    update
  - draft `Actions` (101 B), one struct for both the Chance observation and
    the Calc overrides
  - draft `Durations` (22 B), with libpkmn's convention (0 inactive, 1
    started) and sleep by SLOT. It counts counter steps, so Early Bird adds 2
    per attempt (but see the open choice below).
- Testing so far is only `static_assert`s, checked with `-fsyntax-only`
  (`tools/check_data.cpp` and the headers). There's no build system yet.

**Open work, in no fixed order:**
- the `Battle`, `Side`, `Pokemon` and `ActivePokemon` layout, with the
  battle-state enums (weather, volatiles, side conditions). Per the "data
  covers the full game" rule, volatiles need room for every Gen III status2,
  status3 and disable-struct field.
- the per-`Effect` behaviour (hand-written with the mechanics, not
  generated) and Hidden Power from IVs
- the Chance and Calc hook interface: the calls the engine makes into
  Chance and Calc at each draw. It falls out of the `Random()` catalogue. The
  gen3 null Chance and null Calc come with it, in `chance.hpp` and
  `calc.hpp`. Skipped for now.
- `Chance` (Actions, Durations, probability) and `randomize_hidden`
- `Summaries`, on hold
- a `pkmn.h` stub with the functions birch's `cpp/include/search/mcts.h`
  calls
- pure calculation functions: stats, stat stages, crit stages, base damage,
  type application in `ORDER`
- a coverage report that parses `teams/` into our enums and counts the
  effects, abilities and hold effects in use
- the build: release and ASan/UBSan presets, plus a target that runs the
  tests. The generator stays a one-off dev tool, not part of the build.
- **Open choice: what Durations counts for sleep.**
  - *Counter steps* (the draft): Early Bird adds 2 per attempt.
    `randomize_hidden` needs nothing else, and it stays right when Skill
    Swap or Trace changes the ability mid-sleep.
  - *Move attempts*: a count that doesn't depend on the ability. But
    `randomize_hidden` would have to convert attempts to steps from the
    current ability, which is wrong if the ability changed mid-sleep.
  - The only argument for attempts was imperfect information, which now
    belongs to the log. That favours keeping steps. Not decided.
- **Still unverified:** whether Sleep Talk's called move re-runs the attack
  canceler (a second sleep step). The Actions and Durations drafts haven't
  been checked against the `Battle` layout yet.

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
