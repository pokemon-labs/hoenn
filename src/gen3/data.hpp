#pragma once

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

#include "../common/data.hpp"
#include "../common/rng.hpp"
#include "data/abilities.hpp"
#include "data/items.hpp"
#include "data/moves.hpp"
#include "data/natures.hpp"
#include "data/species.hpp"
#include "data/types.hpp"

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
  // and sleep within range. Asserted in debug builds.
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
//   FaintAfterResiduals    replace; resume at the next turn. Future Sight's
//                          and Perish Song's faints are among these, as
//                          Showdown (decided 2026-10-07; the decomp has a
//                          request after each).
enum class Phase : uint8_t {
  None,
  BatonPass,
  FaintAfterAction,
  FaintAfterResiduals,
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
// `quick_claw` is the turn's Quick Claw draw (gRandomTurnNumber, main:4013),
// None until it's drawn. It's drawn at the first read where it can change the
// order, and the end-of-turn order reads it again (main:4653, util:1209),
// possibly after a request, so it lives here.
//
// No default member initializers, so the struct stays trivial.
struct Request {
  Move move[2];
  Choice choice[2];
  Phase phase;
  Player first;
  uint8_t next;
  Optional quick_claw;

  static constexpr uint8_t END_OF_ACTIONS = 2;

  // A well-formed request. Asserted in debug builds.
  constexpr bool valid() const {
    if (phase > Phase::FaintAfterResiduals) return false;
    if (first != Player::P1 && first != Player::P2) return false;
    if (!choice[0].valid() || !choice[1].valid()) return false;
    if (quick_claw > Optional::False) return false;
    if (phase == Phase::BatonPass) return next == 1 || next == END_OF_ACTIONS;
    return next == 0;
  }

  friend constexpr bool operator==(const Request&, const Request&) = default;
};

static_assert(sizeof(Request) == 10);
static_assert(std::is_trivial_v<Request> && std::is_standard_layout_v<Request>);
static_assert(Request{}.valid());
static_assert(!Request{{}, {}, Phase::BatonPass, Player::P1, 0, Optional::None}.valid());
static_assert(Request{{}, {}, Phase::BatonPass, Player::P2, 2, Optional::None}.valid());
static_assert(!Request{{}, {}, Phase::FaintAfterAction, Player::P1, 1, Optional::None}.valid());
static_assert(!Request{{}, {}, Phase(4), Player::P1, 0, Optional::None}.valid());
static_assert(!Request{{}, {}, Phase::None, Player::P1, 0, Optional(3)}.valid());
// The frame (mechanics.hpp) falls through the phases in this order.
static_assert(Phase::None < Phase::BatonPass &&
              Phase::BatonPass < Phase::FaintAfterAction &&
              Phase::FaintAfterAction < Phase::FaintAfterResiduals);

// ---------------------------------------------------------------------------
// The battle. Draft (2026-10-07): the numbered decisions cited below and the
// map of every ActivePokemon field to the decomp are in src/gen3/README.md.
// Packing comes last: no bitfields yet.
//
// None of these have default member initializers, so they stay trivial. Zero
// them with `T{}`.
// ---------------------------------------------------------------------------

namespace detail {
constexpr bool valid(Type t) { return uint8_t(t) < TYPES; }
constexpr bool valid(Ability a) { return uint8_t(a) < ABILITIES; }
constexpr bool valid(Item i) { return uint8_t(i) < ITEMS; }
constexpr bool valid(Move m) { return uint16_t(m) < MOVES; }
constexpr bool valid(Species s) { return uint16_t(s) < SPECIES; }
} // namespace detail

enum class Gender : uint8_t { None, Male, Female };

// The six stats, in the decomp's order. In a Pokemon, `hp` is max HP.
struct Stats {
  uint16_t hp;
  uint16_t atk;
  uint16_t def;
  uint16_t spe;
  uint16_t spa;
  uint16_t spd;

  friend constexpr bool operator==(const Stats&, const Stats&) = default;
};

// One stat from CALC_STAT (src/pokemon.c:2814), before the nature. The decomp
// computes in s32; nothing here comes near overflow.
constexpr uint16_t calc_stat(uint8_t base, uint16_t iv, uint16_t ev,
                             uint8_t level) {
  assert(iv <= 31 && ev <= 255 && level >= 1 && level <= 100);
  return uint16_t((2 * base + iv + ev / 4) * level / 100 + 5);
}

// Max HP (src/pokemon.c:2844-2852). Shedinja always has 1.
constexpr uint16_t calc_hp(Species species, uint16_t iv, uint16_t ev,
                           uint8_t level) {
  assert(iv <= 31 && ev <= 255 && level >= 1 && level <= 100);
  if (species == Species::Shedinja) return 1;
  return uint16_t((2 * get(species).stats.hp + iv + ev / 4) * level / 100 +
                  level + 10);
}

// CalculateMonStats (src/pokemon.c:2823). `ivs` (0-31) and `evs` (0-255) are
// in Stats order. Checking the EV total is the caller's job.
constexpr Stats calc_stats(Species species, Nature nature, const Stats& ivs,
                           const Stats& evs, uint8_t level) {
  assert(species != Species::None && uint16_t(species) < SPECIES);
  assert(uint8_t(nature) < NATURES);
  const BaseStats& b = get(species).stats;
  // apply_nature's index is gNatureStatTable's column: Atk, Def, Spe, SpA, SpD.
  return {
      calc_hp(species, ivs.hp, evs.hp, level),
      apply_nature(nature, 0, calc_stat(b.atk, ivs.atk, evs.atk, level)),
      apply_nature(nature, 1, calc_stat(b.def, ivs.def, evs.def, level)),
      apply_nature(nature, 2, calc_stat(b.spe, ivs.spe, evs.spe, level)),
      apply_nature(nature, 3, calc_stat(b.spa, ivs.spa, evs.spa, level)),
      apply_nature(nature, 4, calc_stat(b.spd, ivs.spd, evs.spd, level)),
  };
}

// Hidden Power's typed move and power from the IVs (Cmd_hiddenpowercalc,
// bsc:8889). The decomp computes both live at each use from gBattleMons' IVs,
// which never change in battle, so computing them once at team building is
// equivalent.
struct HiddenPower {
  Move move;     // HiddenPowerFighting..HiddenPowerDark
  uint8_t power; // 30-70
};

constexpr HiddenPower hidden_power(const Stats& ivs) {
  assert(ivs.hp <= 31 && ivs.atk <= 31 && ivs.def <= 31 && ivs.spe <= 31 &&
         ivs.spa <= 31 && ivs.spd <= 31);
  // Bit 1 of each IV for the power, bit 0 for the type, in the decomp's order
  // (HP, Atk, Def, Spe, SpA, SpD), which is Stats order.
  const unsigned power_bits =
      ((ivs.hp & 2) >> 1) | ((ivs.atk & 2) << 0) | ((ivs.def & 2) << 1) |
      ((ivs.spe & 2) << 2) | ((ivs.spa & 2) << 3) | ((ivs.spd & 2) << 4);
  const unsigned type_bits =
      ((ivs.hp & 1) << 0) | ((ivs.atk & 1) << 1) | ((ivs.def & 1) << 2) |
      ((ivs.spe & 1) << 3) | ((ivs.spa & 1) << 4) | ((ivs.spd & 1) << 5);
  // (NUMBER_OF_MON_TYPES - 3) * bits / 63 + 1, then skip Mystery.
  unsigned type = 15 * type_bits / 63 + 1;
  if (type >= uint8_t(Type::Mystery)) type++;
  return {hidden_power(Type(type)), uint8_t(40 * power_bits / 63 + 30)};
}

// Checked by hand against the formula and known level 100 stats.
static_assert(calc_hp(Species::Blissey, 31, 252, 100) == 714);
static_assert(calc_hp(Species::Shedinja, 31, 252, 100) == 1);
static_assert(calc_stats(Species::Tyranitar, Nature::Adamant,
                         {31, 31, 31, 31, 31, 31}, {4, 252, 0, 252, 0, 0},
                         100).atk == 403);
static_assert(calc_stats(Species::Skarmory, Nature::Careful,
                         {31, 0, 31, 31, 31, 31}, {252, 0, 8, 0, 0, 248},
                         100).atk == 165);
static_assert(calc_stats(Species::Skarmory, Nature::Careful,
                         {31, 0, 31, 31, 31, 31}, {252, 0, 8, 0, 0, 248},
                         100).spa == 104); // (2 * 40 + 31) + 5, then x0.9
static_assert(calc_stat(100, 31, 0, 50) == 120);
// All 31s: Dark 70. The usual Showdown spreads (30s in Stats order):
static_assert(hidden_power(Stats{31, 31, 31, 31, 31, 31}).move == Move::HiddenPowerDark);
static_assert(hidden_power(Stats{31, 31, 31, 31, 31, 31}).power == 70);
static_assert(hidden_power(Stats{31, 30, 31, 30, 30, 31}).move == Move::HiddenPowerFire);
static_assert(hidden_power(Stats{31, 30, 30, 31, 31, 31}).move == Move::HiddenPowerIce);
static_assert(hidden_power(Stats{31, 30, 31, 31, 30, 31}).move == Move::HiddenPowerGrass);
static_assert(hidden_power(Stats{31, 30, 30, 31, 31, 31}).power == 70);
static_assert(hidden_power(Stats{0, 0, 0, 0, 0, 0}).move == Move::HiddenPowerFighting);
static_assert(hidden_power(Stats{0, 0, 0, 0, 0, 0}).power == 30);

// A move and its PP. `pp_ups` (0-3) is kept for max PP (Leppa Berry, Transform,
// the PP write-back).
struct MoveSlot {
  Move id;
  uint8_t pp;
  uint8_t pp_ups;

  friend constexpr bool operator==(const MoveSlot&, const MoveSlot&) = default;
};

// CalculatePPWithBonus (src/pokemon.c:4650): each PP Up adds a fifth of the
// base PP. At most 64 (a 40 PP move with 3 PP Ups).
constexpr uint8_t max_pp(Move move, uint8_t pp_ups) {
  assert(move != Move::None && uint16_t(move) < MOVES && pp_ups <= 3);
  const uint8_t base = get(move).pp;
  return uint8_t(base + base * 20 * pp_ups / 100);
}

static_assert(max_pp(Move::Spikes, 3) == 32 && max_pp(Move::Spikes, 0) == 20);
static_assert(max_pp(Move::Growl, 3) == 64 && max_pp(Move::Sketch, 3) == 1);

// A party Pokémon. Never reordered: Side::order maps SLOTs to it.
//
// Types live here and switch-in copies them (decision 7). The item
// is Item::KnockedOff once Knock Off has hit it (decision 8; the sentinel is
// still to be added to the generator). Hidden Power's type is in its move ID;
// `hidden_power` (30-70) and `hidden_power_type` are the Pokémon's own, from
// its IVs, set whether or not it knows the move: a Hidden Power it copies
// (Mimic) is its own, as the decomp's hiddenpowercalc and Showdown's hpType.
// The type fills what was padding.
struct Pokemon {
  Stats stats;
  MoveSlot moves[4];
  uint16_t hp;
  Species species;
  Type types[2];
  uint8_t level;
  Status status;
  Item item;
  Ability ability;
  uint8_t happiness;
  Gender gender;
  uint8_t hidden_power;
  Type hidden_power_type; // Fighting..Dark, not Normal or Mystery

  constexpr bool fainted() const { return hp == 0; }

  // A well-formed Pokémon, or an empty party slot (Species::None, all zero).
  constexpr bool valid() const {
    if (species == Species::None) return hp == 0 && stats.hp == 0;
    if (!detail::valid(species) || !detail::valid(types[0]) ||
        !detail::valid(types[1]) || !detail::valid(item) ||
        !detail::valid(ability)) {
      return false;
    }
    if (level < 1 || level > 100 || stats.hp == 0 || hp > stats.hp) return false;
    if (uint8_t(gender) > uint8_t(Gender::Female)) return false;
    if (!status.valid()) return false;
    if (hidden_power < 30 || hidden_power > 70 ||
        !detail::valid(hidden_power_type) ||
        hidden_power_type == Type::Normal ||
        hidden_power_type == Type::Mystery) {
      return false;
    }
    for (const MoveSlot& m : moves) {
      if (!detail::valid(m.id) || m.pp_ups > 3) return false;
      if (m.id == Move::None && m.pp != 0) return false;
    }
    return true;
  }

  friend constexpr bool operator==(const Pokemon&, const Pokemon&) = default;
};

// The flags and small counters the decomp packs into status2 and status3,
// named after their constants. Never take a pointer or reference to a member:
// packing will make them bitfields (decision 6).
struct Volatiles {
  // status2
  uint8_t confusion;      // STATUS2_CONFUSION, 0-5. Hidden. Baton Pass keeps.
  uint8_t uproar;         // STATUS2_UPROAR: Showdown's duration left, 0-5. Hidden.
  uint8_t bide;           // STATUS2_BIDE: Showdown's duration left, 0-3.
  uint8_t thrash;         // STATUS2_LOCK_CONFUSE: Showdown's lockedmove
                          // trueDuration, 0-3. Hidden.
  uint8_t wrapped;        // STATUS2_WRAPPED, 0-6. Hidden. The wrapper is the foe.
  // The binder has left (switched out or fainted) since: Showdown's
  // partiallytrapped stays until the victim's next residual, where it ends,
  // but no longer traps (its source isn't active). See Stage::Bind.
  bool wrapper_gone;
  bool flinched;          // STATUS2_FLINCHED
  bool multiple_turns;    // STATUS2_MULTIPLETURNS, with locked_move
  bool infatuated;        // STATUS2_INFATUATION, with the foe
  bool focus_energy;      // STATUS2_FOCUS_ENERGY. Baton Pass keeps.
  bool transformed;       // STATUS2_TRANSFORMED
  bool recharge;          // STATUS2_RECHARGE, with recharge_timer
  bool rage;              // STATUS2_RAGE
  bool substitute;        // STATUS2_SUBSTITUTE, with substitute_hp. Baton Pass keeps.
  bool destiny_bond;      // STATUS2_DESTINY_BOND
  bool escape_prevention; // STATUS2_ESCAPE_PREVENTION, by the foe. Baton Pass keeps.
  bool nightmare;         // STATUS2_NIGHTMARE
  bool cursed;            // STATUS2_CURSED. Baton Pass keeps.
  bool foresight;         // STATUS2_FORESIGHT
  bool defense_curl;      // STATUS2_DEFENSE_CURL
  bool torment;           // STATUS2_TORMENT
  // status3
  // Showdown's lockon, on the user: the turns left (bits 0-1, 0-2) and the
  // target's party position (bits 2-4, 1-6). Baton Pass keeps.
  uint8_t lock_on;
  uint8_t yawn;           // STATUS3_YAWN, 0-2
  bool leech_seed;        // STATUS3_LEECHSEED. Baton Pass keeps.
  bool perish_song;       // STATUS3_PERISH_SONG, with perish_song_timer. Baton Pass keeps.
  bool on_air;            // STATUS3_ON_AIR
  bool underground;       // STATUS3_UNDERGROUND
  bool underwater;        // STATUS3_UNDERWATER
  bool minimized;         // STATUS3_MINIMIZED
  bool charged_up;        // STATUS3_CHARGED_UP, with charge_timer
  bool rooted;            // STATUS3_ROOTED (Ingrain). Baton Pass keeps.
  bool imprisoned_others; // STATUS3_IMPRISONED_OTHERS
  bool grudge;            // STATUS3_GRUDGE
  bool mud_sport;         // STATUS3_MUDSPORT. Baton Pass keeps.
  bool water_sport;       // STATUS3_WATERSPORT. Baton Pass keeps.
  // RESOURCE_FLAG_FLASH_FIRE
  bool flash_fire;

  // STATUS2_WRAPPED | STATUS2_ESCAPE_PREVENTION
  constexpr bool trapped() const {
    return (wrapped != 0 && !wrapper_gone) || escape_prevention;
  }
  // STATUS3_SEMI_INVULNERABLE
  constexpr bool semi_invulnerable() const {
    return on_air || underground || underwater;
  }

  constexpr bool valid() const {
    return confusion <= 5 && uproar <= 5 && bide <= 3 && thrash <= 3 &&
           wrapped <= 6 && (!wrapper_gone || wrapped != 0) && (lock_on == 0 || (lock_on % 4 != 0 && lock_on % 4 <= 2 && lock_on >> 2 >= 1 && lock_on >> 2 <= 6)) && yawn <= 2 &&
           (on_air + underground + underwater) <= 1;
  }

  friend constexpr bool operator==(const Volatiles&, const Volatiles&) = default;
};

// The stat stages, −6 to 6. Baton Pass keeps them.
struct Boosts {
  int8_t atk;
  int8_t def;
  int8_t spe;
  int8_t spa;
  int8_t spd;
  int8_t acc;
  int8_t eva;

  constexpr bool valid() const {
    for (int8_t b : {atk, def, spe, spa, spd, acc, eva}) {
      if (b < -6 || b > 6) return false;
    }
    return true;
  }

  friend constexpr bool operator==(const Boosts&, const Boosts&) = default;
};

// The battle copy of the active Pokémon (decomp: gBattleMons plus everything
// SwitchInClearSetData clears). Switch-in copies stats, moves, species, types
// and ability from the Pokemon and zeroes the rest (decision 4). The field
// map with citations is in src/gen3/README.md.
struct ActivePokemon {
  uint32_t bide_damage;     // gBideDmg; raw damage, bound unverified
  // atk..spd (hp unused: HP lives on the Pokemon). Transform changes them.
  Stats stats;
  MoveSlot moves[4];
  Species species;
  // The last-move fields differ in when they're written; see the field map.
  Move locked_move;         // gLockedMoves
  Move choice_move;         // gBattleStruct->choicedMove (Choice Band)
  Move wrapped_move;        // gBattleStruct->wrappedMove, for the log
  Move disabled_move;       // DisableStruct
  Move encored_move;        // DisableStruct
  Move last_move;           // gLastMoves: Disable, Encore, Spite, Mimic
  Move last_landed_move;    // gLastLandedMoves: Conversion 2
  Move last_resulting_move; // gLastResultingMoves: Protect's reset
  Move last_printed_move;   // gLastPrintedMoves: Sketch
  Move last_taken_move;     // lastTakenMove: Mirror Move
  Type types[2];
  Type last_hit_type;       // gLastHitByType: Conversion 2
  Ability ability;
  Boosts boosts;
  Volatiles volatiles;
  // DisableStruct and status1's counter
  uint8_t toxic;            // STATUS1_TOXIC_COUNTER, 0-15; resets on switch-in
  uint8_t substitute_hp;    // max HP / 4 fits. Baton Pass keeps.
  uint8_t disable_timer;    // 0-6. Hidden.
  uint8_t encore_timer;     // 0-6. Hidden.
  uint8_t encored_slot;     // 0-3
  uint8_t protect_uses;     // unbounded in the decomp: saturate
  uint8_t stall;            // Showdown's stall duration, 0-2 (protect())
  uint8_t stockpile;        // 0-3
  uint8_t perish_song_timer;// 0-3. Baton Pass keeps.
  uint8_t fury_cutter;      // Showdown's furycutter: 0 off, else the power
                            // multiplier's log2 + 1, 1-5
  uint8_t rollout;          // Showdown's rollout hitCount, 0-5
  uint8_t charge_timer;     // 0-2; unused: Showdown's Charge has no timer
  uint8_t taunt;            // 0-2
  uint8_t first_turn;       // isFirstTurn, 0-2 (decision 14)
  uint8_t recharge_timer;   // 0-2; also a two-turn move's charge (turn_cleanup)
  uint8_t mimicked;         // mimickedMoves: a 4-bit mask of move slots
  bool truant;              // truantCounter
  // Showdown's gen3 skippedTime: move attempts spent asleep on Sleep Talk or
  // Snore since the last that did nothing, 0-4. The cartridge writes the
  // sleep counter back to the party only when the canceler stops the move
  // or the Pokémon wakes (util:2259), so a switch-out gives these back.
  uint8_t sleep_skipped;
  // Showdown's activeMoveActions, 0-2 (saturating): move actions since the
  // switch-in, counted as each starts (runMove), so an action a faint
  // cancelled doesn't count. Fake Out fails above 1.
  uint8_t move_actions;
  // Showdown's duration 2, for the volatiles that a turn's move renews and
  // whose duration otherwise runs out at that turn's residuals: bit 0 for the
  // lock (lockedmove or rollout), bit 1 for furycutter (RENEWED_*).
  uint8_t renewed;

  static constexpr uint8_t RENEWED_LOCK = 1;
  static constexpr uint8_t RENEWED_FURY_CUTTER = 2;
  static constexpr uint8_t MAX_TOXIC = 15;

  constexpr bool valid() const {
    if (!detail::valid(species) || !detail::valid(types[0]) ||
        !detail::valid(types[1]) || !detail::valid(last_hit_type) ||
        !detail::valid(ability)) {
      return false;
    }
    for (Move m : {locked_move, choice_move, wrapped_move, disabled_move,
                   encored_move, last_move, last_landed_move,
                   last_resulting_move, last_printed_move, last_taken_move}) {
      if (!detail::valid(m)) return false;
    }
    for (const MoveSlot& m : moves) {
      if (!detail::valid(m.id) || m.pp_ups > 3) return false;
    }
    return boosts.valid() && volatiles.valid() && toxic <= MAX_TOXIC &&
           sleep_skipped <= Status::MAX_SLEEP - 1 &&
           disable_timer <= 6 && encore_timer <= 6 && encored_slot <= 3 &&
           stockpile <= 3 && perish_song_timer <= 3 && fury_cutter <= 5 &&
           rollout <= 5 && charge_timer <= 2 && taunt <= 2 &&
           first_turn <= 2 && recharge_timer <= 2 && mimicked <= 0xF &&
           move_actions <= 2 && renewed <= 3 &&
           stall <= 2;
  }

  friend constexpr bool operator==(const ActivePokemon&, const ActivePokemon&) = default;
};

// Per-side timers (SideTimer, gWishFutureKnock). 0 is inactive.
struct SideConditions {
  uint8_t spikes;       // layers, 0-3
  uint8_t reflect;      // turns left, 0-5
  uint8_t light_screen; // 0-5
  uint8_t safeguard;    // 0-5
  uint8_t mist;         // 0-5
  uint8_t wish;         // 0-2. Heals the recipient's max HP / 2 on landing.
  uint8_t wisher;       // wishMonId: the wisher's party position (1-6), for
                        // the log. 0 when no Wish is pending.
  uint8_t future_sight; // 0-3: a future attack on this side lands at 0
                        // (Showdown's futuremove duration, 3 at the use)
  uint8_t future_user;  // the attacker's party position on the foe's side
                        // (1-6), for the log and its accuracy. 0 when none.

  constexpr bool valid() const {
    return spikes <= 3 && reflect <= 5 && light_screen <= 5 &&
           safeguard <= 5 && mist <= 5 && wish <= 2 && wisher <= 6 &&
           (wish == 0) == (wisher == 0) && future_sight <= 3 &&
           future_user <= 6 && (future_sight == 0) == (future_user == 0);
  }

  friend constexpr bool operator==(const SideConditions&, const SideConditions&) = default;
};

struct Side {
  Pokemon pokemon[6];
  ActivePokemon active;
  // The future attack on this side's Pokémon (futureSightDmg, by target):
  // its damage, rolled at the use (Showdown's getDamage); s32 in the
  // decomp, bound unverified. The move is Future Sight or Doom Desire, None
  // when no attack is pending.
  uint32_t future_sight_damage;
  Move future_sight_move;
  uint8_t order[6];             // by SLOT; 1-based indexes into pokemon
  SideConditions conditions;
  Item recycle_item;            // usedHeldItems (per position, for Recycle)

  // The Pokémon in SLOT s (1-6).
  constexpr Pokemon& get(uint8_t s) { return pokemon[order[s - 1] - 1]; }
  constexpr const Pokemon& get(uint8_t s) const {
    return pokemon[order[s - 1] - 1];
  }

  constexpr bool valid() const {
    bool seen[6] = {};
    for (uint8_t o : order) {
      if (o < 1 || o > 6 || seen[o - 1]) return false;
      seen[o - 1] = true;
    }
    for (const Pokemon& p : pokemon) {
      if (!p.valid()) return false;
    }
    if (get(1).species == Species::None) return false;
    return active.valid() && conditions.valid() &&
           detail::valid(future_sight_move) && detail::valid(recycle_item) &&
           (conditions.future_sight == 0) == (future_sight_move == Move::None);
  }

  friend constexpr bool operator==(const Side&, const Side&) = default;
};

enum class Weather : uint8_t { None, Rain, Sun, Sandstorm, Hail };

struct Field {
  Weather weather;
  uint8_t weather_turns; // weatherDuration, 0-5
  bool permanent;        // B_WEATHER_*_PERMANENT: set by an ability, no timer

  constexpr bool valid() const {
    if (uint8_t(weather) > uint8_t(Weather::Hail)) return false;
    if (weather == Weather::None) return weather_turns == 0 && !permanent;
    return weather_turns <= 5;
  }

  friend constexpr bool operator==(const Field&, const Field&) = default;
};

struct Battle {
  Side sides[2];
  RNG rng;
  uint16_t turn;
  Field field;
  Request request;

  // The hard cap: update returns Tie once turn reaches it (CLAUDE.md). As
  // Showdown (sim/battle.ts:1843, `turn > 1000`): turn 1000 is played and the
  // tie comes as turn 1001 starts. libpkmn ties at 1000.
  static constexpr uint16_t MAX_TURNS = 1001;

  constexpr Side& side(Player p) { return sides[uint8_t(p)]; }
  constexpr const Side& side(Player p) const { return sides[uint8_t(p)]; }

  // Turn 0 is a battle not yet started, as in libpkmn: the first update
  // (both players Pass) switches in the leads and moves to turn 1. Until then
  // neither side has an active Pokémon, and after it both always do.
  constexpr bool valid() const {
    if (!sides[0].valid() || !sides[1].valid() || turn > MAX_TURNS ||
        !field.valid() || !request.valid()) {
      return false;
    }
    for (const Side& s : sides) {
      if ((turn == 0) != (s.active.species == Species::None)) return false;
    }
    return turn != 0 || request.phase == Phase::None;
  }

  friend constexpr bool operator==(const Battle&, const Battle&) = default;
};

static_assert(sizeof(Stats) == 12);
static_assert(sizeof(MoveSlot) == 4);
static_assert(sizeof(Pokemon) == 42);
static_assert(sizeof(Volatiles) == 36);
static_assert(sizeof(Boosts) == 7);
static_assert(sizeof(ActivePokemon) == 124);
static_assert(sizeof(SideConditions) == 9);
static_assert(sizeof(Side) == 400);
static_assert(sizeof(Field) == 3);
static_assert(sizeof(Battle) == 824);

static_assert(std::is_trivially_copyable_v<Battle>);
static_assert(std::is_trivial_v<Battle> && std::is_standard_layout_v<Battle>);

// valid() works at compile time, and accepts a minimal battle: one Pokémon a
// side, everything else zero.
static_assert([] {
  Battle b{};
  b.turn = 1;
  for (Side& s : b.sides) {
    for (uint8_t i = 0; i < 6; i++) s.order[i] = uint8_t(i + 1);
    Pokemon& p = s.pokemon[0];
    p.species = Species::Pikachu;
    p.stats = {35, 55, 30, 90, 50, 40};
    p.hp = 35;
    p.level = 50;
    p.hidden_power = 70;
    p.hidden_power_type = Type::Dark;
    p.moves[0] = {Move::Tackle, 35, 0};
    s.active.species = p.species;
  }
  if (!b.valid()) return false;
  b.sides[1].pokemon[0].hp = 36;
  if (b.valid()) return false;
  b.sides[1].pokemon[0].hp = 35;
  b.sides[0].order[1] = 1;  // not a permutation
  return !b.valid();
}());
// At turn 0 neither side has an active Pokémon yet.
static_assert([] {
  Battle b{};
  for (Side& s : b.sides) {
    for (uint8_t i = 0; i < 6; i++) s.order[i] = uint8_t(i + 1);
    Pokemon& p = s.pokemon[0];
    p.species = Species::Pikachu;
    p.stats = {35, 55, 30, 90, 50, 40};
    p.hp = 35;
    p.level = 50;
    p.hidden_power = 70;
    p.hidden_power_type = Type::Dark;
    p.moves[0] = {Move::Tackle, 35, 0};
  }
  if (!b.valid()) return false;
  b.turn = 1;  // started, but no active Pokémon
  if (b.valid()) return false;
  b.turn = 0;
  b.request.phase = Phase::FaintAfterAction;
  return !b.valid();
}());

} // namespace pkmn::gen3
