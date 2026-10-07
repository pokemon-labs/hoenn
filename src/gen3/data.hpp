#pragma once

#include <cstdint>
#include <type_traits>

#include "../common/data.hpp"
#include "data/moves.hpp"

namespace pkmn::gen3 {

// A Pokémon's major status: the decomp's status1 (include/constants/battle.h),
// laid out like libpkmn's Status.
//
// `bits`:
//   bits 0-2  SLP: the remaining sleep counter, 0 when awake
//   bit 3     PSN
//   bit 4     BRN
//   bit 5     FRZ
//   bit 6     PAR
//   bit 7     EXT: with SLP, the sleep is self-inflicted (Rest). With PSN, the
//             poison is Toxic's bad poison.
//
// The bad poison counter (STATUS1_TOXIC_COUNTER) doesn't live here, although
// the decomp keeps it in status1. The residual (util:1538) bumps it only in
// gBattleMons, and no write-back to the party happens while it is raised, so
// every switch-in (Baton Pass included) reloads it as 0. It belongs on the
// active Pokémon. Every status write in the decomp also clears the counter
// (Rest sets status1 outright; cures clear STATUS1_TOXIC_COUNTER), so code
// that changes the active Pokémon's status must reset it too.
//
// The sleep counter is hidden information: the battle stores the true value,
// and Chance's Durations records how many ticks of it were seen. Encoders for
// the network must read asleep(), never sleep().
//
// Sleep and poison can't coexist, so EXT is never ambiguous. Sleep Clause
// counts only sleeps without EXT: Rest is the only self-inflicted sleep in
// Gen III singles, and every other sleep comes from the foe.
//
// No default member initializers, so the struct stays trivial. Zero it with
// `Status{}` or Status::none().
struct Status {
  uint8_t bits;

  static constexpr uint8_t SLP = 0b0000'0111;
  static constexpr uint8_t PSN = 1 << 3;
  static constexpr uint8_t BRN = 1 << 4;
  static constexpr uint8_t FRZ = 1 << 5;
  static constexpr uint8_t PAR = 1 << 6;
  static constexpr uint8_t EXT = 1 << 7;
  static constexpr uint8_t TOX = EXT | PSN;

  // Sleep rolls (Random() & 3) + 2 in SetMoveEffect and the Yawn residual.
  static constexpr uint8_t MIN_SLEEP = 2;
  static constexpr uint8_t MAX_SLEEP = 5;
  // Cmd_trysetrest.
  static constexpr uint8_t REST_SLEEP = 3;

  static constexpr Status none() { return {0}; }
  // A sleep with `turns` (1-5) on the counter, inflicted by the foe.
  static constexpr Status slp(uint8_t turns) { return {uint8_t(turns & SLP)}; }
  // A self-inflicted sleep (Rest).
  static constexpr Status slf(uint8_t turns) {
    return {uint8_t(EXT | (turns & SLP))};
  }
  static constexpr Status psn() { return {PSN}; }
  static constexpr Status tox() { return {TOX}; }
  static constexpr Status brn() { return {BRN}; }
  static constexpr Status frz() { return {FRZ}; }
  static constexpr Status par() { return {PAR}; }

  constexpr uint8_t sleep() const { return bits & SLP; }
  constexpr bool asleep() const { return sleep() != 0; }
  constexpr bool self_inflicted() const { return asleep() && (bits & EXT); }
  constexpr bool poisoned() const { return bits & PSN; }
  constexpr bool badly_poisoned() const { return (bits & TOX) == TOX; }
  constexpr bool burned() const { return bits & BRN; }
  constexpr bool frozen() const { return bits & FRZ; }
  constexpr bool paralyzed() const { return bits & PAR; }
  constexpr bool any() const { return bits != 0; }

  // One sleep tick at a move attempt (CANCELER_ASLEEP). `by` is 2 for Early
  // Bird, else 1. Waking clears EXT too. Returns whether it is still asleep.
  constexpr bool tick_sleep(uint8_t by) {
    if (sleep() <= by) {
      *this = none();
      return false;
    }
    bits = uint8_t(bits - by);
    return true;
  }

  // A well-formed status: at most one condition, EXT only with SLP or PSN,
  // and sleep within range. Use this to reject corrupt input with
  // Result::Error.
  constexpr bool valid() const {
    const uint8_t s = sleep();
    const uint8_t rest = bits & uint8_t(~(SLP | EXT));
    if (s > MAX_SLEEP) return false;
    if (s != 0) return rest == 0;
    if (bits & EXT) return rest == PSN;
    return rest == 0 || rest == PSN || rest == BRN || rest == FRZ ||
           rest == PAR;
  }

  friend constexpr bool operator==(Status, Status) = default;
};

static_assert(sizeof(Status) == 1);
static_assert(std::is_trivial_v<Status> && std::is_standard_layout_v<Status>);

static_assert(Status::slf(Status::REST_SLEEP).bits == 0b1000'0011);
static_assert(Status::slf(3).self_inflicted() && !Status::slp(2).self_inflicted());
static_assert(Status::tox().badly_poisoned() && !Status::psn().badly_poisoned());
static_assert(!Status::tox().asleep() && !Status::tox().self_inflicted());
static_assert(Status::MAX_SLEEP <= Status::SLP);
static_assert([] {
  // Early Bird wakes at attempt ceil(N / 2); a normal sleeper at attempt N.
  Status s = Status::slf(5);
  return s.tick_sleep(2) && s.tick_sleep(2) && !s.tick_sleep(2) && s == Status{};
}());
static_assert([] {
  Status s = Status::slp(2);
  return s.tick_sleep(1) && !s.tick_sleep(1);
}());
static_assert(Status::slf(3).valid() && Status::tox().valid() && Status{}.valid());
static_assert(!Status{Status::EXT}.valid() && !Status{Status::PSN | Status::BRN}.valid());
static_assert(!Status{6}.valid() && Status{Status::PSN}.valid());

// Where update resumes after returning a mid-turn request: every point in
// gBattleMainFunc's turn where a Pokémon can faint, plus Baton Pass. See
// "Turn phases" in CLAUDE.md.
//
//   None                   no request: the next update starts a turn
//   BatonPass              the Baton Pass user's side picks the recipient;
//                          resume by finishing its switch-in, then at action
//                          Request::next
//   FaintAfterAction       replace; resume at end of turn. The replacement's
//                          cancelallactions (bsc:5850) cancelled the rest.
//   FaintAfterResiduals    replace; resume at Future Sight
//   FaintAfterFutureSight  replace; resume at Perish Song
//   FaintAfterPerish       replace; resume at the next turn
enum class Phase : uint8_t {
  None,
  BatonPass,
  FaintAfterAction,
  FaintAfterResiduals,
  FaintAfterFutureSight,
  FaintAfterPerish,
};

// What survives a mid-turn request: only what update can't recompute when it
// resumes. Indexed by Player.
//
// `first` acts first. The order is decided once at selection, after the Quick
// Claw and speed-tie draws, which must not be drawn again on resume.
//
// `choice` and `move` are each player's queued action as resolved at
// selection (gChosenMoveByBattler): Encore, Choice Band and Struggle are
// applied then. `move` is Move::None for a switch or a pass.
//
// `next` is the index into the turn order (0 or 1, or 2 for end of turn) of
// the action to run after a Baton Pass. It is 0 in every other phase: a faint
// replacement cancels the rest of the actions, and residual faints don't
// interrupt the residual passes, so nothing else needs a position.
//
// No default member initializers, so the struct stays trivial.
struct Request {
  Move move[2];
  Choice choice[2];
  Phase phase;
  Player first;
  uint8_t next;

  static constexpr uint8_t END_OF_ACTIONS = 2;

  // A well-formed request. Use this to reject corrupt input with
  // Result::Error.
  constexpr bool valid() const {
    if (phase > Phase::FaintAfterPerish) return false;
    if (first != Player::P1 && first != Player::P2) return false;
    if (!choice[0].valid() || !choice[1].valid()) return false;
    if (phase == Phase::BatonPass) return next == 1 || next == END_OF_ACTIONS;
    return next == 0;
  }
};

static_assert(sizeof(Request) == 10); // 9 bytes and 1 of padding
static_assert(std::is_trivial_v<Request> && std::is_standard_layout_v<Request>);
static_assert(Request{}.valid());
static_assert(!Request{{}, {}, Phase::BatonPass, Player::P1, 0}.valid());
static_assert(Request{{}, {}, Phase::BatonPass, Player::P2, 2}.valid());
static_assert(!Request{{}, {}, Phase::FaintAfterAction, Player::P1, 1}.valid());
static_assert(!Request{{}, {}, Phase(6), Player::P1, 0}.valid());

} // namespace pkmn::gen3
