// Checks the turn frame (mechanics.hpp): the start, the order of actions,
// the Quick Claw and speed-tie rolls with Chance and Calc, trapping, faint
// requests and resuming, the turn cap, and a random self-play soak over the
// given team files (every update keeps the battle valid; debug asserts the
// rest). Also the plain hits, Struggle, fainting and Spikes (plan step 3);
// every other move only uses PP so far.
//
// Usage: check_frame [team files...]

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/options.hpp"
#include "common/protocol.hpp"
#include "gen3/helpers.hpp"
#include "gen3/mechanics.hpp"
#include "gen3/showdown.hpp"

using namespace pkmn;
using namespace pkmn::gen3;

namespace {

int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                              \
    }                                                                          \
  } while (0)

using Null = Options<protocol::NullLog, NullChance, NullCalc>;
using Observed = Options<protocol::NullLog, Chance<>, Calc>;

const helpers::Pokemon SNORLAX = {
    .species = Species::Snorlax,
    .moves = {Move::BodySlam, Move::QuickAttack},
    .ability = Ability::Immunity,
    .nature = Nature::Adamant,
    .gender = Gender::Male,
};
const helpers::Pokemon JOLTEON = {
    .species = Species::Jolteon,
    .moves = {Move::Thunderbolt},
    .ability = Ability::VoltAbsorb,
    .nature = Nature::Timid,
    .gender = Gender::Male,
};
const helpers::Pokemon DUGTRIO = {
    .species = Species::Dugtrio,
    .moves = {Move::Earthquake},
    .ability = Ability::ArenaTrap,
    .gender = Gender::Male,
};

helpers::Pokemon holding(helpers::Pokemon p, Item item) {
  p.item = item;
  return p;
}

// A started battle (turn 1) between two teams.
Battle started(std::vector<helpers::Pokemon> p1, std::vector<helpers::Pokemon> p2,
               uint64_t seed = 1) {
  Battle b = *helpers::init_battle(seed, p1, p2);
  Null null{};
  const Result r = update(b, Choice::pass(), Choice::pass(), null);
  CHECK(r == Result::start());
  return b;
}

// Who moves first given both choices, with the given options.
template <typename O> Player first(Battle b, Choice c1, Choice c2, O& options) {
  detail::select(b, Player::P1, c1);
  detail::select(b, Player::P2, c2);
  detail::order(b, options);
  return b.request.first;
}

void check_start() {
  Battle b = *helpers::init_battle(1, std::vector{SNORLAX, JOLTEON},
                                   std::vector{JOLTEON});
  std::array<Choice, MAX_CHOICES> out{};
  CHECK(choices(b, Player::P1, Choice::Pass, out) == 1 && out[0] == Choice::pass());
  Observed o{};
  CHECK(update(b, Choice::pass(), Choice::pass(), o) == Result::start());
  CHECK(b.turn == 1);
  CHECK(b.sides[0].active.species == Species::Snorlax);
  CHECK(b.sides[1].active.species == Species::Jolteon);
  // Switch-in sets 2, and the cleanup before turn 1 takes it to 1.
  CHECK(b.sides[0].active.first_turn == 1);
  CHECK(o.chance.actions == Actions{});
  // Two moves and one switch for P1; one move and no switch for P2.
  CHECK(choices(b, Player::P1, Choice::Move, out) == 3);
  CHECK(out[0] == Choice::move(1) && out[1] == Choice::move(2) &&
        out[2] == Choice::swap(2));
  CHECK(choices(b, Player::P2, Choice::Move, out) == 1);
}

void check_order() {
  Null null{};
  {
    Battle b = started({JOLTEON}, {SNORLAX});
    CHECK(first(b, Choice::move(1), Choice::move(1), null) == Player::P1);
    // Quick Attack's +1 beats speed.
    CHECK(first(b, Choice::move(1), Choice::move(2), null) == Player::P2);
  }
  {
    // A switch goes first.
    Battle b = started({JOLTEON}, {SNORLAX, SNORLAX});
    CHECK(first(b, Choice::move(1), Choice::swap(2), null) == Player::P2);
  }
  {
    // A speed tie: drawn, recorded, half the probability, forceable.
    Battle b = started({SNORLAX}, {SNORLAX});
    Observed o{};
    first(b, Choice::move(1), Choice::move(1), o);
    CHECK(o.chance.actions.turn.speed_tie != 0);
    o.chance.probability.reduce();
    CHECK(o.chance.probability.p == 1 && o.chance.probability.q == 2);
    for (uint8_t forced : {1, 2}) {
      Observed f{};
      f.calc.overrides.turn.speed_tie = forced;
      const Player p = first(b, Choice::move(1), Choice::move(1), f);
      CHECK(p == (forced == 1 ? Player::P1 : Player::P2));
      CHECK(f.chance.actions.turn.speed_tie == forced);
    }
  }
  {
    // A slower Quick Claw holder: the draw can change the order, so it's
    // made and recorded.
    Battle b = started({JOLTEON}, {holding(SNORLAX, Item::QuickClaw)});
    Observed o{};
    o.calc.overrides.turn.quick_claw = Optional::True;
    CHECK(first(b, Choice::move(1), Choice::move(1), o) == Player::P2);
    CHECK(o.chance.actions.turn.quick_claw == Optional::True);
    o.chance.probability.reduce();
    CHECK(o.chance.probability.p == 1 && o.chance.probability.q == 5);
    Observed n{};
    n.calc.overrides.turn.quick_claw = Optional::False;
    CHECK(first(b, Choice::move(1), Choice::move(1), n) == Player::P1);
    // Moving against a switch: no comparison, no draw.
    Battle c = started({JOLTEON, JOLTEON}, {holding(SNORLAX, Item::QuickClaw)});
    Observed s{};
    CHECK(first(c, Choice::swap(2), Choice::move(1), s) == Player::P1);
    CHECK(s.chance.actions == Actions{});
  }
  {
    // A faster holder: Quick Claw can't change the order, so no draw.
    Battle b = started({holding(JOLTEON, Item::QuickClaw)}, {SNORLAX});
    Observed o{};
    CHECK(first(b, Choice::move(1), Choice::move(1), o) == Player::P1);
    CHECK(o.chance.actions.turn.quick_claw == Optional::None);
    CHECK(o.chance.actions == Actions{});
  }
  {
    // Two switches go in speed order (Showdown), Quick Claw ignored.
    Battle b = started({holding(SNORLAX, Item::QuickClaw), SNORLAX},
                       {JOLTEON, JOLTEON});
    Observed o{};
    CHECK(first(b, Choice::swap(2), Choice::swap(2), o) == Player::P2);
    CHECK(o.chance.actions == Actions{});
  }
}

void check_trapping() {
  std::array<Choice, MAX_CHOICES> out{};
  {
    // Arena Trap holds a grounded foe.
    Battle b = started({SNORLAX, JOLTEON}, {DUGTRIO});
    CHECK(choices(b, Player::P1, Choice::Move, out) == 2);
  }
  {
    // But not a Flying type.
    helpers::Pokemon skarmory = helpers::detail::SKARMORY;
    Battle b = started({skarmory, JOLTEON}, {DUGTRIO});
    CHECK(choices(b, Player::P1, Choice::Move, out) == 5);
  }
}

void check_switch() {
  Battle b = started({SNORLAX, JOLTEON}, {SNORLAX});
  Observed o{};
  o.chance.durations.p1.sleep[1] = 3;
  o.chance.durations.p1.confusion = 2;
  const Result r = update(b, Choice::swap(2), Choice::move(1), o);
  CHECK(r == Result::start());
  CHECK(b.turn == 2);
  CHECK(b.sides[0].order[0] == 2 && b.sides[0].order[1] == 1);
  CHECK(b.sides[0].active.species == Species::Jolteon);
  // Switched in this turn: 2, then 1 after the cleanup.
  CHECK(b.sides[0].active.first_turn == 1);
  CHECK(o.chance.durations.p1.sleep[0] == 3 && o.chance.durations.p1.sleep[1] == 0);
  CHECK(o.chance.durations.p1.confusion == 0);
}

// Snorlax whose only move does nothing yet, for the turn cap.
const helpers::Pokemon IDLE = {
    .species = Species::Snorlax,
    .moves = {Move::Splash},
    .ability = Ability::Immunity,
    .nature = Nature::Adamant,
    .gender = Gender::Male,
};

void check_faints() {
  Null null{};
  {
    // A Pokémon fainted mid-turn: replaced at once, its move cancelled, then
    // the end of turn. Quick Attack goes first and KOs a Jolteon at 1 HP.
    Battle b = started({SNORLAX}, {JOLTEON, SNORLAX});
    b.sides[1].get(1).hp = 1;
    const uint16_t hp = b.sides[0].get(1).hp;
    Result r = update(b, Choice::move(2), Choice::move(1), null);
    CHECK(r == Result::init(Result::None, Choice::Pass, Choice::Switch));
    CHECK(b.request.phase == Phase::FaintAfterAction);
    CHECK(b.sides[0].get(1).hp == hp); // Thunderbolt was cancelled
    std::array<Choice, MAX_CHOICES> out{};
    CHECK(choices(b, Player::P2, r.p2(), out) == 1 && out[0] == Choice::swap(2));
    r = update(b, Choice::pass(), Choice::swap(2), null);
    CHECK(r == Result::start());
    CHECK(b.turn == 2 && b.request.phase == Phase::None);
    CHECK(b.sides[1].active.species == Species::Snorlax);
  }
  {
    // No replacement left: the battle ends.
    Battle b = started({SNORLAX}, {JOLTEON});
    b.sides[1].get(1).hp = 1;
    const Result r = update(b, Choice::move(2), Choice::move(1), null);
    CHECK(r.type() == Result::Win);
  }
  {
    // The replacement faints to Spikes: another request, same phase.
    Battle b = started({SNORLAX}, {JOLTEON, SNORLAX, JOLTEON});
    b.sides[1].get(1).hp = 1;
    b.sides[1].get(2).hp = 1;
    b.sides[1].conditions.spikes = 1;
    Result r = update(b, Choice::move(2), Choice::move(1), null);
    CHECK(r == Result::init(Result::None, Choice::Pass, Choice::Switch));
    r = update(b, Choice::pass(), Choice::swap(2), null);
    CHECK(r == Result::init(Result::None, Choice::Pass, Choice::Switch));
    CHECK(b.request.phase == Phase::FaintAfterAction);
    CHECK(b.sides[1].get(1).species == Species::Snorlax);
    CHECK(b.sides[1].get(1).hp == 0);
    r = update(b, Choice::pass(), Choice::swap(3), null);
    CHECK(r == Result::start() && b.turn == 2);
  }
}

void check_moves() {
  {
    // A plain hit (Quick Attack), forced through Calc: no crit, the 100%
    // roll. Recorded in Chance with its probability.
    Battle b = started({SNORLAX}, {SNORLAX});
    Observed o{};
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    o.calc.overrides.p1.hits[0].damage = 100;
    const uint16_t hp = b.sides[1].get(1).hp;
    const uint32_t expected =
        apply_roll(damage(b, Player::P1, Hit::of(Move::QuickAttack)), 15);
    // Body Slam's effect isn't written yet: it only uses PP.
    CHECK(update(b, Choice::move(2), Choice::move(1), o) == Result::start());
    CHECK(b.sides[1].get(1).hp == hp - expected);
    CHECK(o.chance.actions.p1.hits[0].crit == Optional::False);
    CHECK(o.chance.actions.p1.hits[0].damage == 100);
    // Quick Attack's 100% accuracy draws nothing.
    CHECK(o.chance.actions.p1.hit == Optional::None);
    o.chance.probability.reduce();
    CHECK(o.chance.probability.p == 15 && o.chance.probability.q == 256);
    // PP on both the active Pokémon and the party, for both players.
    CHECK(b.sides[0].active.moves[1].pp == b.sides[0].get(1).moves[1].pp);
    CHECK(b.sides[0].get(1).moves[1].pp == max_pp(Move::QuickAttack, 3) - 1);
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::BodySlam, 3) - 1);
    CHECK(b.sides[1].active.last_landed_move == Move::QuickAttack);
  }
  {
    // A Ground move into a Flying type: no effect, no draws, PP used.
    Battle b = started({DUGTRIO}, {helpers::detail::SKARMORY});
    Observed o{};
    const uint16_t hp = b.sides[1].get(1).hp;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[1].get(1).hp == hp);
    CHECK(o.chance.actions.p1 == Action{});
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Earthquake, 3) - 1);
  }
  {
    // Focus Band, forced: the lethal hit leaves 1 HP.
    Battle b = started({SNORLAX}, {holding(SNORLAX, Item::FocusBand)});
    b.sides[1].get(1).hp = 5;
    Observed o{};
    o.calc.overrides.p1.hits[0].focus_band = Optional::True;
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[1].get(1).hp == 1);
    CHECK(o.chance.actions.p1.hits[0].focus_band == Optional::True);
  }
  {
    // Struggle uses no PP and takes a quarter of the damage back. Here the
    // recoil faints the user as the hit faints the target: both last
    // Pokémon down in one action is a tie.
    Battle b = started({SNORLAX}, {IDLE});
    for (MoveSlot& m : b.sides[0].active.moves) m.pp = 0;
    std::array<Choice, MAX_CHOICES> out{};
    CHECK(choices(b, Player::P1, Choice::Move, out) == 1 && out[0] == Choice::move(0));
    b.sides[0].get(1).hp = 1;
    b.sides[1].get(1).hp = 1;
    Null null{};
    CHECK(update(b, Choice::move(0), Choice::move(1), null).type() == Result::Tie);
  }
  {
    // Struggle's recoil when both survive.
    Battle b = started({SNORLAX}, {IDLE});
    for (MoveSlot& m : b.sides[0].active.moves) m.pp = 0;
    Observed o{};
    const uint16_t hp1 = b.sides[0].get(1).hp;
    const uint16_t hp2 = b.sides[1].get(1).hp;
    update(b, Choice::move(0), Choice::move(1), o);
    const uint16_t dealt = uint16_t(hp2 - b.sides[1].get(1).hp);
    CHECK(dealt > 0);
    CHECK(b.sides[0].get(1).hp == hp1 - (dealt / 4 ? dealt / 4 : 1));
    // Struggle took no PP from the party either.
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::BodySlam, 3));
  }
  {
    // Spikes on a switch: 1/8, 1/6, 1/4 of max HP.
    for (uint8_t layers = 1; layers <= 3; layers++) {
      Battle b = started({SNORLAX, JOLTEON}, {IDLE});
      b.sides[0].conditions.spikes = layers;
      Null null{};
      update(b, Choice::swap(2), Choice::move(1), null);
      const Pokemon& j = b.sides[0].get(1);
      CHECK(j.hp == j.stats.hp - j.stats.hp / ((5 - layers) * 2));
    }
    // Not on a Flying type.
    Battle b = started({SNORLAX, helpers::detail::SKARMORY}, {IDLE});
    b.sides[0].conditions.spikes = 3;
    Null null{};
    update(b, Choice::swap(2), Choice::move(1), null);
    CHECK(b.sides[0].get(1).hp == b.sides[0].get(1).stats.hp);
  }
}

void check_turn_cap() {
  Battle b = started({IDLE}, {IDLE});
  Null null{};
  Result r = Result::start();
  int updates = 0;
  while (!r.over() && updates < 2000) {
    // Splash has 64 PP; refill it so nobody Struggles.
    for (Side& side : b.sides) side.active.moves[0].pp = side.get(1).moves[0].pp = 64;
    r = update(b, Choice::move(1), Choice::move(1), null);
    updates++;
  }
  CHECK(r.type() == Result::Tie);
  CHECK(b.turn == Battle::MAX_TURNS && updates == Battle::MAX_TURNS - 1);
}

// Random self-play between consecutive teams, with Chance and Calc on.
void soak(const std::vector<std::vector<helpers::Pokemon>>& teams) {
  if (teams.size() < 2) return;
  RNG rng{42};
  size_t updates = 0;
  const size_t games = teams.size() < 40 ? teams.size() : 40;
  for (size_t g = 0; g < games; g++) {
    const auto& t1 = teams[g];
    const auto& t2 = teams[(g + 1) % teams.size()];
    Battle b = *helpers::init_battle(rng.next(), t1, t2);
    Observed o{};
    Result r = update(b, Choice::pass(), Choice::pass(), o);
    std::array<Choice, MAX_CHOICES> c1{}, c2{};
    while (!r.over()) {
      const uint8_t n1 = choices(b, Player::P1, r.p1(), c1);
      const uint8_t n2 = choices(b, Player::P2, r.p2(), c2);
      CHECK(n1 >= 1 && n2 >= 1);
      r = update(b, c1[rng.range(n1)], c2[rng.range(n2)], o);
      CHECK(b.valid());
      updates++;
    }
    CHECK(b.turn <= Battle::MAX_TURNS);
  }
  std::printf("soak: %zu games, %zu updates\n", games, updates);
}

} // namespace

int main(int argc, char** argv) {
  check_start();
  check_order();
  check_trapping();
  check_switch();
  check_faints();
  check_moves();
  check_turn_cap();

  std::vector<std::vector<helpers::Pokemon>> teams;
  RNG rng{0};
  for (int i = 1; i < argc; i++) {
    std::ifstream in(argv[i]);
    std::stringstream ss;
    ss << in.rdbuf();
    for (const auto& team : showdown::parse_teams(ss.str(), rng)) {
      if (team.value) teams.push_back(team.value->pokemon);
    }
  }
  soak(teams);

  std::printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
