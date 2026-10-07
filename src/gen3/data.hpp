#pragma once

#include <cstdint>
#include <type_traits>

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
// `toxic` is the bad poison counter (STATUS1_TOXIC_COUNTER, 0-15). Like the
// status, it persists through switching. It lives here so that every write of
// the status also resets it, as status1 does in the decomp.
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
  uint8_t toxic;

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
  // ENDTURN_BAD_POISON caps the counter at 15.
  static constexpr uint8_t MAX_TOXIC = 15;

  static constexpr Status none() { return {0, 0}; }
  // A sleep with `turns` (1-5) on the counter, inflicted by the foe.
  static constexpr Status slp(uint8_t turns) { return {uint8_t(turns & SLP), 0}; }
  // A self-inflicted sleep (Rest).
  static constexpr Status slf(uint8_t turns) {
    return {uint8_t(EXT | (turns & SLP)), 0};
  }
  static constexpr Status psn() { return {PSN, 0}; }
  static constexpr Status tox() { return {TOX, 0}; }
  static constexpr Status brn() { return {BRN, 0}; }
  static constexpr Status frz() { return {FRZ, 0}; }
  static constexpr Status par() { return {PAR, 0}; }

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

  // The bad poison residual: bumps the counter (capped) and returns the
  // multiplier on max HP / 16.
  constexpr uint8_t tick_toxic() {
    if (toxic < MAX_TOXIC) toxic++;
    return toxic;
  }

  // A well-formed status: at most one condition, EXT only with SLP or PSN,
  // sleep within range, and a counter only while badly poisoned. Use this to
  // reject corrupt input with Result::Error.
  constexpr bool valid() const {
    const uint8_t s = sleep();
    const uint8_t rest = bits & uint8_t(~(SLP | EXT));
    if (s > MAX_SLEEP) return false;
    if (toxic > MAX_TOXIC) return false;
    if (s != 0) return rest == 0 && toxic == 0;
    if (bits & EXT) return rest == PSN;
    if (toxic != 0) return false;
    return rest == 0 || rest == PSN || rest == BRN || rest == FRZ ||
           rest == PAR;
  }

  friend constexpr bool operator==(Status, Status) = default;
};

static_assert(sizeof(Status) == 2);
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
static_assert(!Status{Status::EXT, 0}.valid() && !Status{Status::PSN | Status::BRN, 0}.valid());
static_assert(!Status{6, 0}.valid() && !Status{Status::PSN, 1}.valid());

} // namespace pkmn::gen3
