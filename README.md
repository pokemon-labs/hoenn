# Hoenn

Hoenn is a header-only C++20 engine for Gen III singles battles (Smogon ADV
OU), written for MCTS and policy-gradient RL. Everything lives in
`namespace hoenn`. The design follows libpkmn: the whole battle is a small
POD `Battle`, you advance it with `update`, and logging, chance tracking and
damage overrides are optional template parameters (Log, Chance and Calc).

The mechanics, the turn order and the log protocol are documented in
[`docs/ENGINE.md`](docs/ENGINE.md), and the test suites in
[`docs/TESTING.md`](docs/TESTING.md).

## Sim Accuracy

The engine aims to match Pokémon Showdown's gen3 implementation (the pinned
version is in [`tools/criteria.toml`](tools/criteria.toml), under
`[showdown]`) rather than the cartridge.

We can't compare games against Showdown one to one, because the engine
doesn't reproduce Showdown's RNG. It uses its own generator (SplitMix64) and
draws random numbers wherever it's convenient, so the same seed produces a
different game and Showdown's draws can't be replayed into it.

Instead we test *realizability*: could the engine have produced the game
Showdown did? `tools/showdown_traces.cjs` plays seeded Showdown games with
random choices and records every step: the teams, both players' available
choices, the pair they picked, and the log. `check_showdown` then replays
each step and checks three things:

- Our `choices()` returns exactly the same choice set as Showdown.
- At least one of our random branches reproduces Showdown's log line for
  line. We rerun the step from fresh seeds, resampling the hidden counters
  with `randomize_hidden`, until one matches or we hit 1M tries. Showdown's
  hidden counters are never copied in, so this only passes if our
  `Durations` tracking makes the true values reachable.
- A state comparison (HP, status, PP, items, boosts, volatiles, side
  conditions, weather and so on) agrees, since the log doesn't show
  everything.

A game counts as matched when every step reproduces through to the end.

Other suites cover what realizability can't:

- `soak` runs random self-play under ASan, UBSan and asserts, and must
  finish with no crashes or invalid states.
- `check_chance` checks that `(c1, c2, Actions)` determines the result with
  no collisions, and that each branch's reported probability agrees with
  how often it actually happens.
- `check_damage`, `check_frame` and the other fast suites run under ctest.

There are three test environments: real OU teams, random OU-legal teams,
and fully random teams. Their pass bars are in
[`tools/criteria.toml`](tools/criteria.toml) and explained in
[`docs/TESTING.md`](docs/TESTING.md) under "Pass criteria". In short, at
least 99.9% of 10,000 OU games must match (99.8% for the two random
environments), with no safety failures, choice-set mismatches or chance-key
collisions anywhere. Run an environment against its bars with
`python3 tools/run_env.py ENV`.

There are limits to what a pass tells you (see "Known test limitations" in
`docs/TESTING.md`):

- Realizability only shows that Showdown's outcome is one of ours, not that
  it's equally likely. A secondary effect that fires 30% of the time when it
  should be 10% would still pass. `check_chance` tests our probabilities
  against our own RNG, not against Showdown's.
- It only checks one direction. If we can produce a branch that Showdown
  can't, nothing catches it.
- Games whose first unmatched step is a multi-hit move are left out of the
  count, because random search can't tell a rare branch apart from a bug.
- Random choices don't reach every state a search would steer into, and the
  known engine gaps (listed under "Known engine gaps" in `docs/TESTING.md`)
  fall within the allowed failures.

## Building a battle

Setup goes through `src/helpers.hpp` and `src/showdown.hpp`. These are only
for setup, so they're allowed to allocate and return error strings.

```cpp
#include "helpers.hpp"
#include "showdown.hpp"
#include "mechanics.hpp"   // update, choices
#include "options.hpp"
#include "chance.hpp"
#include "calc.hpp"
#include "log.hpp"

using namespace hoenn;

// Fill in helpers::Pokemon by hand...
helpers::Pokemon skarm{
    .species = Species::Skarmory,
    .moves = {Move::Spikes, Move::Protect, Move::Roar, Move::Toxic},
    .item = Item::Leftovers, .ability = Ability::KeenEye,
    .nature = Nature::Careful, .evs = {252, 0, 8, 0, 0, 248},  // HP Atk Def Spe SpA SpD
    .gender = Gender::Female};

// ...or parse Showdown exports (rng picks a gender when nothing else decides it).
RNG parse_rng{0};
auto teams = showdown::parse_teams(text, parse_rng);  // vector<Parsed<Team>>: .value or .error

std::optional<Battle> b = helpers::init_battle(seed, team1, team2);  // spans of helpers::Pokemon
```

A few things to know:

- Moves go at the front of the array, between one and four of them. Use the
  generic `Move::HiddenPower`; its type is worked out from the IVs. Every
  move gets 3 PP Ups.
- Only the engine's own limits are checked, not team legality. If something
  is rejected, `init_*` returns `nullopt`, and `check_pokemon` and
  `check_side` will tell you why.
- The new battle is at turn 0 with no active Pokémon. `Battle` is 824 bytes,
  trivially copyable and pointer-free, so a search branches with a plain
  `Battle copy = battle;`.

## Running it

The two entry points are in `src/mechanics.hpp`:

```cpp
Result  update(Battle&, Choice c1, Choice c2, Options&&);
uint8_t choices(const Battle&, Player, Choice::Type request,
                std::span<Choice, MAX_CHOICES> out);  // returns 1..9
```

A basic game loop, as in `tools/soak.cpp`:

```cpp
Options<protocol::NullLog, NullChance, NullCalc> o{};        // search: all null
Result r = update(battle, Choice::pass(), Choice::pass(), o); // turn 0: send out leads, play turn 1
std::array<Choice, MAX_CHOICES> c1{}, c2{};
while (!r.over()) {
  uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
  uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
  r = update(battle, c1[pick1], c2[pick2], o);
}
// r.type() is Win or Lose (from P1's point of view), or Tie
```

`Choice` is a single byte. It's one of `Choice::pass()`, `Choice::move(slot)`
or `Choice::swap(SLOT)`. For a move, `slot` is the move slot 1–4, or 0 for
Struggle or a forced continuation such as charging, a rampage or Bide. For
a switch, `SLOT` is a party slot from 2 to 6.

Party slots depend on the current order, the same way as in libpkmn.
`Side::pokemon[6]` itself never moves; `side.get(s)` returns
`pokemon[order[s - 1] - 1]`, slot 1 is the active Pokémon, and a switch
swaps `order[0]` with `order[s - 1]`.

`Result` is also a single byte. `type()` is `None`, `Win`, `Lose` or `Tie`,
and `p1()` and `p2()` give the kind of `Choice` each player has to make
next. A normal turn asks for `Move` from both. After a faint, the side that
fainted gets `Switch` and the other gets `Pass`. Baton Pass asks for a
`Switch` in the middle of the turn, and the next `update` picks the same
turn back up (`battle.request` records where).

The caller is responsible for valid input: only pass choices that came from
`choices()`, and only battles where `valid()` is true. Debug builds assert
both; release builds check neither, and bad input is undefined behavior.
`update` doesn't return `Error` for bad input.

Battles are capped at 1000 turns. Once `turn` reaches 1001
(`Battle::MAX_TURNS`), `update` returns `Tie`, which matches Showdown.

## Options

`Options<Log, Chance, Calc>` is defined in `src/options.hpp`. Each
parameter has a null version that's empty and compiles away entirely.
Build one with `Options{log, chance, calc}` or
`hoenn::options(log, chance, calc)`.

| Option | Real | Null | Provides |
|---|---|---|---|
| Log | `protocol::FixedLog` (`protocol::fixed_log(writer)`) | `protocol::NullLog`, `protocol::null_log` | The binary protocol stream |
| Chance | `Chance<>` (`Rational<double>`) | `NullChance` | `probability`, `actions`, `durations` |
| Calc | `Calc{.overrides = Actions{...}}` | `NullCalc` | Forcing specific chance branches |

### Log

See `src/log.hpp`, `src/protocol.hpp` and `src/format.hpp`. You own the
buffer; the `Writer` just points into it.

```cpp
std::vector<uint8_t> buf(protocol::MAX_LOG_SIZE);
protocol::Writer w{buf.data(), buf.size(), 0};
Options<protocol::FixedLog, NullChance, NullCalc> o{protocol::fixed_log(w), {}, {}};
update(battle, c1, c2, o);
// buf[0, w.pos) now holds this update's opcodes, ending in ArgType::None.
// Call w.reset() before the next update.
```

To get Showdown's text protocol, use
`format::Formatter(names, exact_hp).format(bytes)`. The output matches
Showdown line for line, including `|split|` triplets. The formatter
allocates, so keep it out of search. `format::Names` holds the player names
and each Pokémon's nickname and shininess by party position;
`tools/check_showdown.cpp` has an example of filling it in.

### Chance

See `src/chance.hpp`. Keep one `Chance` per battle and copy it along with
the battle. Each `update` resets `probability` and `actions`, but
`durations` carries over between turns.

- `actions` (an `Actions`) records which random branch was taken. Together
  with the battle and both choices, it fully determines the `Result` and all
  public state, which makes it usable as the key for MCTS chance nodes.
- `randomize_hidden(battle, chance.durations)` resamples the true hidden
  counters (sleep, confusion, Disable, Encore, binding, Uproar, Thrash).
  Call it on a copy of the battle before each search iteration.
- `mask_hidden(battle)` clears the state that `Actions` doesn't determine,
  so two battles can be compared.

### Calc

See `src/calc.hpp`. Setting a field of `overrides` to a nonzero value forces
that branch, and leaving it at 0 rolls as normal. Hidden durations can't be
overridden this way; to fix how long a Pokémon sleeps, set the counter in
the battle directly.

## Typical setups

- MCTS rollouts: `Options<NullLog, NullChance, NullCalc>`, which is the
  fastest.
- MCTS tree with chance nodes: `Options<NullLog, Chance<>, NullCalc>`, or
  with a `Calc` to force a branch, plus `randomize_hidden`.
- Policy-gradient RL: `FixedLog`. Tokenize the binary opcodes directly, or
  format them as text when you want to read them.

For working code, see `tools/soak.cpp` (random play with Chance),
`tools/bench.cpp` (with the log on and off) and `tools/check_showdown.cpp`
(the log and formatter checked against Showdown).

There's also `examples/rollout.cpp`, which plays random games with the log
on, prints them as Showdown text and checks the log against the battle
state. It isn't a CMake target, so build it by hand from the repo root:

```sh
g++ -std=c++20 -O2 -Isrc examples/rollout.cpp -o rollout && ./rollout 100 1 1
```

The arguments are `[games [seed [show [replay_every [team files...]]]]]`.

## Building the tests

CMake is only used for the tests and tools; the library itself is the
`INTERFACE` target `hoenn`. Configure with `cmake --preset debug` (asserts,
ASan and UBSan) or `cmake --preset release`, then run
`cmake --build --preset X` and `ctest --preset X`.
