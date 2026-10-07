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
cartridge-exact or showdown-exact behaviour for every input; we promise no crashes and correct
competitive play.

## Mechanics references

- **Read Emerald, test against Showdown.** Showdown's ADV very nearly
  matches the cartridge, and the decomp is far easier to read precisely, so
  the decomp is what we write mechanics from. Testing against the decomp
  directly would need a lot of infrastructure first, so tests check behaviour
  against Showdown instead. Where the two disagree, record it in
  `src/gen3/README.md`.
- **Link battles are the default branch.** Showdown's ADV aims to match
  Emerald link battles, so where the decomp branches on `BATTLE_TYPE_LINK`
  (or Frontier and the like), take the link path. Where Showdown departs
  from it anyway, match Showdown, since it's the target, and record the
  exception in `src/gen3/README.md`. An exception doesn't change the
  default: decide each branch on its own. Known exceptions: Knock Off
  (decision 8).
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
  writes it back (util:2259). Checked (2026-10-07): no `REQUEST_STATUS_BATTLE`
  write-back fires on a raised counter while the Pokémon is still badly
  poisoned, and Baton Pass doesn't pass it either: `Cmd_switchindataupdate`
  (bsc:4621) reloads status1 from the party on every switch-in, and Baton
  Pass restores only the stat stages and status2.
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
    the species (`Cmd_switchindataupdate`). Superseded: types stay on
    `Pokemon` and switch-in copies them (decision 7).
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
    already resolved at selection. See [Turn phases](#turn-phases).
- **PP rule.** PP spent is written back to the party unless the user is
  transformed or the slot was replaced by Mimic (`Cmd_ppreduce`,
  `MOVE_IS_PERMANENT`). Transform gives 5 PP per copied move.
- **Left out on purpose:** `gLastLandedMoves` (AI only), `hpOnSwitchout`,
  side-timer battler IDs, Follow Me, Helping Hand, Transform's personality,
  and `gBattleMonForms` (a sprite; Forecast changes only the types).
- **Size.** About 2 × 365 + 20 ≈ 750 B, a little over the 700 B target.
  The levers are u16 move IDs, and keeping active moves only while
  transformed.
- **Decided (2026-10-07):**
  1. The toxic counter moves to `ActivePokemon`, which shrinks `Status` to
     1 byte. It restarts at 0 on every switch-in, Baton Pass included.
  2. **No per-turn struct in `Battle`.** The rule was to keep only what must
     survive a mid-turn resume, and nothing in `ProtectStruct` or
     `SpecialStatus` does. Per-turn state is a local in `update`, zeroed at
     turn start and again on resuming from a request.
     - `SpecialStatus` is per action: `SpecialStatusesClear` (main:4894)
       runs after every action.
     - `ProtectStruct` is per turn (`TurnValuesCleanUp(FALSE)`, main:4871).
       Its cross-action fields (`protected`, `endured`, Magic Coat's
       `bounceMove`, Snatch's `stealMove`, the damage taken that Counter,
       Mirror Coat, Focus Punch and Revenge read) are set and read within
       the actions phase of one `update`.
     - Why nothing crosses a request: the faint phases resume at end of turn
       or later, which reads none of it (`protected`/`endured` are cleared
       on entry, main:4866). A Baton Pass that moves second also resumes
       at end of turn. One that moves first resumes before the foe has
       acted, so nothing the foe reads has been set; the passer's own flags
       (inherited by the recipient, since `SwitchInClearSetData` doesn't
       touch `ProtectStruct`) can't matter, because its action was Baton
       Pass.
     - The argument rests on two conditions: requests only at action
       boundaries or at Baton Pass, and residuals never reading per-turn
       state. Guard it: every per-turn field is read through an accessor
       that asserts the phase is the actions phase.
     - Switch-in still clears what the decomp clears for a mid-actions
       switch-in. `FaintClearSetData` (main:3264) clears most
       `ProtectStruct` flags but not the damage taken; a normal switch and
       Baton Pass clear none.
  3. `request` holds the phase and the queued actions, per
     [Turn phases](#turn-phases).
  4. Switch-in works like libpkmn's: copy stats, species, moves, ability
     (and types, if `Pokemon` stores them) from `pokemon[]` and zero the
     rest of `ActivePokemon`. Nothing is recomputed from in-battle state, which
     matches the decomp's rule that switch-in discards Transform, Trace, Skill
     Swap, Forecast and Color Change. Whether `Pokemon` stores types or the
     species table supplies them is a layout detail; both match.
  5. Baton Pass's switch-in is its own function: it runs the normal
     switch-in, then restores what passes (see Baton Pass in
     [Tricky mechanics](#tricky-mechanics)).
  6. Volatile flags are named fields in one `Volatiles` struct on
     `ActivePokemon`, not one integer with masks. Plain `bool` per flag and
     `uint8_t` per small counter that the decomp packs into status2 and
     status3 (confusion, Uproar, rampage, wrap, Yawn, Lock-On). Wider
     volatile data (Substitute HP, Bide damage, disabled and encored moves)
     are ordinary fields beside it, as in libpkmn.
     - Why: about 274 decomp uses touch a single flag and translate one line
       to one line. Only 8 combine flags: the trapped check
       (`WRAPPED | ESCAPE_PREVENTION`, five places), the semi-invulnerable
       check, and Baton Pass's keep-list (main:3175–3176), which lives in
       its own function anyway.
     - Name each field after its decomp constant and cite it:
       `bool rooted; // STATUS3_ROOTED`. Searching for the constant finds
       the field.
     - Multi-flag checks become helpers written once: `trapped()`,
       `semi_invulnerable()`.
     - `valid()` checks each counter's range.
     - Packing comes last: then each member gets a bit width (`bool rooted :
       1;`), which changes no code that uses it. So never take a pointer or
       reference to a `Volatiles` member. Bitfield layout is up to the
       compiler, so check first whether birch reads raw `Battle` bytes (an
       encoder reading by offset); unverified.
  7. Types live on `Pokemon`, and switch-in copies them (decision 4).
     Equivalent to the decomp, which recomputes them from the species.
  8. The held item lives on `Pokemon`. Knock Off sets it to the sentinel
     `Item::KnockedOff` (no hold effect, so it's inert), and there is no
     Knocked Off flag. This takes the decomp's non-link path, an exception
     to the link-path default, because it matches Showdown: a knocked-off
     Pokémon can never gain an item, so nothing overwrites the sentinel. In link battles the cartridge lets Thief,
     Covet and Trick give it one, and clears it again at switch-in; we don't
     model that. See `src/gen3/README.md`.
     - Only identity checks (`item == Item::None`) must handle the sentinel;
       hold effects go through `get(item).effect`. Before relying on that,
       grep the decomp for every `.item == ITEM_NONE` / `!= ITEM_NONE` to
       confirm only Thief, Covet, Trick and Knock Off care (Recycle?).
     - Needs `KnockedOff` added to `Item` in `tools/generate.py`, after
       `Other`.
  9. **Battler-ID fields become victim-side state owned by the foe.** In
     singles the other battler is always the foe, so no field stores a
     battler. What the IDs did is done by clearing at the right switch: the
     foe's normal switch-out and its Baton Pass each clear a fixed subset.
     | Decomp | Becomes | Cleared when the foe… |
     |---|---|---|
     | `STATUS2_INFATUATED_WITH(b)` | `bool infatuated` | switches out, Baton Pass included (main:3197) |
     | `STATUS3_LEECHSEED_BATTLER` | dropped; the drain heals the foe's active Pokémon if alive (util:1507) | (never) |
     | `battlerPreventingEscape` | dropped; `bool escape_prevention` stays | switches out normally, not on Baton Pass (main:3164) |
     | `wrappedBy` | dropped; `wrapped_move` kept for the log | switches out, Baton Pass included (main:3199) |
     | `battlerWithSureHit` | dropped; `uint8_t lock_on` (0–2) on the target | switches out normally; on Baton Pass the lock resets to 2 (main:3166, 3178–3186) |
     - Lock-On (and Mind Reader): `Cmd_setalwayshitflag` (bsc:8134) sets
       the counter to 2 on the target; `ENDTURN_LOCK_ON` (util:1734)
       decrements it; while nonzero, the foe's moves skip the accuracy roll
       (bsc:1056) and OHKO moves skip it too, level check kept
       (bsc:7523–7533). So it lasts through the user's next turn.
  10. **Write-only fields are dropped:** every `*StartValue` in
      `DisableStruct` (Disable, Encore, Perish Song, Rollout, Charge) and
      `tauntTimer2`. Their only uses are writes (bsc:8008, 8040, 8524,
      8550, 9104, 9161) and Baton Pass's copy of the Perish Song one
      (main:3215). `Durations` does the "how far along" bookkeeping.
      Checked: a grep of all of `src/` and `include/` finds only these
      writes.
  11. **Multi-turn state mirrors the decomp, unmerged:** `bool
      multiple_turns` (`STATUS2_MULTIPLETURNS`), `Move locked_move`
      (`gLockedMoves`), and separate counters for Thrash (`LOCK_CONFUSE`),
      Uproar, Bide, Rollout and Fury Cutter, plus the semi-invulnerable
      flags. `cancel_multi_turn_moves()` clears them together, as
      `CancelMultiTurnMoves` does. Recharge stays separate
      (`STATUS2_RECHARGE` and `rechargeTimer`).
  12. **Hidden counters** are the ones in `Durations` (`chance.hpp`):
      confusion, Disable, Encore, binding, and Uproar or Thrash. Uproar and
      Thrash stay two fields in the battle; `Durations::attacking` covers
      both, and the locked move says which. Everything else (Bide, Rollout,
      Taunt, Yawn, Perish Song, Lock-On, Charge, recharge, Stockpile, Fury
      Cutter) is fixed-length and public. `Durations` doesn't change.
  13. **Also dropped:** `STATUS2_UNUSED`, the `DisableStruct` fillers,
      `transformedMonPersonality`, and:
      - `truantSwitchInHack`: only the Battle Arena judgments set it
        (bsc:6413–6431), so for us switch-in always sets
        `truantCounter = 1` (bsc:5258–5263). `bool truant_counter` stays;
        the end-of-turn abilities flip it (util:2640).
      - `STATUS3_INTIMIDATE_POKES` and `STATUS3_TRACE`: queued events, not
        conditions of the Pokémon. Switch-in sets them (util:2545–2566) and
        a later pass fires them (`ABILITYEFFECT_INTIMIDATE1`/`TRACE`). We
        implement Intimidate and Trace directly in the switch-in code
        instead. Still to trace when we do: where the pass fires after a
        Baton Pass or a Roar drag-in, and whether Trace can stay pending
        across turns when it has no valid target.
  14. **`isFirstTurn` stays a `uint8_t`, 0–2.** Switch-in sets 2
      (main:3220); `TurnValuesCleanUp(FALSE)` at the end of each turn
      decrements it (main:4875). Fake Out works while it's nonzero
      (bsc:6790); Speed Boost works while it isn't 2 (util:2629). A `bool`
      can't express both.
  - *(verify against Showdown)* A Truant replacement after a residual faint
    arrives after the end-of-turn abilities, so its counter is still 1 and it
    loafs on its first turn.
- **Packing comes last.** Natural alignment and compiler padding for now
  (`Request` is 10 B with 1 of padding). Revisit padding, explicit filler and
  `has_unique_object_representations_v` once the layout is settled.

#### Turn phases

From `gBattleMainFunc` (`battle_main.c`). A turn runs:

1. Selection. `gRandomTurnNumber` (Quick Claw) is drawn at the end of the
   previous turn (main:4013) and before the first.
2. Order (`SetActionsAndBattlersTurnOrder`), then Focus Punch setup.
3. The actions (`RunTurnActionsFunctions`). Each action's script ends in
   `end`, which sets `B_ACTION_TRY_FINISH` (bsc:3957), which runs
   `HandleFaintedMonActions` (util:638) before the next action.
4. `BattleTurnPassed` (main:3956): `DoFieldEndTurnEffects`, then
   `DoBattlerEndTurnEffects`, then `HandleFaintedMonActions`, then
   `HandleWishPerishSongOnTurnEnd` (Future Sight, then Perish Song). After
   each Future Sight or Perish Song event `BattleTurnPassed` re-enters, so
   faint replacement runs between them.

Every place `update` can suspend is a place a Pokémon can faint, plus Baton
Pass:

| Phase | Who chooses | Resume at |
|---|---|---|
| `BatonPass` | user's side `Switch`, other `Pass` | finish Baton Pass's switch-in, then the remaining action |
| `FaintAfterAction` | either side or both | end of turn (the remaining action was cancelled) |
| `FaintAfterResiduals` | either side or both | Future Sight |
| `FaintAfterFutureSight` | either side or both | Perish Song |
| `FaintAfterPerish` | either side or both | the next turn |

`request` holds the phase, the index of the next action (only `BatonPass`
needs it), who moves first (decided once at selection, after the Quick Claw
and speed-tie draws, which must not be redrawn), and each player's queued
action resolved at selection (the choice and the move ID, since Encore,
Choice Band and Struggle are resolved then, as `gChosenMoveByBattler`). No
end-of-turn position: residual faints don't interrupt the residual passes.

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
  - When both sides replace at once, `HandleFaintedMonActions` loops in
    battler-ID order (util, its case 4), not speed order *(verify against
    Showdown)*.
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
    - `DoFieldEndTurnEffects`: screens, Mist, Safeguard, Wish
      (`ENDTURN_WISH`, util:1173), weather damage and so on
    - `DoBattlerEndTurnEffects`: Ingrain, Leftovers, Leech Seed, poison,
      burn, Nightmare, Curse, binding, Uproar, rampage, Disable, Encore,
      Taunt, Yawn and so on, in speed order
  - Then faint replacement, then Future Sight, then Perish Song
    (`HandleWishPerishSongOnTurnEnd`), each followed by faint replacement.
    See [Turn phases](#turn-phases).
  - Encode the order once, as a table or a switch, not spread across move
    handlers.
- **A faint "ends the turn".** A cornerstone ADV mechanic (Gen IV
  differs). When a Pokémon faints during the actions, it is replaced at once,
  and every action still queued that turn is cancelled.
  - Mechanism: `BattleScript_FaintedMonSendOutNew` (bs1:2883) ends with
    `cancelallactions` in singles. `Cmd_cancelallactions` (bsc:5850) sets
    every queued action to `B_ACTION_CANCEL_PARTNER`, which equals
    `B_ACTION_FINISHED` (`battle.h:39–40`), so they do nothing.
  - So if the faster Pokémon explodes, or a Pokémon switched in dies to
    Spikes, the foe's move never happens: heals and setup can be denied.
  - The replacement arrives before end of turn, so it takes that turn's
    residuals (sandstorm, Leftovers and so on).
  - In the result protocol: `Switch` for the fainted side and `Pass` for the
    other, or `Switch` for both.
  - *(verify)* A double KO in link battles goes through
    `BattleScript_LinkHandleFaintedMonMultiple` (via `checkteamslost`,
    bsc:3582), which has no `cancelallactions`. If both faint on the first
    action, it's untraced whether the replacement then uses the fainted
    Pokémon's queued move. `HandleAction_UseMove` checks only the absent
    flags copied at turn start. Needs a Showdown scenario test.
- **Residual faints.** A Pokémon that faints during end of turn is replaced
  only after both residual passes finish, so the replacement skips that
  turn's residuals (no sandstorm, no Leftovers). Future Sight and Perish Song
  still come after it.
- **The battle ends mid-residuals.** `checkteamslost` sets `gBattleOutcome`
  as soon as a side has no Pokémon left (bsc:3564–3577). The weather loop
  exits on it (bs1:3219), and `BattleTurnPassed` runs the residual passes
  only while `gBattleOutcome == 0` (main:3961). So if both last Pokémon
  would die to residuals, the faster one dies first and the battle ends: the
  slower side wins, not a tie.
  - Audit every other way a battle ends the same way: Explosion,
    Self-Destruct, Destiny Bond, recoil, Perish Song *(verify the tie rules
    against Showdown and Smogon ADV)*.
- **Baton Pass.** Its switch-in is its own function (main:3158–3217,
  bsc:4648).
  - Passes: stat stages; status2 confusion (with its hidden counter), Focus
    Energy, Substitute, escape prevention, Curse; status3 Leech Seed,
    Lock-On (a lock *on* the passer stays on the recipient), Perish Song,
    Ingrain, Mud Sport, Water Sport; and from the disable struct, the
    Substitute HP and the Perish Song timer. (The decomp also copies the
    battler-ID and start-value fields, which we drop; decisions 9 and 10.)
  - Doesn't pass: the Toxic counter, the rest of the disable struct, the
    last-move fields.
  - Lock-On is stored on the target. If the passer had locked on to the foe,
    the foe's lock resets to 2 (main:3178–3186), so the recipient inherits a
    fresh lock that covers its next turn too. *(verify against Showdown)*
  - Baton Pass doesn't cancel the foe's queued action, so its request resumes
    at the remaining action.
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

Add these to the list when they come up: Pursuit on a switching target, Choice Band locking, Focus
Punch, Truant, Destiny Bond and Perish Song endgames, Self-Destruct and
Explosion halving Defense, Struggle, and running out of PP.

## Plan

**Approach: soften the nut.** Most move effects are variations on a few
shared structures. Build those first, so each effect becomes a few lines:

1. **The turn frame.** `gBattleMainFunc`'s phases as an explicit table; see
   [Turn phases](#turn-phases). Settle suspend and resume (`request`) here,
   once.
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
     Chance and Calc overrides. Named cases, waiting on the build (each
     checked against Showdown):
     - *Explosion ends the turn*: a faster Explosion user faints, and the
       surviving foe's queued move (a heal or setup) never happens.
     - *Spikes sac*: a Pokémon switched in dies to Spikes, and the foe's
       queued attack is cancelled.
     - *Mid-turn replacement takes residuals*: a Pokémon KO'd by an attack is
       replaced before end of turn, and the replacement takes sandstorm and
       gets Leftovers.
     - *Residual faint skips the replacement's residuals*: a Pokémon dies to
       sandstorm, and its replacement takes no residuals that turn.
     - *The battle ends mid-residuals*: both last Pokémon would die to
       residuals; the faster dies first and its side loses (not a tie).
     - *Double KO on the first action*: both faint (Explosion), both replace;
       does the second replacement act? Also the replacement order.
     - *Baton Pass resumes*: after the pass, the foe's queued move still hits
       the recipient; the Toxic counter restarts at 0.
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
- `src/gen3/data.hpp` has:
  - `Status`: libpkmn's 1-byte status (hidden sleep counter, `EXT` for Rest
    or bad poison). The Toxic counter is left for `ActivePokemon`, which
    doesn't exist yet.
  - `Phase` and `Request` (10 B), per [Turn phases](#turn-phases). Neither
    is placed in a `Battle` yet, so the field order may change.
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
