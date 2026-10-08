// Checks the turn frame (mechanics.hpp): the start, the order of actions,
// the Quick Claw and speed-tie rolls with Chance and Calc, trapping, faint
// requests and resuming, the turn cap, and a random self-play soak over the
// given team files (every update keeps the battle valid; debug asserts the
// rest). Also the plain hits, Struggle, fainting and Spikes (plan step 3);
// moves the engine doesn't handle yet only use PP.
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
    CHECK(b.sides[0].get(1).moves[1].pp == max_pp(Move::QuickAttack, 3) - 1);
    CHECK(b.sides[1].get(1).moves[0].pp == max_pp(Move::Splash, 3) - 1);
    // Splash targets its user, so its moveend overwrites last_landed_move.
    CHECK(b.sides[1].active.last_taken_move == Move::QuickAttack);
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
    CHECK(b.sides[0].get(1).moves[1].pp == max_pp(Move::QuickAttack, 3));
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
    b.sides[0].active.substitute_hp = 50;
    b.sides[0].get(1).hp = 2;
    Observed o{};
    o.calc.overrides.p1.confusion = Optional::True;
    o.calc.overrides.p1.hits[0].focus_band = Optional::True;
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(b.sides[0].get(1).hp == 1);
    CHECK(b.sides[0].active.substitute_hp == 50);
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
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::ConfuseRay, 3) - 1);
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
    c.sides[1].active.substitute_hp = 10;
    update(c, Choice::move(1), Choice::move(1), null);
    CHECK(c.sides[1].active.volatiles.confusion == 0);

    Battle d = started({confuser}, {IDLE});
    d.sides[1].conditions.safeguard = 5;
    update(d, Choice::move(1), Choice::move(1), null);
    CHECK(d.sides[1].active.volatiles.confusion == 0);
    CHECK(d.sides[0].active.last_move == Move::ConfuseRay);

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
    o.calc.overrides.p1.secondary = Optional::True;
    update(b, Choice::move(2), Choice::move(1), o);
    CHECK(b.sides[1].active.volatiles.confusion >= 1);
    CHECK(o.chance.actions.p1.secondary == Optional::True);
    // Not through Safeguard: nothing is drawn.
    Battle c = started({confuser}, {IDLE});
    c.sides[1].conditions.safeguard = 5;
    Observed q{};
    update(c, Choice::move(2), Choice::move(1), q);
    CHECK(c.sides[1].active.volatiles.confusion == 0);
    CHECK(q.chance.actions.p1.secondary == Optional::None);
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
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Toxic, 3) - 1);
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
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Toxic, 3) - 1);
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
             b.sides[1].active.substitute_hp = 10;
           }).sides[1].get(1).status.any());
    Battle b = after(2, plain, [](Battle& b) { b.sides[1].conditions.safeguard = 5; });
    CHECK(!b.sides[1].get(1).status.any());
    CHECK(b.sides[0].active.last_move == Move::ThunderWave);
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
    CHECK(c.sides[0].active.toxic == 2);
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
    // A speed tie draws the residual order only when it can matter.
    Battle b = started({plain}, {plain});
    Observed o{};
    update(b, Choice::move(1), Choice::move(1), o);
    CHECK(o.chance.actions.turn.residual_tie == 0);
    Battle c = started({plain}, {plain});
    c.sides[0].active.volatiles.leech_seed = true;
    Observed q{};
    update(c, Choice::move(1), Choice::move(1), q);
    CHECK(q.chance.actions.turn.residual_tie != 0);
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
    // Salac Berry: +1 Speed at end of turn at a quarter or less; not at +6.
    Battle b = started({holding(plain, Item::SalacBerry)}, {IDLE});
    Pokemon& m = b.sides[0].get(1);
    m.hp = uint16_t(m.stats.hp / 4);
    b.sides[0].active.boosts.spe = 6;
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(m.item == Item::SalacBerry);
    b.sides[0].active.boosts.spe = 0;
    update(b, Choice::move(1), Choice::move(1), null);
    CHECK(b.sides[0].active.boosts.spe == 1 && m.item == Item::None);
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
    o.calc.overrides.p1.secondary = Optional::False;
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
    // Effect Spore: 1/10, then the forced pick. Poison takes; sleep doesn't
    // yet (Sleep Clause pending).
    const helpers::Pokemon breloom =
        mon(Species::Breloom, Ability::EffectSpore, {Move::Splash});
    Battle b = started({slammer}, {breloom});
    const Observed o = run(b, 1, [](Observed& o) {
      o.calc.overrides.p1.hits[0].contact = Optional::True;
      o.calc.overrides.p1.spore = 2;
    });
    CHECK(b.sides[0].get(1).status.poisoned());
    CHECK(o.chance.actions.p1.spore == 2);
    Battle c = started({slammer}, {breloom});
    run(c, 1, [](Observed& o) {
      o.calc.overrides.p1.hits[0].contact = Optional::True;
      o.calc.overrides.p1.spore = 1;
    });
    CHECK(!c.sides[0].get(1).status.any());
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
    o.calc.overrides.p1.secondary = Optional::False;
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
    CHECK(b.sides[0].get(1).moves[0].pp == max_pp(Move::Thunderbolt, 3) - 1);
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
    o.calc.overrides.p1.secondary = Optional::False;
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

// accuracy() against Showdown's gen3 arithmetic, worked by hand
// (data/mods/gen3/scripts.ts:319-363). Returns the outcomes of 100 that hit.
uint32_t accuracy_of(const Battle& b, Move move) {
  detail::MoveState m{};
  m.move = move;
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
  check_residuals();
  check_items();
  check_move_abilities();
  check_absorbing();
  check_switch_ins();
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
