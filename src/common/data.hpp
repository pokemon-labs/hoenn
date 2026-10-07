// Types shared by every generation, as in libpkmn's src/lib/common/data.zig.
// Choice and Result keep libpkmn's one-byte layouts. There is no C API, so
// nothing depends on the exact bits; they're kept because they're small.

#pragma once

#include <cstdint>
#include <type_traits>

namespace pkmn {

enum class Player : uint8_t { P1 = 0, P2 = 1 };

constexpr Player foe(Player p) { return Player(uint8_t(p) ^ 1); }

// Identifies a Pokémon for the log: its original party position (1-6) in
// bits 0-2 and the player in bit 3, as libpkmn's ID. The position is
// order[SLOT - 1], the 1-based index into Side::pokemon. It never changes,
// unlike the Pokémon's SLOT (see Choice).
struct ID {
  uint8_t bits;

  static constexpr ID init(Player player, uint8_t position) {
    return {uint8_t((uint8_t(player) << 3) | (position & 7))};
  }

  constexpr Player player() const { return Player((bits >> 3) & 1); }
  constexpr uint8_t position() const { return bits & 7; }

  friend constexpr bool operator==(ID, ID) = default;
};

// A player's choice: the type in bits 0-1 and the data in bits 2-7.
//
// Data by type:
//   Pass    0
//   Move    the move slot, 1-4. 0 is Struggle, or a forced continuation (a
//           charging move, a rampage, Bide).
//   Switch  the SLOT to switch in, 2-6.
//
// SLOT is order-dependent, as in libpkmn: SLOT 1 is the active Pokémon and
// SLOT 6 the last. Side::pokemon never moves. Side::order maps SLOTs to it,
// so the Pokémon in SLOT s is pokemon[order[s - 1] - 1], and the active one
// is pokemon[order[0] - 1]. Switching to SLOT s swaps order[0] and
// order[s - 1], so switching to the same SLOT twice is a round trip.
//
// Type 3 is representable but invalid: update must return Result::error()
// for it, never assume it away.
struct Choice {
  enum Type : uint8_t { Pass = 0, Move = 1, Switch = 2 };

  uint8_t bits;

  static constexpr Choice init(Type type, uint8_t data) {
    return {uint8_t(((data & 63) << 2) | (type & 3))};
  }
  static constexpr Choice pass() { return {0}; }
  static constexpr Choice move(uint8_t slot) { return init(Move, slot); }
  static constexpr Choice swap(uint8_t slot) { return init(Switch, slot); }
  // `slot` is a move slot (1-4) for move() and a SLOT (2-6) for swap().

  constexpr Type type() const { return Type(bits & 3); }
  constexpr uint8_t data() const { return bits >> 2; }
  constexpr bool valid() const { return (bits & 3) != 3; }

  friend constexpr bool operator==(Choice, Choice) = default;
};

// The outcome of an update: the type in bits 0-3, then the type of choice
// each player must make next, p1 in bits 4-5 and p2 in bits 6-7.
//
// Win and Lose are from P1's view. A faint replacement is Switch for the
// fainted side and Pass for the other; a Baton Pass replacement is a
// mid-turn Switch.
struct Result {
  enum Type : uint8_t { None = 0, Win = 1, Lose = 2, Tie = 3, Error = 4 };

  uint8_t bits;

  static constexpr Result init(Type type, Choice::Type p1, Choice::Type p2) {
    return {uint8_t((type & 15) | ((p1 & 3) << 4) | ((p2 & 3) << 6))};
  }
  // The start of an ordinary turn: both players choose a move or a switch.
  static constexpr Result start() {
    return init(None, Choice::Move, Choice::Move);
  }
  static constexpr Result error() {
    return init(Error, Choice::Pass, Choice::Pass);
  }

  constexpr Type type() const { return Type(bits & 15); }
  constexpr Choice::Type p1() const { return Choice::Type((bits >> 4) & 3); }
  constexpr Choice::Type p2() const { return Choice::Type(bits >> 6); }
  constexpr Choice::Type request(Player p) const {
    return p == Player::P1 ? p1() : p2();
  }
  constexpr bool over() const { return type() != None; }

  friend constexpr bool operator==(Result, Result) = default;
};

static_assert(sizeof(ID) == 1 && std::is_trivial_v<ID>);
static_assert(sizeof(Choice) == 1 && std::is_trivial_v<Choice>);
static_assert(sizeof(Result) == 1 && std::is_trivial_v<Result>);

static_assert(foe(Player::P1) == Player::P2 && foe(Player::P2) == Player::P1);
static_assert(ID::init(Player::P2, 6).player() == Player::P2);
static_assert(ID::init(Player::P2, 6).position() == 6);
// pkmn_choice_init(type, data) = (data << 2) | type.
static_assert(Choice::move(4).bits == ((4 << 2) | 1));
static_assert(Choice::swap(6).type() == Choice::Switch);
static_assert(Choice::swap(6).data() == 6);
static_assert(Choice::pass() == Choice{} && Choice::pass().data() == 0);
static_assert(!Choice{3}.valid() && Choice::move(1).valid());
// pkmn_result_p1 = (r >> 4) & 3, pkmn_result_p2 = r >> 6.
static_assert(Result::init(Result::None, Choice::Switch, Choice::Pass).bits ==
              (2 << 4));
static_assert(Result::start().p1() == Choice::Move);
static_assert(Result::start().p2() == Choice::Move && !Result::start().over());
static_assert(Result::error().type() == Result::Error && Result::error().over());
static_assert(Result::init(Result::Tie, Choice::Pass, Choice::Switch)
                  .request(Player::P2) == Choice::Switch);

} // namespace pkmn
