# Gen III

These are the design decisions and deviations for the Gen III engine, in the
spirit of libpkmn's `gen*/README.md`. Extend this file as mechanics land.

## Data

`types`, `abilities`, `moves`, `species`, `items` and `names.hpp` in `data/`
were generated once from the decomp (pret/pokeemerald at `731ad5b`); the
generator is gone and they are edited by hand now. `names.hpp` holds
Showdown-style IDs of the decomp's names, for the team parser only. New small
tables are written by hand with decomp citations:

- `natures.hpp`: `Nature`, the nature stat table and the flavors a nature
  dislikes (for the confusion berries).
- `tables.hpp`: stat and accuracy stage ratios, crit chances, Protect rates,
  Flail, Low Kick, Magnitude, multi-hit, type-boosting
  items, sound moves (Soundproof), and the link-battle constants for Nature
  Power, Secret Power and Camouflage. Protect's rate index is clamped where
  the decomp reads out of bounds.
 Each enum is a scoped `enum class` whose underlying type is
`uint8_t`, or `uint16_t` for `Move` and `Species`. Each table is a `constexpr`
array indexed by the enum, with `get(x)` as the accessor.

| Header | Enum | Size | Order |
|---|---|---|---|
| `types.hpp` | `Type` | 18, u8 | cartridge (`TYPE_*`) |
| `abilities.hpp` | `Ability` | 78, u8 | cartridge (`ABILITY_*`) |
| `moves.hpp` | `Move` | 371, u16 | cartridge 0–354, then the typed Hidden Powers |
| `moves.hpp` | `Effect` | u8 | cartridge (`EFFECT_*`) |
| `species.hpp` | `Species` | 390, u16 | National Dex, then the Deoxys forms |
| `items.hpp` | `Item` | 74, u8 | cartridge (hold-effect items and the Enigma Berry), then `Mail` and `Other` |
| `items.hpp` | `HoldEffect` | u8 | cartridge (`HOLD_EFFECT_*`) |

### Weird cases

Statements marked *(verify)* haven't been checked against the decomp yet.

- **Hidden Power is split by type.**
  - `HiddenPowerFighting` through `HiddenPowerDark` (16 moves) come after
    `PsychoBoost`, so the type is static move data and nothing computes it
    from IVs in the hot path.
  - The generic `HiddenPower` keeps its cartridge slot (237). Team building
    takes it and retypes it from the IVs (`hidden_power(ivs)` in `data.hpp`,
    a port of `Cmd_hiddenpowercalc`); Metronome could also call it. Its data
    type is meaningless.
  - Power (30–70) is computed from the IVs at the same time and stored per
    Pokémon (`Pokemon::hidden_power`), because splitting by power too would
    mean 16 × 41 moves. The decomp computes both at each use, but IVs never
    change in battle.
  - Copy semantics, from `Cmd_hiddenpowercalc`, which reads the *user's* IVs:
    - **Transform:** `Cmd_transformdataexecution` copies `BattlePokemon` up
      to `pp`, which includes the IVs. Copying the target's typed variant
      and power is therefore exact.
    - **Mimic:** the copied move must be retyped to the user's Hidden Power
      type and power.
    - **Sleep Talk:** calls the user's own move, so it's already correct.
    - **Metronome:** can call the generic `HiddenPower`, which must resolve
      to the user's type. Metronome is banned in Smogon play, so this can
      wait.
- **Deoxys** has four species entries.
  - On the cartridge, Deoxys's stats depend on the game:
    - Ruby and Sapphire: Normal form
    - FireRed: Attack form
    - LeafGreen: Defense form
    - Emerald: Speed form (`GetDeoxysStat`)
    - Link battles use Normal (`ShouldIgnoreDeoxysForm`)
  - We follow Showdown's separate species: `Deoxys` (Normal, from
    `species_info.h`), `DeoxysSpeed` (from `sDeoxysBaseStats`), and
    `DeoxysAttack` and `DeoxysDefense`, whose stats are hard-coded in
    `species.hpp` *(verify against pokefirered)*. All four share Normal's types,
    abilities and weight.
- **Castform** is one species. Forecast changes the active Pokémon's types
  (`ActivePokemon.types`). Its forms are only sprites.
- **Unown's** letter comes from its personality value and has no effect in
  battle, so it's one species. The cartridge's 25 unused "old Unown" slots
  (internal IDs 252–276) are dropped.
- **Species order is National Dex order**, not the cartridge's internal
  order. Internally, Hoenn species are shuffled and come after the unused
  slots. So `Species` values don't match the decomp's `SPECIES_*` values.
  Move, ability, type and item values do match the decomp.
- **Mystery (???)** is a real type at index 9, and only Curse has it.
  Whether a move is physical or special depends on its type: below Mystery
  is physical, above is special (`is_special`).
- **Struggle** skips `Cmd_typecalc` entirely. It ignores type effectiveness,
  so it hits Ghost types, and it gets no STAB. Its data says `Normal`
  anyway, so mechanics has to special-case it.
- **Type effectiveness depends on order.**
  - `ModulateDmgByType` does `damage * mul / 10` once for each matching entry
    of `gTypeEffectiveness`, in table order, flooring each time. Against a
    dual type, ×2 then ×0.5 can differ from ×0.5 then ×2 when damage is odd.
  - `ORDER[atk][def]` gives each pair's table position. Apply the defender's
    two types in increasing order.
  - The Normal and Fighting → Ghost immunities come after the
    `TYPE_FORESIGHT` marker, so Foresight skips them. They're in the chart,
    and mechanics has to check Foresight.
- **Items.**
  - The enum has the 70 items with a `holdEffect`, plus three items without
    one:
    - The **Enigma Berry** has no hold effect in Gen III; its effect would
      come from e-Reader data.
    - **`Mail`** stands for any of the 12 Mail items.
    - **`Other`** stands for every other holdable item (balls, stones and so
      on).
  - These matter only to item-moving moves. `Cmd_tryswapitems` (Trick) and the
    Thief/Covet steal check in `battle_script_commands.c` refuse Mail and the
    Enigma Berry. `Other` can be moved and Knocked Off like any item.
  - Type-boosting items are +10% in Gen III, stored as `param`.
- **Weight** comes from the Pokédex entries (`pokedex_entries.h`), in
  hectograms. Only Low Kick uses it.
- **Abilities.** A species lists up to two. Which one a Pokémon has comes
  from a bit of its personality, so the battle stores the ability on each
  Pokémon and doesn't derive it from the species.
- **Move targets** are bit flags in the decomp (`MOVE_TARGET_*`), but every
  move has exactly one, so `Target` is a plain enum with the flag values.
- **Restrictions on calling moves** come from `Extra`:
  - `NoMimic` and `NoMetronome` are the prefixes of `sMovesForbiddenToCopy`.
  - `NoAssist` is that list plus `IsInvalidForSleepTalkOrAssist`.
  - `NoSleepTalk` is:
    - `IsInvalidForSleepTalkOrAssist` (Sleep Talk, Assist, Mirror Move,
      Metronome)
    - Focus Punch and Uproar
    - `IsTwoTurnsMove`, which covers Skull Bash, Razor Wind, Sky Attack,
      SolarBeam, Fly, Dig, Dive, Bounce and also Bide
  - Sleep Talk also filters with `CheckMoveLimitations`, ignoring PP: Disable,
    Torment, Taunt, Imprison and the Choice lock. That check is mechanics,
    not data.

## Mechanics

Where the decomp and Showdown disagree, or the decomp has a branch we don't
take, it's recorded here. Where the decomp branches on link battles, the
default is the link path, as Showdown aims to match Emerald link battles;
entries here are the exceptions.

- **Knock Off: the non-link path** (decision 8). An exception to the default
  (link battles; see CLAUDE.md, "Mechanics references"). Showdown departs from the
  link path here, and we match Showdown. A knocked-off Pokémon holds
  `Item::KnockedOff` for the rest of the battle and can never gain an item.
  - The cartridge in link battles differs. Knock Off clears the battle copy
    of the item and sets `knockedOffMons` (bsc:2880–2884). Thief/Covet
    (bsc:2757–2766) and Trick (`Cmd_tryswapitems`, bsc:9206–9216) check that
    bit only outside `BATTLE_TYPE_LINK` and friends. So in a link battle a
    knocked-off Pokémon can steal or Trick for an item, which is also written
    to the party (bsc:2786). Nothing clears the bit in battle, and switch-in
    clears the item whenever it's set (bsc:4641–4645), so the gained item is
    lost again at the next switch-in. (That last step is inferred from the
    bit never being cleared.)
  - Showdown 0.11.10: Knock Off marks the item `knockedOff`
    (`data/mods/gen4/moves.ts:875`, inherited by gen3), and `setItem` refuses
    a knocked-off Pokémon (`sim/pokemon.ts:1811`). Its hint: "In Gens 3-4,
    Knock Off only makes the target's item unusable; it cannot obtain a new
    item."

- **Two switches go in speed order with Quick Claw, as Showdown** (decided
  2026-10-07; Quick Claw corrected the same day: gen3 Showdown's
  `getActionSpeed`, `data/mods/gen3/scripts.ts:18`, serves every action).
  The decomp doesn't sort switches: `SetActionsAndBattlersTurnOrder` queues
  them in battler-ID order and only compares speed between moves
  (main:4756). It matters because the first switch-in's Intimidate hits the
  foe that's about to leave.
- **Switch-in effects follow Showdown's `runSwitch`** (decided 2026-10-07).
  Each Pokémon's Spikes, switch-in ability (weather, Intimidate, Trace) and
  item run together when it arrives (gen4's `runSwitch`, inherited by gen3).
  The decomp flags Intimidate and Trace and runs them after the action
  (util:1945). When two Pokémon enter together (the battle start, both
  sides replacing), both enter, then each arrives in speed order (Quick
  Claw except at the start, a random tie) and the battle can end between
  them; the decomp uses `GetWhoStrikesFirst` at the start
  (main:3841) and battler-ID order for replacements (util:1880).

### Turn frame

From `gBattleMainFunc` (`battle_main.c`), reshaped where Showdown differs.
`update` (`mechanics.hpp`) is one `switch (request.phase)` whose cases fall
through in turn order, so the order is written once; a resumed case first
does its replacement. `Phase` must be declared in that order (asserted).
Internal steps return a `Flow` (`std::optional<Result>`): empty continues,
a value (a request or the end of the battle) is returned.

| Phase | Who chooses | Resume at |
|---|---|---|
| `None` | both | a new turn: selection, order, Focus Punch, the actions |
| `BatonPass` | the user's side `Switch`, the other `Pass` | finish the switch-in, then action `Request::next` |
| `FaintAfterAction` | either side or both | the end of the turn |
| `FaintAfterResiduals` | either side or both | the next turn |

- **Start (turn 0), as libpkmn.** A built battle has no active Pokémon
  (`valid()` enforces it). The first `update` (both Pass) sends in SLOT 1 on
  each side; they arrive in speed order (no Quick Claw, a random tie), and
  the battle is at turn 1.
- **Selection and order.** `Request` keeps what must not be recomputed on
  resume: each player's action as resolved at selection (Encore, the Choice
  Band lock, Struggle), who goes first, and the turn's one Quick Claw draw,
  made lazily at the first comparison it can change and reused by the
  end-of-turn order. Switches go first, in speed order with Quick Claw; then
  priority, Quick Claw, Speed, a random tie. Two Quick Claw holders that
  both proc are a speed tie.
- **A faint ends the actions** (Gen IV differs). The replacement's
  `cancelallactions` (bsc:5850) cancels everything still queued, so a faster
  Explosion, or a switch-in dying to Spikes, denies the foe its move. The
  replacement takes that turn's residuals. Baton Pass doesn't cancel the
  foe's action: its request resumes at it.
- **The end of turn** (`end_of_turn`): `field_residuals` (side timers, Wish,
  weather and its damage), the per-Pokémon stages (`battler_stage`), the
  future attacks, Perish Song's count, the lock and Disable timers, then an
  `item_cure` pass (Showdown's Update) and the residual faints'
  replacements, which skip the turn's residuals. The per-Pokémon stages
  run in Showdown's order: every gen3 residual has order 10, so with unequal
  Speeds (Quick Claw counting) the faster Pokémon takes all of its stages
  first; on a Speed tie they interleave by subOrder, a coin per stage,
  recorded only where the two orders give different states
  (`stage_order_matters`).
- **The battle can end mid-residuals**, as Showdown checks after every
  handler (`sim/battle.ts:569`): if both last Pokémon would die to separate
  residuals, the first to faint loses. Sand, hail and Perish Song hit both
  first, so both last fainting there is a tie, as is both last fainting in
  one action (Explosion included: `Cmd_checkteamslost`, bsc:3563, and
  Showdown's `checkWin` before gen 4).
- **Spikes** (`spikes()`, `Cmd_switchineffects`, bsc:5180) hit every
  switch-in: voluntary, replacement, Baton Pass, Roar's drag. They come
  before the switch-in ability and item, which a Spikes faint skips; that
  faint raises another request in the same phase. When both sides replace
  at once, both enter, then each takes Spikes and its switch-in effects in
  speed order, and a first arrival that faints cancels the second's
  switch-in effects (`arrive_both`).
- **Sleep Clause and Freeze Clause** (Smogon's mods, always on) are
  compile-time rules with no field. A foe's sleep fails while a living
  Pokémon on the target's side sleeps from the foe (`sleep_clause`; Rest
  doesn't count), for sleep moves after the accuracy check, Yawn and Effect
  Spore alike.

### Showdown and the cartridge disagree

A shortlist of the visible differences found so far; we follow Showdown in
every one (CLAUDE.md, "Mechanics references"). The details are in the code
comments at each site and in the entries above. "Log only" means the state
is the same and only the messages differ.

| Mechanic | Cartridge (pokeemerald) | Showdown (what we do) |
|---|---|---|
| Damage arithmetic | integer steps of `CalculateBaseDamage` | Showdown's modifier chain ("Damage") |
| Accuracy | combined integer stage ratios | accuracy then evasion stage as fractions |
| Crit vs immunity | crit drawn before the type check | immunity first, no crit draw |
| Quick Claw | `gRandomTurnNumber < 0xFFFF * 20 / 100`, about 1/5; moves only | exactly 1/5; also orders two switches |
| Two switches | battler-ID order | speed order |
| Switch-in effects | Intimidate and Trace after the action | Spikes, ability, item on arrival, in speed order |
| Confusion self-hit | Reflect and type items apply; no Focus Band behind a Substitute | typeless 40 power; Focus Band always |
| Knock Off | link battles: a stolen item can come back | the item is gone for good (decision 8) |
| Perish Song | own pass after Future Sight | counts in the residual pass |
| Future Sight, Doom Desire | typed damage, no roll; lands after the residual faints are replaced, with its own request | typeless, rolled at the use; lands in the residual pass (order 11), its accuracy drawn then |
| Binder leaving | frees the victim at once | the victim stays bound, untrapped, until its residual, which ends it |
| Stockpile family | Stockpile raises nothing; failures print | the same; failures are silent; Spit Up rolls |
| Leech Seed KO of the last Pokémon | no `checkteamslost` | the battle ends there |
| Sleep Talk's pick | skips disabled, taunted... moves; calls 0-PP moves | uniform over allowed slots; fails on 0 PP |
| Brick Break into an immune target | breaks the screens | breaks none (its onTryHit is in moveHit) |
| Present's heal on a Rage user | no boost (MOVEEND_RAGE needs damage) | raises its Attack (the Hit event) |
| Sleep Talk's Counter and Mirror Coat | work | fail silently (no `beforeTurnCallback`) |
| Statuses from contact abilities | ignore Safeguard | Safeguard blocks them |
| Status berries at move end | only the first Pokémon | both |
| Protect's chance after 4 uses | indexes past its 4-entry table (bsc:6514) | stays at 1/8 |
| Roar and Whirlwind | a lower-level user can fail (`TryDoForceSwitchOut`) | no level check |
| Safeguard vs Confuse Ray | script ends without moveend | fails normally |
| Drain moves | Substitute's half rounds down; drains on no effect | rounds up; no drain without effect |
| Counter under Pressure, failed | spares the PP | costs 2 PP |
| Baton Pass | binding not passed; Lock-On reset | binding passes; Lock-On keeps its count |
| Beat Up | own formula, no roll, no screens (bsc:8957) | typeless special hit with a roll ("Damage") |
| Psych Up into Fly, Dig or Dive | copies the stages | misses |
| Flash Fire and Will-O-Wisp | absorbed whatever the holder | not absorbed by a Fire type, a statused holder or one behind a Substitute |
| Last move after a lost turn | `MOVE_UNAVAILABLE` (Encore, Mimic fail) | keeps the previous move |
| 2-5 hit moves | stop after Endure or Focus Band | keep hitting |
| Sleep or freeze on a charging Pokémon | cancels the charge at once | charge kept until the move is aborted |
| Memento | user faints into Protect, Substitute, Fly; fails at -6/-6 | user lives if blocked; faints at -6/-6 |
| Fake Out against Focus Punch | the flinch stops Focus Punch; no PP | no flinch (focus blocks it); Focus Punch loses focus and spends its PP |
| Pain Split | the HP change counts as damage | not damage (Counter, Focus Punch) |
| Thief and Covet | can't take Enigma Berry or Mail | can |
| Imprison under Pressure | costs extra PP | doesn't |
| King's Rock | 10%, once more after a multi-hit loop | 10% per hit (Beat Up too), doubled by Serene Grace |
| Shell Bell | an eighth of the last hit's HP, Substitute included | an eighth of all hits on the target itself |
| Leppa Berry | at the end of the turn | as soon as a slot reaches 0 PP |
| Flavor berries | also right after a hit | at the end of the turn only |
| Nightmare | no Protect check | Protect blocks it |
| Leech Seed, Taunt, Encore vs Protect/evasion | fail before the accuracy draw | draw first, so a miss is logged (log only) |
| Quick Claw activation | — | not logged by Showdown's gen3 (log only) |
| Pressure on field-wide moves | none (`MOVE_TARGET_USER`): weathers, Haze, Perish Song... | an extra PP (target `'all'` includes the foe); Conversion 2 too |
| Sleep Talk into Pressure | 2 PP for Sleep Talk itself (`MOVE_TARGET_DEPENDS`), none for the called move | 1 for Sleep Talk (`'self'`), plus the called move's extra PP |
| Protect's chain after a turn without acting | kept (`gLastResultingMoves` unchanged) | ended (`stall` expires after a turn without a success) |
| Endure's priority | +3, as Protect and Detect | +4 (the gen4 mod lowers only Protect and Detect); set in `src/data/moves.hpp` |
| Thief taking a Choice Band | not locked (item arrives after `MOVEEND_CHOICE_MOVE`) | locked into Thief |
| Speed | floors per factor; weather ×2 before the stage | stage, then modifiers chained and rounded once (paralysis 147 and 149 both 37) |
| End-of-turn order | `GetWhoStrikesFirst`: move priority, Quick Claw; one order for the whole pass | Speed with Quick Claw (one order for all per-Pokémon residuals, onResidualOrder 10); on a Speed tie, each handler pair by subOrder with its own coin |
| Move-stopping checks | sleep, freeze, Truant before recharge; Taunt before Imprison; paralysis before Attract | `BeforeMove` priorities: recharge first, Attract before paralysis, Taunt last |
| Sand or hail KOs both last Pokémon | faster faints first and loses | both hit, then a tie |
| Perish Song KOs both last Pokémon | — | both counts, then a tie |
| Pursuit on a switching target | final damage ×2; no attack canceler; partial moveend | base power ×2; canceler (paralysis...) and full moveend |
| Stat berries with nothing to do | kept (stat at +6, Lansat under Focus Energy, Starf at all +6) | eaten anyway |

Not yet compared with Showdown: everything not on `teams/ou/` (Uproar,
Thrash, Bide, Future Sight, Transform...).

### Damage

`damage.hpp`, written 2026-10-07. Decided:

- **The arithmetic is Showdown's, not the decomp's.** An exception to
  "read Emerald" and to "don't write mechanics from Showdown's source"
  (CLAUDE.md), decided because the two disagree on common OU damage (Thick
  Fat Snorlax, Choice Band Guts Heracross) and the target is matching
  Showdown. `damage.hpp` follows 0.11.10's `getDamage` (0.11.11 gives the same cases)
  (`sim/battle-actions.ts`) and gen3 `modifyDamage`
  (`data/mods/gen3/scripts.ts`), citing the handler for each modifier.
  `tools/check_damage` compares it with Showdown over 74 cases, crit and
  not, every roll (fixture from `tools/damage_oracle.cjs`).
- **The formula lives in `damage.hpp`**, included by `mechanics.hpp` and
  `calc.hpp` (so `min_ko_roll` reuses it without side effects). Its functions
  read a `const Battle&` and take no `options`: they never draw. The draws
  are roll helpers in `effects.hpp`, as libpkmn's `Rolls`.
- **A zero divisor is asserted, not checked.** It needs a Def or SpD of 0–3,
  which no valid Pokémon has (the lowest is 4, which is still 1 at −6), so
  it's corrupt state (CLAUDE.md, "Safety"). Showdown would compute `Infinity`
  and truncate.
- **The dynamic inputs are a `Hit`** (move, power, type, crit). The hit
  backbone's per-action `State` (a local, as libpkmn gen2's, never in
  `Battle`) holds one. The caller resolves power and type, as Showdown's
  base power callbacks and `onModifyMove` do (Hidden Power, Weather Ball ×2
  in weather, Revenge, Pursuit, Flail, Return...). Modifiers come from the
  battle state inside `damage()`.
- **Beat Up is Showdown's** (2026-10-07): each hit goes through `damage()`
  as a typeless special hit with the ally's base Atk against the target's
  base Def (`Hit::ally`), so it rolls 85–100%, Light Screen halves it and
  Mirror Coat returns it. The decomp's `Cmd_trydobeatup` (bsc:8957) has
  its own formula with no roll and no screens. Up to six hits, so
  `Action::hits` has six entries (decided 2026-10-07).

Showdown's model, in order:

1. `matchup()`: immunity (Levitate, Foresight), the summed type modifier,
   Wonder Guard (type modifier ≤ 0). Struggle is typeless: none of these, no
   STAB, no type-keyed modifier. The absorbing abilities are the backbone's.
2. Base power × one chained modifier: Charge (on the move's actual type, so
   Hidden Power Electric too), Mud and Water Sport (per user), the pinch
   abilities, Thick Fat, Facade (any status but sleep), Solar Beam in rain,
   sand or hail, Gust and Twister on Bounce.
3. Attacking stat: the stage first (a crit ignores a drop), then one chained
   modifier: Huge/Pure Power, Guts, Choice Band, Thick Club, Plus/Minus, Deep
   Sea Tooth, Light Ball, Soul Dew, type items ×1.1 (Sea Incense ×1.05).
   Hustle applies its ×1.5 directly, before the chain. Defending stat
   likewise: Marvel Scale, Metal Powder, Soul Dew, Deep Sea Scale; then
   Explosion and Self-Destruct halve Def (at least 1).
4. `(2·level/5 + 2) · power · atk / def / 50`, floored at each division.
5. Burn ×0.5 (physical, not Guts); the screens (not on a crit) and Flash
   Fire as one chained modifier; weather (suppressed by an unfainted Cloud
   Nine or Air Lock); physical at least 1; + 2; crit ×2; STAB ×1.5; ×2 per
   super-effective type, then ÷2 per resisted one; Earthquake on Dig, Surf
   and Whirlpool on Dive, Gust and Twister on Fly, Stomp on Minimize ×2.
6. `apply_roll`: × (85 + index) / 100, at least 1.

A chained modifier is in 4096ths: each factor is `trunc(m · 4096)` (so 1.1
is 4505), folded in with `(a · b + 2048) >> 12`, and applied once as
`(x · m + 2047) / 4096` (Showdown's `chain`, `chainModify`, `modify`).

Where the decomp differs (pokemon = `src/pokemon.c`, `CalculateBaseDamage`
at 3106; bsc; bs1 = `data/battle_scripts_1.s`):

- It floors after every modifier and applies the stage *after* them.
  Choice Band Guts Heracross's Megahorn on Celebi: 1713–2016 there,
  1717–2020 here.
- Thick Fat halves SpA (pokemon:3198), not base power. Charizard's
  Flamethrower on Snorlax in sun: 105–124 there, 102–121 here.
- Type items are ×(100 + param)/100 on the stat; Soul Dew applies outside
  Frontier; Explosion halves Def before the stage, which can reach 0.
- Weather, Flash Fire and the Sports: weather after the screens and Flash
  Fire after weather, all on damage; the Sports on power, once whoever has
  them; `ABILITY_ON_FIELD2` counts fainted Pokémon.
- `gBattleScripting.dmgMultiplier` ×2 applies after the + 2, before STAB:
  Weather Ball (bsc:9792), Revenge (bsc:9346), Facade, Earthquake on Dig and
  the other semi-invulnerable bonuses (bs1:240–2274). Charge reads the
  table type, so Hidden Power Electric isn't boosted.
- Type effectiveness goes entry by entry in `gTypeEffectiveness` order
  (`ORDER`), each step floored and at least 1. A special hit has no minimum
  of 1 before the + 2.
- Wonder Guard spares the charge turn (`AttacksThisTurn`); in Showdown the
  charge turn never reaches it.

Draws in the decomp's hit, for the roll helpers: crit (`Cmd_critcalc`,
bsc:1253) before the type calculation; the damage roll and Focus Band
(`Cmd_adjustnormaldamage`, bsc:1658), which `jumpifmovehadnoeffect`
(bs1:623) skips when the move had no effect. Focus Band draws whenever the
target holds it, at full HP or behind a Substitute. Multi-hit runs crit
through the roll per hit (`BattleScript_MultiHitLoop`, bs1:612).

**Decided: immunity before crit, as Showdown** (gen3 `tryMoveHit`,
`data/mods/gen3/scripts.ts:311`, before `getDamage` rolls crit,
`sim/battle-actions.ts:1628`). A crit on a move with no effect changes
nothing, so the only difference from the decomp is that no crit is drawn;
following Showdown keeps the log's message order the same as its.

### Hit backbone

`mechanics.hpp`'s `use_move` is `HandleAction_UseMove` (util:78). Every
move, called ones included, runs through one frame, `execute`: the attack
canceler (`cancel`: `AtkCanceler_UnableToUseMove`, util:1987, then the rest
of `Cmd_attackcanceler`, bsc:915), attackstring and `ppreduce` for every
script but the hits', then `script`, one `switch` with each effect once,
then `moveend`. `hit` is `BattleScript_EffectHit` (bs1:236) and the scripts
built on it (`plain_hit`): its own attackstring and `ppreduce`, accuracy, crit, damage, the roll,
`adjustnormaldamage`'s survival checks, `datahpupdate`, recoil, `tryfaintmon`.
Per-action state is a `MoveState` on the stack. Follow Me and Helping Hand,
the only unwritten effects (doubles only), run the canceler and use PP, then
`moveend`; Helping Hand logs `[notarget]` and `-notarget`, as Showdown's
singles.

Where it departs from the decomp, or from the draws the decomp makes:

- **Immunity first** (decided, as Showdown): a move with no effect
  (immunity, Levitate, Wonder Guard) draws neither accuracy nor crit nor the
  roll. The decomp draws accuracy and crit before `typecalc`, but on a no-
  effect move no outcome changes anything. Showdown's gen3 `tryMoveHit`
  draws accuracy before it reports the immunity, which only changes the log
  message (`-miss` or `-immune`); settle that with the log.
- **Accuracy uses Showdown's arithmetic** (decided 2026-10-07; `accuracy()`
  cites it): the accuracy stage, then the evasion stage, as exact fractions
  in floating point; Compound Eyes, Sand Veil and Hustle chained, BrightPowder
  and Lax Incense multiplied in directly; and the chain dropped when the
  value isn't whole (`runEvent`, `sim/battle.ts:933`). Foresight ignores
  only raised evasion. The decomp combines the stages with integer ratios
  (`ACCURACY_STAGE_RATIOS`) and floors each modifier, so the two differ by
  a point or more, and a lot when the chain is dropped (Fire Blast into
  Sand Veil and BrightPowder: 77 against the decomp's 61).
- **Confusion's self-hit is Showdown's** (decided 2026-10-07): a typeless
  40-power physical hit with the roll, ignoring the user's own screens and
  type items, and Focus Band can save it behind a Substitute. The decomp
  computes it as Pound (so Reflect and Silk Scarf apply) and skips Focus Band
  behind a Substitute. Its draws reuse `hits[0]` (`chance.hpp`).
- **Focus Band draws only on a lethal hit**, and not for False Swipe, which
  spares the target anyway. The decomp draws whenever the target holds it.
- **A thawing move (`EFFECT_THAW_HIT`) still draws the 1/5 thaw**, as the
  decomp: the draw decides whether the user stays frozen if a later canceler
  case stops the move.
- **`MOVE_UNAVAILABLE` is `Move::None`** until `src/data/moves.hpp` has a sentinel
  (CLAUDE.md, "Open"). It's what `gLastMoves` and `gLastLandedMoves` record
  for a move that was stopped, which matters to Encore, Disable and Mimic.
- **A lethal hit records only `lethal`** (2026-10-21, `rolls::hit_damage`):
  every (crit, roll) that reaches the cap (the target's HP, or its
  Substitute's) leaves the same state, so the key holds `HitDraws::lethal`
  with the odds of any lethal pair instead of the crit and roll, or nothing
  when every pair is lethal. This changed what the `Actions` key holds
  (153 to 167 B). The same goes for Jump Kick's crash on its user, and for
  a future attack's landing. Non-lethal rolls that floor to equal damage
  still split.
- **A future attack rolls at its landing** (2026-10-21): the use stores the
  damage before the roll and the landing draws it, in `Action::future`.
  Showdown's `getDamage` rolls at the use; the odds are the same, and a miss,
  a fainted target or a KO by every roll then needs no roll.

`faint()` is `cleareffectsonfaint` (status cleared) and `FaintClearSetData`
(main:3264), run where the HP reaches 0, so a residual between the faint and
the replacement sees the cleared state. Spikes (`Cmd_switchineffects`,
bsc:5180) hits every switch-in, faint replacements included; a Pokémon that
faints to them skips its switch-in abilities (`tryfaintmon_spikes`,
bs1:3394), and the frame raises another request in the same phase.

## Protocol

The binary log: plumbing and opcodes in `log.hpp`, the gen3
messages in `protocol.hpp`, the text formatter in `format.hpp`.
Written by groups of messages (see the end of this section).
It comes from a survey of Showdown 0.11.10,
redone on `showdown/` 0.11.11 (see below):
the `add(...)` calls of every gen3 move, ability, item, condition, the gen3
scripts and the sim core, plus 3,000 random gen3 OU games over the OU teams
and 4,000 `gen3randombattle` games, collecting every message shape
(`tools/protocol_survey.cjs`). Random
play reaches what's common, not everything; Lock-On, Uproar, Thrash, Bide,
Future Sight, Hail and Disable come from the static scan only.

Redone (2026-10-08) on `showdown/` (0.11.11, `c046106`), which the
realizability test pins (`TESTING.md`): the static scan, and 3,000
gen3 OU games over the OU teams on each version with the same seed. The
table below follows 0.11.11, which differs from 0.11.10 in:

- Trace prints the old ability: `-ability|P|NEW|Trace|[from] ability:
  Trace|[of] P` (`sim/pokemon.ts` `setAbility`).
- Natural Cure's `-curestatus` adds `[silent]` (gen4 mod).
- Recoil's `-damage` has no `[of]`.
- Hyper Cutter's `-fail|P|unboost` names the stat by ID (`atk`).
- `move`'s `[from]` has a space: `[from] Sleep Talk`, `[from] lockedmove`,
  `[from] Pursuit`.
- `move: Light Screen` in `-sidestart` and `-sideend` (Reflect stays
  `Reflect`); `Mud Sport` in `-start`; `Disable` in `-end`.
- Beat Up's `-activate` is `move: Beat Up|[of] NAME`.

Random play on 0.11.11 also reached `cant|P|move: Imprison|MOVE` and
`-activate|P|ability: Immunity`, both already in the table.

Decided (2026-10-07):

- **Opcodes are libpkmn's `ArgType` values, unchanged** (`protocol.zig`).
  Its gen3 section already reserves `Block`, `Ability`, `EndAbility`,
  `ClearNegativeBoost`, `FormeChange` and `LastNoTarget`. libpkmn has no
  `Unboost`: `Boost` carries a signed amount.
- **Sources are hybrid.** `[from]` is a closed reason per opcode for
  conditions (psn, brn, Spikes, Sandstorm, Hail, Recoil, Leech Seed...),
  plus `Ability`, `Item` and `Move` reasons carrying an ID; `[of]` is an
  `ID` byte where Showdown prints one.
- **Wider fields than libpkmn:** species and moves are 2 bytes (386 species,
  354 moves); abilities and items 1.
- **HP is exact** (hp, max HP, status). The formatter writes both lines of
  the `|split|` triplet, including HP Percentage Mod's public percentage.
- **The formatter needs the nicknames**, which `Battle` doesn't hold: Wish's
  `[wisher]` and Beat Up print them. They're a separate formatter input,
  built from the `helpers::Pokemon` teams (2026-10-08), so `Battle` stays
  free of strings.
- **The buffer can't fill** (2026-10-08): it is sized for the most one
  `update` can write, and debug asserts the bound; release checks nothing,
  as "don't hold it wrong". So `update` never returns `Result::Error`. The
  bound is derived once the opcodes are in (a soak measures the largest
  update meanwhile).
- **`-nothing`, `-notarget` and `-anim` get opcodes** (2026-10-08), after
  libpkmn's values, so the log mirrors Showdown line for line.
- **Dropped, as libpkmn's `FILTER`:** `t:`, `gametype`, `player`,
  `teamsize`, `gen`, `tier`, `rule`, `start`, `upkeep`, `message`,
  `-message`, `-hint`, `bigerror`, `debug`. Not needed in gen3 singles: the
  gen4+ messages (`-center`, `swap`, `-mega`, `-primal`, `-burst`,
  `-zpower`, `-terastallize`, `-candynamax`, `-clearpositiveboost`,
  `typeadd`) and the doubles-only Follow Me, Helping Hand and `[spread]`.

Pressure is written with `addSplit` and no public line, so it's in the
omniscient stream only.

The inventory (reasons in *italics* are new over libpkmn's gen1 and gen2):

| ArgType | Showdown forms (gen3) |
|---|---|
| `Move` | plain, `[from]` *Sleep Talk*, *Pursuit*, *lockedmove*; `LastStill`, `LastMiss`, *`LastNoTarget`* |
| `Switch`, `Drag` | species, level, gender, hp, max HP, status; *`[from] Baton Pass`* |
| `Cant` | slp, frz, par, flinch, recharge, Attract, *Truant*, *Focus Punch*, *Taunt* + move, Disable + move, *Imprison* + move, nopp + move, *Damp* + move + of |
| `Faint`, `Turn`, `Win`, `Tie` | as libpkmn |
| `Damage` | plain, silent, psn, brn, confusion, Leech Seed + of, Recoil, Spikes, Sandstorm, *Hail*, Curse, Nightmare, partiallytrapped + move, *ability* + of (Rough Skin, Liquid Ooze) |
| `Heal` | plain, silent (Rest), drain + of, item (Leftovers, Sitrus...), *ability* + of (Volt Absorb, Water Absorb, Rain Dish), *Wish* + wisher, *Ingrain* |
| `SetHP` | Pain Split, plain and silent |
| `Status` | plain; *`[from] move`* on every sleep (Rest, Spore...); *`[from] ability`* + of (Synchronize, Static, Poison Point, Flame Body, Effect Spore) |
| `CureStatus` | msg, silent, *Natural Cure* (silent), *a thawing move* (Sacred Fire) |
| `CureTeam` | Aromatherapy |
| `Boost` | signed amount, plain or *item* (Salac, Liechi, Petaya...) |
| `SetBoost`, `CopyBoost`, `ClearAllBoost`, `ClearNegativeBoost` | Belly Drum, Psych Up, Haze, White Herb (silent) |
| `Fail` | plain, status, heal, Substitute (+ weak), *unboost [+ stat] from an ability* + of (Clear Body, Hyper Cutter, White Smoke, Keen Eye), *Uproar slp*, *Rest from Insomnia or Vital Spirit* |
| `Miss`, `HitCount`, `Prepare`, `MustRecharge`, `OHKO`, `Crit`, `SuperEffective`, `Resisted` | as libpkmn (Bide's `Miss` has no target) |
| `Immune` | plain, ohko, *`[from] ability`* (Levitate, Wonder Guard, the absorbers, the status immunities, Soundproof, Flash Fire, Sturdy, Oblivious) |
| `Activate` | Protect, Substitute damage, confusion, trapped, Focus Band, Destiny Bond, Endure, Heal Bell, Mimic + move, Pursuit, Spite + move + n, Magnitude + n, Bide, Mist, Safeguard, Lock-On / Mind Reader + of, Attract + of, Trick + of, binding + of, Ingrain, Snatch + of, Grudge + move, Leppa + move, Skill Swap, Beat Up + of name, *ability activations* (Shed Skin, Suction Cups, Sticky Hold, the status cures) |
| `FieldActivate` | Perish Song, Pay Day |
| `Start` | confusion (+ fatigue), Substitute, Leech Seed, Curse + of, Nightmare, Encore, Disable + move, Taunt, Torment, Yawn + of, Attract (+ Cute Charm + of), Foresight, Imprison, Ingrain, Mud Sport, Water Sport, Charge, Uproar (+ upkeep), Bide, *Flash Fire*, *typechange* + type (Color Change, Conversion, Conversion 2, Camouflage), *stockpile1-3*, Future Sight, Doom Desire, perish0-3 (+ silent) |
| `End` | the matching ends, many silent; Leech Seed from Rapid Spin + of; Attract from Oblivious or Mental Herb; partiallytrapped + move; Future Sight and Doom Desire on the target |
| `SingleMove`, `SingleTurn` | Destiny Bond, Grudge, Rage; Protect, Endure, Focus Punch, Snatch |
| `SideStart`, `SideEnd` | Reflect, Light Screen, Spikes, Safeguard (+ persistent), Mist; Spikes from Rapid Spin + of |
| `Weather` | type, none, upkeep, *`[from] ability`* + of (Sand Stream, Drizzle, Drought) |
| *`Ability`* | Intimidate and Speed Boost (boost), *Trace* and *Role Play* + old ability + of, *Pressure* (silent, split) |
| `Item`, `EndItem` | `-item`: Thief / Covet + of, Trick; `-enditem`: eat (every berry), plain (White Herb, Mental Herb, Berry Juice), Knock Off + of, Thief + of (silent; Covet prints none), Trick (silent) |
| *`FormeChange`*, `Transform` | Castform's Forecast; Transform |

Decided (2026-10-08):

- **`Nothing`, `NoTarget` and `Anim` are 73, 74 and 75**, after libpkmn's
  last value (`Terastallize`), so every libpkmn value stays.
- **The formatter is a library header** (`format.hpp`) that `update`
  never includes. It takes a `Names` (player names, nicknames, shininess
  by party position) and writes Showdown's raw lines, `|split|` triplets
  included. Display names come from Showdown's dex
  (`tools/display_names.cjs` writes `data/display.hpp`).
- **`check_showdown` compares the raw lines, both lines of each triplet**,
  after Showdown's `FILTER`; not channel -1 as libpkmn, which drops the
  public line. The state guard stays alongside.
- **The buffer bound is a placeholder**, `MAX_LOG_SIZE` = 2048, until it is
  derived (debug asserts every write; release checks nothing). It is not a
  proven bound: derive it once every message is written.
- **By groups**, each checked against the traces as it lands:
  `check_showdown` compares only the message types written so far
  (`LOGGED`), on both sides. Group 1, the frame: `move` (with `[from]`,
  `[still]`, `[miss]`), `switch`, `drag`, `cant`, `faint`, `turn`, `win`,
  `tie`, `-damage`, `-heal`, `-miss`. Group 2, the hit results
  (2026-10-08): `-fail`, `-miss`, `-immune`, the move line's `[still]`
  and `[miss]`, and the `-activate` of what stops a move (Protect,
  Safeguard, Mist, Suction Cups, Ingrain). Group 3a, the rest of the hit
  results (2026-10-08): `-crit`, `-supereffective`, `-resisted` (formula
  damage only, before the damage line), `-hitcount` (before the queued
  faint), `-prepare` and `-anim` (Solar Beam). 3b (done 2026-10-08): a
  sleep names its move and the other statuses their ability (Showdown's
  gen3 `slp` and conditions); every cure is `[msg]` but Heal Bell's
  `[silent]` (benched Pokémon print `p1: NAME`) and Natural Cure's. 3c
  (done 2026-10-08), boosts, as Showdown's `Battle.boost`
  (`sim/battle.ts:2024`): one line per stat in the move's key order; a
  primary boost or drop at the cap prints its 0 (`-boost|P|atk|0`,
  `-unboost|P|def|0`) and no `-fail`, as does Intimidate and Rage, while
  self and secondary boosts print nothing at the cap; a 0 never has
  `[from] item`. Curse's order is spe, atk, def (the base `onTryHit`
  overwrites gen4's `self`). Belly Drum's `-damage` comes before its
  `-setboost`. 3d (done 2026-10-08), volatiles (`-start`, `-end`, the
  other `-activate`s, `-singleturn`, `-singlemove`, `-sethp`,
  `-fieldactivate`), every `-activate` now compared: a Substitute logs
  `-activate|[damage]` or `-end` at the hit (the volatile stays until
  moveend); Endure, then False Swipe (silent), then Focus Band, as
  Showdown's onDamage priorities; Destiny Bond's `-activate` and Flash
  Fire's silent `-end` (the ability's End event) follow the fainted
  Pokémon's `faint` line, so they ride in the `FaintQueue`, and Flash
  Fire's `-end` also comes at every switch-out; a cured sleep ends a
  Nightmare with `-end|[silent]` after the `-curestatus` (`wake()`);
  Perish Song prints `perish3|[silent]` per Pokémon, then
  `-fieldactivate`, and each count prints the timer before its step;
  Oblivious and Mental Herb
  print Attract's silent `-end`, then their own; Beat Up prints no
  `-activate` (Beat Up Nicknames Mod is on). Forms whose moves aren't
  written (binding, Lock-On, Spite, Snatch, Uproar...) have reasons but
  no site; Spite and Skill Swap have neither yet. Each subgroup writes every form of its
  types, those from items and abilities too (Synchronize's `-status`,
  Salac's `-boost`); the `-ability` and `-enditem` lines beside them wait
  for group 5. Their reason enums cover the whole gen3 inventory above.
  Group 4 (done 2026-10-08), side conditions and weather (`-sidestart`,
  `-sideend`, `-weather`): the end-of-turn order is Showdown's gen4
  orders, Reflect, Light Screen, Mist, Safeguard (both sides each), then
  Wish, then the weather (upkeep or `none`, the upkeep printed even under
  Cloud Nine), then the per-Pokémon residuals. A side condition's handler
  has no Speed, so two sides' screens running out in one turn print in
  random order (an unrecorded coin). Brick Break prints Reflect's
  `-sideend`, then Light Screen's, before its damage; Rapid Spin's Spikes
  `-sideend` comes between Leech Seed's and the binding's `-end`. Group 5
  (done 2026-10-08), items and abilities (`-item`, `-enditem`,
  `-ability`): an item used up prints its `-enditem` before its effect's
  lines (`use_item`: eatItem's `[eat]` for a berry, else useItem's plain
  form); Knock Off's and Thief's lines come at the hit, before the contact
  abilities (gen < 5 runs AfterHit before DamagingHit,
  `sim/battle-actions.ts:1113`), and Covet prints only its `-item`;
  Intimidate's `-ability` is skipped with the boost into a Substitute or a
  fainted foe; Speed Boost's comes with its boost, so not at +6; Pressure's
  is a `|split|` with an empty public line, and a traced Pressure prints
  none (gen3's setAbility doesn't start it). Air Lock and Cloud Nine print
  nothing in gen3 (gen4 mod). Trick's and Role Play's forms have reasons but
  no site (the moves aren't written); `-formechange` and `-transform`
  (Castform, Ditto; on no team) have neither.
- **Group 2 follows Showdown's gen3 `tryMoveHit` order** (`try_hit` in
  `mechanics.hpp`): a semi-invulnerable miss, the accuracy roll, TryHit
  (Protect, Soundproof, the absorbing abilities), a natural immunity, then
  each move's own checks (TryPrimaryHit's Substitute, `setStatus`,
  `addVolatile`...). The decomp checks most failures before its accuracy
  roll; every reordered check fails the move the same way, so only the
  messages and the draws change. A roll whose outcome changes only the log
  (into Protect or Soundproof, or before a failure) is drawn and not
  recorded (CLAUDE.md, "Chance").
- **`-anim` is the last move line** (Battle.addMove): `[miss]` amends it
  and `[still]` removes it (Battle.attrLastMove, `sim/battle.ts:3121`), in
  the formatter. `-mustrecharge` has no site yet: no recharge move is on
  `teams/ou/`. `-notarget` is Helping Hand's, with no ally to target. `-ohko` follows a lethal OHKO hit, or any OHKO hit on
  a Substitute (gen4 Substitute's onTryPrimaryHit).
- **Faints are queued** (`FaintQueue` in the log, not in `Battle`): Showdown
  queues a faint at 0 HP and writes the queue at fixed points (after each
  action, residual handler, arrival and multi-hit loop), so recoil and
  drain lines come between a KO and its `faint`.
- **The gen3 messages live in `hoenn::protocol`**, with the plumbing, as
  libpkmn's single protocol namespace.
- `update` ends its messages with `None` (0), libpkmn's terminator.

Decided (2026-10-20):

- **A player's view of the bytes** (`protocol::View`, `view.hpp`) deletes
  everything the log shows and Showdown's stream for that player doesn't:
  the foe's exact HP, the sleep counter and Rest flag, the foe's Pressure,
  and the foe's party positions. The ident is libpkmn's original party
  position (libpkmn `docs/PROTOCOL.md`, "PokemonIdent"), which Showdown
  never shows, so the view numbers the foe's Pokémon in order of first
  appearance, as a model built from the text holds them. Every other field
  the formatter reads is printed, so public. Formatting the view with names
  permuted the same way (`Stream::View`) gives Showdown's split filter of
  the omniscient text; `check_view` tests it. Side relabelling (the viewer
  always p1) is the driver's.
- **The spectators' view** (`protocol::Spectator`, 2026-10-22) is one
  player's view of the other's: Showdown's channel 0, every triplet's
  shared line. Each `View` touches only the other side, so the order
  doesn't matter; `check_view` tests that too.

## Teams

`helpers.hpp` builds a battle from `helpers::Pokemon` sets, as libpkmn's
`helpers.zig`: every move gets 3 PP Ups, and the battle starts at turn 0 with
no active Pokémon ("Turn frame"). `showdown.hpp` parses
Showdown exports into those sets; its header lists where it departs from
Showdown's importer (a mismatched `Hidden Power [T]` is an error; Return with
Frustration gets 255 happiness; Cute Charm is male when it can be).
`tools/check_teams.cpp` runs it over `teams/ou/`.

## Battle layout decisions

Settled 2026-10-07 and implemented in `data.hpp`, which cites them by number.
Paths: bsc = `src/battle_script_commands.c`, util = `src/battle_util.c`,
main = `src/battle_main.c`.

1. **The toxic counter is on `ActivePokemon`**, so `Status` is 1 byte. It
   restarts at 0 on every switch-in, Baton Pass included: the residual
   (util:1533) bumps it only in `gBattleMons`, nothing writes it back while
   it's raised, and `Cmd_switchindataupdate` (bsc:4621) reloads status1 from
   the party. The sleep counter does persist: the canceler writes it back
   (util:2259).
2. **No per-turn struct in `Battle`.** Per-turn state (`ProtectStruct`,
   `SpecialStatus`) is a local in `update`, zeroed at turn start and on
   resuming from a request.
   - `SpecialStatus` is per action (`SpecialStatusesClear`, main:4894).
     `ProtectStruct` is per turn (`TurnValuesCleanUp(FALSE)`, main:4871); its
     cross-action fields (`protected`, `endured`, Magic Coat, Snatch, the
     damage taken that Counter, Mirror Coat, Focus Punch and Revenge read) are
     set and read within one actions phase.
   - Nothing crosses a request: faint phases resume at end of turn or later,
     which reads none of it (main:4866 clears `protected`/`endured`). A Baton
     Pass that moves first resumes before the foe acts, and the passer's own
     flags can't matter because its action was Baton Pass.
   - This rests on requests only at action boundaries or Baton Pass, and on
     residuals never reading per-turn state. Guard it: read per-turn fields
     through an accessor that asserts the actions phase.
   - **Reached through `MoveState::turn`** (decided 2026-10-07): a pointer
     to a `Turn` local of `actions()`, which starts empty each turn and on
     the Baton Pass resume; `use_move` sets it and readers assert it isn't
     null. The pointer lives only on the stack, so `Battle` stays
     pointer-free, and the residuals have no `MoveState` to reach it with.
     It holds Protect and Endure so far.
   - **Exception found** (field check, 2026-10-07): residuals do read it.
     `ENDTURN_UPROAR` and `ENDTURN_THRASH` (util:1648, 1674) call
     `WasUnableToUseMove` (util:890), the OR of nine immobility bits set in
     the actions. After a `FaintAfterAction` request the residuals run in a
     later `update`, so a stack struct would lose them. Most of the nine
     don't matter, because the canceler already ran `CancelMultiTurnMoves`
     and the lock is gone. `targetNotAffected` does (Thrash or Uproar into a
     Ghost type, then the Ghost faints itself, e.g. by Explosion). The
     smallest fix is one "unable to use its move" bit per player in
     `Request`. **Settled** (step 5 group 4, 2026-10-09): Thrash and Uproar
     follow Showdown (`lockedmove`, `data/conditions.ts:253`; `uproar`),
     whose locks run out by their own durations and never read it, so no
     bit is kept. Every other field is set and read within one actions
     phase, or cleared before the residuals (`protected` and `endured`,
     `TurnValuesCleanUp(TRUE)`, main:3959). `noValidMoves` is
     `Request::move` = Struggle.
   - `FaintClearSetData` (main:3264) clears most `ProtectStruct` flags but not
     the damage taken; a normal switch and Baton Pass clear none.
3. **`request`** holds the phase, the next action (Baton Pass only), who moves
   first and each player's queued action resolved at selection ("Turn
   frame").
4. **Switch-in works like libpkmn's:** copy stats, species, moves, ability and
   types from `pokemon[]` and zero the rest of `ActivePokemon`. This matches
   the decomp's rule that switch-in discards Transform, Trace, Skill Swap,
   Forecast and Color Change.
5. **Baton Pass's switch-in is its own function:** the normal switch-in, then
   restore what passes (`baton_pass_in`).
6. **Volatiles are named fields** in one `Volatiles` struct: a `bool` per
   flag, a `uint8_t` per small counter, each named after and citing its decomp
   constant. About 274 decomp uses touch one flag; the 8 that combine flags
   become helpers (`trapped()`, `semi_invulnerable()`, Baton Pass's
   keep-list). Packing will make them bitfields, so never take a pointer or
   reference to a member. Before packing, check whether search reads raw
   `Battle` bytes (unverified).
7. **Types live on `Pokemon`** and switch-in copies them. Equivalent to the
   decomp, which recomputes them from the species.
8. **Knock Off sets the item to the sentinel `Item::KnockedOff`** (inert, no
   hold effect); there's no Knocked Off flag. Non-link path; see "Mechanics"
   above. It comes after `Other` in `src/data/items.hpp`. Only identity
   checks (`item == Item::None`) must handle it; Thief, Covet and Trick
   will need to refuse a knocked-off Pokémon. Open: grep the decomp's
   `ITEM_NONE` comparisons to confirm nothing else cares (Recycle?).
9. **Battler-ID fields become victim-side state.** In singles the other
   battler is always the foe, so clearing at the right foe switch replaces
   the IDs:
   | Decomp | Becomes | Cleared when the foe… |
   |---|---|---|
   | `STATUS2_INFATUATED_WITH(b)` | `bool infatuated` | switches out, Baton Pass included (main:3197) |
   | `STATUS3_LEECHSEED_BATTLER` | dropped; the drain heals the foe's active Pokémon if alive (util:1507) | never |
   | `battlerPreventingEscape` | dropped; `escape_prevention` stays | switches out normally, not on Baton Pass (main:3164) |
   | `wrappedBy` | dropped; `wrapped_move` kept for the log | switches out, Baton Pass included (main:3199) |
   | `battlerWithSureHit` | dropped; `lock_on` on the user, as Showdown (below) | — |
   - Lock-On and Mind Reader follow Showdown's lockon: on the user, with
     the turns left (2, stepped at the user's residual) and the target's
     party position. It applies while that Pokémon is the foe in: no
     accuracy roll and no semi-invulnerability (bsc:1056 skips the roll,
     OHKO moves included with the level check kept, bsc:7523–7533). A new
     one fails while it runs, even after the target left. Baton Pass passes
     it with its count; the decomp keeps it on the target and resets it to
     2 (main:3166, 3178–3186).
10. **Write-only fields are dropped:** every `*StartValue` in `DisableStruct`
    and `tauntTimer2` (only written: bsc:8008, 8040, 8524, 8550, 9104, 9161,
    main:3215). `Durations` does that bookkeeping.
11. **Multi-turn state keeps the decomp's fields, with Showdown's
    meanings:** `multiple_turns` (the lock) with `locked_move`, and
    separate Thrash, Uproar, Bide, Rollout and Fury Cutter counters
    holding Showdown's `trueDuration`, durations, `hitCount` and multiplier
    (field map). `ActivePokemon::renewed` stands for Showdown's duration 2
    of the volatiles a turn's move renews (lockedmove, rollout,
    furycutter). A move the canceler stops loses only a two-turn charge
    and Bide (`abort_move`, Showdown's MoveAborted); the other locks run
    out at the residuals (`lock_residuals`, the Uproar stage). The decomp's
    `CancelMultiTurnMoves` ends them all; it is gone. Recharge stays
    separate, but its timer also ends a two-turn move's charge.
12. **Hidden counters** are the ones in `Durations`: confusion, Disable,
    Encore, binding, and Uproar or Thrash (`Durations::attacking`; the locked
    move says which). Everything else is fixed-length and public.
13. **Also dropped:** `STATUS2_UNUSED`, the `DisableStruct` fillers,
    `transformedMonPersonality`, `truantSwitchInHack` (Battle Arena only, so
    switch-in always sets `truant`, bsc:5258), and `STATUS3_INTIMIDATE_POKES`
    and `STATUS3_TRACE`, which are queued events: Intimidate and Trace go
    directly in the switch-in code. Still to trace: where that pass fires
    after Baton Pass or a Roar drag-in, and whether Trace can stay pending
    across turns with no valid target.
14. **`first_turn` (`isFirstTurn`) is a `uint8_t`, 0–2.** Switch-in sets 2
    (main:3220) and end of turn decrements it (main:4875). Speed Boost needs
    it not 2 (util:2629). Fake Out (bsc:6790 reads it) follows Showdown's
    `activeMoveActions` instead (`move_actions`): an action a faint
    cancelled doesn't use up the first turn.
15. **`bide_damage` and `future_sight_damage` stay `uint32_t`** (settled
    2026-10-19). No bound is derived; Bide's sum saturates. Packing leaves
    them as they are.

Packing comes last: natural alignment for now (`Battle` is 824 B against a
~700 B target). Revisit padding and `has_unique_object_representations_v`
once the layout is settled.

## `ActivePokemon` field map

Every per-battler field the decomp clears on switch-in (`SwitchInClearSetData`,
main:3152; `FaintClearSetData`, main:3264) and where it goes. This is the
checklist `ActivePokemon` is written from; the decisions behind it are above
(2 and 6–14). **BP**: Baton Pass keeps it (main:3173–3217).
**Hidden**: the true value is hidden information, observed through `Durations`.
Counter ranges are the values the field can hold; "set" is what infliction
writes.

### Copied from `Pokemon` at switch-in

`BattlePokemon` (`include/pokemon.h:260`). Transform, Trace, Skill Swap,
Forecast, Color Change, Conversion and Mimic change these; switch-in discards
the changes.

| Decomp | Field | Type | Notes |
|---|---|---|---|
| `attack`…`spDefense` | `stats` | 5 × u16 | Transform copies the target's |
| `species` | `species` | `Species` | Transform |
| `types[2]` | `types` | 2 × `Type` | Forecast, Color Change, Conversion (1 and 2), Transform |
| `ability` | `ability` | `Ability` | Trace, Skill Swap, Transform |
| `moves[4]`, `pp[4]` | `moves` | 4 × {`Move`, u8 pp} | Transform (5 PP each), Mimic. PP is written back to the party unless transformed or the slot is Mimic's (`Cmd_ppreduce`, `MOVE_IS_PERMANENT`) |
| `statStages[1..7]` | `boosts` | 7 × i8, −6..6 | **BP**. Index 0 (HP) is unused |

### status2

| Decomp | Field | Type | BP | Hidden | Notes |
|---|---|---|---|---|---|
| `CONFUSION` (3 bits) | `confusion` | u8 0–5 | **BP** | yes | set 2–5 (bsc:2541); steps at the canceler (util:2145) |
| `FLINCHED` | `flinched` | bool | | | cleared at turn start |
| `UPROAR` (3 bits) | `uproar` | u8 0–5 | | yes | Showdown's duration: set 2–5 (gen3 mod; bsc:2573 agrees); steps at end of turn |
| `UNUSED` | — | | | | dropped |
| `BIDE` (2 bits) | `bide` | u8 0–3 | | | Showdown's duration: set 3, −1 per residual; releases at 1 (bsc:7126 sets 2) |
| `LOCK_CONFUSE` (2 bits) | `thrash` | u8 0–3 | | yes | Showdown's lockedmove `trueDuration`: set 2–3; steps at a residual that renews the lock |
| `MULTIPLETURNS` | `multiple_turns` | bool | | | the lock, with `locked_move` (decision 11) |
| `WRAPPED` (3 bits) | `wrapped` | u8 0–6 | | yes | set 3–6 (bsc:2618); steps at end of turn (util:1591). Wrapper is the foe |
| | `wrapper_gone` | bool | | | Showdown's partiallytrapped whose source has left: no longer traps, ends at the victim's next residual (`Stage::Bind`) |
| `INFATUATION` (4 bits) | `infatuated` | bool | | | cleared on any foe switch-out |
| `FOCUS_ENERGY` | `focus_energy` | bool | **BP** | | |
| `TRANSFORMED` | `transformed` | bool | | | blocks the PP write-back and Sketch |
| `RECHARGE` | `recharge` | bool | | | with `recharge_timer` |
| `RAGE` | `rage` | bool | | | cleared at the user's next move attempt, as Showdown (the decomp: turn start unless Rage was chosen) |
| `SUBSTITUTE` | `substitute` | bool | **BP** | | with `substitute_hp` |
| `DESTINY_BOND` | `destiny_bond` | bool | | | |
| `ESCAPE_PREVENTION` | `escape_prevention` | bool | **BP** | | cleared when the foe switches out normally, not on its Baton Pass |
| `NIGHTMARE` | `nightmare` | bool | | | |
| `CURSED` | `cursed` | bool | **BP** | | |
| `FORESIGHT` | `foresight` | bool | | | |
| `DEFENSE_CURL` | `defense_curl` | bool | | | |
| `TORMENT` | `torment` | bool | | | |

### status3

| Decomp | Field | Type | BP | Notes |
|---|---|---|---|---|
| `LEECHSEED_BATTLER` | — | | | dropped: the drain heals the foe's active Pokémon (util:1507) |
| `LEECHSEED` | `leech_seed` | bool | **BP** | |
| `ALWAYS_HITS` (2 bits) | `lock_on` | u8: turns 0–2, target position 1–6 | **BP** | on the user, as Showdown (decision 9); set 2 (bsc:8137), steps at the user's end of turn (util:1736) |
| `PERISH_SONG` | `perish_song` | bool | **BP** | with `perish_song_timer` |
| `ON_AIR`, `UNDERGROUND`, `UNDERWATER` | `on_air`, `underground`, `underwater` | bool | | `semi_invulnerable()` |
| `MINIMIZED` | `minimized` | bool | | |
| `CHARGED_UP` | `charged_up` | bool | | until the user's next move other than Charge (Showdown) |
| `ROOTED` | `rooted` | bool | **BP** | Ingrain |
| `YAWN` (2 bits) | `yawn` | u8 0–2 | | set 2 (bsc:9361); steps at end of turn (util:1752) |
| `IMPRISONED_OTHERS` | `imprisoned_others` | bool | | |
| `GRUDGE` | `grudge` | bool | | |
| `CANT_SCORE_A_CRIT` | — | | | dropped: read (bsc:1280) but never set |
| `MUDSPORT`, `WATERSPORT` | `mud_sport`, `water_sport` | bool | **BP** | on the user, not the field, in Gen III |
| `INTIMIDATE_POKES`, `TRACE` | — | | | dropped: implemented directly in switch-in |

### `DisableStruct` (`include/battle.h:70`)

| Decomp | Field | Type | BP | Hidden | Notes |
|---|---|---|---|---|---|
| `transformedMonPersonality` | — | | | | dropped |
| `disabledMove`, `disableTimer` | `disabled_move`, `disable_timer` | `Move`, u8 0–6 | | yes | set 2–5, +1 if the target already moved (Showdown's gen3; bsc:8007 has no +1) |
| `encoredMove`, `encoredMovePos`, `encoreTimer` | `encored_move`, `encored_slot`, `encore_timer` | `Move`, u8 0–3, u8 0–6 | | yes | set 3–6 (bsc:8039) |
| `protectUses` | `protect_uses` | u8 | | | ++ on success (bsc:6526); reset when `stall` runs out (Showdown), where the decomp resets unless the last resulting move was Protect, Detect or Endure (bsc:6506). Unbounded in the decomp: saturate, and clamp the rate index |
| — | `stall` | u8 | | | Showdown's stall duration, 0-2: 2 on a success, counted down after the residuals; at 0 the chain ends |
| `stockpileCounter` | `stockpile` | u8 0–3 | | | |
| `substituteHP` | `substitute_hp` | u8 | **BP** | | max HP / 4 ≤ 178, fits |
| `perishSongTimer` | `perish_song_timer` | u8 0–3 | **BP** | | |
| `furyCutterCounter` | `fury_cutter` | u8 0–5 | | | Showdown's multiplier as log2 + 1; renewed by `renewed` |
| `rolloutTimer` | `rollout` | u8 0–5 | | | Showdown's `hitCount`, counting up (bsc:8549 counts down from 5) |
| `chargeTimer` | `charge_timer` | u8 0–2 | | | unused: Charge follows Showdown, ending after the user's next move (bsc:9103 sets 2) |
| `tauntTimer` | `taunt` | u8 0–2 | | | set 2 (bsc:9160) |
| `battlerPreventingEscape`, `battlerWithSureHit` | — | | | | dropped (decision 9) |
| `isFirstTurn` | `first_turn` | u8 0–2 | | | decision 14 |
| (Showdown's `activeMoveActions`) | `move_actions` | u8 0–2 | | | saturating; Fake Out (decision 14) |
| `truantCounter` | `truant` | bool | | | switch-in sets 1; flips at end of turn |
| `truantSwitchInHack` | — | | | | dropped (Battle Arena only) |
| — | `sleep_skipped` | u8 0–4 | | | Showdown's gen3 `skippedTime`: Sleep Talk/Snore attempts since the last asleep turn that did nothing; given back to the sleep counter on switch-out (the cartridge writes status1 to the party only at util:2259) |
| `mimickedMoves` | `mimicked` | u4 mask | | | slots Mimic replaced; blocks the PP write-back |
| `rechargeTimer` | `recharge_timer` | u8 0–2 | | | set 2 (bsc:2730); also set 2 by a two-turn move's charge, which ends with it (Showdown's twoturnmove duration; the decomp has no timer) |
| every `*StartValue`, `tauntTimer2`, fillers | — | | | | dropped (write-only) |

### Other per-battler globals cleared on switch-in

| Decomp | Field | Type | Read by |
|---|---|---|---|
| `gLockedMoves` | `locked_move` | `Move` | forced selection (util:109) |
| `gBattleStruct->choicedMove` | `choice_move` | `Move` | Choice Band lock (util:981–1119) |
| `gBattleStruct->wrappedMove` | `wrapped_move` | `Move` | the wrap residual's message (util:1595–1612), so the log |
| `gLastMoves` | `last_move` | `Move` | Disable, Encore, Spite, Mimic (bsc:7848–8337) |
| (Showdown's `lastMoveUsed`) | `last_used_move` | `Move` | Conversion 2, as Showdown |
| (its type as used) | `last_used_type` | `Type` | Conversion 2 |
| `gLastHitBy` | — | | dropped: Conversion 2 reads it only to find the foe (bsc:8082) |
| `gLastResultingMoves` | `last_resulting_move` | `Move` | Protect's consecutive-use reset (bsc:6506) |
| `gLastPrintedMoves` | `last_printed_move` | `Move` | Sketch (bsc:8148) |
| `lastTakenMove`, `lastTakenMoveFrom` | `last_taken_move` | `Move` | Mirror Move (bsc:6640–6662). `lastTakenMoveFrom` is per attacker; in singles it collapses *(verify they're always set together)* |
| `gBideDmg`, `gBideTarget` | `bide_damage` | u32 | Bide (util:2218, bsc:1912). The target is the foe. Accumulates raw damage, saturating (decision 15) |
| `RESOURCE_FLAG_FLASH_FIRE` | `flash_fire` | bool | util:2692–2700 |

### Open points from the map

- **`MOVE_UNAVAILABLE` (0xFFFF)** is a distinct value in the last-move
  fields: Mirror Move, Sketch and Conversion 2 treat it like `MOVE_NONE`, but
  it's written in different cases. `Move` needs a sentinel for it, like
  `Item::KnockedOff`, or each field needs to show it never matters.
- **Five "last move" fields.** They differ in when they're written
  (attempted, landed, resulting, printed, taken). Keep all five until each
  writer is traced; merging any two needs proof that they never differ.
- *(verify against Showdown)* A Truant replacement after a residual faint
  arrives after the end-of-turn abilities, so its counter is still 1 and it
  loafs on its first turn.
