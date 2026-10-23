// Checks the turn frame (mechanics.hpp): the start, the order of actions,
// the Quick Claw and speed-tie rolls with Chance and Calc, trapping, faint
// requests and resuming, the turn cap, and a random self-play soak over the
// given team files (every update keeps the battle valid; debug asserts the
// rest). Also the plain hits, Struggle, fainting, Spikes and a few move
// scenarios.
//
// Usage: check_frame [team files...]

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "options.hpp"
#include "log.hpp"
#include "helpers.hpp"
#include "mechanics.hpp"
#include "showdown.hpp"

using namespace hoenn;

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
  CHECK(b.sides[0].active.volatiles.first_turn == 1);
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
    // Two switches go in speed order with Quick Claw, as Showdown's gen3
    // getActionSpeed: the slower holder draws, and goes first on a proc.
    Battle b = started({holding(SNORLAX, Item::QuickClaw), SNORLAX},
                       {JOLTEON, JOLTEON});
    Observed o{};
    o.calc.overrides.turn.quick_claw = Optional::False;
    CHECK(first(b, Choice::swap(2), Choice::swap(2), o) == Player::P2);
    CHECK(o.chance.actions.turn.quick_claw == Optional::False);
    Observed q{};
    q.calc.overrides.turn.quick_claw = Optional::True;
    CHECK(first(b, Choice::swap(2), Choice::swap(2), q) == Player::P1);
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
  CHECK(b.sides[0].active.volatiles.first_turn == 1);
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
    Battle b = started({SNORLAX}, {IDLE});
    Observed o{};
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    o.calc.overrides.p1.hits[0].damage = 100;
    const uint16_t hp = b.sides[1].get(1).hp;
    const uint32_t expected =
        apply_roll(damage(b, Player::P1, Hit::of(Move::QuickAttack)), 15);
    // Splash does nothing yet: it only uses PP.
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
    CHECK(b.sides[0].get(1).moves[1].pp == max_pp(Move::QuickAttack) - 1);
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::Splash) - 1);
    // Mirror Move's record is kept only where it can be read.
    CHECK(b.sides[1].active.volatiles.last_taken_move == Move::None);
  }
  {
    // A Ground move into a Flying type: no effect, no draws, PP used.
    Battle b = started({DUGTRIO}, {helpers::detail::SKARMORY});
    Observed o{};
    const uint16_t hp = b.sides[1].get(1).hp;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[1].get(1).hp == hp);
    CHECK(o.chance.actions.p1 == Action{});
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Earthquake) - 1);
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
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::BodySlam));
  }
  {
    // Explosion: the user faints whether or not it hits, here into a Ghost
    // type, which takes nothing. Both last Pokémon down is a tie.
    const helpers::Pokemon boomer = {
        .species = Species::Snorlax,
        .moves = {Move::Explosion},
        .ability = Ability::Immunity,
        .nature = Nature::Adamant,
        .gender = Gender::Male,
    };
    const helpers::Pokemon gengar = {
        .species = Species::Gengar,
        .moves = {Move::Splash},
        .ability = Ability::Levitate,
        .gender = Gender::Male,
    };
    Null null{};
    Battle b = started({boomer, SNORLAX}, {gengar});
    const uint16_t hp = b.sides[1].get(1).hp;
    Result r = update(b, Choice::move(1), Choice::move(1), null);
    CHECK(r == Result::init(Result::None, Choice::Switch, Choice::Pass));
    CHECK(b.sides[0].get(1).hp == 0 && b.sides[1].get(1).hp == hp);
    Battle c = started({boomer}, {IDLE});
    c.sides[1].get(1).hp = 1;
    CHECK(update(c, Choice::move(1), Choice::move(1), null).type() == Result::Tie);
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

void check_confusion() {
  {
    // A forced self-hit: Showdown's typeless 40-power hit with the roll,
    // no PP used, and the move doesn't happen. Count 1 (just confused), so
    // the counter can't end at this step: probability 1/2 * 1/16. Quick
    // Attack's priority avoids the speed tie.
    Battle b = started({SNORLAX}, {IDLE});
    b.sides[0].active.volatiles.confusion = 3;
    Observed o{};
    o.chance.durations.p1.confusion = 1;
    o.calc.overrides.p1.confusion = Optional::True;
    o.calc.overrides.p1.hits[0].damage = 100;
    const uint16_t hp1 = b.sides[0].get(1).hp;
    const uint16_t hp2 = b.sides[1].get(1).hp;
    const uint32_t expected =
        apply_roll(damage(b, Player::P1, Player::P1, Hit::confusion()), 15);
    CHECK(expected > 0);
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[0].get(1).hp == hp1 - expected);
    CHECK(b.sides[1].get(1).hp == hp2);
    CHECK(b.sides[0].get(1).moves[1].pp == max_pp(Move::QuickAttack));
    CHECK(b.sides[0].active.volatiles.confusion == 2);
    CHECK(o.chance.actions.p1.confusion == Optional::True);
    CHECK(o.chance.actions.p1.confusion_end == Optional::False);
    CHECK(o.chance.actions.p1.hits[0].damage == 100);
    CHECK(o.chance.actions.p1.hits[0].crit == Optional::None);
    CHECK(o.chance.durations.p1.confusion == 2);
    o.chance.probability.reduce();
    CHECK(o.chance.probability.p == 1 && o.chance.probability.q == 32);
  }
  {
    // Snapping out: the counter reaches 0 and the move goes ahead. At count
    // 4 (three steps) the roll was 4 or 5, so this ends with 1/2; then no
    // crit (15/16) and the roll (1/16).
    Battle b = started({SNORLAX}, {IDLE});
    b.sides[0].active.volatiles.confusion = 1;
    Observed o{};
    o.chance.durations.p1.confusion = 4;
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    o.calc.overrides.p1.hits[0].damage = 100;
    const uint16_t hp2 = b.sides[1].get(1).hp;
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[1].get(1).hp < hp2);
    CHECK(b.sides[0].active.volatiles.confusion == 0);
    CHECK(o.chance.actions.p1.confusion_end == Optional::True);
    CHECK(o.chance.actions.p1.confusion == Optional::None);
    CHECK(o.chance.durations.p1.confusion == 0);
    o.chance.probability.reduce();
    CHECK(o.chance.probability.p == 15 && o.chance.probability.q == 512);
  }
  {
    // The user's own Reflect doesn't halve its self-hit (Showdown's
    // `target !== source`; the decomp's would).
    Battle b = started({SNORLAX}, {IDLE});
    const uint32_t plain = damage(b, Player::P1, Player::P1, Hit::confusion());
    b.sides[0].conditions.reflect = 5;
    CHECK(damage(b, Player::P1, Player::P1, Hit::confusion()) == plain);
  }
  {
    // Focus Band saves the user from a lethal self-hit, here behind its own
    // Substitute, which the self-hit ignores.
    Battle b = started({holding(SNORLAX, Item::FocusBand)}, {IDLE});
    b.sides[0].active.volatiles.confusion = 3;
    b.sides[0].active.volatiles.substitute = true;
    b.sides[0].active.volatiles.substitute_hp = 50;
    b.sides[0].get(1).hp = 2;
    Observed o{};
    o.calc.overrides.p1.confusion = Optional::True;
    o.calc.overrides.p1.hits[0].focus_band = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[0].get(1).hp == 1);
    CHECK(b.sides[0].active.volatiles.substitute_hp == 50);
    CHECK(o.chance.actions.p1.hits[0].focus_band == Optional::True);
  }
  {
    // A self-hit that faints the user raises a replacement request.
    Battle b = started({SNORLAX, JOLTEON}, {IDLE});
    b.sides[0].active.volatiles.confusion = 3;
    b.sides[0].get(1).hp = 1;
    Observed o{};
    o.calc.overrides.p1.confusion = Optional::True;
    const Result r = update(b, Choice::move(1), Choice::move(1), o);
    CHECK(r == Result::init(Result::None, Choice::Switch, Choice::Pass));
    CHECK(b.sides[0].get(1).fainted());
    CHECK(b.sides[0].active.volatiles.confusion == 0);
  }
}

void check_confuse() {
  const helpers::Pokemon confuser = {
      .species = Species::Gengar,
      .moves = {Move::ConfuseRay, Move::Psybeam},
      .ability = Ability::Levitate,
      .gender = Gender::Male,
  };
  {
    // Confuse Ray: a hidden 2-5 counter, public count 1, no draw recorded
    // (accuracy 100). Gengar is faster, so Snorlax's tick comes this turn.
    Battle b = started({confuser}, {SNORLAX});
    Observed o{};
    update(b, Choice::move(1), Choice::move(1), o);
    const uint8_t c = b.sides[1].active.volatiles.confusion;
    // Snorlax ticked once after being confused: 1-4 left, count 2.
    CHECK(c >= 1 && c <= 4);
    CHECK(o.chance.durations.p2.confusion == 2);
    CHECK(o.chance.actions.p2.confusion_end == Optional::False);
    CHECK(o.chance.actions.p2.confusion != Optional::None);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::ConfuseRay) - 1);
  }
  {
    // It fails into Own Tempo, a Substitute, Safeguard or an existing
    // confusion, which keeps its counter.
    helpers::Pokemon tempo = IDLE;
    tempo.species = Species::Slowbro;
    tempo.ability = Ability::OwnTempo;
    Battle b = started({confuser}, {tempo});
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].active.volatiles.confusion == 0);

    Battle c = started({confuser}, {IDLE});
    c.sides[1].active.volatiles.substitute = true;
    c.sides[1].active.volatiles.substitute_hp = 10;
    update(c, Choice::move(1), Choice::move(1), null);
    CHECK(c.sides[1].active.volatiles.confusion == 0);

    Battle d = started({confuser}, {IDLE});
    d.sides[1].conditions.safeguard = 5;
    update(d, Choice::move(1), Choice::move(1), null);
    CHECK(d.sides[1].active.volatiles.confusion == 0);
    CHECK(d.sides[0].active.volatiles.last_move == Move::ConfuseRay);

    Battle e = started({confuser}, {IDLE});
    e.sides[1].active.volatiles.confusion = 5;
    Observed o{};
    o.calc.overrides.p2.confusion = Optional::False;
    update(e, Choice::move(1), Choice::move(1), o);
    // Not reset to a fresh roll: 5, then Splash's tick.
    CHECK(e.sides[1].active.volatiles.confusion == 4);
  }
  {
    // Psybeam's 10% confusion, forced.
    Battle b = started({confuser}, {IDLE});
    Observed o{};
    o.calc.overrides.p1.hits[0].secondary = Optional::True;
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[1].active.volatiles.confusion >= 1);
    CHECK(o.chance.actions.p1.hits[0].secondary == Optional::True);
    // Not through Safeguard: nothing is drawn.
    Battle c = started({confuser}, {IDLE});
    c.sides[1].conditions.safeguard = 5;
    Observed q{};
    update(c, Choice::move(2), Choice::move(1), q);
    CHECK(c.sides[1].active.volatiles.confusion == 0);
    CHECK(q.chance.actions.p1.hits[0].secondary == Optional::None);
  }
}

void check_status_moves() {
  const helpers::Pokemon user = {
      .species = Species::Gengar,
      .moves = {Move::Toxic, Move::ThunderWave, Move::WillOWisp, Move::Glare},
      .ability = Ability::Levitate,
      .gender = Gender::Male,
  };
  const auto after = [&](uint8_t slot, helpers::Pokemon foe, auto&& setup) {
    Battle b = started({user}, {foe});
    setup(b);
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    update(b, Choice::move(slot), Choice::move(1), o);
    return b;
  };
  const auto none = [](Battle&) {};
  // IDLE has Immunity; this one doesn't.
  helpers::Pokemon plain = IDLE;
  plain.ability = Ability::ThickFat;
  // Each lands on a plain target and uses PP.
  {
    Battle b = after(1, plain, none);
    CHECK(b.sides[1].get(1).status.badly_poisoned());
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Toxic) - 1);
    CHECK(after(2, plain, none).sides[1].get(1).status.paralyzed());
    CHECK(after(3, plain, none).sides[1].get(1).status.burned());
    CHECK(after(4, plain, none).sides[1].get(1).status.paralyzed());
  }
  // The immunities.
  {
    CHECK(!after(1, helpers::detail::SKARMORY, none).sides[1].get(1).status.any());
    CHECK(!after(2, DUGTRIO, none).sides[1].get(1).status.any());
    helpers::Pokemon ghost = IDLE;
    ghost.species = Species::Gengar;
    ghost.ability = Ability::Levitate;
    CHECK(!after(4, ghost, none).sides[1].get(1).status.any());
    // Foresight lets Glare hit a Ghost.
    CHECK(after(4, ghost, [](Battle& b) {
            b.sides[1].active.volatiles.foresight = true;
          }).sides[1].get(1).status.paralyzed());
    helpers::Pokemon fire = IDLE;
    fire.species = Species::Charizard;
    fire.ability = Ability::Blaze;
    CHECK(!after(3, fire, none).sides[1].get(1).status.any());
    helpers::Pokemon limber = IDLE;
    limber.species = Species::Hitmonlee;
    limber.ability = Ability::Limber;
    CHECK(!after(2, limber, none).sides[1].get(1).status.any());
    // Not Snorlax's Immunity: Toxic fails, PP still used.
    Battle b = started({user}, {SNORLAX});
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(!b.sides[1].get(1).status.any());
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Toxic) - 1);
  }
  // Thunder Wave into Wonder Guard lands: status moves skip it.
  {
    helpers::Pokemon shedinja = IDLE;
    shedinja.species = Species::Shedinja;
    shedinja.ability = Ability::WonderGuard;
    shedinja.gender = Gender::None;
    CHECK(after(2, shedinja, none).sides[1].get(1).status.paralyzed());
  }
  // Substitute, Safeguard, an existing status; and a forced miss.
  {
    CHECK(!after(2, plain, [](Battle& b) {
             b.sides[1].active.volatiles.substitute = true;
             b.sides[1].active.volatiles.substitute_hp = 10;
           }).sides[1].get(1).status.any());
    Battle b = after(2, plain, [](Battle& b) { b.sides[1].conditions.safeguard = 5; });
    CHECK(!b.sides[1].get(1).status.any());
    CHECK(b.sides[0].active.volatiles.last_move == Move::ThunderWave);
    CHECK(after(2, plain, [](Battle& b) {
            b.sides[1].get(1).status = Status::brn();
          }).sides[1].get(1).status.burned());
    Battle c = started({user}, {plain});
    Observed o{};
    o.calc.overrides.p1.hit = Optional::False;
    update(c, Choice::move(1), Choice::move(1), o);
    CHECK(!c.sides[1].get(1).status.any());
    CHECK(o.chance.actions.p1.hit == Optional::False);
  }
}

// The stat moves: one-stat and two-stat raises at and below the cap,
// Minimize, Belly Drum and both Curses. The foe only Splashes.
void check_stat_moves() {
  const auto after = [](helpers::Pokemon user, uint8_t slot, auto&& setup) {
    Battle b = started({user}, {IDLE});
    setup(b);
    Null null{};
    update(b, Choice::move(slot), Choice::move(1), null);
    return b;
  };
  const auto none = [](Battle&) {};
  const helpers::Pokemon dancer = {
      .species = Species::Gyarados,
      .moves = {Move::DragonDance, Move::SwordsDance, Move::CalmMind, Move::Minimize},
      .ability = Ability::Intimidate,
      .gender = Gender::Male,
  };
  // Calm Mind raises both stats and uses PP; Minimize sets its flag.
  {
    Battle b = after(dancer, 3, none);
    CHECK(b.sides[0].active.boosts.spa == 1 && b.sides[0].active.boosts.spd == 1);
    CHECK(b.sides[0].get(1).moves[2].pp == max_pp(Move::CalmMind) - 1);
    b = after(dancer, 4, none);
    CHECK(b.sides[0].active.boosts.eva == 1 && b.sides[0].active.volatiles.minimized);
  }
  // Swords Dance from +5 gains one stage.
  CHECK(after(dancer, 2, [](Battle& b) { b.sides[0].active.boosts.atk = 5; })
            .sides[0].active.boosts.atk == 6);
  // Dragon Dance at +6 Attack still raises Speed; at +6 in both it fails.
  {
    Battle b = after(dancer, 1, [](Battle& b) { b.sides[0].active.boosts.atk = 6; });
    CHECK(b.sides[0].active.boosts.atk == 6 && b.sides[0].active.boosts.spe == 1);
    b = after(dancer, 1, [](Battle& b) {
      b.sides[0].active.boosts.atk = 6;
      b.sides[0].active.boosts.spe = 6;
    });
    CHECK(b.sides[0].active.boosts.atk == 6 && b.sides[0].active.boosts.spe == 6);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::DragonDance) - 1);
  }
  helpers::Pokemon snorlax = SNORLAX;
  snorlax.moves[0] = Move::Curse;
  snorlax.moves[1] = Move::BellyDrum;
  // Curse: Speed -1, Attack and Defense +1; at the caps it fails, and with
  // Speed at -6 alone it still raises.
  {
    const Boosts& c = after(snorlax, 1, none).sides[0].active.boosts;
    CHECK(c.spe == -1 && c.atk == 1 && c.def == 1);
    Battle b = after(snorlax, 1, [](Battle& b) { b.sides[0].active.boosts.spe = -6; });
    CHECK(b.sides[0].active.boosts.spe == -6 && b.sides[0].active.boosts.atk == 1);
    b = after(snorlax, 1, [](Battle& b) {
      b.sides[0].active.boosts.atk = 6;
      b.sides[0].active.boosts.def = 6;
      b.sides[0].active.boosts.spe = -6;
    });
    CHECK(b.sides[0].active.boosts.atk == 6 && b.sides[0].active.boosts.spe == -6);
  }
  // Belly Drum: +6 Attack for half the max HP; it fails at or below half.
  {
    Battle b = after(snorlax, 2, none);
    const Pokemon& mon = b.sides[0].get(1);
    CHECK(b.sides[0].active.boosts.atk == 6);
    CHECK(mon.hp == mon.stats.hp - mon.stats.hp / 2);
    b = after(snorlax, 2, [](Battle& b) {
      b.sides[0].get(1).hp = uint16_t(b.sides[0].get(1).stats.hp / 2);
    });
    CHECK(b.sides[0].active.boosts.atk == 0);
  }
  // Ghost Curse: the foe is cursed for half the user's max HP; a second
  // Curse or a Substitute fails it at no cost.
  {
    const helpers::Pokemon gengar = {
        .species = Species::Gengar,
        .moves = {Move::Curse},
        .ability = Ability::Levitate,
        .gender = Gender::Male,
    };
    Battle b = after(gengar, 1, none);
    const Pokemon& mon = b.sides[0].get(1);
    CHECK(b.sides[1].active.volatiles.cursed);
    CHECK(mon.hp == mon.stats.hp - mon.stats.hp / 2);
    CHECK(b.sides[0].active.boosts.atk == 0);
    b = after(gengar, 1, [](Battle& b) { b.sides[1].active.volatiles.cursed = true; });
    CHECK(b.sides[0].get(1).hp == b.sides[0].get(1).stats.hp);
    b = after(gengar, 1, [](Battle& b) {
      b.sides[1].active.volatiles.substitute = true;
      b.sides[1].active.volatiles.substitute_hp = 10;
    });
    CHECK(!b.sides[1].active.volatiles.cursed);
    CHECK(b.sides[0].get(1).hp == b.sides[0].get(1).stats.hp);
  }
}

// Roar and Whirlwind: the drag (forced and observed), Spikes on the
// dragged-in Pokémon, the failures, Soundproof, and a cancelled action.
void check_roar() {
  helpers::Pokemon roarer = SNORLAX;
  roarer.moves[0] = Move::Roar;
  roarer.moves[1] = Move::Whirlwind;
  const std::vector<helpers::Pokemon> foes = {IDLE, JOLTEON, DUGTRIO};
  const auto after = [&](uint8_t slot, uint8_t target, auto&& setup) {
    Battle b = started({roarer}, foes);
    setup(b);
    Observed o{};
    o.calc.overrides.p1.switch_target = target;
    update(b, Choice::move(slot), Choice::move(1), o);
    return std::pair{b, o};
  };
  const auto none = [](Battle&) {};
  // A forced pick: Dugtrio comes in, one of two choices.
  {
    auto [b, o] = after(1, 3, none);
    CHECK(b.sides[1].get(1).species == Species::Dugtrio);
    CHECK(b.sides[1].get(3).species == Species::Snorlax);
    CHECK(o.chance.actions.p1.switch_target == 3);
    o.chance.probability.reduce();
    CHECK(o.chance.probability.p == 1 && o.chance.probability.q == 2);
  }
  // A fainted Pokémon can't be picked; Spikes hit the one dragged in.
  {
    auto [b, o] = after(1, 0, [](Battle& b) {
      b.sides[1].get(3).hp = 0;
      b.sides[1].conditions.spikes = 1;
    });
    const Pokemon& in = b.sides[1].get(1);
    CHECK(in.species == Species::Jolteon);
    CHECK(in.hp == in.stats.hp - in.stats.hp / 8);
    CHECK(o.chance.actions.p1.switch_target == 2);
  }
  // Suction Cups, Ingrain and Soundproof stop it; each uses PP.
  // Whirlwind isn't a sound move.
  {
    const auto stays = [&](uint8_t slot, auto&& setup) {
      auto [b, o] = after(slot, 0, setup);
      return b.sides[1].get(1).species == Species::Snorlax &&
             o.chance.actions.p1.switch_target == 0 &&
             b.sides[0].get(1).moves[slot - 1].pp == max_pp(roarer.moves[slot - 1]) - 1;
    };
    CHECK(stays(1, [](Battle& b) { b.sides[1].active.ability = Ability::SuctionCups; }));
    CHECK(stays(1, [](Battle& b) { b.sides[1].active.volatiles.rooted = true; }));
    CHECK(stays(1, [](Battle& b) { b.sides[1].active.ability = Ability::Soundproof; }));
    CHECK(!stays(2, [](Battle& b) { b.sides[1].active.ability = Ability::Soundproof; }));
  }
  // Evasion can make it miss: a forced miss drags nothing.
  {
    Battle b = started({roarer}, foes);
    b.sides[1].active.boosts.eva = 6;
    Observed o{};
    o.calc.overrides.p1.hit = Optional::False;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[1].get(1).species == Species::Snorlax);
    CHECK(o.chance.actions.p1.hit == Optional::False);
    CHECK(o.chance.actions.p1.switch_target == 0);
  }
  // No bench: it fails.
  {
    Battle b = started({roarer}, {IDLE});
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].get(1).species == Species::Snorlax);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Roar) - 1);
  }
  // Both Roar: the faster drags the slower out, whose Roar is cancelled.
  {
    helpers::Pokemon fast = JOLTEON;
    fast.moves[0] = Move::Roar;
    helpers::Pokemon slow = SNORLAX;
    slow.moves[0] = Move::Roar;
    Battle b = started({fast, DUGTRIO}, {slow, DUGTRIO});
    Observed o{};
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[0].get(1).species == Species::Jolteon);
    CHECK(b.sides[1].get(1).species == Species::Dugtrio);
    CHECK(b.sides[1].get(2).moves[0].pp == max_pp(Move::Roar));
    CHECK(o.chance.actions.p2.switch_target == 0);
  }
}

// Baton Pass: the request and its resume at the foe's move, what passes,
// passing last, failing with no bench, and a recipient fainting to Spikes.
void check_baton_pass() {
  helpers::Pokemon passer = JOLTEON;
  passer.moves[0] = Move::BatonPass;
  // The faster passer: P1 is asked for the recipient, P2's move waits.
  {
    Battle b = started({passer, DUGTRIO}, {IDLE});
    ActivePokemon& a = b.sides[0].active;
    a.boosts.atk = 2;
    a.volatiles.substitute = true;
    a.volatiles.substitute_hp = 20;
    a.volatiles.confusion = 3;
    a.volatiles.focus_energy = true;
    a.volatiles.escape_prevention = true; // Baton Pass ignores trapping
    // The passer's lock on the foe passes with its count; the foe's on the
    // passer stays on the foe but no longer applies.
    a.volatiles.lock_on = uint8_t(2 | b.sides[1].order[0] << 2);
    a.volatiles.infatuated = true;
    b.sides[0].get(1).status = Status::tox();
    a.volatiles.status_counter = 3;
    b.sides[1].active.volatiles.lock_on = uint8_t(2 | b.sides[0].order[0] << 2);
    Observed o{};
    o.chance.durations.p1.confusion = 2;
    o.calc.overrides.p1.confusion = Optional::False;
    Result r = update(b, Choice::move(1), Choice::move(1), o);
    CHECK(r == Result::init(Result::None, Choice::Switch, Choice::Pass));
    CHECK(b.request.phase == Phase::BatonPass && b.request.next == 1);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::BatonPass) - 1);
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::Splash));
    std::array<Choice, MAX_CHOICES> out{};
    CHECK(choices(b, Player::P1, r.p1(), out) == 1 && out[0] == Choice::swap(2));
    r = update(b, Choice::swap(2), Choice::pass(), o);
    CHECK(r == Result::start() && b.turn == 2);
    const ActivePokemon& in = b.sides[0].active;
    CHECK(in.species == Species::Dugtrio);
    CHECK(in.boosts.atk == 2);
    CHECK(in.volatiles.substitute && in.volatiles.substitute_hp == 20);
    // Confusion stepped once, on Baton Pass itself.
    CHECK(in.volatiles.confusion == 2 && o.chance.durations.p1.confusion == 3);
    CHECK(in.volatiles.focus_energy && in.volatiles.escape_prevention);
    CHECK(in.volatiles.lock_on == uint8_t(1 | b.sides[1].order[0] << 2));
    CHECK(!in.volatiles.infatuated && in.volatiles.status_counter == 0);
    CHECK(b.sides[1].active.volatiles.lock_on % 4 == 1);
    CHECK(!detail::locked_on(b, Player::P2));
    // The foe's Splash ran after the switch.
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::Splash) - 1);
  }
  // The slower passer: the request comes last and the end of turn follows.
  {
    Battle b = started({passer, DUGTRIO}, {IDLE});
    b.sides[0].active.stats.spe = 1;
    Null null{};
    Result r = update(b, Choice::move(1), Choice::move(1), null);
    CHECK(r == Result::init(Result::None, Choice::Switch, Choice::Pass));
    CHECK(b.request.next == Request::END_OF_ACTIONS);
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::Splash) - 1);
    r = update(b, Choice::swap(2), Choice::pass(), null);
    CHECK(r == Result::start() && b.turn == 2);
    CHECK(b.sides[0].active.species == Species::Dugtrio);
  }
  // No bench: it fails and uses PP, and the turn goes on.
  {
    Battle b = started({passer}, {IDLE});
    Null null{};
    const Result r = update(b, Choice::move(1), Choice::move(1), null);
    CHECK(r == Result::start() && b.turn == 2);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::BatonPass) - 1);
  }
  // The recipient faints to Spikes: a replacement, and the foe's move is
  // cancelled.
  {
    Battle b = started({passer, DUGTRIO, SNORLAX}, {IDLE});
    b.sides[0].conditions.spikes = 1;
    b.sides[0].get(2).hp = 1;
    Null null{};
    Result r = update(b, Choice::move(1), Choice::move(1), null);
    r = update(b, Choice::swap(2), Choice::pass(), null);
    CHECK(r == Result::init(Result::None, Choice::Switch, Choice::Pass));
    CHECK(b.request.phase == Phase::FaintAfterAction);
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::Splash));
    r = update(b, Choice::swap(3), Choice::pass(), null);
    CHECK(r == Result::start() && b.turn == 2);
    CHECK(b.sides[0].active.species == Species::Snorlax);
  }
}

// Wish: it lands at the end of the next turn, fails while pending, and heals
// whoever is active then by its own max HP / 2.
void check_wish() {
  helpers::Pokemon wisher = SNORLAX;
  wisher.moves[0] = Move::Wish;
  Null null{};
  {
    Battle b = started({wisher}, {IDLE});
    Pokemon& mon = b.sides[0].get(1);
    mon.hp = 1;
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].conditions.wish == 1 && b.sides[0].conditions.wisher == 1);
    CHECK(mon.hp == 1);
    // A second Wish fails (and uses PP); the first lands.
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(mon.moves[0].pp == max_pp(Move::Wish) - 2);
    CHECK(b.sides[0].conditions.wish == 0 && b.sides[0].conditions.wisher == 0);
    CHECK(mon.hp == 1 + mon.stats.hp / 2);
  }
  // The Pokémon that switched in takes it, by its own max HP.
  {
    Battle b = started({wisher, JOLTEON}, {IDLE});
    update(b, Choice::move(1), Choice::move(1), null);
    Pokemon& jolteon = b.sides[0].get(2);
    jolteon.hp = 1;
    update(b, Choice::swap(2), Choice::move(1), null);
    CHECK(b.sides[0].get(1).species == Species::Jolteon);
    CHECK(b.sides[0].get(1).hp == 1 + b.sides[0].get(1).stats.hp / 2);
  }
}

// Leech Seed: it seeds (and the residual drains), it fails into a
// Substitute, a Grass type or an existing seed with no draw, and it can
// miss.
void check_leech_seed() {
  helpers::Pokemon seeder = JOLTEON;
  seeder.moves[0] = Move::LeechSeed;
  const auto after = [&](helpers::Pokemon foe, Optional hit, auto&& setup) {
    Battle b = started({seeder}, {foe});
    setup(b);
    b.sides[0].get(1).hp = 1;
    Observed o{};
    o.calc.overrides.p1.hit = hit;
    update(b, Choice::move(1), Choice::move(1), o);
    return std::pair{b, o};
  };
  const auto none = [](Battle&) {};
  {
    auto [b, o] = after(IDLE, Optional::True, none);
    const Pokemon& foe = b.sides[1].get(1);
    CHECK(b.sides[1].active.volatiles.leech_seed);
    CHECK(foe.hp == foe.stats.hp - foe.stats.hp / 8);
    CHECK(b.sides[0].get(1).hp == 1 + foe.stats.hp / 8);
    CHECK(o.chance.actions.p1.hit == Optional::True);
  }
  {
    auto [b, o] = after(IDLE, Optional::False, none);
    CHECK(!b.sides[1].active.volatiles.leech_seed);
    CHECK(o.chance.actions.p1.hit == Optional::False);
  }
  helpers::Pokemon grass = IDLE;
  grass.species = Species::Venusaur;
  grass.ability = Ability::Overgrow;
  {
    auto [b, o] = after(grass, Optional::None, none);
    CHECK(!b.sides[1].active.volatiles.leech_seed);
    CHECK(o.chance.actions.p1.hit == Optional::None);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::LeechSeed) - 1);
  }
  {
    auto [b, o] = after(IDLE, Optional::None, [](Battle& b) {
      b.sides[1].active.volatiles.substitute = true;
      b.sides[1].active.volatiles.substitute_hp = 10;
    });
    CHECK(!b.sides[1].active.volatiles.leech_seed);
    CHECK(o.chance.actions.p1.hit == Optional::None);
  }
  {
    auto [b, o] = after(IDLE, Optional::None, [](Battle& b) {
      b.sides[1].active.volatiles.leech_seed = true;
    });
    CHECK(o.chance.actions.p1.hit == Optional::None);
  }
}

// Refresh cures poison, burn and paralysis and fails otherwise; Heal Bell
// and Aromatherapy cure the whole side, Heal Bell sparing Soundproof ones,
// and reset the cured sleep counts; a Sleep Talk turn kept for switch-out
// doesn't outlive the cure.
void check_cures() {
  helpers::Pokemon user = SNORLAX;
  user.moves[0] = Move::Refresh;
  user.moves[1] = Move::HealBell;
  user.moves[2] = Move::Aromatherapy;
  user.moves[3] = Move::SleepTalk;
  user.ability = Ability::ThickFat; // not Immunity, which cures poison itself
  helpers::Pokemon proof = JOLTEON;
  proof.species = Species::Exploud;
  proof.ability = Ability::Soundproof;
  {
    Battle b = started({user}, {IDLE});
    b.sides[0].get(1).status = Status::tox();
    b.sides[0].active.volatiles.status_counter = 4;
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(!b.sides[0].get(1).status.any() && b.sides[0].active.volatiles.status_counter == 0);
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Refresh) - 2);
  }
  {
    Battle b = started({user}, {IDLE});
    b.sides[0].get(1).status = Status::slp(3);
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].get(1).status.asleep()); // Refresh fails
  }
  for (const uint8_t slot : {2, 3}) {
    Battle b = started({user, JOLTEON, proof}, {IDLE});
    b.sides[0].get(1).status = Status::par();
    b.sides[0].get(2).status = Status::slp(3);
    b.sides[0].get(3).status = Status::brn();
    Observed o{};
    o.chance.durations.p1.sleep[1] = 2;
    update(b, Choice::move(slot), Choice::move(1), o);
    CHECK(!b.sides[0].get(1).status.any() && !b.sides[0].get(2).status.any());
    CHECK(o.chance.durations.p1.sleep[1] == 0);
    CHECK(b.sides[0].get(3).status.burned() == (slot == 2));
  }
  // Asleep, Sleep Talk calls Heal Bell: the kept turn goes with the sleep.
  {
    Battle b = started({user, JOLTEON}, {IDLE});
    b.sides[0].get(1).status = Status::slp(3);
    Observed o{};
    o.calc.overrides.p1.move_slot = 2; // Heal Bell
    update(b, Choice::move(4), Choice::move(1), o);
    CHECK(!b.sides[0].get(1).status.any());
    CHECK(b.sides[0].active.volatiles.status_counter == 0);
  }
}

// Counter and Mirror Coat: twice the HP the foe's physical or special move
// took, failing otherwise; Hidden Power counts as physical whatever its
// type; Ghost is immune to Counter.
void check_counter() {
  helpers::Pokemon user = SNORLAX;
  user.moves[0] = Move::Counter;
  user.moves[1] = Move::MirrorCoat;
  helpers::Pokemon slammer = SNORLAX;
  slammer.moves[2] = Move::HiddenPower;
  slammer.ivs = {31, 30, 31, 30, 30, 31}; // Fire
  // Returns the user's HP lost and the foe's.
  const auto run = [&](helpers::Pokemon foe, uint8_t slot, uint8_t foe_slot) {
    Battle b = started({user}, {foe});
    Observed o{};
    o.calc.overrides.p2.hits[0].secondary = Optional::False;
    o.calc.overrides.p2.hits[0].crit = Optional::False;
    update(b, Choice::move(slot), Choice::move(foe_slot), o);
    const Pokemon& u = b.sides[0].get(1);
    const Pokemon& f = b.sides[1].get(1);
    return std::pair{u.stats.hp - u.hp, f.stats.hp - f.hp};
  };
  CHECK(started({user}, {slammer}).sides[1].get(1).moves[2].id == Move::HiddenPowerFire);
  {
    auto [lost, dealt] = run(slammer, 1, 1); // Counter, Body Slam
    CHECK(lost > 0 && dealt == 2 * lost);
    std::tie(lost, dealt) = run(slammer, 2, 1); // Mirror Coat, Body Slam
    CHECK(lost > 0 && dealt == 0);
    std::tie(lost, dealt) = run(JOLTEON, 2, 1); // Mirror Coat, Thunderbolt
    CHECK(lost > 0 && dealt == 2 * lost);
    std::tie(lost, dealt) = run(JOLTEON, 1, 1); // Counter, Thunderbolt
    CHECK(lost > 0 && dealt == 0);
    std::tie(lost, dealt) = run(slammer, 1, 3); // Counter, Hidden Power Fire
    CHECK(lost > 0 && dealt == 2 * lost);
    std::tie(lost, dealt) = run(slammer, 2, 3); // Mirror Coat, Hidden Power
    CHECK(lost > 0 && dealt == 0);
  }
  // No damage taken: it fails and uses PP.
  {
    Battle b = started({user}, {IDLE});
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].get(1).hp == b.sides[1].get(1).stats.hp);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Counter) - 1);
  }
  // A Ghost type is immune to Counter.
  {
    helpers::Pokemon ghost = slammer;
    ghost.species = Species::Gengar;
    ghost.ability = Ability::Levitate;
    auto [lost, dealt] = run(ghost, 1, 1); // Counter, Body Slam
    CHECK(lost > 0 && dealt == 0);
  }
}

// Giga Drain: half the HP dealt (rounded down, at least 1), half a
// Substitute's damage rounded up, and Liquid Ooze turning it into damage.
void check_drain() {
  helpers::Pokemon drainer = JOLTEON;
  drainer.moves[0] = Move::GigaDrain;
  const auto run = [&](helpers::Pokemon foe, auto&& setup) {
    Battle b = started({drainer}, {foe});
    b.sides[0].get(1).hp = 100;
    setup(b);
    Observed o{};
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    update(b, Choice::move(1), Choice::move(1), o);
    return b;
  };
  {
    Battle b = run(IDLE, [](Battle&) {});
    const Pokemon& f = b.sides[1].get(1);
    const uint32_t dealt = f.stats.hp - f.hp;
    CHECK(dealt > 1 && b.sides[0].get(1).hp == 100 + dealt / 2);
  }
  {
    Battle b = run(IDLE, [](Battle& b) {
      b.sides[1].active.volatiles.substitute = true;
      b.sides[1].active.volatiles.substitute_hp = 5;
    });
    CHECK(b.sides[0].get(1).hp == 103); // ceil(5 / 2)
  }
  {
    helpers::Pokemon ooze = IDLE;
    ooze.species = Species::Tentacruel;
    ooze.ability = Ability::LiquidOoze;
    Battle b = run(ooze, [](Battle&) {});
    const Pokemon& f = b.sides[1].get(1);
    const uint32_t dealt = f.stats.hp - f.hp;
    CHECK(dealt > 1 && b.sides[0].get(1).hp == 100 - dealt / 2);
  }
}

// Taunt: two turns of no status moves (not offered, and stopped if chosen
// before it landed), failing on a Taunted target.
void check_taunt() {
  helpers::Pokemon taunter = JOLTEON;
  taunter.moves[0] = Move::Taunt;
  taunter.moves[1] = Move::Thunderbolt;
  helpers::Pokemon target = SNORLAX;
  target.moves[2] = Move::Splash;
  Null null{};
  Battle b = started({taunter}, {target});
  // Faster Taunt stops this turn's Splash, without PP.
  update(b, Choice::move(1), Choice::move(3), null);
  CHECK(b.sides[1].active.volatiles.taunt == 1);
  CHECK(b.sides[1].get(1).moves[2].pp == max_pp(Move::Splash));
  std::array<Choice, MAX_CHOICES> out{};
  const uint8_t n = choices(b, Player::P2, Choice::Move, out);
  bool splash = false;
  for (uint8_t i = 0; i < n; i++) splash |= out[i] == Choice::move(3);
  CHECK(!splash);
  // A second Taunt fails; then it runs out.
  update(b, Choice::move(1), Choice::move(1), null);
  CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Taunt) - 2);
  CHECK(b.sides[1].active.volatiles.taunt == 0);
}

// Encore: fails with no last move or on an encored target; a target that
// hasn't moved yet uses the encored move this turn; only that slot is
// offered until the hidden 3-6 turn timer runs out. A full paralysis keeps
// the last move (Showdown's lastMove).
void check_encore() {
  helpers::Pokemon encorer = JOLTEON;
  encorer.moves[0] = Move::Encore;
  encorer.moves[1] = Move::Splash;
  helpers::Pokemon target = SNORLAX;
  target.moves[2] = Move::Splash;
  Observed o{};
  Battle b = started({encorer}, {target});
  const auto pp = [&](uint8_t slot) { return b.sides[1].get(1).moves[slot].pp; };
  const auto only = [&](Choice c) {
    std::array<Choice, MAX_CHOICES> out{};
    const uint8_t n = choices(b, Player::P2, Choice::Move, out);
    return n == 1 && out[0] == c;
  };
  // Faster, before the target has a last move: fails.
  update(b, Choice::move(1), Choice::move(3), o);
  CHECK(b.sides[1].active.volatiles.encored_move == Move::None);
  CHECK(b.sides[1].active.volatiles.last_move == Move::Splash);
  // Now it works, and Body Slam turns into Splash this turn.
  o.calc.overrides.p1.hit = Optional::True;
  update(b, Choice::move(1), Choice::move(1), o);
  CHECK(b.sides[1].active.volatiles.encored_move == Move::Splash);
  CHECK(pp(0) == max_pp(Move::BodySlam));
  CHECK(pp(2) == max_pp(Move::Splash) - 2);
  CHECK(o.chance.durations.p2.encore == 2);
  CHECK(only(Choice::move(3)));
  // A second Encore fails; the timer runs out within 6 ends of turn.
  update(b, Choice::move(1), Choice::move(3), o);
  CHECK(b.sides[1].active.volatiles.encored_move == Move::Splash);
  for (int i = 0; i < 4 && b.sides[1].active.volatiles.encore_timer != 0; i++) {
    CHECK(only(Choice::move(3)));
    update(b, Choice::move(2), Choice::move(3), o);
  }
  CHECK(b.sides[1].active.volatiles.encore_timer == 0);
  CHECK(b.sides[1].active.volatiles.encored_move == Move::None);
  CHECK(o.chance.durations.p2.encore == 0);
  CHECK(!only(Choice::move(3)));

  // Fully paralyzed: the last move stays Splash, so Encore still works.
  b.sides[1].get(1).status = Status::par();
  o.calc.overrides = {};
  o.calc.overrides.p2.paralysis = Optional::True;
  update(b, Choice::move(2), Choice::move(1), o);
  CHECK(b.sides[1].active.volatiles.last_move == Move::Splash);
  o.calc.overrides.p1.hit = Optional::True;
  update(b, Choice::move(1), Choice::move(1), o);
  CHECK(b.sides[1].active.volatiles.encored_move == Move::Splash);
}

// Solar Beam: charges a turn (PP spent, the move locked, Protect no help),
// then hits with no PP; in sun it hits at once; a full paralysis on the
// second turn loses it.
void check_solar_beam() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::SolarBeam;
  user.moves[1] = Move::SunnyDay;
  helpers::Pokemon target = SNORLAX;
  target.moves[2] = Move::Splash;
  target.moves[3] = Move::Protect;
  Observed o{};
  o.calc.overrides.p1.hit = Optional::True;
  const uint8_t pp = max_pp(Move::SolarBeam);
  Battle b = started({user}, {target});
  const Pokemon& t = b.sides[1].get(1);
  const ActivePokemon& a = b.sides[0].active;
  const auto forced = [&] {
    std::array<Choice, MAX_CHOICES> out{};
    const uint8_t n = choices(b, Player::P1, Choice::Move, out);
    return n == 1 && out[0] == Choice::move(0);
  };
  // Charging through Protect.
  update(b, Choice::move(1), Choice::move(4), o);
  CHECK(t.hp == t.stats.hp);
  CHECK(a.volatiles.multiple_turns() && a.volatiles.locked_move == Move::SolarBeam);
  CHECK(b.sides[0].get(1).moves[0].pp == pp - 1);
  CHECK(forced());
  update(b, Choice::move(0), Choice::move(3), o);
  CHECK(t.hp < t.stats.hp);
  CHECK(!a.volatiles.multiple_turns());
  CHECK(b.sides[0].get(1).moves[0].pp == pp - 1);
  CHECK(!forced());
  // Protect on the second turn blocks it.
  update(b, Choice::move(1), Choice::move(3), o);
  const uint16_t hp = t.hp;
  update(b, Choice::move(0), Choice::move(4), o);
  CHECK(t.hp == hp && !a.volatiles.multiple_turns());
  // A full paralysis on the second turn loses the charge.
  b.sides[0].get(1).status = Status::par();
  o.calc.overrides.p1.paralysis = Optional::False;
  update(b, Choice::move(1), Choice::move(3), o);
  CHECK(a.volatiles.multiple_turns());
  o.calc.overrides.p1.paralysis = Optional::True;
  update(b, Choice::move(0), Choice::move(3), o);
  CHECK(t.hp == hp && !a.volatiles.multiple_turns() && !forced());
  // In sun it hits at once.
  b.sides[0].get(1).status = Status::none();
  o.calc.overrides.p1.paralysis = Optional::None;
  update(b, Choice::move(2), Choice::move(3), o);
  update(b, Choice::move(1), Choice::move(3), o);
  CHECK(t.hp < hp && !a.volatiles.multiple_turns());
}

// Dig: underground for a turn (the foe's move misses, no PP on the second
// turn); a charge whose second turn never comes (a faint ends the actions
// first) ends with that turn, as Showdown's twoturnmove. Skull Bash raises
// Defense as it charges.
void check_semi_invulnerable() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::Dig;
  user.moves[1] = Move::SkullBash;
  helpers::Pokemon target = SNORLAX;
  target.moves[2] = Move::Explosion;
  target.moves[3] = Move::Splash;
  Observed o{};
  o.calc.overrides.p1.hit = Optional::True;
  const uint8_t pp = max_pp(Move::Dig);
  Battle b = started({user}, {target, SNORLAX});
  const Pokemon& u = b.sides[0].get(1);
  const ActivePokemon& a = b.sides[0].active;
  const auto forced = [&] {
    std::array<Choice, MAX_CHOICES> out{};
    const uint8_t n = choices(b, Player::P1, Choice::Move, out);
    return n == 1 && out[0] == Choice::move(0);
  };
  update(b, Choice::move(1), Choice::move(1), o);
  CHECK(a.volatiles.kind == Volatiles::Kind::Underground && forced());
  CHECK(u.hp == u.stats.hp);
  const uint16_t hp = b.sides[1].get(1).hp;
  update(b, Choice::move(0), Choice::move(4), o);
  CHECK(a.volatiles.kind != Volatiles::Kind::Underground && !forced());
  CHECK(b.sides[1].get(1).hp < hp);
  CHECK(u.moves[0].pp == pp - 1);
  // Slower now: the foe's Explosion misses and ends the actions, so the
  // second turn never comes, and the charge ends with the turn.
  b.sides[0].active.stats.spe = 1;
  update(b, Choice::move(1), Choice::move(4), o);
  CHECK(a.volatiles.kind == Volatiles::Kind::Underground);
  const Result r = update(b, Choice::move(0), Choice::move(3), o);
  CHECK(b.sides[1].get(1).fainted() && r.p2() == Choice::Switch);
  CHECK(u.hp == u.stats.hp);
  update(b, Choice::pass(), Choice::swap(2), o);
  CHECK(a.volatiles.kind != Volatiles::Kind::Underground && !a.volatiles.multiple_turns() && !forced());
  // Skull Bash: Defense +1 on the charge turn.
  update(b, Choice::move(2), Choice::move(1), o);
  CHECK(a.boosts.def == 1 && forced());
}

// Hyper Beam: a hit makes the user recharge the next turn (no PP, no
// switch); a miss doesn't.
void check_recharge() {
  helpers::Pokemon user = SNORLAX;
  user.moves[2] = Move::HyperBeam;
  Observed o{};
  const uint8_t pp = max_pp(Move::HyperBeam);
  Battle b = started({user, JOLTEON}, {SNORLAX});
  const Pokemon& t = b.sides[1].get(1);
  const ActivePokemon& a = b.sides[0].active;
  const auto only_recharge = [&] {
    std::array<Choice, MAX_CHOICES> out{};
    const uint8_t n = choices(b, Player::P1, Choice::Move, out);
    return n == 1 && out[0] == Choice::move(0);
  };
  o.calc.overrides.p1.hit = Optional::False;
  update(b, Choice::move(3), Choice::move(2), o);
  CHECK(a.volatiles.kind != Volatiles::Kind::Recharge && !only_recharge());
  o.calc.overrides.p1.hit = Optional::True;
  o.calc.overrides.p1.hits[0].crit = Optional::False;
  update(b, Choice::move(3), Choice::move(2), o);
  CHECK(a.volatiles.kind == Volatiles::Kind::Recharge && only_recharge());
  const uint16_t hp = t.hp;
  update(b, Choice::move(0), Choice::move(2), o);
  CHECK(t.hp == hp && a.volatiles.kind != Volatiles::Kind::Recharge && !only_recharge());
  CHECK(b.sides[0].get(1).moves[2].pp == pp - 2);
}

// Destiny Bond: the foe that KOs the user faints too (both last: a tie);
// the user's next move attempt ends it.
void check_destiny_bond() {
  helpers::Pokemon user = JOLTEON;
  user.moves[1] = Move::DestinyBond;
  Observed o{};
  o.calc.overrides.p2.hit = Optional::True;
  {
    Battle b = started({user}, {SNORLAX});
    b.sides[0].get(1).hp = 1;
    const Result r = update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[0].get(1).fainted() && b.sides[1].get(1).fainted());
    CHECK(r.type() == Result::Tie);
  }
  {
    Battle b = started({user, DUGTRIO}, {SNORLAX, DUGTRIO});
    update(b, Choice::move(2), Choice::move(2), o);
    CHECK(b.sides[0].active.volatiles.kind == Volatiles::Kind::DestinyBond);
    b.sides[0].get(1).hp = 1;
    // Thunderbolt first ends it, so the KO costs Snorlax nothing.
    o.calc.overrides.p1.hit = Optional::True;
    const Result r = update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[0].get(1).fainted() && !b.sides[1].get(1).fainted());
    CHECK(r.p1() == Choice::Switch && r.p2() == Choice::Pass);
  }
}

// 2-5 hit moves: each hit draws its own crit and roll; a contact ability
// acts per hit; the loop stops when the target faints, and goes on after
// Endure (Showdown).
void check_multi_hit() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::RockBlast;
  user.moves[1] = Move::ArmThrust;
  helpers::Pokemon target = SNORLAX;
  target.moves[2] = Move::Splash;
  target.moves[3] = Move::Endure;
  const auto run = [&](Battle& b, uint8_t move, uint8_t foe_move, uint8_t n) {
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    o.calc.overrides.p1.multi_hit = n;
    o.calc.overrides.p2.protect = Optional::True;
    for (uint8_t i = 0; i < MAX_HITS; i++) {
      o.calc.overrides.p1.hits[i].crit = Optional::False;
      o.calc.overrides.p1.hits[i].damage = 100;
      o.calc.overrides.p1.hits[i].contact = Optional::False;
    }
    update(b, Choice::move(move), Choice::move(foe_move), o);
    return o.chance.actions.p1;
  };
  {
    Battle b = started({user}, {target});
    const Pokemon& t = b.sides[1].get(1);
    const Action a = run(b, 1, 3, 4);
    CHECK(a.multi_hit == 4 && a.hits[3].damage == 100 && a.hits[4].damage == 0);
    const uint16_t four = uint16_t(t.stats.hp - t.hp);
    CHECK(four % 4 == 0 && four != 0);
  }
  {
    Battle b = started({user}, {target});
    b.sides[1].active.ability = Ability::RoughSkin;
    const Pokemon& u = b.sides[0].get(1);
    CHECK(run(b, 2, 3, 3).multi_hit == 3);
    CHECK(u.stats.hp - u.hp == 3 * (u.stats.hp / 16));
  }
  {
    Battle b = started({user}, {target, DUGTRIO});
    b.sides[1].get(1).hp = 1;
    // Stopped at hit 1, and every roll is lethal: nothing to record.
    CHECK(run(b, 1, 3, 5).multi_hit == 0);
    CHECK(b.sides[1].get(1).fainted());
  }
  {
    // Endure holds at 1 HP and the remaining hits still come.
    Battle b = started({user}, {target});
    b.sides[1].get(1).hp = 2;
    CHECK(run(b, 1, 4, 5).multi_hit == 5);
    CHECK(b.sides[1].get(1).hp == 1);
  }
  {
    // A faint on hit k makes every count from k up one outcome, count >= k;
    // without a stop the count is exact. Each hit takes `per`.
    uint16_t per = 0;
    {
      Battle b = started({user}, {target});
      (void)run(b, 1, 3, 2);
      per = uint16_t((b.sides[1].get(1).stats.hp - b.sides[1].get(1).hp) / 2);
    }
    const auto key = [&](uint16_t hp, uint8_t n) {
      Battle b = started({user}, {target});
      b.sides[1].get(1).hp = hp;
      Observed o{};
      o.calc.overrides.p1.hit = Optional::True;
      o.calc.overrides.p1.multi_hit = n;
      for (uint8_t i = 0; i < MAX_HITS; i++) {
        o.calc.overrides.p1.hits[i].crit = Optional::False;
        o.calc.overrides.p1.hits[i].damage = 100;
      }
      update(b, Choice::move(1), Choice::move(3), o);
      o.chance.probability.reduce();
      return o.chance;
    };
    const auto at3 = key(uint16_t(2 * per + 1), 3);
    const auto at5 = key(uint16_t(2 * per + 1), 5);
    const auto exact3 = key(uint16_t(4 * per), 3);
    CHECK(at3.actions == at5.actions && at3.actions.p1.multi_hit == 3);
    CHECK(at3.probability.p == at5.probability.p &&
          at3.probability.q == at5.probability.q);
    CHECK(exact3.actions.p1.multi_hit == 3);
    // 5/8 against 3/8, and hit 3 on 1 HP is lethal whatever it rolls, where
    // the exact count's hit 3 records its crit (15/16) and roll (1/16).
    CHECK(at3.probability.p * exact3.probability.q * 3 * 15 ==
          exact3.probability.p * at3.probability.q * 5 * 256);
    // A faint by hit 2 leaves nothing to record.
    CHECK(key(uint16_t(per + 1), 4).actions.p1.multi_hit == 0);
    CHECK(key(uint16_t(per), 5).actions.p1.multi_hit == 0);
  }
}

// A hit that reaches its cap records only `lethal`, with the odds of any
// lethal (crit, roll), and nothing when every pair is lethal; Calc's lethal
// replays it.
void check_lethal() {
  Hit h = Hit::of(Move::QuickAttack);
  const Battle fresh = started({SNORLAX}, {IDLE});
  const uint32_t normal = damage(fresh, Player::P1, h);
  h.crit = true;
  const uint32_t crit = damage(fresh, Player::P1, h);
  // Some rolls KO without a crit, not all.
  const uint16_t hp = uint16_t(apply_roll(normal, 8));
  CHECK(apply_roll(normal, 0) < hp);
  uint64_t normals = 0, crits = 0;
  for (uint8_t k = 0; k < 16; k++) {
    normals += apply_roll(normal, k) >= hp;
    crits += apply_roll(crit, k) >= hp;
  }
  const auto run = [&](uint16_t foe_hp, uint8_t sub, Optional c, uint8_t roll, Optional lethal) {
    Battle b = fresh;
    b.sides[1].get(1).hp = foe_hp;
    b.sides[1].active.volatiles.substitute = sub != 0;
    b.sides[1].active.volatiles.substitute_hp = sub;
    Observed o{};
    o.calc.overrides.p1.hits[0] = {c, roll, lethal, Optional::None, Optional::None, 0,
                                   Optional::None, Optional::None};
    update(b, Choice::move(2), Choice::move(1), o);
    o.chance.probability.reduce();
    return std::pair{b, o.chance};
  };
  {
    const auto [b, c] = run(hp, 0, Optional::False, 100, Optional::None);
    const HitDraws& d = c.actions.p1.hits[0];
    CHECK(b.sides[1].get(1).fainted());
    CHECK(d.lethal == Optional::True && d.crit == Optional::None && d.damage == 0);
    CHECK(c.probability.p * 256 == (crits + 15 * normals) * c.probability.q);
    // Forcing lethal replays the same key.
    const auto [b2, c2] = run(hp, 0, Optional::None, 0, Optional::True);
    CHECK(b2.sides[1].get(1).fainted() && c2.actions == c.actions);
  }
  {
    const auto [b, c] = run(hp, 0, Optional::False, 85, Optional::None);
    const HitDraws& d = c.actions.p1.hits[0];
    CHECK(!b.sides[1].get(1).fainted());
    CHECK(d.lethal == Optional::None && d.crit == Optional::False && d.damage == 85);
  }
  {
    // Every pair lethal: nothing recorded.
    const auto [b, c] = run(uint16_t(apply_roll(normal, 0)), 0, Optional::None, 0, Optional::None);
    CHECK(b.sides[1].get(1).fainted() && c.actions == Actions{});
    CHECK(c.probability.p == 1 && c.probability.q == 1);
  }
  {
    // A Substitute's HP is the cap.
    const auto [b, c] = run(hp, 1, Optional::None, 0, Optional::None);
    CHECK(b.sides[1].get(1).hp == hp && b.sides[1].active.volatiles.substitute_hp == 0);
    CHECK(c.actions == Actions{});
  }
}

// A future attack's roll and Jump Kick's crash merge lethal pairs as a hit
// does: the use stores the damage before the roll and records nothing, the
// landing records the roll or lethal in Action::future; a crash that KOs its
// user records lethal in hits[1], or nothing when every pair does.
void check_lethal_landings() {
  const helpers::Pokemon seer = {
      .species = Species::Snorlax,
      .moves = {Move::FutureSight},
      .ability = Ability::Immunity,
      .nature = Nature::Adamant,
      .gender = Gender::Male,
  };
  const Battle fresh = started({seer}, {IDLE});
  const uint32_t base = damage(fresh, Player::P1, Hit::of(Move::FutureSight));
  const uint16_t hp = uint16_t(apply_roll(base, 8));
  CHECK(apply_roll(base, 0) < hp);
  const auto land = [&](uint16_t foe_hp, uint8_t roll) {
    Battle b = fresh;
    Observed used{};
    update(b, Choice::move(1), Choice::move(1), used);
    CHECK(b.sides[1].future_sight_damage == base && used.chance.actions.p1 == Action{});
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    b.sides[1].get(1).hp = foe_hp;
    Observed o{};
    o.calc.overrides.p1.future_hit = Optional::True;
    o.calc.overrides.p1.future.damage = roll;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[1].conditions.future_sight == 0);
    return std::pair{b, o.chance.actions.p1.future};
  };
  {
    const auto [b, f] = land(hp, 100);
    CHECK(b.sides[1].get(1).fainted() && f.lethal == Optional::True && f.damage == 0);
  }
  {
    const auto [b, f] = land(hp, 85);
    CHECK(b.sides[1].get(1).hp == hp - apply_roll(base, 0));
    CHECK(f.lethal == Optional::None && f.damage == 85);
  }

  const helpers::Pokemon kicker = {
      .species = Species::Snorlax,
      .moves = {Move::JumpKick},
      .ability = Ability::Immunity,
      .nature = Nature::Adamant,
      .gender = Gender::Male,
  };
  const Battle start = started({kicker}, {IDLE});
  const uint32_t kick = damage(start, Player::P1, Hit::of(Move::JumpKick));
  const uint16_t user_hp = uint16_t(apply_roll(kick, 8) / 2);
  CHECK(apply_roll(kick, 0) / 2 < user_hp && user_hp <= start.sides[1].get(1).stats.hp / 2);
  const auto crash = [&](uint16_t hp_, Optional crit, uint8_t roll) {
    Battle b = start;
    b.sides[0].get(1).hp = hp_;
    Observed o{};
    o.calc.overrides.p1.hit = Optional::False;
    o.calc.overrides.p1.hits[1].crit = crit;
    o.calc.overrides.p1.hits[1].damage = roll;
    update(b, Choice::move(1), Choice::move(1), o);
    return std::pair{b, o.chance.actions.p1.hits[1]};
  };
  {
    const auto [b, d] = crash(user_hp, Optional::False, 100);
    CHECK(b.sides[0].get(1).fainted());
    CHECK(d.lethal == Optional::True && d.crit == Optional::None && d.damage == 0);
  }
  {
    const auto [b, d] = crash(user_hp, Optional::False, 85);
    CHECK(b.sides[0].get(1).hp == user_hp - apply_roll(kick, 0) / 2);
    CHECK(d.lethal == Optional::None && d.crit == Optional::False && d.damage == 85);
  }
  {
    // Every pair lethal: nothing recorded.
    const auto [b, d] = crash(1, Optional::None, 0);
    CHECK(b.sides[0].get(1).fainted() && d == HitDraws{});
  }
}

// Mimic: Mimic's slot takes the target's last move with min(5, base PP),
// the party keeps Mimic, and it fails with no last move, on a Substitute,
// or on a move the user knows.
void check_mimic() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::Mimic;
  user.moves[1] = Move::Splash;
  helpers::Pokemon target = SNORLAX;
  target.moves[2] = Move::Splash;
  Null null{};
  {
    Battle b = started({user}, {target});
    // Faster, so the target has no last move yet.
    update(b, Choice::move(1), Choice::move(3), null);
    CHECK(b.sides[0].active.moves[0].id == Move::Mimic);
    // Quick Attack goes first and is copied.
    update(b, Choice::move(1), Choice::move(2), null);
    const MoveSlot& ms = b.sides[0].active.moves[0];
    CHECK(ms.id == Move::QuickAttack && ms.pp == 5);
    CHECK(b.sides[0].get(1).moves[0].id == Move::Mimic);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Mimic) - 2);
    // Using the copy spends its PP only.
    update(b, Choice::move(1), Choice::move(3), null);
    CHECK(b.sides[0].active.moves[0].pp == 4);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Mimic) - 2);
  }
  {
    // A copied Hidden Power is the user's own (Dark from 31s), and works.
    helpers::Pokemon hp = target;
    hp.moves[3] = Move::HiddenPower;
    hp.ivs = {31, 30, 31, 30, 30, 31}; // Fire
    Battle b = started({user}, {hp});
    update(b, Choice::move(2), Choice::move(4), null);
    update(b, Choice::move(1), Choice::move(3), null);
    CHECK(b.sides[0].active.moves[0].id == Move::HiddenPowerDark);
    const uint16_t foe = b.sides[1].get(1).hp;
    update(b, Choice::move(1), Choice::move(3), null);
    CHECK(b.sides[1].get(1).hp < foe);
  }
  {
    // A move the user knows can't be copied.
    Battle b = started({user}, {target});
    update(b, Choice::move(2), Choice::move(3), null);
    update(b, Choice::move(1), Choice::move(3), null);
    CHECK(b.sides[0].active.moves[0].id == Move::Mimic);
  }
  {
    Battle b = started({user}, {target});
    update(b, Choice::move(2), Choice::move(2), null);
    b.sides[1].active.volatiles.substitute = true;
    b.sides[1].active.volatiles.substitute_hp = 100;
    update(b, Choice::move(1), Choice::move(3), null);
    CHECK(b.sides[0].active.moves[0].id == Move::Mimic);
  }
}

// The last single-slot moves on teams/ou/: Water Spout, Gust, Fake Out,
// Pain Split, Memento, Imprison and Nightmare.
void check_misc_moves() {
  Observed o{};
  o.calc.overrides.p1.hit = Optional::True;
  for (uint8_t i = 0; i < MAX_HITS; i++) {
    o.calc.overrides.p1.hits[i].crit = Optional::False;
    o.calc.overrides.p1.hits[i].damage = 100;
  }
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::WaterSpout;
  user.moves[1] = Move::Gust;
  user.moves[2] = Move::FakeOut;
  user.moves[3] = Move::PainSplit;
  helpers::Pokemon target = SNORLAX;
  target.moves[2] = Move::Splash;
  target.moves[3] = Move::Rest;
  {
    // Water Spout's power falls with the user's HP.
    Battle full = started({user}, {target});
    Battle half = full;
    half.sides[0].get(1).hp = uint16_t(half.sides[0].get(1).stats.hp / 2);
    update(full, Choice::move(1), Choice::move(3), o);
    update(half, Choice::move(1), Choice::move(3), o);
    const Pokemon& a = full.sides[1].get(1);
    const Pokemon& b = half.sides[1].get(1);
    CHECK(a.hp < b.hp && b.hp < b.stats.hp);
  }
  {
    // Gust hits a target in the air, doubled.
    Battle ground = started({user}, {target});
    Battle air = ground;
    air.sides[1].active.volatiles.kind = Volatiles::Kind::OnAir;
    air.sides[1].active.volatiles.lock = 1;
    air.sides[1].active.volatiles.locked_move = Move::Fly;
    update(ground, Choice::move(2), Choice::move(3), o);
    update(air, Choice::move(2), Choice::move(0), o); // Fly's second turn
    const Pokemon& g = ground.sides[1].get(1);
    const Pokemon& f = air.sides[1].get(1);
    CHECK(f.stats.hp - f.hp >= 2 * (g.stats.hp - g.hp) - 1);
  }
  {
    // Fake Out flinches on the first turn, then fails.
    Battle b = started({user}, {target});
    update(b, Choice::move(3), Choice::move(1), o);
    const uint16_t hp = b.sides[0].get(1).hp;
    CHECK(hp == b.sides[0].get(1).stats.hp);
    const uint16_t foe = b.sides[1].get(1).hp;
    update(b, Choice::move(3), Choice::move(3), o);
    CHECK(b.sides[1].get(1).hp == foe);
  }
  {
    // Pain Split averages the HP.
    Battle b = started({user}, {target});
    b.sides[0].get(1).hp = 100;
    const uint16_t t = b.sides[1].get(1).hp;
    update(b, Choice::move(4), Choice::move(3), o);
    const uint16_t avg = uint16_t((100 + t) / 2);
    CHECK(b.sides[1].get(1).hp == avg);
    CHECK(b.sides[0].get(1).hp ==
          std::min<uint16_t>(avg, b.sides[0].get(1).stats.hp));
  }
  helpers::Pokemon other = JOLTEON;
  other.moves[0] = Move::Memento;
  other.moves[1] = Move::Imprison;
  other.moves[2] = Move::Nightmare;
  other.moves[3] = Move::Splash;
  {
    // Memento: -2 Atk and SpA, the user faints; into Protect it lives.
    Battle b = started({other, DUGTRIO}, {target});
    const Result r = update(b, Choice::move(1), Choice::move(3), o);
    CHECK(b.sides[0].get(1).fainted() && r.p1() == Choice::Switch);
    CHECK(b.sides[1].active.boosts.atk == -2 && b.sides[1].active.boosts.spa == -2);
    helpers::Pokemon protector = target;
    protector.moves[3] = Move::Protect;
    Battle c = started({other, DUGTRIO}, {protector});
    Observed p{};
    p.calc.overrides.p2.protect = Optional::True;
    update(c, Choice::move(1), Choice::move(4), p);
    CHECK(!c.sides[0].get(1).fainted() && c.sides[1].active.boosts.atk == 0);
  }
  {
    // Imprison: Splash is shared, so the foe can't pick it.
    Battle b = started({other}, {target});
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[0].active.volatiles.imprisoned_others);
    std::array<Choice, MAX_CHOICES> out{};
    const uint8_t n = choices(b, Player::P2, Choice::Move, out);
    for (uint8_t i = 0; i < n; i++) CHECK(!(out[i] == Choice::move(3)));
  }
  {
    // Nightmare fails on an awake foe, works on a sleeping one.
    Battle b = started({other}, {target});
    update(b, Choice::move(3), Choice::move(3), o);
    CHECK(!b.sides[1].active.volatiles.nightmare);
    b.sides[1].get(1).status = Status::slp(3);
    update(b, Choice::move(3), Choice::move(3), o);
    CHECK(b.sides[1].active.volatiles.nightmare);
  }
}

// The weather moves: 5 turns counting this one, replacing an ability's
// permanent weather, failing on the same weather.
void check_weather_moves() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::RainDance;
  user.moves[1] = Move::SunnyDay;
  Null null{};
  Battle b = started({user}, {IDLE});
  update(b, Choice::move(1), Choice::move(1), null);
  CHECK(b.field.weather == Weather::Rain && b.field.weather_turns == 4);
  update(b, Choice::move(1), Choice::move(1), null); // fails
  CHECK(b.field.weather_turns == 3);
  update(b, Choice::move(2), Choice::move(1), null); // replaces the rain
  CHECK(b.field.weather == Weather::Sun && b.field.weather_turns == 4);
  b.field = {Weather::Sun, 0, true};
  update(b, Choice::move(2), Choice::move(1), null);
  CHECK(b.field.weather == Weather::Sun && b.field.permanent);
  update(b, Choice::move(1), Choice::move(1), null);
  CHECK(b.field.weather == Weather::Rain && !b.field.permanent &&
        b.field.weather_turns == 4);
}

// Endeavor: the target drops to the user's HP; it fails at equal or lower
// HP and doesn't affect a Ghost type.
void check_endeavor() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::Endeavor;
  Null null{};
  {
    Battle b = started({user}, {IDLE});
    b.sides[0].get(1).hp = 30;
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].get(1).hp == 30);
    update(b, Choice::move(1), Choice::move(1), null); // fails
    CHECK(b.sides[1].get(1).hp == 30);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Endeavor) - 2);
  }
  {
    helpers::Pokemon ghost = IDLE;
    ghost.species = Species::Gengar;
    ghost.ability = Ability::Levitate;
    Battle b = started({user}, {ghost});
    b.sides[0].get(1).hp = 30;
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].get(1).hp == b.sides[1].get(1).stats.hp);
  }
}

// Beat Up, as Showdown: one hit per healthy party member in SLOT order,
// each ally's base Atk against the target's base Def, typeless special.
void check_beat_up() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::BeatUp;
  helpers::Pokemon statused = JOLTEON;
  statused.status = Status::par();
  helpers::Pokemon fainted = SNORLAX;
  fainted.hp = 0;
  Battle b = started({user, SNORLAX, statused, DUGTRIO, fainted}, {IDLE});
  Observed o{};
  for (HitDraws& h : o.calc.overrides.p1.hits) {
    h.crit = Optional::False;
    h.damage = 100;
  }
  update(b, Choice::move(1), Choice::move(1), o);
  const Pokemon& t = b.sides[1].get(1);
  CHECK(t.stats.hp - t.hp == 10 + 16 + 12);
  CHECK(o.chance.actions.p1.hits[2].damage == 100);
  CHECK(o.chance.actions.p1.hits[3].damage == 0);
  // Light Screen halves each hit (before the +2).
  b = started({user, SNORLAX, statused, DUGTRIO, fainted}, {IDLE});
  b.sides[1].conditions.light_screen = 5;
  update(b, Choice::move(1), Choice::move(1), o);
  CHECK(t.stats.hp - b.sides[1].get(1).hp == 6 + 9 + 7);
}

// Knock Off: the item becomes inert, a Choice Band lock ends, Sticky Hold
// and a Substitute keep it.
void check_knock_off() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::KnockOff;
  const auto run = [&](helpers::Pokemon foe, auto&& setup) {
    Battle b = started({user}, {foe});
    setup(b);
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    return b;
  };
  const auto none = [](Battle&) {};
  {
    Battle b = run(holding(IDLE, Item::Leftovers), [](Battle& b) {
      b.sides[1].active.volatiles.choice_move = Move::Splash;
    });
    CHECK(b.sides[1].get(1).item == Item::KnockedOff);
    CHECK(b.sides[1].active.volatiles.choice_move == Move::None);
    // Leftovers no longer heal.
    const Pokemon& t = b.sides[1].get(1);
    CHECK(t.hp < t.stats.hp);
  }
  {
    helpers::Pokemon sticky = holding(IDLE, Item::Leftovers);
    sticky.species = Species::Muk;
    sticky.ability = Ability::StickyHold;
    CHECK(run(sticky, none).sides[1].get(1).item == Item::Leftovers);
  }
  {
    Battle b = run(holding(IDLE, Item::Leftovers), [](Battle& b) {
      b.sides[1].active.volatiles.substitute = true;
      b.sides[1].active.volatiles.substitute_hp = 200;
    });
    CHECK(b.sides[1].get(1).item == Item::Leftovers);
  }
}

// Thief: an empty-handed user takes the item after a hit on the target
// itself, ending the target's Choice Band lock; a Choice Band taken locks
// the user into Thief, as Showdown.
// Sticky Hold, a Substitute or the user's own item stop it.
void check_thief() {
  helpers::Pokemon user = JOLTEON;
  user.item = Item::None;
  user.moves[0] = Move::Thief;
  const auto run = [&](helpers::Pokemon thief, helpers::Pokemon foe,
                       auto&& setup) {
    Battle b = started({thief}, {foe});
    setup(b);
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    return b;
  };
  const auto none = [](Battle&) {};
  {
    Battle b = run(user, holding(IDLE, Item::ChoiceBand), [](Battle& b) {
      b.sides[1].active.volatiles.choice_move = Move::Splash;
    });
    CHECK(b.sides[0].get(1).item == Item::ChoiceBand);
    CHECK(b.sides[0].active.volatiles.choice_move == Move::Thief);
    CHECK(b.sides[1].get(1).item == Item::None);
    CHECK(b.sides[1].active.volatiles.choice_move == Move::None);
  }
  CHECK(run(holding(user, Item::Leftovers), holding(IDLE, Item::ChoiceBand),
            none).sides[1].get(1).item == Item::ChoiceBand);
  {
    helpers::Pokemon sticky = holding(IDLE, Item::Leftovers);
    sticky.species = Species::Muk;
    sticky.ability = Ability::StickyHold;
    const Battle b = run(user, sticky, none);
    CHECK(b.sides[1].get(1).item == Item::Leftovers);
    CHECK(b.sides[0].get(1).item == Item::None);
  }
  {
    Battle b = run(user, holding(IDLE, Item::Leftovers), [](Battle& b) {
      b.sides[1].active.volatiles.substitute = true;
      b.sides[1].active.volatiles.substitute_hp = 200;
    });
    CHECK(b.sides[1].get(1).item == Item::Leftovers);
    CHECK(b.sides[0].get(1).item == Item::None);
  }
}

// Perish Song: both count 3, 2, 1, then faint at the fourth end of turn;
// both last Pokémon perishing is a tie, as Showdown.
void check_perish_song() {
  helpers::Pokemon singer = JOLTEON;
  singer.moves[0] = Move::PerishSong;
  singer.moves[1] = Move::Splash;
  Null null{};
  {
    Battle b = started({singer, DUGTRIO}, {IDLE, SNORLAX});
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].active.volatiles.perish_song == 3 && b.sides[1].active.volatiles.perish_song == 3);
    update(b, Choice::move(2), Choice::move(1), null);
    update(b, Choice::move(2), Choice::move(1), null);
    const Result r = update(b, Choice::move(2), Choice::move(1), null);
    CHECK(r == Result::init(Result::None, Choice::Switch, Choice::Switch));
    CHECK(b.sides[0].get(1).fainted() && b.sides[1].get(1).fainted());
  }
  {
    Battle b = started({singer}, {IDLE});
    Result r = Result::start();
    for (int i = 0; i < 4; i++) r = update(b, Choice::move(i == 0 ? 1 : 2), Choice::move(1), null);
    CHECK(r.type() == Result::Tie);
  }
}

// The stat-change batch: Screech, Metal Sound, Tickle (and Clear Body),
// Superpower and Overheat on the user, Haze, Psych Up (stages only).
void check_stat_changes() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::Screech;
  user.moves[1] = Move::Tickle;
  user.moves[2] = Move::Haze;
  user.moves[3] = Move::PsychUp;
  Observed o{};
  o.calc.overrides.p1.hit = Optional::True;
  {
    Battle b = started({user}, {IDLE});
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[1].active.boosts.def == -2);
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[1].active.boosts.atk == -1 && b.sides[1].active.boosts.def == -3);
    b.sides[1].active.boosts.spa = 2;
    b.sides[1].active.volatiles.focus_energy = true;
    update(b, Choice::move(4), Choice::move(1), o);
    CHECK(b.sides[0].active.boosts.def == -3 && b.sides[0].active.boosts.spa == 2);
    CHECK(!b.sides[0].active.volatiles.focus_energy);
    update(b, Choice::move(3), Choice::move(1), o);
    CHECK(b.sides[0].active.boosts.def == 0 && b.sides[1].active.boosts.def == 0);
  }
  {
    helpers::Pokemon clear = IDLE;
    clear.species = Species::Metagross;
    clear.ability = Ability::ClearBody;
    clear.gender = Gender::None;
    Battle b = started({user}, {clear});
    Observed none{};
    update(b, Choice::move(1), Choice::move(1), none);
    CHECK(b.sides[1].active.boosts.def == 0);
    // Recorded: a blocked drop still lands (last_taken_move).
    CHECK(none.chance.actions.p1.hit != Optional::None);
  }
  {
    // Tickle goes through a Substitute (neither the decomp nor Showdown's
    // gen3, bypasssub, checks one).
    Battle b = started({user}, {IDLE});
    b.sides[1].active.volatiles.substitute = true;
    b.sides[1].active.volatiles.substitute_hp = 10;
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[1].active.boosts.atk == -1 && b.sides[1].active.boosts.def == -1);
  }
  {
    // Into Protect the accuracy roll changes only the log (-miss or
    // -activate), so it isn't recorded.
    helpers::Pokemon protector = SNORLAX;
    protector.moves[0] = Move::Protect;
    Battle b = started({user}, {protector});
    b.sides[1].active.stats.spe = 999;
    Observed q{};
    update(b, Choice::move(1), Choice::move(1), q);
    CHECK(b.sides[1].active.boosts.def == 0);
    CHECK(q.chance.actions.p1.hit == Optional::None);
  }
  {
    helpers::Pokemon hitter = SNORLAX;
    hitter.moves[0] = Move::Superpower;
    hitter.moves[1] = Move::Overheat;
    Battle b = started({hitter}, {IDLE});
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[0].active.boosts.atk == -1 && b.sides[0].active.boosts.def == -1);
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[0].active.boosts.spa == -2);
  }
}

// Reflect and Light Screen; Return, Frustration and Reversal's power.
void check_screens_and_power() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::Reflect;
  user.moves[1] = Move::LightScreen;
  Null null{};
  {
    Battle b = started({user}, {IDLE});
    update(b, Choice::move(1), Choice::move(1), null);
    update(b, Choice::move(2), Choice::move(1), null);
    CHECK(b.sides[0].conditions.reflect == 3 && b.sides[0].conditions.light_screen == 4);
    update(b, Choice::move(1), Choice::move(1), null); // fails
    CHECK(b.sides[0].conditions.reflect == 2);
  }
  // Each power, through a forced max roll with no crit: the damage grows
  // with the power, so compare against a fixed-power move of the same type.
  const auto dealt = [&](helpers::Pokemon attacker, uint8_t slot, auto&& setup) {
    Battle b = started({attacker}, {IDLE});
    setup(b);
    Observed o{};
    o.calc.overrides.p1.hits[0] = {Optional::False, 100, Optional::None, Optional::None, Optional::None, 0, Optional::None, Optional::None};
    update(b, Choice::move(slot), Choice::move(1), o);
    const Pokemon& t = b.sides[1].get(1);
    return t.stats.hp - t.hp;
  };
  helpers::Pokemon normal = SNORLAX; // Body Slam (85), then the others
  normal.moves[1] = Move::Return;
  normal.moves[2] = Move::Frustration;
  const auto none = [](Battle&) {};
  CHECK(dealt(normal, 2, none) > dealt(normal, 1, none)); // Return 102
  CHECK(dealt(normal, 3, none) <= 5);                     // Frustration 1
  helpers::Pokemon fighter = SNORLAX;
  fighter.moves[0] = Move::Reversal;
  fighter.moves[1] = Move::BrickBreak; // 75
  const auto low = [](Battle& b) { b.sides[0].get(1).hp = 1; };
  const auto full = [](Battle&) {};
  CHECK(dealt(fighter, 1, low) > 2 * dealt(fighter, 2, low));  // 200
  CHECK(dealt(fighter, 1, full) < dealt(fighter, 2, full) / 2); // 20
}

// Mean Look: the target can't switch until the user leaves; Protect stops
// it (the decomp and Showdown's gen3 agree), and so does a Substitute.
void check_mean_look() {
  helpers::Pokemon user = JOLTEON;
  user.moves[0] = Move::MeanLook;
  helpers::Pokemon protector = SNORLAX;
  protector.moves[0] = Move::Protect;
  Null null{};
  {
    Battle b = started({user}, {protector});
    b.sides[1].active.stats.spe = 999; // Protect first
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(!b.sides[1].active.volatiles.escape_prevention);
  }
  {
    Battle b = started({user, DUGTRIO}, {IDLE, protector});
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].active.volatiles.escape_prevention);
    std::array<Choice, MAX_CHOICES> out{};
    const uint8_t n = choices(b, Player::P2, Choice::Move, out);
    for (uint8_t i = 0; i < n; i++) CHECK(out[i].type() != Choice::Switch);
    update(b, Choice::swap(2), Choice::move(1), null);
    CHECK(!b.sides[1].active.volatiles.escape_prevention);
  }
  {
    Battle b = started({user}, {IDLE});
    b.sides[1].active.volatiles.substitute = true;
    b.sides[1].active.volatiles.substitute_hp = 10;
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(!b.sides[1].active.volatiles.escape_prevention);
  }
}

// Substitute: its cost and HP, the two failures, freeing from a binding,
// and a hit that the Substitute takes.
void check_substitute() {
  helpers::Pokemon user = SNORLAX;
  user.moves[0] = Move::Substitute;
  const auto after = [&](helpers::Pokemon foe, auto&& setup) {
    Battle b = started({user}, {foe});
    setup(b);
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    return b;
  };
  const auto none = [](Battle&) {};
  {
    Battle b = after(IDLE, none);
    const Pokemon& mon = b.sides[0].get(1);
    CHECK(b.sides[0].active.volatiles.substitute);
    CHECK(b.sides[0].active.volatiles.substitute_hp == mon.stats.hp / 4);
    CHECK(mon.hp == mon.stats.hp - mon.stats.hp / 4);
    CHECK(mon.moves[0].pp == max_pp(Move::Substitute) - 1);
  }
  // At a quarter or less, or with one up, it fails at no cost.
  {
    Battle b = after(IDLE, [](Battle& b) {
      b.sides[0].get(1).hp = uint16_t(b.sides[0].get(1).stats.hp / 4);
    });
    CHECK(!b.sides[0].active.volatiles.substitute);
    CHECK(b.sides[0].get(1).hp == b.sides[0].get(1).stats.hp / 4);
    b = after(IDLE, [](Battle& b) {
      b.sides[0].active.volatiles.substitute = true;
      b.sides[0].active.volatiles.substitute_hp = 7;
    });
    CHECK(b.sides[0].active.volatiles.substitute_hp == 7);
    CHECK(b.sides[0].get(1).hp == b.sides[0].get(1).stats.hp);
  }
  // It ends a binding on the user.
  {
    Battle b = after(IDLE, [](Battle& b) {
      b.sides[0].active.volatiles.wrapped = 3;
      b.sides[0].active.volatiles.wrapped_move = Move::Wrap;
    });
    CHECK(b.sides[0].active.volatiles.wrapped == 0);
    CHECK(b.sides[0].active.volatiles.wrapped_move == Move::None);
  }
  // A slower foe's hit (forced) lands on the Substitute, not the Pokémon.
  {
    Battle b = started({user}, {DUGTRIO});
    b.sides[1].active.stats.spe = 1;
    Observed o{};
    o.calc.overrides.p2.hit = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    const Pokemon& mon = b.sides[0].get(1);
    CHECK(mon.hp == mon.stats.hp - mon.stats.hp / 4);
    CHECK(b.sides[0].active.volatiles.substitute_hp < mon.stats.hp / 4 ||
          !b.sides[0].active.volatiles.substitute);
  }
}

// Protect, Detect and Endure: blocking, failing when last, the consecutive
// draw (forced and observed), Endure's 1 HP, and what Protect lets through.
void check_protect() {
  helpers::Pokemon user = SNORLAX;
  user.moves[0] = Move::Protect;
  user.moves[1] = Move::Endure;
  // P1 moves first among equal priorities.
  const auto first = [](Battle& b) { b.sides[1].active.stats.spe = 1; };
  // Protect blocks the foe's hit, and its PP is used.
  {
    Battle b = started({user}, {DUGTRIO});
    first(b);
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].get(1).hp == b.sides[0].get(1).stats.hp);
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::Earthquake) - 1);
    CHECK(b.sides[0].active.volatiles.protect_uses == 1);
  }
  // Acting last (the foe switches, which goes first), it fails with no
  // draw and restarts the count.
  {
    Battle b = started({user}, {DUGTRIO, JOLTEON});
    b.sides[0].active.volatiles.protect_uses = 2;
    b.sides[0].active.volatiles.stall = 1;
    Observed o{};
    update(b, Choice::move(1), Choice::swap(2), o);
    CHECK(b.sides[0].active.volatiles.protect_uses == 0);
    CHECK(o.chance.actions.p1.protect == Optional::None);
  }
  // A second use draws 1/2; a third, 1/4.
  {
    Battle b = started({user}, {DUGTRIO});
    first(b);
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    Battle c = b;
    Observed o{};
    o.calc.overrides.p1.protect = Optional::False;
    o.calc.overrides.p2.hit = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(o.chance.actions.p1.protect == Optional::False);
    o.chance.probability.reduce();
    CHECK(b.sides[0].active.volatiles.protect_uses == 0);
    CHECK(b.sides[0].get(1).hp < b.sides[0].get(1).stats.hp);
    Observed p{};
    p.calc.overrides.p1.protect = Optional::True;
    update(c, Choice::move(1), Choice::move(1), p);
    CHECK(c.sides[0].active.volatiles.protect_uses == 2);
    Observed q{};
    q.calc.overrides.p1.protect = Optional::True;
    update(c, Choice::move(1), Choice::move(1), q);
    q.chance.probability.reduce();
    CHECK(c.sides[0].active.volatiles.protect_uses == 3);
    CHECK(q.chance.probability.p == 1 && q.chance.probability.q == 4);
  }
  // As Showdown's stall, the chain ends after a turn without a successful
  // use, even one the user didn't act in (its last resulting move is still
  // Protect, which kept the chain in the decomp): no draw.
  {
    Battle b = started({user}, {DUGTRIO});
    first(b);
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].active.volatiles.stall == 1 && b.sides[0].active.volatiles.protect_uses == 1);
    b.sides[0].active.volatiles.stall = 0; // as after a turn without acting
    Observed o{};
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(o.chance.actions.p1.protect == Optional::None);
    CHECK(b.sides[0].active.volatiles.protect_uses == 1);
  }
  // A locked Thrash into Protect: the accuracy roll comes first, and only a
  // hit (blocked) renews the lock, so the roll is recorded.
  {
    helpers::Pokemon u = user;
    u.moves[2] = Move::Splash;
    helpers::Pokemon foe = SNORLAX;
    foe.moves[2] = Move::Thrash;
    Battle b = started({u}, {foe});
    first(b);
    Observed o{};
    o.calc.overrides.p2.hit = Optional::True;
    update(b, Choice::move(3), Choice::move(3), o);
    CHECK(b.sides[1].active.volatiles.kind == Volatiles::Kind::Thrash);
    b.sides[1].active.volatiles.lock = 2;
    b.sides[1].active.boosts.acc = -1;
    for (const Optional hit : {Optional::True, Optional::False}) {
      Battle c = b;
      Observed p{};
      p.calc.overrides.p2.hit = hit;
      update(c, Choice::move(1), Choice::move(0), p);
      CHECK(p.chance.actions.p2.hit == hit);
      CHECK(c.sides[1].active.volatiles.multiple_turns() == (hit == Optional::True));
      CHECK(c.sides[1].active.volatiles.confusion == 0);
    }
  }
  // Endure leaves 1 HP against a lethal hit.
  {
    Battle b = started({user}, {DUGTRIO});
    first(b);
    b.sides[0].get(1).hp = 5;
    Observed o{};
    o.calc.overrides.p2.hit = Optional::True;
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[0].get(1).hp == 1);
  }
  // Protect stops Roar and Thunder Wave, not Ghost Curse.
  {
    helpers::Pokemon foe = JOLTEON;
    foe.moves[0] = Move::Roar;
    foe.moves[1] = Move::ThunderWave;
    Battle b = started({user, DUGTRIO}, {foe});
    first(b);
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].get(1).species == Species::Snorlax);
    b.sides[0].active.volatiles.protect_uses = 0;
    b.sides[0].active.volatiles.stall = 0;
    update(b, Choice::move(1), Choice::move(2), null);
    CHECK(!b.sides[0].get(1).status.any());
    helpers::Pokemon gengar = {
        .species = Species::Gengar,
        .moves = {Move::Curse},
        .ability = Ability::Levitate,
        .gender = Gender::Male,
    };
    Battle c = started({user}, {gengar});
    first(c);
    update(c, Choice::move(1), Choice::move(1), null);
    CHECK(c.sides[0].active.volatiles.cursed);
  }
}

// The Spikes move (three layers, then failing) and the healing moves.
void check_spikes_and_heals() {
  helpers::Pokemon user = SNORLAX;
  user.moves[0] = Move::Spikes;
  user.moves[1] = Move::Recover;
  user.moves[2] = Move::MorningSun;
  {
    Battle b = started({user}, {IDLE});
    Null null{};
    for (int i = 0; i < 4; i++) update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].conditions.spikes == 3);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Spikes) - 4);
  }
  const auto healed = [&](uint8_t slot, uint16_t hp, auto&& setup) {
    Battle b = started({user}, {IDLE});
    b.sides[0].get(1).hp = hp;
    setup(b);
    Null null{};
    update(b, Choice::move(slot), Choice::move(1), null);
    return b.sides[0].get(1).hp;
  };
  const auto none = [](Battle&) {};
  const uint16_t max = started({user}, {IDLE}).sides[0].get(1).stats.hp;
  CHECK(healed(2, 1, none) == 1 + max / 2);
  CHECK(healed(2, max, none) == max);
  CHECK(healed(3, 1, [](Battle& b) {
          b.field.weather = Weather::Sun;
          b.field.weather_turns = 5;
        }) == 1 + 20 * max / 30);
  CHECK(healed(3, 1, [](Battle& b) {
          b.field.weather = Weather::Sandstorm;
          b.field.weather_turns = 5;
        }) == 1 + max / 4 - max / 16); // then sand's residual
}

// Focus Punch: a hit on the user first breaks its focus (PP still used);
// a hit on its Substitute doesn't.
void check_focus_punch() {
  helpers::Pokemon user = SNORLAX;
  user.moves[0] = Move::FocusPunch;
  const auto after = [&](helpers::Pokemon foe, auto&& setup) {
    Battle b = started({user}, {foe});
    setup(b);
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    o.calc.overrides.p2.hit = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    return b;
  };
  const auto none = [](Battle&) {};
  {
    Battle b = after(DUGTRIO, none);
    CHECK(b.sides[1].get(1).hp == b.sides[1].get(1).stats.hp);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::FocusPunch) - 1);
  }
  CHECK(after(IDLE, none).sides[1].get(1).hp < after(IDLE, none).sides[1].get(1).stats.hp);
  {
    Battle b = after(DUGTRIO, [](Battle& b) {
      b.sides[0].active.volatiles.substitute = true;
      b.sides[0].active.volatiles.substitute_hp = 200;
    });
    CHECK(b.sides[0].get(1).hp == b.sides[0].get(1).stats.hp);
    CHECK(b.sides[1].get(1).hp < b.sides[1].get(1).stats.hp);
  }
}

void check_residuals() {
  helpers::Pokemon plain = IDLE;
  plain.ability = Ability::ThickFat;
  Null null{};
  const auto turn = [&](Battle& b) {
    return update(b, Choice::move(1), Choice::move(1), null);
  };
  {
    // Poison 1/8, burn 1/8, toxic n/16 with its counter.
    Battle b = started({plain}, {plain});
    Pokemon& m1 = b.sides[0].get(1);
    Pokemon& m2 = b.sides[1].get(1);
    m1.status = Status::psn();
    m2.status = Status::brn();
    turn(b);
    CHECK(m1.hp == m1.stats.hp - m1.stats.hp / 8);
    CHECK(m2.hp == m2.stats.hp - m2.stats.hp / 8);
    Battle c = started({plain}, {IDLE});
    Pokemon& t = c.sides[0].get(1);
    t.status = Status::tox();
    turn(c);
    CHECK(t.hp == t.stats.hp - t.stats.hp / 16);
    turn(c);
    CHECK(t.hp == t.stats.hp - 3 * (t.stats.hp / 16));
    CHECK(c.sides[0].active.volatiles.status_counter == 2);
  }
  {
    // Leftovers: 1/16, capped at max HP, before poison in the same pass, so
    // a poisoned Pokémon at full HP gains nothing first.
    Battle b = started({holding(plain, Item::Leftovers)}, {IDLE});
    Pokemon& m = b.sides[0].get(1);
    m.hp = uint16_t(m.stats.hp - 3);
    turn(b);
    CHECK(m.hp == m.stats.hp);
    m.status = Status::psn();
    turn(b);
    CHECK(m.hp == m.stats.hp - m.stats.hp / 8);
    turn(b);
    CHECK(m.hp == m.stats.hp - 2 * (m.stats.hp / 8) + m.stats.hp / 16);
  }
  {
    // Sandstorm: 1/16 to each Pokémon but Rock, Ground and Steel types; on
    // its last turn it does nothing and ends.
    Battle b = started({plain}, {helpers::detail::SKARMORY});
    b.field.weather = Weather::Sandstorm;
    b.field.weather_turns = 2;
    const Pokemon& m = b.sides[0].get(1);
    turn(b);
    CHECK(m.hp == m.stats.hp - m.stats.hp / 16);
    CHECK(b.sides[1].get(1).hp == b.sides[1].get(1).stats.hp);
    turn(b);
    CHECK(m.hp == m.stats.hp - m.stats.hp / 16);
    CHECK(b.field.weather == Weather::None && b.field.weather_turns == 0);
  }
  {
    // The side timers count down and end.
    Battle b = started({plain}, {IDLE});
    b.sides[0].conditions.reflect = 2;
    b.sides[1].conditions.safeguard = 1;
    turn(b);
    CHECK(b.sides[0].conditions.reflect == 1);
    CHECK(b.sides[1].conditions.safeguard == 0);
  }
  {
    // Both last Pokémon at 1 HP and burned: the faster dies first and its
    // side loses; the battle ends before the slower one's burn.
    helpers::Pokemon fast = JOLTEON;
    fast.moves[0] = Move::Splash;
    Battle b = started({fast}, {plain});
    b.sides[0].get(1).hp = 1;
    b.sides[0].get(1).status = Status::brn();
    b.sides[1].get(1).hp = 1;
    b.sides[1].get(1).status = Status::brn();
    const Result r = update(b, Choice::move(1), Choice::move(1), null);
    CHECK(r.type() == Result::Lose);
    CHECK(b.sides[1].get(1).hp == 1);
  }
  {
    // A residual KO with a Pokémon left raises a replacement request.
    Battle b = started({plain, JOLTEON}, {IDLE});
    b.sides[0].get(1).hp = 1;
    b.sides[0].get(1).status = Status::psn();
    CHECK(turn(b) == Result::init(Result::None, Choice::Switch, Choice::Pass));
  }
  {
    // Leech Seed: 1/8 from the seeded, to the seeder.
    Battle b = started({plain}, {plain});
    b.sides[0].active.volatiles.leech_seed = true;
    Pokemon& seeded = b.sides[0].get(1);
    Pokemon& seeder = b.sides[1].get(1);
    seeder.hp = 10;
    turn(b);
    CHECK(seeded.hp == seeded.stats.hp - seeded.stats.hp / 8);
    CHECK(seeder.hp == 10 + seeded.stats.hp / 8);
  }
  {
    // Speed Boost from the lead's first turn.
    const helpers::Pokemon ninjask = {
        .species = Species::Ninjask,
        .moves = {Move::Splash},
        .ability = Ability::SpeedBoost,
        .gender = Gender::Male,
    };
    Battle b = started({ninjask}, {IDLE});
    turn(b);
    CHECK(b.sides[0].active.boosts.spe == 1);
  }
  {
    // On a speed tie, a residual stage's coin is recorded only when its
    // order changes the state. One seeded Pokémon: Leech Seed's stage has
    // one handler, so no coin.
    Battle b = started({plain}, {plain});
    Observed o{};
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(o.chance.actions.turn.residual_tie == 0);
    Battle c = started({plain}, {plain});
    c.sides[0].active.volatiles.leech_seed = true;
    c.sides[1].get(1).hp = 10;
    Observed q{};
    update(c, Choice::move(1), Choice::move(1), q);
    CHECK(q.chance.actions.turn.residual_tie == 0);

    // Both seeded, P1 at full HP: P2's drain heals P1 only if P1's drain
    // comes first, so the order shows. P1 first: P1 ends one heal short.
    for (const uint8_t forced : {1, 2}) {
      Battle d = started({plain}, {plain});
      for (Side& side : d.sides) side.active.volatiles.leech_seed = true;
      Observed r{};
      r.calc.overrides.turn.residual_tie = forced;
      update(d, Choice::move(1), Choice::move(1), r);
      CHECK(r.chance.actions.turn.residual_tie == forced);
      const Pokemon& p1 = d.sides[0].get(1);
      const Pokemon& p2 = d.sides[1].get(1);
      const uint16_t eighth = uint16_t(p1.stats.hp / 8);
      CHECK(p1.hp == (forced == 1 ? p1.stats.hp : p1.stats.hp - eighth));
      CHECK(p2.hp == (forced == 1 ? p2.stats.hp - eighth : p2.stats.hp));
    }

    // Both last Pokémon at 1 HP and poisoned: the first to faint loses, a
    // coin in bits 2-3.
    for (const uint8_t forced : {1, 2}) {
      Battle e = started({plain}, {plain});
      for (Side& side : e.sides) {
        side.get(1).hp = 1;
        side.get(1).status = Status::psn();
      }
      Observed r{};
      r.calc.overrides.turn.residual_tie = uint8_t(forced << 2);
      const Result result = update(e, Choice::move(1), Choice::move(1), r);
      CHECK(r.chance.actions.turn.residual_tie == uint8_t(forced << 2));
      CHECK(result.type() == (forced == 1 ? Result::Lose : Result::Win));
    }
  }
  {
    // Quick Claw in the residual order. Protect against Splash: the move
    // order goes by priority, so the turn's roll isn't drawn there. A
    // slower holder's roll can change the residual order: with no effect on
    // the state it is drawn but not recorded; with both last Pokémon at 1
    // HP and burned, it decides who faints first and loses.
    helpers::Pokemon slow = holding(SNORLAX, Item::QuickClaw);
    slow.moves[0] = Move::Protect;
    slow.moves[1] = Move::None;
    helpers::Pokemon fast = JOLTEON;
    fast.moves[0] = Move::Splash;
    Battle b = started({slow}, {fast});
    Observed o{};
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(o.chance.actions == Actions{});
    for (const Optional forced : {Optional::True, Optional::False}) {
      Battle c = started({slow}, {fast});
      for (Side& side : c.sides) {
        side.get(1).hp = 1;
        side.get(1).status = Status::brn();
      }
      Observed r{};
      r.calc.overrides.turn.quick_claw = forced;
      const Result result = update(c, Choice::move(1), Choice::move(1), r);
      CHECK(r.chance.actions.turn.quick_claw == forced);
      CHECK(result.type() ==
            (forced == Optional::True ? Result::Lose : Result::Win));
    }
  }
}

void check_items() {
  helpers::Pokemon plain = IDLE;
  plain.ability = Ability::ThickFat;
  const helpers::Pokemon toxer = {
      .species = Species::Gengar,
      .moves = {Move::Toxic, Move::Splash},
      .ability = Ability::Levitate,
      .gender = Gender::Male,
  };
  Null null{};
  {
    // Lum Berry cures Toxic right after the move, and is used up.
    Battle b = started({toxer}, {holding(plain, Item::LumBerry)});
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(!b.sides[1].get(1).status.any());
    CHECK(b.sides[1].get(1).item == Item::None);
    CHECK(b.sides[1].recycle_item == Item::LumBerry);
    CHECK(b.sides[1].get(1).hp == b.sides[1].get(1).stats.hp);
  }
  {
    // Sitrus Berry: 30 HP at end of turn once at half or less.
    Battle b = started({holding(plain, Item::SitrusBerry)}, {IDLE});
    Pokemon& m = b.sides[0].get(1);
    m.hp = uint16_t(m.stats.hp / 2 + 1);
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(m.item == Item::SitrusBerry);
    m.hp = uint16_t(m.stats.hp / 2);
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(m.hp == m.stats.hp / 2 + 30 && m.item == Item::None);
  }
  {
    // Salac Berry: +1 Speed at end of turn at a quarter or less; eaten at
    // +6 too, as Showdown.
    Battle b = started({holding(plain, Item::SalacBerry)}, {IDLE});
    Pokemon& m = b.sides[0].get(1);
    m.hp = uint16_t(m.stats.hp / 4 + 1);
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(m.item == Item::SalacBerry);
    m.hp = uint16_t(m.stats.hp / 4);
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].active.boosts.spe == 1 && m.item == Item::None);
    Battle c = started({holding(plain, Item::SalacBerry)}, {IDLE});
    c.sides[0].get(1).hp = uint16_t(c.sides[0].get(1).stats.hp / 4);
    c.sides[0].active.boosts.spe = 6;
    update(c, Choice::move(1), Choice::move(1), null);
    CHECK(c.sides[0].active.boosts.spe == 6 && c.sides[0].get(1).item == Item::None);
  }
  {
    // Starf Berry: +2 to the forced stat, recorded with 1/k over the stats
    // below +6 (here Def is maxed, so 1/4), times the 1/2 move-order tie.
    Battle b = started({holding(plain, Item::StarfBerry)}, {IDLE});
    b.sides[0].get(1).hp = 1;
    b.sides[0].active.boosts.def = 6;
    Observed o{};
    o.calc.overrides.p1.starf = 4; // SpA
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[0].active.boosts.spa == 2);
    CHECK(o.chance.actions.p1.starf == 4);
    o.chance.probability.reduce();
    CHECK(o.chance.probability.p == 1 && o.chance.probability.q == 8);
  }
  {
    // Chesto Berry wakes the holder at the end of the turn it fell asleep in
    // (here set directly), resetting the public sleep count.
    Battle b = started({holding(plain, Item::ChestoBerry)}, {IDLE});
    b.sides[0].get(1).status = Status::slp(3);
    Observed o{};
    o.chance.durations.p1.sleep[0] = 1;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(!b.sides[0].get(1).status.any());
    CHECK(b.sides[0].get(1).item == Item::None);
  }
}

helpers::Pokemon mon(Species species, Ability ability, std::array<Move, 4> moves,
                     Gender gender = Gender::Male) {
  helpers::Pokemon p{};
  p.species = species;
  p.ability = ability;
  for (size_t i = 0; i < 4; i++) p.moves[i] = moves[i];
  p.gender = gender;
  return p;
}

void check_move_abilities() {
  const helpers::Pokemon slammer =
      mon(Species::Snorlax, Ability::ThickFat,
          {Move::BodySlam, Move::Toxic, Move::ThunderWave, Move::Splash});
  const helpers::Pokemon umbreon =
      mon(Species::Umbreon, Ability::Synchronize, {Move::Splash});
  const helpers::Pokemon electabuzz =
      mon(Species::Electabuzz, Ability::Static, {Move::Splash});
  const auto run = [](Battle& b, uint8_t m1, auto&& force) {
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    o.calc.overrides.p1.hits[0].damage = 85;
    o.calc.overrides.p1.hits[0].secondary = Optional::False;
    force(o);
    update(b, Choice::move(m1), Choice::move(1), o);
    return o;
  };
  const auto none = [](Observed&) {};
  {
    // Synchronize passes Toxic back as plain poison, Thunder Wave as
    // paralysis.
    Battle b = started({slammer}, {umbreon});
    run(b, 2, none);
    CHECK(b.sides[1].get(1).status.badly_poisoned());
    CHECK(b.sides[0].get(1).status.poisoned() &&
          !b.sides[0].get(1).status.badly_poisoned());
    Battle c = started({slammer}, {umbreon});
    run(c, 3, none);
    CHECK(c.sides[0].get(1).status.paralyzed());
    // Not through the user's Safeguard (Showdown).
    Battle d = started({slammer}, {umbreon});
    d.sides[0].conditions.safeguard = 5;
    run(d, 3, none);
    CHECK(d.sides[1].get(1).status.paralyzed() && !d.sides[0].get(1).status.any());
  }
  {
    // Static on contact, forced; not when the draw fails; no draw under
    // the attacker's Safeguard.
    Battle b = started({slammer}, {electabuzz});
    run(b, 1, [](Observed& o) { o.calc.overrides.p1.hits[0].contact = Optional::True; });
    CHECK(b.sides[0].get(1).status.paralyzed());
    Battle c = started({slammer}, {electabuzz});
    run(c, 1, [](Observed& o) { o.calc.overrides.p1.hits[0].contact = Optional::False; });
    CHECK(!c.sides[0].get(1).status.any());
    Battle d = started({slammer}, {electabuzz});
    d.sides[0].conditions.safeguard = 5;
    const Observed o = run(d, 1, none);
    CHECK(!d.sides[0].get(1).status.any());
    CHECK(o.chance.actions.p1.hits[0].contact == Optional::None);
  }
  {
    // Static paralyzes a Synchronize attacker, which passes it back.
    const helpers::Pokemon alakazam = mon(Species::Alakazam, Ability::Synchronize,
                                          {Move::BodySlam});
    Battle b = started({alakazam}, {electabuzz});
    run(b, 1, [](Observed& o) { o.calc.overrides.p1.hits[0].contact = Optional::True; });
    CHECK(b.sides[0].get(1).status.paralyzed());
    CHECK(b.sides[1].get(1).status.paralyzed());
  }
  {
    // Rough Skin: 1/16 of the attacker's max HP on contact.
    Battle b = started({slammer},
                       {mon(Species::Sharpedo, Ability::RoughSkin, {Move::Splash})});
    run(b, 1, none);
    const Pokemon& m = b.sides[0].get(1);
    CHECK(m.hp == m.stats.hp - m.stats.hp / 16);
  }
  {
    // Effect Spore: 1/10, then the forced pick: poison, or sleep.
    const helpers::Pokemon breloom =
        mon(Species::Breloom, Ability::EffectSpore, {Move::Splash});
    Battle b = started({slammer}, {breloom});
    const Observed o = run(b, 1, [](Observed& o) {
      o.calc.overrides.p1.hits[0].contact = Optional::True;
      o.calc.overrides.p1.hits[0].spore = 2;
    });
    CHECK(b.sides[0].get(1).status.poisoned());
    CHECK(o.chance.actions.p1.hits[0].spore == 2);
    Battle c = started({slammer}, {breloom});
    run(c, 1, [](Observed& o) {
      o.calc.overrides.p1.hits[0].contact = Optional::True;
      o.calc.overrides.p1.hits[0].spore = 1;
    });
    CHECK(c.sides[0].get(1).status.asleep());
  }
  {
    // Immunity cures a poison it got before (set here directly) at the
    // next moveend.
    Battle b = started({SNORLAX}, {IDLE});
    b.sides[0].get(1).status = Status::psn();
    Null null{};
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(!b.sides[0].get(1).status.any());
  }
}

void check_absorbing() {
  const helpers::Pokemon zapper =
      mon(Species::Jolteon, Ability::VoltAbsorb,
          {Move::Thunderbolt, Move::ThunderWave, Move::Flamethrower, Move::WillOWisp});
  const helpers::Pokemon lanturn =
      mon(Species::Lanturn, Ability::VoltAbsorb, {Move::Splash});
  const helpers::Pokemon houndoom =
      mon(Species::Houndoom, Ability::FlashFire, {Move::Splash});
  const auto run = [](Battle& b, uint8_t m1) {
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    o.calc.overrides.p1.hits[0].secondary = Optional::False;
    update(b, Choice::move(m1), Choice::move(1), o);
    return o;
  };
  {
    // Volt Absorb heals a quarter instead of taking the hit; PP is used and
    // nothing else is drawn.
    Battle b = started({zapper}, {lanturn});
    Pokemon& l = b.sides[1].get(1);
    l.hp = 10;
    const Observed o = run(b, 1);
    CHECK(l.hp == 10 + l.stats.hp / 4);
    CHECK(o.chance.actions.p1.hits[0].damage == 0);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Thunderbolt) - 1);
    // Thunder Wave has no power, so it isn't absorbed.
    Battle c = started({zapper}, {lanturn});
    run(c, 2);
    CHECK(c.sides[1].get(1).status.paralyzed());
  }
  {
    // Flash Fire: no damage and the boost; not while frozen.
    Battle b = started({zapper}, {houndoom});
    run(b, 3);
    CHECK(b.sides[1].get(1).hp == b.sides[1].get(1).stats.hp);
    CHECK(b.sides[1].active.volatiles.flash_fire);
    Battle c = started({zapper}, {houndoom});
    c.sides[1].get(1).status = Status::frz();
    Observed o{};
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    o.calc.overrides.p1.hits[0].damage = 85;
    o.calc.overrides.p1.hits[0].secondary = Optional::False;
    o.calc.overrides.p2.thaw = Optional::False;
    update(c, Choice::move(3), Choice::move(1), o);
    CHECK(c.sides[1].get(1).hp < c.sides[1].get(1).stats.hp);
    CHECK(!c.sides[1].active.volatiles.flash_fire);
    // Will-O-Wisp into a Flash Fire that isn't Fire type (as from Trace).
    Battle d = started({zapper}, {IDLE});
    d.sides[1].active.ability = Ability::FlashFire;
    run(d, 4);
    CHECK(!d.sides[1].get(1).status.any());
    CHECK(d.sides[1].active.volatiles.flash_fire);
  }
}

void check_switch_ins() {
  const helpers::Pokemon ttar =
      mon(Species::Tyranitar, Ability::SandStream, {Move::Splash});
  const helpers::Pokemon kyogre =
      mon(Species::Kyogre, Ability::Drizzle, {Move::Splash}, Gender::None);
  const helpers::Pokemon gyarados =
      mon(Species::Gyarados, Ability::Intimidate, {Move::Splash});
  const helpers::Pokemon metagross =
      mon(Species::Metagross, Ability::ClearBody, {Move::Splash}, Gender::None);
  Null null{};
  {
    // Both leads set weather: the slower (Tyranitar) arrives last, so sand
    // stays, with no timer. The speeds differ, so nothing is drawn.
    const std::vector<helpers::Pokemon> t1{kyogre}, t2{ttar};
    Battle b = *helpers::init_battle(1, t1, t2);
    Observed o{};
    CHECK(update(b, Choice::pass(), Choice::pass(), o) == Result::start());
    CHECK(b.field.weather == Weather::Sandstorm && b.field.permanent);
    CHECK(o.chance.actions == Actions{});
  }
  {
    // Intimidate at the start and on a switch; not into Clear Body; White
    // Herb undoes it at once.
    Battle b = started({gyarados}, {IDLE});
    CHECK(b.sides[1].active.boosts.atk == -1);
    Battle c = started({gyarados}, {metagross});
    CHECK(c.sides[1].active.boosts.atk == 0);
    Battle d = started({gyarados}, {holding(IDLE, Item::WhiteHerb)});
    CHECK(d.sides[1].active.boosts.atk == 0);
    CHECK(d.sides[1].get(1).item == Item::None);
    Battle e = started({IDLE, gyarados}, {IDLE});
    update(e, Choice::swap(2), Choice::move(1), null);
    CHECK(e.sides[1].active.boosts.atk == -1);
  }
  {
    // Trace copies Sand Stream, which doesn't take effect on entry in Gen
    // III; the weather is Tyranitar's own.
    const helpers::Pokemon porygon2 =
        mon(Species::Porygon2, Ability::Trace, {Move::Splash}, Gender::None);
    Battle b = started({porygon2}, {ttar});
    CHECK(b.sides[0].active.ability == Ability::SandStream);
    CHECK(b.field.weather == Weather::Sandstorm);
  }
  {
    // Natural Cure on switching out.
    const helpers::Pokemon starmie =
        mon(Species::Starmie, Ability::NaturalCure, {Move::Splash}, Gender::None);
    Battle b = started({starmie, IDLE}, {IDLE});
    b.sides[0].get(1).status = Status::par();
    update(b, Choice::swap(2), Choice::move(1), null);
    CHECK(!b.sides[0].get(2).status.any());
  }
  {
    // Both sides replace onto three layers of Spikes with their last
    // Pokémon at 1 HP: the faster (Jolteon) arrives first and faints, and
    // its side loses before Snorlax arrives.
    const helpers::Pokemon boomer =
        mon(Species::Snorlax, Ability::ThickFat, {Move::Explosion});
    helpers::Pokemon fast = JOLTEON;
    fast.moves[0] = Move::Splash;
    Battle b = started({boomer, fast}, {IDLE, IDLE});
    b.sides[1].get(1).hp = 1;
    b.sides[0].get(2).hp = 1;
    b.sides[1].get(2).hp = 1;
    b.sides[0].conditions.spikes = 3;
    b.sides[1].conditions.spikes = 3;
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    o.calc.overrides.p1.hits[0].damage = 85;
    const Result r = update(b, Choice::move(1), Choice::move(1), o);
    CHECK(r == Result::init(Result::None, Choice::Switch, Choice::Switch));
    CHECK(update(b, Choice::swap(2), Choice::swap(2), null).type() == Result::Lose);
    CHECK(b.sides[1].get(1).hp == 1);
  }
}

void check_sleep() {
  // Faster than the Snorlax targets, so a sleep lands before they move.
  const helpers::Pokemon sporer =
      mon(Species::Jolteon, Ability::VoltAbsorb,
          {Move::Spore, Move::Yawn, Move::Splash});
  helpers::Pokemon plain = IDLE;
  plain.ability = Ability::ThickFat;
  Null null{};
  const auto spore = [&](Battle& b) {
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    // A foe in an uproar can only go on with it.
    const bool locked = detail::forced(b.sides[1].active);
    update(b, Choice::move(1), Choice::move(locked ? 0 : 1), o);
    return o;
  };
  {
    // Spore: a 2-5 sleep, ticked once by the target's own attempt this
    // turn, so 1-4 left and a public count of 2.
    Battle b = started({sporer}, {plain});
    const Observed o = spore(b);
    const Status st = b.sides[1].get(1).status;
    CHECK(st.asleep() && !st.self_inflicted() && st.sleep() >= 1 && st.sleep() <= 4);
    CHECK(o.chance.durations.p2.sleep[0] == 2);
  }
  {
    // Sleep Clause: not while a foe-inflicted sleeper stands on that side;
    // a Rest sleeper doesn't count.
    Battle b = started({sporer}, {plain, plain});
    b.sides[1].get(2).status = Status::slp(3);
    spore(b);
    CHECK(!b.sides[1].get(1).status.any());
    Battle c = started({sporer}, {plain, plain});
    c.sides[1].get(2).status = Status::slf(2);
    spore(c);
    CHECK(c.sides[1].get(1).status.asleep());
  }
  {
    // Insomnia, Uproar, Substitute, Safeguard, another status.
    const auto fails = [&](helpers::Pokemon foe, auto&& setup) {
      Battle b = started({sporer}, {foe});
      setup(b);
      spore(b);
      return !b.sides[1].get(1).status.asleep();
    };
    const helpers::Pokemon hypno = mon(Species::Hypno, Ability::Insomnia, {Move::Splash});
    CHECK(fails(hypno, [](Battle&) {}));
    CHECK(fails(plain, [](Battle& b) {
      b.sides[1].active.volatiles.kind = Volatiles::Kind::Uproar;
      b.sides[1].active.volatiles.lock = 3;
      b.sides[1].active.volatiles.locked_move = Move::Uproar;
    }));
    CHECK(fails(plain, [](Battle& b) {
      b.sides[1].active.volatiles.substitute = true;
      b.sides[1].active.volatiles.substitute_hp = 10;
    }));
    CHECK(fails(plain, [](Battle& b) { b.sides[1].conditions.safeguard = 5; }));
    CHECK(fails(plain, [](Battle& b) { b.sides[1].get(1).status = Status::par(); }));
  }
  {
    // Rest: full HP, 3 turns of self-inflicted sleep, burn replaced; it
    // fails at full HP.
    const helpers::Pokemon rester =
        mon(Species::Snorlax, Ability::ThickFat, {Move::Rest, Move::SleepTalk, Move::BodySlam});
    Battle b = started({rester}, {IDLE});
    Pokemon& m = b.sides[0].get(1);
    m.hp = 10;
    m.status = Status::brn();
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(m.hp == m.stats.hp && m.status.self_inflicted() &&
          m.status.sleep() == Status::REST_SLEEP);
    Battle c = started({rester}, {IDLE});
    update(c, Choice::move(1), Choice::move(1), null);
    CHECK(!c.sides[0].get(1).status.any());
  }
  {
    // Yawn: asleep at the end of the next turn; not under Sleep Clause.
    Battle b = started({sporer}, {plain});
    update(b, Choice::move(2), Choice::move(1), null);
    CHECK(b.sides[1].active.volatiles.yawn == 1 && !b.sides[1].get(1).status.any());
    update(b, Choice::move(3), Choice::move(1), null);
    CHECK(b.sides[1].get(1).status.asleep());
    Battle c = started({sporer}, {plain, plain});
    c.sides[1].get(2).status = Status::slp(3);
    update(c, Choice::move(2), Choice::move(1), null);
    update(c, Choice::move(3), Choice::move(1), null);
    CHECK(!c.sides[1].get(1).status.any());
  }
  {
    // Sleep Talk: a forced pick among the callable slots (Rest and Body Slam,
    // 1/2), no PP for the called move, Sleep Talk recorded as the last move.
    const helpers::Pokemon talker =
        mon(Species::Snorlax, Ability::ThickFat, {Move::SleepTalk, Move::BodySlam, Move::Rest});
    Battle b = started({talker}, {plain});
    b.sides[0].get(1).status = Status::slp(4);
    Observed o{};
    o.calc.overrides.p1.move_slot = 2;
    o.calc.overrides.p1.hits[0].crit = Optional::False;
    o.calc.overrides.p1.hits[0].damage = 100;
    o.calc.overrides.p1.hits[0].secondary = Optional::False;
    o.calc.overrides.turn.speed_tie = 1;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[1].get(1).hp < b.sides[1].get(1).stats.hp);
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::SleepTalk) - 1);
    CHECK(b.sides[0].get(1).moves[1].pp == max_pp(Move::BodySlam));
    CHECK(b.sides[0].active.volatiles.last_move == Move::SleepTalk);
    CHECK(o.chance.actions.p1.move_slot == 2);
    CHECK(b.sides[0].active.volatiles.status_counter == 1);
    // A pick with no PP fails.
    Battle c = started({talker}, {plain});
    c.sides[0].get(1).status = Status::slp(4);
    c.sides[0].active.moves[1].pp = 0;
    Observed q{};
    q.calc.overrides.p1.move_slot = 2;
    update(c, Choice::move(1), Choice::move(1), q);
    CHECK(c.sides[1].get(1).hp == c.sides[1].get(1).stats.hp);
    // Not while awake.
    Battle d = started({talker}, {plain});
    update(d, Choice::move(1), Choice::move(1), null);
    CHECK(d.sides[1].get(1).hp == d.sides[1].get(1).stats.hp);
  }
  {
    // Snore only while asleep.
    const helpers::Pokemon snorer = mon(Species::Snorlax, Ability::ThickFat, {Move::Snore});
    Battle b = started({snorer}, {plain});
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[1].get(1).hp == b.sides[1].get(1).stats.hp);
    Battle c = started({snorer}, {plain});
    c.sides[0].get(1).status = Status::slp(4);
    Observed o{};
    o.calc.overrides.p1.hit = Optional::True;
    update(c, Choice::move(1), Choice::move(1), o);
    CHECK(c.sides[1].get(1).hp < c.sides[1].get(1).stats.hp);
  }
  {
    // Showdown's gen3 skippedTime: a Sleep Talk turn goes back on the sleep
    // counter and off the public count on switching out.
    const helpers::Pokemon talker =
        mon(Species::Snorlax, Ability::ThickFat, {Move::SleepTalk, Move::Splash});
    Battle b = started({talker, plain}, {IDLE});
    b.sides[0].get(1).status = Status::slp(4);
    Observed o{};
    o.chance.durations.p1.sleep[0] = 1;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[0].get(1).status.sleep() == 3);
    CHECK(o.chance.durations.p1.sleep[0] == 2);
    Observed q{};
    q.chance.durations = o.chance.durations;
    update(b, Choice::swap(2), Choice::move(1), q);
    CHECK(b.sides[0].get(2).status.sleep() == 4);
    CHECK(q.chance.durations.p1.sleep[1] == 1);
  }
}

// accuracy() against Showdown's gen3 arithmetic, worked by hand
// (data/mods/gen3/scripts.ts:319-363). Returns the outcomes of 100 that hit.
uint32_t accuracy_of(const Battle& b, Move move) {
  detail::Turn turn{};
  detail::MoveState m{};
  m.move = move;
  m.turn = &turn;
  return detail::accuracy(b, Player::P1, m);
}

void check_accuracy() {
  const helpers::Pokemon hitter = {
      .species = Species::Snorlax,
      .moves = {Move::HydroPump, Move::FireBlast},
      .ability = Ability::Immunity,
      .gender = Gender::Male,
  };
  helpers::Pokemon veil = IDLE;
  veil.species = Species::Sandslash;
  veil.ability = Ability::SandVeil;
  {
    Battle b = started({hitter}, {IDLE});
    CHECK(accuracy_of(b, Move::HydroPump) == 80);
    // +1 accuracy: 80 * 4/3 = 106.7, certain (the decomp: 80 * 133/100).
    b.sides[0].active.boosts.acc = 1;
    CHECK(accuracy_of(b, Move::HydroPump) >= 100);
    // +1 evasion cancels it exactly: 80 * 4/3 / (4/3) = 80.
    b.sides[1].active.boosts.eva = 1;
    CHECK(accuracy_of(b, Move::HydroPump) == 80);
    // +1 evasion alone: 80 / (4/3) = 60, a whole number in floating point.
    b.sides[0].active.boosts.acc = 0;
    CHECK(accuracy_of(b, Move::HydroPump) == 60);
    // +2 evasion: 80 * 3/5 = 48.
    b.sides[1].active.boosts.eva = 2;
    CHECK(accuracy_of(b, Move::HydroPump) == 48);
    // Fire Blast at +1 evasion: 85 * 3/4 = 63.75, so 64 outcomes.
    b.sides[1].active.boosts.eva = 1;
    CHECK(accuracy_of(b, Move::FireBlast) == 64);
    // Foresight ignores only a raised evasion.
    b.sides[1].active.volatiles.foresight = true;
    CHECK(accuracy_of(b, Move::FireBlast) == 85);
    b.sides[1].active.boosts.eva = -1;
    CHECK(accuracy_of(b, Move::FireBlast) >= 100);
  }
  {
    // Sand Veil in sand chains 0.8 (3276/4096): modify(80) = 64.
    Battle b = started({hitter}, {veil});
    b.field.weather = Weather::Sandstorm;
    CHECK(accuracy_of(b, Move::HydroPump) == 64);
    // With BrightPowder, 80 * 0.9 = 72 is whole, so the chain applies:
    // modify(72, 3276/4096) = (235872 + 2047) / 4096 = 58.
    b.sides[1].get(1).item = Item::BrightPowder;
    CHECK(accuracy_of(b, Move::HydroPump) == 58);
    // 85 * 0.9 = 76.5 isn't whole, so runEvent drops the chained Sand Veil
    // (sim/battle.ts:933): 77 outcomes.
    CHECK(accuracy_of(b, Move::FireBlast) == 77);
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
  check_confusion();
  check_confuse();
  check_status_moves();
  check_stat_moves();
  check_roar();
  check_baton_pass();
  check_wish();
  check_leech_seed();
  check_cures();
  check_counter();
  check_drain();
  check_taunt();
  check_weather_moves();
  check_endeavor();
  check_beat_up();
  check_knock_off();
  check_thief();
  check_encore();
  check_solar_beam();
  check_semi_invulnerable();
  check_recharge();
  check_destiny_bond();
  check_multi_hit();
  check_lethal();
  check_lethal_landings();
  check_mimic();
  check_misc_moves();
  check_perish_song();
  check_stat_changes();
  check_screens_and_power();
  check_mean_look();
  check_substitute();
  check_protect();
  check_spikes_and_heals();
  check_focus_punch();
  check_residuals();
  check_items();
  check_move_abilities();
  check_absorbing();
  check_switch_ins();
  check_sleep();
  check_accuracy();
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
