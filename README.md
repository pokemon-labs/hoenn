# Hoenn

A header-only Gen III singles engine (Smogon ADV OU) in C++20, in
`namespace hoenn`, for MCTS and policy-gradient RL. It follows libpkmn's
design: a small POD `Battle`, the choice/result protocol, and the Log,
Chance and Calc options. `update` runs a turn, or the rest of one after a
mid-turn request (a faint replacement or Baton Pass), so a turn can take
more than one call. Mechanics, the turn frame and the protocol are in
[`docs/ENGINE.md`](docs/ENGINE.md); tests in [`docs/TESTING.md`](docs/TESTING.md).

## Sim Accuracy

The target is Pokémon Showdown's gen3 (pinned in
[`tools/criteria.toml`](tools/criteria.toml), `[showdown]`), not the
cartridge.

**It can't be matched to Showdown one to one.** The engine doesn't
reproduce Showdown's RNG: it uses its own generator (SplitMix64) and draws
wherever convenient, so the same seed gives different games, and
Showdown's draws can't be replayed into it. Comparing the two game by game
needs a weaker question.

**Realizability** asks that question: could the engine have produced what
Showdown did? `tools/showdown_traces.cjs` plays seeded Showdown games with
random choices and records each step (teams, both choice sets, the chosen
pair, the log). `check_showdown` replays them, and at every step:

- our `choices()` must equal Showdown's choice set exactly;
- some branch of ours must reproduce Showdown's log, line for line: the
  step is rerun from fresh seeds, with hidden counters resampled by
  `randomize_hidden`, until one matches, up to a cap of 1M tries. Showdown's
  hidden counters are never copied in, so correct `Durations` must make
  the true values reachable;
- a state guard (HP, status, PP, items, boosts, volatiles, side
  conditions, weather and so on) must also agree, since the log hides
  some state.

A game is *matched* when every step is reproduced to the end. The other
suites cover what realizability can't:

- `soak`: random self-play under ASan, UBSan and asserts; zero crashes or
  invalid states.
- `check_chance`: `(c1, c2, Actions)` must determine the result, with zero
  collisions, and each branch's reported probability must fit how often
  it occurs.
- `check_damage`, `check_frame` and the other fast suites in ctest.

The bars per environment (real OU teams, random OU-legal, truly random)
are in [`tools/criteria.toml`](tools/criteria.toml), with their rationale
in [`docs/TESTING.md`](docs/TESTING.md), "Pass criteria": at least 99.9%
of 10,000 OU games matched (99.8% for the random environments), and zero
safety failures, choice-set mismatches or chance-key collisions anywhere.
`python3 tools/run_env.py ENV` runs an environment against its bars.

What a pass doesn't show (`docs/TESTING.md`, "Known test limitations"):

- **Support, not probability.** Realizability shows Showdown's outcome is
  one of ours, not that it's as likely: a 30% secondary effect that
  should be 10% passes. `check_chance` checks our probabilities against
  our own RNG, not against Showdown.
- **One direction.** Branches we have and Showdown doesn't go unnoticed.
- **Multi-hit games are excluded.** A game whose first unmatched step is a
  multi-hit move leaves the count, since random search can't tell its rare
  branches from a bug.
- **Random choices** miss states a search steers into, and the known
  engine gaps (`docs/TESTING.md`, "Known engine gaps") are inside the
  allowed failures.

## Building a battle

`src/helpers.hpp` and `src/showdown.hpp`. Setup only: these may allocate and
return error strings.

```cpp
#include "helpers.hpp"
#include "showdown.hpp"
#include "mechanics.hpp"   // update, choices
#include "options.hpp"
#include "chance.hpp"
#include "calc.hpp"
#include "log.hpp"

using namespace hoenn;

// Fill helpers::Pokemon by hand...
helpers::Pokemon skarm{
    .species = Species::Skarmory,
    .moves = {Move::Spikes, Move::Protect, Move::Roar, Move::Toxic},
    .item = Item::Leftovers, .ability = Ability::KeenEye,
    .nature = Nature::Careful, .evs = {252, 0, 8, 0, 0, 248},  // HP Atk Def Spe SpA SpD
    .gender = Gender::Female};

// ...or parse Showdown exports (rng draws a gender when nothing decides it).
RNG parse_rng{0};
auto teams = showdown::parse_teams(text, parse_rng);  // vector<Parsed<Team>>: .value or .error

std::optional<Battle> b = helpers::init_battle(seed, team1, team2);  // spans of helpers::Pokemon
```

- Moves are packed at the front, 1–4 of them. Pass the generic
  `Move::HiddenPower`; its type comes from the IVs. Every move gets 3 PP Ups.
- Only the engine's bounds are checked, not legality. `check_pokemon` and
  `check_side` say why something is rejected; `init_*` return `nullopt`.
- The result is at turn 0 with no active Pokémon. `Battle` is 824 bytes,
  trivially copyable, with no pointers: `Battle copy = battle;` is how
  search branches.

## Running it

`src/mechanics.hpp`:

```cpp
Result  update(Battle&, Choice c1, Choice c2, Options&&);
uint8_t choices(const Battle&, Player, Choice::Type request,
                std::span<Choice, MAX_CHOICES> out);  // returns 1..9
```

The loop (as `tools/soak.cpp`):

```cpp
Options<protocol::NullLog, NullChance, NullCalc> o{};        // search: all null
Result r = update(battle, Choice::pass(), Choice::pass(), o); // turn 0: leads in, turn 1
std::array<Choice, MAX_CHOICES> c1{}, c2{};
while (!r.over()) {
  uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
  uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
  r = update(battle, c1[pick1], c2[pick2], o);
}
// r.type(): Win or Lose (from P1's view), or Tie
```

- **`Choice`** is one byte: `Choice::pass()`, `Choice::move(slot)` with a
  move slot 1–4 (0 is Struggle or a forced continuation: charging, rampage,
  Bide), `Choice::swap(SLOT)` with a SLOT 2–6.
- **SLOT is order-dependent**, as libpkmn: `Side::pokemon[6]` never moves,
  `side.get(s)` is `pokemon[order[s - 1] - 1]`, SLOT 1 is active, and a
  switch swaps `order[0]` and `order[s - 1]`.
- **`Result`** is one byte: `type()` is `None/Win/Lose/Tie`, and
  `p1()`/`p2()` are the `Choice::Type` each player makes next. An ordinary
  turn is `Move`/`Move`; a faint replacement is `Switch` for the fainted side
  and `Pass` for the other; Baton Pass is a mid-turn `Switch`, and the next
  `update` resumes the same turn (`battle.request` holds where).
- **Don't hold it wrong.** Pass only choices from `choices()` and battles
  that are `valid()`. Debug asserts both; release checks neither (bad input
  is UB). `update` never returns `Error` for bad input.
- **Turn cap:** `update` returns `Tie` once `turn` reaches 1001
  (`Battle::MAX_TURNS`), so turn 1000 is played, as Showdown.

## Options

`Options<Log, Chance, Calc>` (`src/options.hpp`). The null types are empty
and compile to nothing. Build with `Options{log, chance, calc}` or
`hoenn::options(log, chance, calc)`.

| Option | Real | Null | What it gives |
|---|---|---|---|
| Log | `protocol::FixedLog` (`protocol::fixed_log(writer)`) | `protocol::NullLog`, `protocol::null_log` | The binary protocol stream |
| Chance | `Chance<>` (`Rational<double>`) | `NullChance` | `probability`, `actions`, `durations` |
| Calc | `Calc{.overrides = Actions{...}}` | `NullCalc` | Forces chance branches |

### Log

`src/log.hpp`, `src/protocol.hpp`, `src/format.hpp`. The caller owns the
buffer; the `Writer` only points into it.

```cpp
std::vector<uint8_t> buf(protocol::MAX_LOG_SIZE);
protocol::Writer w{buf.data(), buf.size(), 0};
Options<protocol::FixedLog, NullChance, NullCalc> o{protocol::fixed_log(w), {}, {}};
update(battle, c1, c2, o);
// buf[0, w.pos) holds the update's opcodes, ending in ArgType::None; w.reset() before the next
```

`format::Formatter(names, exact_hp).format(bytes)` turns them into
Showdown's text, line for line, `|split|` triplets included. It allocates,
so it stays out of search. `format::Names` holds the player names and each
Pokémon's nickname and shininess by party position; `tools/check_showdown.cpp`
shows how to fill it.

### Chance

`src/chance.hpp`. Keep one `Chance` per battle and copy it with the battle:
`update` resets `probability` and `actions`, while `durations` carry over.

- `actions` (`Actions`) is the random branch taken. With the battle and both
  choices it determines the `Result` and all public state, so it keys MCTS
  chance nodes.
- `randomize_hidden(battle, chance.durations)` resamples the true hidden
  counters (sleep, confusion, Disable, Encore, binding, Uproar, Thrash) on a
  copy before each search iteration.
- `mask_hidden(battle)` strips what `Actions` doesn't determine, for
  comparing states.

### Calc

`src/calc.hpp`. A nonzero field of `overrides` forces that branch; 0 rolls
normally. Hidden durations aren't overrides: to fix a sleep length, set the
counter in the battle.

## Typical setups

- **MCTS rollouts:** `Options<NullLog, NullChance, NullCalc>`, the fastest.
- **MCTS tree / chance nodes:** `Options<NullLog, Chance<>, NullCalc>` (or
  `Calc` to force a branch), with `randomize_hidden`.
- **Policy-gradient RL:** `FixedLog`; tokenize the binary opcodes directly,
  or format them to text to inspect.

Working examples: `tools/soak.cpp` (random play with Chance),
`tools/bench.cpp` (null and log-on options), `tools/check_showdown.cpp` (log
and formatter against Showdown).

## Building the tests

CMake, for tests and tools only; the library is the `INTERFACE` target
`hoenn`. `cmake --preset debug` (asserts, ASan, UBSan) or `release`, then
`cmake --build --preset X` and `ctest --preset X`.
