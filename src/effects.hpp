#pragma once

// What each move, item, ability and residual does, the roll helpers and the
// shared helpers: the parts a player already knows. mechanics.hpp includes it
// after the turn's state, and it doesn't stand alone.

namespace hoenn {

namespace detail {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// A stat stage, in Boosts order: the decomp's STAT_ATK to STAT_EVASION, minus
// 1.
enum Stat : uint8_t { ATK, DEF, SPE, SPA, SPD, ACC, EVA };

constexpr int8_t& stage(Boosts& b, uint8_t stat) {
  switch (stat) {
  case ATK: return b.atk;
  case DEF: return b.def;
  case SPE: return b.spe;
  case SPA: return b.spa;
  case SPD: return b.spd;
  case ACC: return b.acc;
  default: assert(stat == EVA); return b.eva;
  }
}

constexpr bool holds(const Side& side, HoldEffect effect) {
  return item_data(side.get(1).item).effect == effect;
}

constexpr bool has_type(const ActivePokemon& active, Type type) {
  return active.types[0] == type || active.types[1] == type;
}

// Whether the last move to hit `a` can matter: `a` knows Mirror Move, or a move
// that can call or gain it (Metronome, Mimic, Sketch, Transform). Elsewhere
// last_taken_move isn't kept, so a roll that changes nothing else stays
// log-only.
constexpr bool mirrors(const ActivePokemon& a) {
  for (const MoveSlot& ms : a.moves) {
    if (ms.id == Move::MirrorMove || ms.id == Move::Metronome || ms.id == Move::Mimic ||
        ms.id == Move::Sketch || ms.id == Move::Transform) {
      return true;
    }
  }
  return false;
}

constexpr bool two_turn(Effect effect) {
  return effect == Effect::SolarBeam || effect == Effect::RazorWind ||
         effect == Effect::SkullBash || effect == Effect::SkyAttack ||
         effect == Effect::SemiInvulnerable;
}

// Whether the move hits 2-5 times or a fixed 2 or 3 (multi_hit_strikes).
constexpr bool multi_hit(Effect effect) {
  return effect == Effect::MultiHit || effect == Effect::DoubleHit ||
         effect == Effect::Twineedle || effect == Effect::TripleKick;
}

// Ends a two-turn move's charge: its lock and its semi-invulnerable turn.
constexpr void end_charge(Volatiles& v) {
  v.multiple_turns = false;
  v.on_air = false;
  v.underground = false;
  v.underwater = false;
}

// Ends a binding on `a` and forgets its move.
constexpr void unbind(ActivePokemon& a) {
  a.volatiles.wrapped = 0;
  a.volatiles.wrapper_gone = false;
  a.wrapped_move = Move::None;
}

// Charging, rampage, Bide, Rollout, Uproar and recharge: the only choice is
// Move(0), the locked move (main:4160, util:107).
constexpr bool forced(const ActivePokemon& active) {
  return active.volatiles.multiple_turns || active.volatiles.recharge;
}

// Showdown's gen3 metronome flag: every cartridge move but
// sMovesForbiddenToCopy.
constexpr bool metronome_can_call(Move m) {
  return m != Move::None && uint16_t(m) < CARTRIDGE_MOVES &&
         !(get(m).extra & Extra::NoMetronome);
}

struct MetronomeMoves {
  Move moves[CARTRIDGE_MOVES];
  uint16_t n;
};

inline constexpr MetronomeMoves METRONOME_MOVES = [] {
  MetronomeMoves list{};
  for (uint16_t i = 1; i < CARTRIDGE_MOVES; i++) {
    if (metronome_can_call(Move(i))) list.moves[list.n++] = Move(i);
  }
  return list;
}();
static_assert(METRONOME_MOVES.n == 336);

// The Speed GetWhoStrikesFirst compares (main:4595), before Quick Claw, with
// Showdown's arithmetic: the stage (floored), then Swift Swim, Chlorophyll,
// Macho Brace and paralysis chained in 4096ths and rounded once. The decomp
// floors after each factor, weather first; paralysis differs most (147 and 149
// both give 37).
constexpr uint32_t speed(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  const ActivePokemon& active = side.active;
  const Weather weather = effective_weather(battle);
  const Fraction& stage = STAT_STAGE_RATIOS[unsigned(active.boosts.spe + 6) % 13];
  const uint32_t spe = active.stats.spe * stage.num / stage.den;
  uint32_t modifier = 4096;
  if ((active.ability == Ability::SwiftSwim && weather == Weather::Rain) ||
      (active.ability == Ability::Chlorophyll && weather == Weather::Sun)) {
    modifier *= 2;
  }
  if (holds(side, HoldEffect::MachoBrace)) modifier /= 2;
  if (side.get(1).status.paralyzed()) modifier /= 4;
  return (spe * modifier + 2047) / 4096;
}

// The order of two things that only order the log's lines: Speed, then an
// unrecorded coin (CLAUDE.md, "Chance").
constexpr Player speed_order(Battle& battle, uint32_t a, uint32_t b) {
  if (a != b) return a > b ? Player::P1 : Player::P2;
  return battle.rng.chance(1, 2) ? Player::P1 : Player::P2;
}

constexpr Player log_order(Battle& battle) {
  return speed_order(battle, speed(battle, Player::P1), speed(battle, Player::P2));
}

// The typed Hidden Powers are all the cartridge's MOVE_HIDDEN_POWER.
constexpr bool same_move(Move a, Move b) {
  return a == b || (is_hidden_power(a) && is_hidden_power(b));
}

// Adds `d` to the Bide damage, saturating.
constexpr void add_bide(ActivePokemon& a, uint32_t d) {
  a.bide_damage = a.bide_damage > UINT32_MAX - d ? UINT32_MAX : a.bide_damage + d;
}

// No move slot: Struggle, a called move, a future attack.
inline constexpr uint8_t NO_SLOT = 4;

// The slot holding `move`, or NO_SLOT.
constexpr uint8_t slot_of(const ActivePokemon& a, Move move) {
  for (uint8_t s = 0; s < 4; s++) {
    if (a.moves[s].id == move) return s;
  }
  return NO_SLOT;
}

// Whether `a` knows `move`, the typed Hidden Powers as one.
constexpr bool knows(const ActivePokemon& a, Move move) {
  for (const MoveSlot& ms : a.moves) {
    if (same_move(ms.id, move)) return true;
  }
  return false;
}

// Whether the foe Imprisoned `move` (GetImprisonedMovesCount, util:1082).
constexpr bool imprisoned(const ActivePokemon& foe, Move move) {
  return foe.volatiles.imprisoned_others && knows(foe, move);
}

constexpr bool lost(const Side& side) {
  for (const Pokemon& p : side.pokemon) {
    if (p.species != Species::None && !p.fainted()) return false;
  }
  return true;
}

// Cmd_checkteamslost (bsc:3534): both sides out is a draw, whoever fainted
// first, as Showdown's gen3.
constexpr Flow outcome(const Battle& battle) {
  const bool l1 = lost(battle.sides[0]);
  const bool l2 = lost(battle.sides[1]);
  if (!l1 && !l2) return std::nullopt;
  const Result::Type type = l1 && l2 ? Result::Tie
                            : l1     ? Result::Lose
                                     : Result::Win;
  return Result::init(type, Choice::Pass, Choice::Pass);
}

} // namespace detail

namespace rolls {

// ---------------------------------------------------------------------------
// Roll helpers, as libpkmn's Rolls: one per catalogue row in chance.hpp. Each
// takes the Calc override if one is set, else draws, then records the outcome
// and its probability in Chance. Each is called only where its outcome can
// matter (CLAUDE.md, "Chance").
// ---------------------------------------------------------------------------

// The position of the k-th set bit of `mask`, counting from bit 0.
constexpr uint8_t nth_bit(uint8_t mask, uint8_t k) {
  assert(k < std::popcount(mask));
  for (uint8_t i = 0; i < 8; i++) {
    if ((mask >> i & 1) && k-- == 0) return i;
  }
  assert(false);
  return 0;
}

// Sets an Actions field, at most once per update (a second write means the
// catalogue's multiplicity is wrong).
template <typename T> constexpr void record(T& field, T value) {
  assert(field == T{} && "Actions field set twice in one update");
  field = value;
}

constexpr void record(MovePick& field, Move move) {
  assert(field.get() == Move::None && "Actions field set twice in one update");
  field.set(move);
}

template <typename O>
constexpr void observe(O& options, uint64_t num, uint64_t den) {
  if constexpr (detail::CHANCE<O>) {
    assert(num >= 1 && num <= den);
    const bool ok = options.chance.probability.update(num, den);
    assert(ok);
    (void)ok;
  }
}

// The turn's Quick Claw draw, exactly 1/5 as Showdown's (sim/battle.ts:1802;
// the decomp's 13107/65536, main:4653): one draw for both holders and every
// order of the turn (main:4013), kept in the request. The residuals draw it
// before they know whether it matters, so the draw and its record
// (quick_claw_keep) are apart.
template <typename O> constexpr bool quick_claw_draw(Battle& battle, O& options) {
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.turn.quick_claw;
  assert(forced <= Optional::False);
  return forced != Optional::None ? forced == Optional::True : battle.rng.chance(1, 5);
}

template <typename O>
constexpr void quick_claw_keep(Battle& battle, bool proc, O& options) {
  assert(battle.request.quick_claw == Optional::None);
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.turn.quick_claw, optional(proc));
    observe(options, proc ? 1 : 4, 5);
  }
  battle.request.quick_claw = optional(proc);
}

template <typename O> constexpr bool quick_claw(Battle& battle, O& options) {
  const Optional drawn = battle.request.quick_claw;
  if (drawn != Optional::None) return drawn == Optional::True;
  const bool proc = quick_claw_draw(battle, options);
  quick_claw_keep(battle, proc, options);
  return proc;
}

// A speed tie: `Random() & 1` in GetWhoStrikesFirst (main:4728, 4745). Returns
// who goes first.
template <typename O> constexpr Player speed_tie(Battle& battle, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.turn.speed_tie;
  assert(forced <= 2);
  const bool p1 = forced != 0 ? forced == 1 : battle.rng.chance(1, 2);

  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.turn.speed_tie, uint8_t(p1 ? 1 : 2));
    observe(options, 1, 2);
  }
  return p1 ? Player::P1 : Player::P2;
}

// A residual stage's tie coin where the order changes the state
// (stage_order_matters): Leech Seed's in bits 0-1 of Turn::residual_tie, the
// damage stages' in bits 2-3, each 1 for P1 first or 2 for P2.
template <typename O>
constexpr Player residual_tie(Battle& battle, bool leech_seed, O& options) {
  const unsigned shift = leech_seed ? 0 : 2;
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) {
    forced = uint8_t((options.calc.overrides.turn.residual_tie >> shift) & 3);
  }
  assert(forced <= 2);
  const bool p1 = forced != 0 ? forced == 1 : battle.rng.chance(1, 2);

  if constexpr (detail::CHANCE<O>) {
    uint8_t& field = options.chance.actions.turn.residual_tie;
    assert(((field >> shift) & 3) == 0 && "Actions field set twice in one update");
    field = uint8_t(field | (p1 ? 1 : 2) << shift);
    observe(options, 1, 2);
  }
  return p1 ? Player::P1 : Player::P2;
}

// A yes/no draw with probability num / den: `forced` is the Calc override,
// `field` the Actions field.
template <typename O>
constexpr bool coin(Battle& battle, Optional forced, Optional* field,
                    uint32_t num, uint32_t den, O& options) {
  assert(num >= 1 && num < den);
  assert(forced <= Optional::False);
  const bool yes = forced != Optional::None ? forced == Optional::True
                                            : battle.rng.chance(num, den);
  if constexpr (detail::CHANCE<O>) {
    record(*field, optional(yes));
    observe(options, yes ? num : den - num, den);
  }
  (void)field;
  return yes;
}

// coin() on p's Action field `m`.
template <typename O>
constexpr bool coin(Battle& battle, Player p, Optional Action::*m,
                    uint32_t num, uint32_t den, O& options) {
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).*m;
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) field = &(options.chance.actions.get(p).*m);
  return coin(battle, forced, field, num, den, options);
}

// The hit index of a future attack's landing: its draws are Action::future.
inline constexpr uint8_t FUTURE = MAX_HITS;

constexpr HitDraws& draws(Action& a, uint8_t i) {
  assert(i <= FUTURE);
  return i == FUTURE ? a.future : a.hits[i];
}

// coin() on p's draws `m` for hit `i` (or FUTURE).
template <typename O>
constexpr bool coin(Battle& battle, Player p, uint8_t i, Optional HitDraws::*m,
                    uint32_t num, uint32_t den, O& options) {
  Optional forced = Optional::None;
  if constexpr (detail::CALC<O>) forced = draws(options.calc.overrides.get(p), i).*m;
  Optional* field = nullptr;
  if constexpr (detail::CHANCE<O>) field = &(draws(options.chance.actions.get(p), i).*m);
  return coin(battle, forced, field, num, den, options);
}

// Protect, Detect or Endure after n successes in a row, n 1-3: exactly 1 / 2^n
// (sProtectSuccessRates, bsc:6514). Past the table it stays at 1/8, as
// Showdown's stall (data/mods/gen4/conditions.ts:142).
template <typename O>
constexpr bool protect(Battle& battle, Player p, uint8_t n, O& options) {
  assert(n >= 1 && n <= 3);
  return coin(battle, p, &Action::protect, 1, 1u << n, options);
}

template <typename O>
constexpr bool thaw(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::thaw, 1, 5, options);
}

template <typename O>
constexpr bool paralysis(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::paralysis, 1, 4, options);
}

template <typename O>
constexpr bool confusion(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::confusion, 1, 2, options);
}

template <typename O>
constexpr bool infatuation(Battle& battle, Player p, O& options) {
  return coin(battle, p, &Action::infatuation, 1, 2, options);
}

template <typename O>
constexpr bool hit(Battle& battle, Player p, uint32_t calc, O& options) {
  assert(calc > 0 && calc < 100);
  return coin(battle, p, &Action::hit, calc, 100, options);
}

// A move's secondary effect on hit `i`: Random() % 100 < chance
// (seteffectwithchance, bsc:2923). Per hit, as Showdown's (only Twineedle has
// both); the decomp draws once after the loop.
template <typename O>
constexpr bool secondary(Battle& battle, Player p, uint8_t i, uint32_t chance,
                         O& options) {
  assert(chance > 0 && chance < 100 && i < MAX_HITS);
  return coin(battle, p, i, &HitDraws::secondary, chance, 100, options);
}

// The attacker's King's Rock flinch on hit `i` (util:3763).
template <typename O>
constexpr bool kings_rock(Battle& battle, Player p, uint8_t i, uint32_t chance,
                          O& options) {
  assert(chance > 0 && chance < 100 && i < MAX_HITS);
  return coin(battle, p, i, &HitDraws::kings_rock, chance, 100, options);
}

// Triple Kick's accuracy check before hit `i` (1 or 2), as Showdown's
// multiaccuracy (bs1:1397). Actions keeps the hits that landed in multi_hit
// (kicks()); a forced count f makes hits before f land and hit f miss.
template <typename O>
constexpr bool kick(Battle& battle, Player p, uint8_t i, uint32_t calc,
                    O& options) {
  assert(i >= 1 && i <= 2 && calc > 0 && calc < 100);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).multi_hit;
  assert(forced <= 3);
  const bool yes = forced != 0 ? i < forced : battle.rng.chance(calc, 100);
  observe(options, yes ? calc : 100 - calc, 100);
  return yes;
}

// Records Triple Kick's landed hits, 1-3.
template <typename O>
constexpr void kicks(Player p, uint8_t n, O& options) {
  assert(n >= 1 && n <= 3);
  if constexpr (detail::CHANCE<O>) record(options.chance.actions.get(p).multi_hit, n);
  (void)p, (void)n, (void)options;
}

// Tri Attack's status: Random() % 3, 1 burn, 2 freeze, 3 paralysis (bsc:2601).
template <typename O>
constexpr uint8_t tri_attack(Battle& battle, Player p, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).tri_attack;
  assert(forced <= 3);
  const uint8_t which = forced != 0 ? forced : uint8_t(1 + battle.rng.range(3));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).tri_attack, which);
    observe(options, 1, 3);
  }
  return which;
}

// Present's, Magnitude's or Psywave's outcome (Action::variable), by Showdown's
// draws:
// - Present's random(10): 1 heals (2/10); 2, 3, 4 hit for 40, 80, 120 (4/10,
//   3/10, 1/10). The decomp's odds differ (bsc:8613).
// - Magnitude's random(100): 4-10 by MAGNITUDE's bounds, as the decomp
//   (bsc:8670).
// - Psywave's random(50, 151): the percentage of the user's level (the decomp's
//   steps of 10, bsc:7936).
// variable_matters() records it once it can change the state.
template <typename O>
constexpr uint8_t variable(Battle& battle, Player p, Effect effect, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).variable;
  if (effect == Effect::Present) {
    assert(forced <= 4);
    if (forced != 0) return forced;
    const uint32_t r = battle.rng.range(10);
    return r < 2 ? 1 : r < 6 ? 2 : r < 9 ? 3 : 4;
  }
  if (effect == Effect::Magnitude) {
    assert(forced == 0 || (forced >= 4 && forced <= 10));
    if (forced != 0) return forced;
    const uint32_t r = battle.rng.range(100);
    for (const MagnitudeEntry& e : MAGNITUDE) {
      if (r < e.bound) return e.magnitude;
    }
    assert(false);
  }
  assert(effect == Effect::Psywave);
  assert(forced == 0 || (forced >= 50 && forced <= 150));
  return forced != 0 ? forced : uint8_t(50 + battle.rng.range(101));
}

template <typename O>
constexpr void variable_matters(Player p, Effect effect, uint8_t value, O& options) {
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).variable, value);
    if (effect == Effect::Present) {
      constexpr uint8_t TENTHS[4] = {2, 4, 3, 1};
      observe(options, TENTHS[value - 1], 10);
    } else if (effect == Effect::Magnitude) {
      const uint8_t i = uint8_t(value - 4);
      observe(options, MAGNITUDE[i].bound - (i == 0 ? 0 : MAGNITUDE[i - 1].bound), 100);
    } else {
      observe(options, 1, 101);
    }
  }
  (void)p, (void)effect, (void)value, (void)options;
}

// A crit on hit `i`: !(Random() % n) with n from the crit stage (bsc:1282).
template <typename O>
constexpr bool crit(Battle& battle, Player p, uint8_t i, uint8_t stage,
                    O& options) {
  assert(i < MAX_HITS && stage < 5);
  return coin(battle, p, i, &HitDraws::crit, 1, CRITICAL_HIT_CHANCE[stage], options);
}

// The defender's Focus Band on hit `i` (or FUTURE): Random() % 100 < param
// (bsc:1677). Only for a lethal hit.
template <typename O>
constexpr bool focus_band(Battle& battle, Player p, uint8_t i, uint8_t param,
                          O& options) {
  assert(i <= FUTURE);
  return coin(battle, p, i, &HitDraws::focus_band, param, 100, options);
}

// A future attack's accuracy at its landing, for its user's side.
template <typename O>
constexpr bool future_hit(Battle& battle, Player p, uint32_t calc, O& options) {
  assert(calc > 0 && calc < 100);
  return coin(battle, p, &Action::future_hit, calc, 100, options);
}

// The damage roll on hit `i`: 100 - Random() % 16 percent
// (ApplyRandomDmgMultiplier, bsc:1641). Returns apply_roll's index, 0-15;
// Actions holds the percentage.
template <typename O>
constexpr uint8_t damage(Battle& battle, Player p, uint8_t i, O& options) {
  assert(i < MAX_HITS);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].damage;
  assert(forced == 0 || (forced >= 85 && forced <= 100));
  const uint8_t index =
      forced != 0 ? uint8_t(forced - 85) : uint8_t(battle.rng.range(16));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).hits[i].damage, uint8_t(85 + index));
    observe(options, 1, 16);
  }
  return index;
}

// Sleep Talk's pick: uniform over the slots in `mask`, as Showdown's sample
// (the decomp redraws until it hits one, bsc:8266). Returns the slot, 0-3;
// Actions holds 1-4.
template <typename O>
constexpr uint8_t sleep_talk(Battle& battle, Player p, uint8_t mask,
                             O& options) {
  assert(mask != 0 && mask < 16);
  const uint8_t n = uint8_t(std::popcount(mask));
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).move_slot;
  assert(forced <= 4 && (forced == 0 || (mask >> (forced - 1) & 1)));
  const uint8_t slot =
      forced != 0 ? uint8_t(forced - 1) : nth_bit(mask, uint8_t(battle.rng.range(n)));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).move_slot, uint8_t(slot + 1));
    observe(options, 1, n);
  }
  return slot;
}

// Metronome's pick: uniform over METRONOME_MOVES, the user's own moves
// included.
template <typename O> constexpr Move metronome(Battle& battle, Player p, O& options) {
  Move forced = Move::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).metronome.get();
  const Move move = forced != Move::None
                        ? forced
                        : detail::METRONOME_MOVES.moves[battle.rng.range(detail::METRONOME_MOVES.n)];
  assert(detail::metronome_can_call(move));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).metronome, move);
    observe(options, 1, detail::METRONOME_MOVES.n);
  }
  return move;
}

// A uniform pick from the `n` entries of `list`, which may repeat (Assist's
// moves, Conversion's types), or the entry equal to `*forced`. The value is
// recorded, with its count over n. Returns its index.
template <typename T, typename O>
constexpr uint8_t sample(Battle& battle, const T* list, uint8_t n, const T* forced,
                         O& options) {
  assert(n != 0);
  uint8_t k = n;
  if (forced != nullptr) {
    for (uint8_t i = 0; i < n; i++) {
      if (list[i] == *forced) k = i;
    }
    assert(k < n);
  } else {
    k = uint8_t(battle.rng.range(n));
  }
  if constexpr (detail::CHANCE<O>) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < n; i++) count = uint8_t(count + (list[i] == list[k]));
    observe(options, count, n);
  }
  (void)options;
  return k;
}

// Assist's pick, as Showdown's gen4: every move of the user's party but the
// user's own, fainted Pokémon included, with repeats.
template <typename O>
constexpr Move assist(Battle& battle, Player p, const Move* list, uint8_t n, O& options) {
  Move forced = Move::None;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).assist.get();
  const Move move =
      list[sample(battle, list, n, forced == Move::None ? nullptr : &forced, options)];
  if constexpr (detail::CHANCE<O>) record(options.chance.actions.get(p).assist, move);
  return move;
}

// Conversion's or Conversion 2's type (Action::variable holds the type + 1).
template <typename O>
constexpr Type conversion(Battle& battle, Player p, const Type* list, uint8_t n, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).variable;
  assert(forced <= TYPES);
  const Type f = Type(forced - 1);
  const Type type = list[sample(battle, list, n, forced == 0 ? nullptr : &f, options)];
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).variable, uint8_t(uint8_t(type) + 1));
  }
  return type;
}

// Roar or Whirlwind's pick: uniform over the bench SLOTs in `mask` (bit s - 1
// for SLOT s), as Showdown's getRandomSwitchable (the decomp redraws,
// bsc:7346). Returns the SLOT.
template <typename O>
constexpr uint8_t switch_target(Battle& battle, Player p, uint8_t mask,
                                O& options) {
  assert(mask != 0 && (mask & 1) == 0 && mask < 64);
  const uint8_t n = uint8_t(std::popcount(mask));
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).switch_target;
  assert(forced == 0 || (forced >= 2 && forced <= 6 && (mask >> (forced - 1) & 1)));
  const uint8_t slot =
      forced != 0 ? forced : uint8_t(1 + nth_bit(mask, uint8_t(battle.rng.range(n))));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).switch_target, slot);
    observe(options, 1, n);
  }
  return slot;
}

// Spite's PP, 2-5 (Showdown's gen3 random(2, 6)). Only where it can change the
// PP taken: the move has at least 3 left.
template <typename O> constexpr uint8_t spite(Battle& battle, Player p, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).spite;
  assert(forced == 0 || (forced >= 2 && forced <= 5));
  const uint8_t n = forced != 0 ? forced : uint8_t(2 + battle.rng.range(4));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).spite, n);
    observe(options, 1, 4);
  }
  return n;
}

// A 2-5 hit move's count: 2 or 3 at 3/8 each, 4 or 5 at 1/8
// (Cmd_setmultihitcounter, bsc:7139; Showdown's sample agrees).
template <typename O>
constexpr uint8_t multi_hit(Battle& battle, Player p, O& options) {
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).multi_hit;
  assert(forced == 0 || (forced >= 2 && forced <= 5));
  uint8_t n = forced;
  if (n == 0) {
    const uint32_t r = battle.rng.range(8);
    n = r < 3 ? 2 : r < 6 ? 3 : r == 6 ? 4 : 5;
  }
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).multi_hit, n);
    observe(options, n <= 3 ? 3 : 1, 8);
  }
  return n;
}

// Effect Spore's status: 1 sleep, 2 poison, 3 paralysis (util:2776; Showdown
// agrees).
template <typename O>
constexpr uint8_t spore(Battle& battle, Player p, uint8_t i, O& options) {
  assert(i < MAX_HITS);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).hits[i].spore;
  assert(forced <= 3);
  const uint8_t which = forced != 0 ? forced : uint8_t(1 + battle.rng.range(3));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).hits[i].spore, which);
    observe(options, 1, 3);
  }
  return which;
}

// A contact ability's chance on hit `i` (util:2768-2850): Effect Spore's 1/10,
// or 1/3 for Static, Poison Point, Flame Body and Cute Charm.
template <typename O>
constexpr bool contact(Battle& battle, Player p, uint8_t i, uint32_t den,
                       O& options) {
  assert(i < MAX_HITS);
  return coin(battle, p, i, &HitDraws::contact, 1, den, options);
}

// Starf Berry's stat: uniform over Atk, Def, Spe, SpA and SpD below +6
// (util:3462; Showdown agrees). Returns 1-5 (STAT_ATK to STAT_SPDEF).
template <typename O>
constexpr uint8_t starf(Battle& battle, Player p, O& options) {
  const Boosts& b = battle.side(p).active.boosts;
  const int8_t stages[5] = {b.atk, b.def, b.spe, b.spa, b.spd};
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 5; i++) mask = uint8_t(mask | (stages[i] < 6) << i);
  const uint8_t open = uint8_t(std::popcount(mask));
  assert(open != 0);
  uint8_t forced = 0;
  if constexpr (detail::CALC<O>) forced = options.calc.overrides.get(p).starf;
  assert(forced <= 5 && (forced == 0 || stages[forced - 1] < 6));
  const uint8_t stat =
      forced != 0 ? forced : uint8_t(1 + nth_bit(mask, uint8_t(battle.rng.range(open))));
  if constexpr (detail::CHANCE<O>) {
    record(options.chance.actions.get(p).starf, stat);
    observe(options, 1, open);
  }
  return stat;
}

// A hidden counter's tick, observed: the battle holds the true counter, so
// nothing is drawn; this records whether the effect ends now, its probability
// given the public count, and the new count. `count` is the Durations count (0:
// untracked), `lo`-`hi` the roll's range, `by` the step (2 for Early Bird). A
// count c means c - 1 steps were taken, so the roll N is uniform on [max(lo,
// c), hi] and the effect ends now if N <= c - 1 + by.
template <typename O>
constexpr void tick(Optional& field, uint8_t& count, bool ended, uint8_t lo,
                    uint8_t hi, uint8_t by, O& options) {
  static_assert(detail::CHANCE<O>);
  record(field, optional(ended));
  const uint8_t c = count;
  if (c == 0) return;
  const unsigned min = c > lo ? c : lo;
  assert(min <= hi);
  const unsigned last = unsigned(c - 1 + by) < hi ? unsigned(c - 1 + by) : hi;
  const unsigned ends = last >= min ? last - min + 1 : 0;
  const unsigned total = hi - min + 1;
  observe(options, ended ? ends : total - ends, total);
  count = ended ? 0 : uint8_t(c + by);
}

} // namespace rolls

namespace detail {

// ---------------------------------------------------------------------------
// Stats, HP and PP
// ---------------------------------------------------------------------------

constexpr void deduct_pp(Battle& battle, Player player, uint8_t slot, uint8_t n);

// Showdown's TryBoost (sim/battle.ts:2038) for the foe's primary drops of the
// stats in `mask` on p (ChangeStatBuffs, bsc:6937): a stat at -6 can't fall;
// Clear Body and White Smoke stop every drop, Hyper Cutter Attack's and Keen
// Eye accuracy's (one -fail|unboost); then Mist stops the rest. The decomp
// checks Mist first. Returns the stats that fall.
template <typename L>
constexpr uint8_t drops(const Battle& battle, Player p, uint8_t mask, L& log) {
  const Side& side = battle.side(p);
  const ActivePokemon& t = side.active;
  Boosts b = t.boosts;
  for (uint8_t i = 0; i < 7; i++) {
    if (stage(b, i) <= -6) mask = uint8_t(mask & ~(1 << i));
  }
  if (mask == 0) return 0;
  uint8_t stopped = 0;
  switch (t.ability) {
  case Ability::ClearBody:
  case Ability::WhiteSmoke: stopped = mask; break;
  case Ability::HyperCutter: stopped = mask & 1 << ATK; break;
  case Ability::KeenEye: stopped = mask & 1 << ACC; break;
  default: break;
  }
  const ID id = protocol::ident(battle, p);
  if (stopped != 0) {
    protocol::fail_unboost(log, id, t.ability);
    mask = uint8_t(mask & ~stopped);
  }
  if (mask != 0 && side.conditions.mist != 0) {
    protocol::activate(log, id, protocol::Activate::Mist);
    mask = 0;
  }
  return mask;
}

// Raises a stage by up to n, up to +6. Returns the stages gained.
constexpr int8_t raise(ActivePokemon& a, uint8_t stat, int8_t n) {
  int8_t& b = stage(a.boosts, stat);
  const int8_t gained = b + n > 6 ? int8_t(6 - b) : n;
  b = int8_t(b + gained);
  return gained;
}

// Lowers a stage by up to n, down to -6. Returns the stages lost.
constexpr int8_t lower(ActivePokemon& a, uint8_t stat, int8_t n) {
  int8_t& b = stage(a.boosts, stat);
  const int8_t lost = b - n < -6 ? int8_t(b + 6) : n;
  b = int8_t(b - lost);
  return lost;
}

// A primary drop of n stages from the foe on `mask`: drops(), then Showdown's
// -unboost per stat in stage order, 0 for one at -6. Returns the stats that
// fell.
template <typename L>
constexpr uint8_t lower_stats(Battle& battle, Player p, uint8_t mask, int8_t n, L& log) {
  ActivePokemon& t = battle.side(p).active;
  const uint8_t falls = drops(battle, p, mask, log);
  const ID id = protocol::ident(battle, p);
  for (uint8_t i = 0; i < 7; i++) {
    if (!(mask & (1 << i))) continue;
    if (falls & (1 << i)) {
      protocol::boost(log, id, i, lower(t, i, n), true);
    } else if (stage(t.boosts, i) <= -6) {
      protocol::boost(log, id, i, 0, true);
    }
  }
  return falls;
}

constexpr bool physical(Type type) { return type < Type::Mystery; }

constexpr void pp_reduce(Battle& battle, Player player, const MoveState& m);

// lockedmove's onRestart, for a hit or a Protect that stops the locked move:
// the lock renews for the turn, which lock_residuals reads (trueDuration
// changes only there).
constexpr void lock_renew(Battle& battle, Player player) {
  battle.side(player).active.renewed |= ActivePokemon::RENEWED_LOCK;
}

// Ends p's Bide: a silent -end, unless it fainted (sim/pokemon.ts:2036), and
// the lock.
template <typename O>
constexpr void end_bide(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  a.volatiles.bide = 0;
  a.volatiles.multiple_turns = false;
  if (battle.side(player).get(1).fainted()) return;
  protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::BideSilent);
}

// Showdown's MoveAborted, for a move the canceler stops: a charge is lost and
// Bide ends; the other locks run out at the residuals (lock_residuals). The
// decomp's CancelMultiTurnMoves (util:877) ends them all.
template <typename O>
constexpr void abort_move(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  Volatiles& v = a.volatiles;
  if (v.multiple_turns && two_turn(get(a.locked_move).effect)) end_charge(v);
  if (v.bide != 0) end_bide(battle, player, options);
}

// Showdown's arithmetic on JavaScript numbers (IEEE doubles, as C++'s).
// js_trunc is Dex.trunc (`num >>> 0`, sim/dex.ts:365).
constexpr double js_trunc(double x) { return double(uint32_t(x)); }

// Battle.modify (sim/battle.ts:2336).
constexpr double js_modify(double value, double modifier) {
  const double m = js_trunc(modifier * 4096);
  return js_trunc((js_trunc(value * m) + 2048 - 1) / 4096);
}

// Battle.chainModify (sim/battle.ts:2325): folds `next` into `modifier`.
constexpr double js_chain(double modifier, double next) {
  const uint32_t prev = uint32_t(modifier * 4096);
  const uint32_t n = uint32_t(next * 4096);
  return double((prev * n + 2048) >> 12) / 4096;
}

// boostTable in Showdown's gen3 tryMoveHit (data/mods/gen3/scripts.ts:319).
inline constexpr double ACCURACY_BOOSTS[7] = {1,         4.0 / 3, 5.0 / 3, 2,
                                              7.0 / 3,   8.0 / 3, 3};

// Whether the foe's Protect or Detect blocks m (DEFENDER_IS_PROTECTED, bsc:57):
// FLAG_PROTECT_AFFECTED, plus Counter and Mirror Coat, as Showdown's protect
// flag.
constexpr bool protected_from(const MoveState& m, Player player) {
  assert(m.turn != nullptr);
  return m.turn->protect[index(foe(player))] &&
         ((get(m.move).flags & Flag::ProtectAffected) ||
          m.move == Move::Counter || m.move == Move::MirrorCoat);
}

// Whether p's Lock-On or Mind Reader is on the foe now in (Volatiles::lock_on).
constexpr bool locked_on(const Battle& battle, Player p) {
  const uint8_t lock = battle.side(p).active.volatiles.lock_on;
  return lock != 0 && lock >> 2 == battle.side(foe(p)).order[0];
}

// Showdown's Invulnerability event (gen3 tryMoveHit, before the accuracy
// roll): a semi-invulnerable target the move can't reach. Lock-On and Mind
// Reader reach it. HITMARKER_IGNORE_ON_AIR: Thunder, Sky Uppercut, Gust and
// Twister (bs1:1892, 1826); IGNORE_UNDERGROUND: Earthquake and Magnitude;
// IGNORE_UNDERWATER: Surf and Whirlpool (bs1:239).
constexpr bool invulnerable(const Battle& battle, Player player, const MoveState& m) {
  const Volatiles& v = battle.side(foe(player)).active.volatiles;
  if (m.pursuing || locked_on(battle, player)) return false;
  const Effect e = get(m.move).effect;
  const bool air = e == Effect::Thunder || e == Effect::SkyUppercut || e == Effect::Gust ||
                   e == Effect::Twister;
  const bool ground = e == Effect::Earthquake || e == Effect::Magnitude;
  const bool water = m.move == Move::Surf || m.move == Move::Whirlpool;
  return (v.on_air && !air) || (v.underground && !ground) || (v.underwater && !water);
}

// The accuracy arithmetic from the move's accuracy `acc` on (accuracy()), for
// an attacker with accuracy stage `boost` and `ability`: the number of the 100
// outcomes that hit.
constexpr uint32_t staged_accuracy(const Battle& battle, Player target, double acc,
                                   int8_t boost, Ability ability, bool phys) {
  const Side& foe_side = battle.side(target);
  const ActivePokemon& f = foe_side.active;
  const Weather weather = effective_weather(battle);
  if (boost > 0) {
    acc *= ACCURACY_BOOSTS[boost];
  } else {
    acc /= ACCURACY_BOOSTS[-boost];
  }
  int8_t evasion = f.boosts.eva;
  if (f.volatiles.foresight && evasion > 0) evasion = 0;
  if (evasion > 0) {
    acc /= ACCURACY_BOOSTS[evasion];
  } else if (evasion < 0) {
    acc *= ACCURACY_BOOSTS[-evasion];
  }

  double modifier = 1;
  if (ability == Ability::CompoundEyes) modifier = js_chain(modifier, 1.3);
  if (f.ability == Ability::SandVeil && weather == Weather::Sandstorm) {
    modifier = js_chain(modifier, 0.8);
  }
  if (ability == Ability::Hustle && phys) {
    modifier = js_chain(modifier, 3277.0 / 4096);
  }
  const ItemData item = item_data(foe_side.get(1).item);
  if (item.effect == HoldEffect::EvasionUp) {
    assert(item.param == 10 || item.param == 5);
    acc *= item.param == 10 ? 0.9 : 0.95;
  }
  if (acc == double(uint32_t(acc))) acc = js_modify(acc, modifier);

  // ceil(acc): every r in 0-99 with r < acc hits (Showdown's randomChance).
  const uint32_t floor = uint32_t(acc);
  return double(floor) == acc ? floor : floor + 1;
}

// The accuracy check without its draw, for a target that isn't protected or out
// of reach (try_hit): 100 or more for a sure hit, else the outcomes of 100 that
// hit. The sure cases are the decomp's (Cmd_accuracycheck, bsc:1099;
// AccuracyCalcHelper, bsc:1054); the arithmetic is Showdown's gen3 tryMoveHit
// (data/mods/gen3/scripts.ts:319-363):
// - The accuracy stage, then the evasion stage, as fractions; the decomp
//   combines them into one stage (bsc:588). Foresight drops only a positive
//   evasion stage.
// - Compound Eyes, Sand Veil and Hustle chain, applied only to a whole number
//   (sim/battle.ts:933); BrightPowder and Lax Incense multiply directly.
constexpr uint32_t accuracy(const Battle& battle, Player player,
                            const MoveState& m) {
  const Side& side = battle.side(player);
  const ActivePokemon& a = side.active;
  const Side& foe_side = battle.side(foe(player));
  const MoveData& data = get(m.move);
  const Weather weather = effective_weather(battle);

  assert(!invulnerable(battle, player, m));
  // Pursuit on a switching target can't miss (Showdown's onModifyMove; the
  // decomp's script has no accuracy check).
  if (m.pursuing) return 100;
  if (locked_on(battle, player)) return 100;
  // OHKO moves, as Showdown's gen3: 30 plus the level difference, no stages;
  // the decomp's Cmd_tryKO (bsc:7490) hits 29 in 100.
  if (data.effect == Effect::Ohko) {
    return 30u + side.get(1).level - foe_side.get(1).level;
  }
  if ((weather == Weather::Rain && data.effect == Effect::Thunder) ||
      data.effect == Effect::AlwaysHit || data.effect == Effect::VitalThrow) {
    return 100;
  }

  double acc = data.accuracy;
  if (weather == Weather::Sun && data.effect == Effect::Thunder) acc = 50;
  return staged_accuracy(battle, foe(player), acc, a.boosts.acc, a.ability,
                         physical(m.type));
}

// Whether the foe's Pressure takes an extra PP from `move` (aimed at the user
// if `self`), as Showdown's: any move whose targets include the foe
// (sim/pokemon.ts:791), so the field-wide moves and Conversion 2 too, but not
// Sleep Talk, Snatch or Magic Coat ('self'). Cmd_ppreduce (bsc:1205) spares
// every move aimed at the user and takes 2 from Sleep Talk.
constexpr bool pressure_applies(const Battle& battle, Player player, Move move,
                                bool self) {
  if (battle.side(foe(player)).active.ability != Ability::Pressure) return false;
  if (move == Move::SleepTalk) return false;
  if (!self) return true;
  switch (move) {
  case Move::Haze:
  case Move::PerishSong:
  case Move::MudSport:
  case Move::WaterSport:
  case Move::RainDance:
  case Move::SunnyDay:
  case Move::Sandstorm:
  case Move::Hail:
  case Move::Conversion2: return true;
  default: return false;
  }
}

// Takes `n` PP (at most what's left) from a move slot, and from the party's
// unless the slot is Mimic's or Transform's (MOVE_IS_PERMANENT).
constexpr void deduct_pp(Battle& battle, Player player, uint8_t slot, uint8_t n) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  MoveSlot& ms = a.moves[slot];
  ms.pp = ms.pp > n ? uint8_t(ms.pp - n) : 0;
  if (!a.volatiles.transformed && !(a.mimicked & (1 << slot))) {
    side.get(1).moves[slot].pp = ms.pp;
  }
}

// Cmd_ppreduce (bsc:1205): one PP, two into Pressure, none for Struggle.
constexpr void pp_reduce(Battle& battle, Player player, const MoveState& m) {
  if (m.no_pp || m.slot == NO_SLOT) return;
  if (battle.side(player).active.moves[m.slot].pp == 0) return;
  deduct_pp(battle, player, m.slot,
            uint8_t(1 + pressure_applies(battle, player, m.move, m.self)));
}

// The move is marked FAILED, with no message of its own.
constexpr void mark_failed(MoveState& m) { m.result |= MoveResult::FAILED; }

// BattleScript_ButItFailed: -fail, and the move is marked FAILED (or `flag`).
template <typename O>
constexpr void but_it_failed(const Battle& battle, Player player, MoveState& m, O& options,
                             uint8_t flag = MoveResult::FAILED) {
  protocol::failed(options.log, protocol::ident(battle, player));
  m.result |= flag;
}

// Cmd_datahpupdate (bsc:1844) for a move's damage to `target`: its Substitute
// takes it if there is one (-activate, or -end as it breaks; the volatile stays
// until MOVEEND_SUBSTITUTE), otherwise the Pokémon does, for Bide and, by the
// table's type, Counter or Mirror Coat (bsc:1938). Returns gHpDealt.
template <typename O>
constexpr uint16_t hp_update(Battle& battle, Player target, uint32_t damage,
                             MoveState& m, O& options) {
  Side& side = battle.side(target);
  ActivePokemon& a = side.active;
  if (a.volatiles.substitute && a.substitute_hp != 0) {
    const uint8_t dealt =
        a.substitute_hp >= damage ? uint8_t(damage) : a.substitute_hp;
    a.substitute_hp = uint8_t(a.substitute_hp - dealt);
    const ID id = protocol::ident(battle, target);
    if (a.substitute_hp == 0) {
      protocol::end_effect(options.log, id, protocol::End::Substitute);
    } else {
      protocol::activate(options.log, id, protocol::Activate::SubstituteDamage);
    }
    return dealt;
  }
  add_bide(a, damage);
  Pokemon& mon = side.get(1);
  const uint16_t dealt = mon.hp > damage ? uint16_t(damage) : mon.hp;
  mon.hp = uint16_t(mon.hp - dealt);
  m.damaged = true;
  m.total = uint16_t(m.total + dealt);
  // A future attack's landing has no Turn.
  if (m.turn != nullptr) {
    m.turn->hit[index(target)] = true;
    const MoveData& data = get(m.move);
    const bool phys = data.effect == Effect::HiddenPower || physical(m.type);
    (phys ? m.turn->physical : m.turn->special)[index(target)] = dealt;
  }
  return dealt;
}

// The -damage line for a hit on p's foe itself, with its Faint queued at once,
// as Showdown's, before any recoil or drain.
template <typename O>
constexpr void log_hit(const Battle& battle, Player player, const MoveState& m,
                       O& options) {
  if (!m.damaged) return;
  const Player target = foe(player);
  const Pokemon& mon = battle.side(target).get(1);
  protocol::damage(options.log, protocol::ident(battle, target), mon);
  // A move's KO, so Destiny Bond applies.
  if (mon.fainted()) {
    const Volatiles& v = battle.side(target).active.volatiles;
    protocol::faint(options.log, protocol::ident(battle, target), v.destiny_bond);
  }
}

// Showdown's gen3 modifyDamage messages for one hit
// (data/mods/gen3/scripts.ts:92-106): the effectiveness, then -crit. Fixed
// damage has none.
template <typename O>
constexpr void log_modifiers(const Battle& battle, Player player, const MoveState& m,
                             bool crit, O& options) {
  const ID id = protocol::ident(battle, foe(player));
  protocol::effectiveness(options.log, id, m.result & MoveResult::SUPER_EFFECTIVE,
                          m.result & MoveResult::NOT_VERY_EFFECTIVE);
  if (crit) protocol::crit(options.log, id);
}

// A heal, capped at max HP.
constexpr void heal(Pokemon& mon, uint32_t amount) {
  const uint32_t room = uint32_t(mon.stats.hp - mon.hp);
  mon.hp = uint16_t(mon.hp + (amount < room ? amount : room));
}

// Max HP / n, at least 1.
constexpr uint32_t fraction(const Pokemon& mon, uint32_t n) {
  const uint32_t d = mon.stats.hp / n;
  return d == 0 ? 1 : d;
}

// Passive damage (residuals, Rough Skin): datahpupdate with no Substitute, Bide
// or Counter (util:1465), then tryfaintmon. Returns the HP lost.
template <typename O>
constexpr uint16_t passive_damage(Battle& battle, Player player,
                                   uint32_t damage, O& options) {
  Pokemon& mon = battle.side(player).get(1);
  assert(!mon.fainted());
  const uint16_t dealt = mon.hp > damage ? uint16_t(damage) : mon.hp;
  mon.hp = uint16_t(mon.hp - dealt);
  if (mon.fainted()) faint(battle, player, options);
  return dealt;
}

// A move's type as used: the table's, Weather Ball's (setweatherballtype,
// bsc:9787), or Beat Up's '???' (Showdown's gen3). Hidden Power's is in its
// move ID.
constexpr Type move_type(const Battle& battle, Move move) {
  const MoveData& data = get(move);
  if (data.effect == Effect::BeatUp) return Type::Mystery;
  if (data.effect != Effect::WeatherBall) return data.type;
  switch (effective_weather(battle)) {
  case Weather::Sun: return Type::Fire;
  case Weather::Rain: return Type::Water;
  case Weather::Sandstorm: return Type::Rock;
  case Weather::Hail: return Type::Ice;
  default: return data.type;
  }
}

// The damage of a move with no formula (Showdown's getDamage,
// sim/battle-actions.ts:1593), or 0:
// - Seismic Toss and Night Shade (dmgtolevel, bsc:7926): the user's level.
// - Sonic Boom and Dragon Rage (bs1:1720, 819): 20 and 40.
// - Super Fang (bsc:7577): half the target's HP, at least 1. Showdown doesn't
//   floor it, which matches against whole-number hits.
// - Psywave (bs1:1206): `psywave` percent of the user's level, at least 1.
// - OHKO moves (bs1:762): the target's HP; Endure, Focus Band and a Substitute
//   work as for any hit.
constexpr uint32_t fixed_damage(const Battle& battle, Player player, Effect effect,
                                uint8_t psywave) {
  const Pokemon& user = battle.side(player).get(1);
  const Pokemon& target = battle.side(foe(player)).get(1);
  switch (effect) {
  case Effect::LevelDamage: return user.level;
  case Effect::Sonicboom: return 20;
  case Effect::DragonRage: return 40;
  case Effect::SuperFang: return target.hp / 2u == 0 ? 1u : target.hp / 2u;
  case Effect::Psywave: {
    assert(psywave >= 50 && psywave <= 150);
    const uint32_t d = uint32_t(psywave) * user.level / 100u;
    return d == 0 ? 1u : d;
  }
  case Effect::Ohko: return target.hp;
  default: return 0;
  }
}

// ChangeStatBuffs (bsc:6937) raising the user's own stat by n: a raise at +6
// marks the move MISSED (bsc:7097). Returns the stages gained; Showdown prints
// `-boost|...|0` at the cap (sim/battle.ts:2083).
constexpr int8_t raise(ActivePokemon& a, uint8_t stat, int8_t n, MoveState& m) {
  const int8_t gained = raise(a, stat, n);
  if (gained == 0) m.result |= MoveResult::MISSED;
  return gained;
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

// Spikes on a switch-in (Cmd_switchineffects, bsc:5180): max HP / ((5 - layers)
// * 2), at least 1. Returns whether the Pokémon fainted, which skips its
// switch-in ability and item (bs1:3394).
template <typename O>
constexpr bool spikes(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  const uint8_t layers = side.conditions.spikes;
  if (layers == 0 || has_type(a, Type::Flying) ||
      a.ability == Ability::Levitate) {
    return false;
  }
  assert(layers <= 3);
  Pokemon& mon = side.get(1);
  a.volatiles.destiny_bond = false;
  uint16_t damage = uint16_t(mon.stats.hp / ((5 - layers) * 2));
  if (damage == 0) damage = 1;
  mon.hp = mon.hp > damage ? uint16_t(mon.hp - damage) : 0;
  protocol::damage(options.log, protocol::ident(battle, player), mon,
                   protocol::Damage::Spikes);
  if (!mon.fainted()) return false;
  faint(battle, player, options);
  return true;
}

// Cmd_removeitem (bsc:5901): the item is used up, and Recycle remembers it.
constexpr void consume_item(Side& side) {
  side.recycle_item = side.get(1).item;
  side.get(1).item = Item::None;
}

constexpr bool is_berry(Item i) { return i >= Item::CheriBerry && i <= Item::EnigmaBerry; }

// p uses up its item: Showdown's eatItem or useItem, whose -enditem comes
// before the effect (sim/pokemon.ts:1783, 1831).
template <typename O>
constexpr void use_item(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  const Item item = side.get(1).item;
  protocol::end_item(options.log, protocol::ident(battle, player), item,
                     is_berry(item) ? protocol::EndItemFrom::Eat : protocol::EndItemFrom::None);
  consume_item(side);
}

// What ending p's sleep clears besides the status: Nightmare (its -end after
// the -curestatus, sim/pokemon.ts:1677), the Sleep Talk turns kept for
// switch-out, and the public count. Callers clear the status first.
template <typename O>
constexpr void wake(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.volatiles.nightmare) {
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Nightmare);
  }
  a.volatiles.nightmare = false;
  a.sleep_skipped = 0;
  if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 0;
}

// The cures ITEMEFFECT_NORMAL (util:3485-3606) and ITEMEFFECT_MOVE_END
// (util:3627-3752) share; White Herb waits for Showdown's checks (white_herb).
// Returns whether the item was used.
template <typename O>
constexpr bool item_cure_held(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  const auto unconfuse = [&] {
    if (v.confusion == 0) return;
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Confusion);
    v.confusion = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).confusion = 0;
  };
  // Showdown's cureStatus: -curestatus after the item's -enditem.
  const auto cure = [&] {
    use_item(battle, player, options);
    protocol::cure_status(options.log, protocol::ident(battle, player), mon.status);
    mon.status = Status::none();
  };
  switch (item_data(mon.item).effect) {
  case HoldEffect::CurePar:
    if (!mon.status.paralyzed()) return false;
    cure();
    break;
  case HoldEffect::CurePsn:
    if (!mon.status.poisoned()) return false;
    cure();
    a.toxic = 0;
    break;
  case HoldEffect::CureBrn:
    if (!mon.status.burned()) return false;
    cure();
    break;
  case HoldEffect::CureFrz:
    if (!mon.status.frozen()) return false;
    cure();
    break;
  case HoldEffect::CureSlp:
    if (!mon.status.asleep()) return false;
    cure();
    wake(battle, player, options);
    break;
  case HoldEffect::CureConfusion:
    if (v.confusion == 0) return false;
    use_item(battle, player, options);
    unconfuse();
    break;
  case HoldEffect::CureStatus: {
    // Lum Berry: the status, then the confusion.
    if (!mon.status.any() && v.confusion == 0) return false;
    const bool asleep = mon.status.asleep();
    use_item(battle, player, options);
    if (mon.status.any()) {
      protocol::cure_status(options.log, protocol::ident(battle, player), mon.status);
      mon.status = Status::none();
    }
    if (asleep) wake(battle, player, options);
    a.toxic = 0;
    unconfuse();
    break;
  }
  case HoldEffect::RestorePp: {
    // Leppa Berry, as Showdown's onUpdate (data/items.ts:3360): 10 PP to the
    // first slot at 0, at once; the decomp waits for the end of the turn. A
    // Transformed or Mimicked slot's maximum has 3 PP Ups too
    // (sim/pokemon.ts:1320, data/mods/gen4/moves.ts:896).
    uint8_t slot = 0;
    while (slot < 4 && (a.moves[slot].id == Move::None || a.moves[slot].pp != 0)) slot++;
    if (slot == 4) return false;
    use_item(battle, player, options);
    MoveSlot& ms = a.moves[slot];
    const uint8_t max = max_pp(ms.id);
    ms.pp = uint8_t(max < 10 ? max : 10);
    if (!a.volatiles.transformed && !(a.mimicked & (1 << slot))) mon.moves[slot].pp = ms.pp;
    protocol::activate_with(options.log, protocol::ident(battle, player),
                            protocol::Activate::Leppa, ms.id);
    break;
  }
  case HoldEffect::CureAttract: {
    // Mental Herb (gen4 onUpdate): -enditem, a silent -end, then its own.
    if (!v.infatuated) return false;
    use_item(battle, player, options);
    v.infatuated = false;
    const ID id = protocol::ident(battle, player);
    protocol::end_effect(options.log, id, protocol::End::Attract);
    protocol::end_effect(options.log, id, protocol::End::AttractMentalHerb);
    break;
  }
  default:
    return false;
  }
  return true;
}

constexpr bool cure_item(HoldEffect effect) {
  switch (effect) {
  case HoldEffect::CurePar:
  case HoldEffect::CurePsn:
  case HoldEffect::CureBrn:
  case HoldEffect::CureFrz:
  case HoldEffect::CureSlp:
  case HoldEffect::CureConfusion:
  case HoldEffect::CureStatus:
  case HoldEffect::RestorePp:
  case HoldEffect::CureAttract:
    return true;
  default:
    return false;
  }
}

// The check inline, the cure out of line: nearly every call finds no curing
// item.
template <typename O>
[[gnu::always_inline]] constexpr bool item_cure(Battle& battle, Player player, O& options) {
  return cure_item(item_data(battle.side(player).get(1).item).effect) &&
         item_cure_held(battle, player, options);
}

// White Herb: every lowered stage back to 0. Not with the other cures, as the
// decomp, but at Showdown's checks: its onStart (switch-in, Trick),
// onAnySwitchIn, onAnyAfterMove and the residual at order 29
// (data/items.ts:7679).
template <typename O>
constexpr void white_herb_held(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  if (side.get(1).fainted()) return;
  bool lowered = false;
  for (uint8_t i = ATK; i <= EVA; i++) {
    int8_t& b = stage(side.active.boosts, i);
    lowered |= b < 0;
    if (b < 0) b = 0;
  }
  if (!lowered) return;
  use_item(battle, player, options);
  protocol::clear_negative_boost(options.log, protocol::ident(battle, player));
}

template <typename O>
[[gnu::always_inline]] constexpr void white_herb(Battle& battle, Player player, O& options) {
  if (holds(battle.side(player), HoldEffect::RestoreStats)) white_herb_held(battle, player, options);
}

// Both White Herbs, by Speed (onAny handlers): the order changes only the log.
template <typename O> constexpr void white_herbs(Battle& battle, O& options) {
  const Player first = log_order(battle);
  white_herb(battle, first, options);
  white_herb(battle, foe(first), options);
}

// ITEMEFFECT_ON_SWITCH_IN (util:3291): White Herb.
template <typename O>
constexpr void item_switch_in(Battle& battle, Player player, O& options) {
  white_herb(battle, player, options);
}

// ---------------------------------------------------------------------------
// Abilities and weather
// ---------------------------------------------------------------------------

// Forecast, as Showdown's onWeatherChange (data/abilities.ts:1476): Castform
// takes the effective weather's type. It runs at its switch-in, after each
// weather change and when a Cloud Nine or Air Lock Pokémon leaves, not when one
// enters (data/mods/gen4/abilities.ts:37); the decomp also checks after every
// move (util:2504). The log line follows `after`'s Faint if set.
template <typename O>
constexpr void forecast(Battle& battle, Player p, Weather w, O& options,
                        ID after = protocol::NO_ID) {
  ActivePokemon& a = battle.side(p).active;
  if (a.species != Species::Castform || a.ability != Ability::Forecast ||
      a.volatiles.transformed || battle.side(p).get(1).fainted()) {
    return;
  }
  const Type t = w == Weather::Sun    ? Type::Fire
                 : w == Weather::Rain ? Type::Water
                 : w == Weather::Hail ? Type::Ice
                                      : Type::Normal;
  if (a.types[0] == t && a.types[1] == t) return;
  a.types[0] = t;
  a.types[1] = t;
  if (after.bits != 0) {
    protocol::forme_change_after(options.log, after, protocol::ident(battle, p), t);
  } else {
    protocol::forme_change(options.log, protocol::ident(battle, p), t);
  }
}

// Showdown's eachEvent('WeatherChange'): Forecast on both sides, in Speed
// order. `leaving` is a Pokémon whose Cloud Nine or Air Lock is ending.
template <typename O>
constexpr void weather_change(Battle& battle, O& options,
                              std::optional<Player> leaving = std::nullopt) {
  const bool p1 = battle.sides[0].active.species == Species::Castform;
  const bool p2 = battle.sides[1].active.species == Species::Castform;
  if (!p1 && !p2) return;
  Weather w = effective_weather(battle);
  if (leaving) {
    const Side& other = battle.side(foe(*leaving));
    const Ability ab = other.active.ability;
    const bool suppressed =
        (ab == Ability::CloudNine || ab == Ability::AirLock) && !other.get(1).fainted();
    w = suppressed ? Weather::None : battle.field.weather;
  }
  const Player first = p1 && p2 ? log_order(battle) : p1 ? Player::P1 : Player::P2;
  forecast(battle, first, w, options);
  forecast(battle, foe(first), w, options);
}

constexpr Weather ability_weather(Ability ability) {
  switch (ability) {
  case Ability::Drizzle: return Weather::Rain;
  case Ability::Drought: return Weather::Sun;
  case Ability::SandStream: return Weather::Sandstorm;
  default: return Weather::None;
  }
}

// Intimidate (bs1:4024; Showdown's gen3 onStart,
// data/mods/gen3/abilities.ts:61): -1 Attack to the foe, not through a
// Substitute, and drops() decides the rest. White Herb doesn't react until its
// next check (white_herb).
template <typename O>
constexpr void intimidate(Battle& battle, Player player, O& options) {
  const Player target = foe(player);
  Side& side = battle.side(target);
  if (side.get(1).fainted() || side.active.volatiles.substitute) return;
  protocol::ability(options.log, protocol::ident(battle, player), Ability::Intimidate,
                    protocol::AbilityFrom::Boost);
  lower_stats(battle, target, 1, 1, options.log);
}

constexpr bool immunity_ability(Ability ability) {
  switch (ability) {
  case Ability::Immunity:
  case Ability::OwnTempo:
  case Ability::Limber:
  case Ability::Insomnia:
  case Ability::VitalSpirit:
  case Ability::WaterVeil:
  case Ability::MagmaArmor:
  case Ability::Oblivious:
    return true;
  default:
    return false;
  }
}

template <typename O> constexpr void ability_immunity_held(Battle& battle, O& options);

// The check inline, the cure out of line: nearly every call finds neither
// ability.
template <typename O>
[[gnu::always_inline]] constexpr void ability_immunity(Battle& battle, O& options) {
  if (immunity_ability(battle.sides[0].active.ability) ||
      immunity_ability(battle.sides[1].active.ability)) {
    ability_immunity_held(battle, options);
  }
}

// ABILITYEFFECT_ON_SWITCHIN (util:2470) with Intimidate and Trace:
// - Drizzle, Drought and Sand Stream set a permanent weather, unless an
//   ability's is already there.
// - Trace copies a standing foe's ability, which doesn't start in Gen III
//   (sim/pokemon.ts:1941).
// - Pressure announces itself, in the log only
//   (data/mods/gen3/abilities.ts:148).
template <typename O>
constexpr void ability_switch_in(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (const Weather w = ability_weather(a.ability); w != Weather::None) {
    Field& field = battle.field;
    if (field.weather != w || !field.permanent) {
      field.weather = w;
      field.weather_turns = 0;
      field.permanent = true;
      protocol::weather_ability(options.log, w, a.ability, protocol::ident(battle, player));
      weather_change(battle, options);
    }
    return;
  }
  if (a.ability == Ability::Forecast) {
    return forecast(battle, player, effective_weather(battle), options);
  }
  if (a.ability == Ability::Intimidate) return intimidate(battle, player, options);
  if (a.ability == Ability::Trace) {
    const Side& f = battle.side(foe(player));
    if (!f.get(1).fainted() && f.active.ability != Ability::None) {
      a.ability = f.active.ability;
      protocol::ability_copy(options.log, protocol::ident(battle, player), a.ability,
                             Ability::Trace, protocol::AbilityFrom::Trace,
                             protocol::ident(battle, foe(player)));
    }
    return;
  }
  if (a.ability == Ability::Pressure) {
    protocol::ability(options.log, protocol::ident(battle, player), Ability::Pressure,
                      protocol::AbilityFrom::Pressure);
  }
}

// Baton Pass's switch-in (ENGINE.md decision 5): the passer leaves, the
// recipient enters, gets what passes and arrives (bs1:1713). What passes is the
// decomp's keep-list (bsc:4646; main:3172-3209) with Showdown's differences
// (copyVolatileFrom, sim/pokemon.ts:1246): binding passes (the decomp clears
// STATUS2_WRAPPED), and the passer's Lock-On keeps its count (the decomp resets
// it, main:3178).
template <typename O>
constexpr void baton_pass_in(Battle& battle, Player player, uint8_t slot,
                             O& options) {
  const ActivePokemon old = battle.side(player).active;
  leave(battle, player, slot, options, Leave::BatonPass);
  ActivePokemon& a = battle.side(player).active;
  const Volatiles& v = old.volatiles;
  Volatiles& n = a.volatiles;
  a.boosts = old.boosts;
  n.confusion = v.confusion;
  n.focus_energy = v.focus_energy;
  n.substitute = v.substitute;
  a.substitute_hp = old.substitute_hp;
  n.escape_prevention = v.escape_prevention;
  n.cursed = v.cursed;
  n.leech_seed = v.leech_seed;
  n.perish_song = v.perish_song;
  a.perish_song_timer = old.perish_song_timer;
  n.rooted = v.rooted;
  n.mud_sport = v.mud_sport;
  n.water_sport = v.water_sport;
  n.wrapped = v.wrapped;
  n.wrapper_gone = v.wrapper_gone;
  n.lock_on = v.lock_on;
  a.wrapped_move = old.wrapped_move;
  // Showdown's gen3 Update after the action, before the recipient's runSwitch:
  // a berry takes what was passed.
  item_cure(battle, player, options);
  arrive(battle, player, options);
}

// ---------------------------------------------------------------------------
// Move properties
// ---------------------------------------------------------------------------

// The sound moves (sSoundMovesTable, util:688), which Soundproof stops
// (try_hit). Showdown's flag adds Perish Song and Heal Bell, which handle
// Soundproof themselves.
constexpr bool sound(Move move) {
  switch (move) {
  case Move::Growl:
  case Move::Roar:
  case Move::Sing:
  case Move::Supersonic:
  case Move::Screech:
  case Move::Snore:
  case Move::Uproar:
  case Move::MetalSound:
  case Move::GrassWhistle:
  case Move::HyperVoice: return true;
  default: return false;
  }
}

// Whether the move is aimed at the user: MOVE_TARGET_USER, and Showdown's
// 'self' among the DEPENDS moves (not Sleep Talk or Curse). Conversion 2 is
// Showdown's 'normal'.
constexpr bool targets_user(Move move) {
  switch (move) {
  case Move::MagicCoat:
  case Move::Snatch:
  case Move::Metronome:
  case Move::Assist:
  case Move::MirrorMove:
  case Move::NaturePower: return true;
  case Move::Conversion2: return false;
  default: return get(move).target == Target::User;
  }
}

// MoveState::self: aimed at the user, a non-Ghost's Curse included.
constexpr bool self_aimed(const ActivePokemon& a, Move move) {
  return targets_user(move) || (move == Move::Curse && !has_type(a, Type::Ghost));
}

// Whether Showdown's |move| line names the user: moves aimed at the user or the
// field (sim/battle.ts:2506), Sleep Talk, and a non-Ghost's Curse.
constexpr bool aims_at_user(const ActivePokemon& a, Move move) {
  if (move == Move::Curse) return !has_type(a, Type::Ghost);
  return targets_user(move) || move == Move::SleepTalk;
}

// The effects whose script is BattleScript_EffectHit (bs1:236) or built on it;
// hit() runs them all.
constexpr bool plain_hit(Effect effect) {
  switch (effect) {
  // The plain hit and its accuracy, crit and priority variants.
  case Effect::Hit:
  case Effect::AlwaysHit:
  case Effect::HighCritical:
  case Effect::VitalThrow:
  case Effect::FalseSwipe:
  case Effect::QuickAttack:
  case Effect::Pursuit:
  // Hits that reach a semi-invulnerable target.
  case Effect::Thunder:
  case Effect::SkyUppercut:
  case Effect::Earthquake:
  case Effect::Gust:
  case Effect::Twister:
  // Secondary effects (secondary_of).
  case Effect::PoisonHit:
  case Effect::PoisonTail:
  case Effect::PoisonFang:
  case Effect::BurnHit:
  case Effect::BlazeKick:
  case Effect::ThawHit:
  case Effect::FreezeHit:
  case Effect::ParalyzeHit:
  case Effect::FlinchHit:
  case Effect::FlinchMinimizeHit:
  case Effect::ConfuseHit:
  case Effect::TriAttack:
  case Effect::SecretPower:
  case Effect::AttackDownHit:
  case Effect::DefenseDownHit:
  case Effect::SpeedDownHit:
  case Effect::SpecialAttackDownHit:
  case Effect::SpecialDefenseDownHit:
  case Effect::AccuracyDownHit:
  case Effect::AttackUpHit:
  case Effect::DefenseUpHit:
  case Effect::AllStatsUpHit:
  // Their own failures first.
  case Effect::FocusPunch:
  case Effect::Snore:
  case Effect::FakeOut:
  case Effect::SpitUp:
  // Variable power.
  case Effect::HiddenPower:
  case Effect::Return:
  case Effect::Frustration:
  case Effect::Flail:
  case Effect::Eruption:
  case Effect::Facade:
  case Effect::Revenge:
  case Effect::Smellingsalt:
  case Effect::WeatherBall:
  case Effect::LowKick:
  case Effect::Magnitude:
  case Effect::Present:
  case Effect::FuryCutter:
  // Fixed damage.
  case Effect::LevelDamage:
  case Effect::Sonicboom:
  case Effect::DragonRage:
  case Effect::SuperFang:
  case Effect::Psywave:
  case Effect::Ohko:
  case Effect::Counter:
  case Effect::MirrorCoat:
  case Effect::Endeavor:
  // Several hits.
  case Effect::MultiHit:
  case Effect::DoubleHit:
  case Effect::Twineedle:
  case Effect::TripleKick:
  // Two turns, a recharge or a lock.
  case Effect::SolarBeam:
  case Effect::RazorWind:
  case Effect::SkullBash:
  case Effect::SkyAttack:
  case Effect::SemiInvulnerable:
  case Effect::Recharge:
  case Effect::Rampage:
  case Effect::Uproar:
  case Effect::Rollout:
  // Effects on the user.
  case Effect::Recoil:
  case Effect::DoubleEdge:
  case Effect::RecoilIfMiss:
  case Effect::Explosion:
  case Effect::Superpower:
  case Effect::Overheat:
  case Effect::Absorb:
  case Effect::DreamEater:
  case Effect::RapidSpin:
  case Effect::Rage:
  // Effects on the target, its item or its side.
  case Effect::Trap:
  case Effect::PayDay:
  case Effect::KnockOff:
  case Effect::Thief:
  case Effect::BrickBreak:
    return true;
  default:
    return false;
  }
}

// setmoveeffect in each hit's script (bs1): Thunder paralyzes, Stomp and its
// kin flinch (bs1:1898), Twineedle poisons (bs1:1078), and Secret Power
// paralyzes in a link battle, as Showdown's gen4.
constexpr Secondary secondary_of(Effect effect) {
  switch (effect) {
  case Effect::PoisonHit:
  case Effect::PoisonTail:
  case Effect::Twineedle: return Secondary::Poison;
  case Effect::PoisonFang: return Secondary::Toxic;
  case Effect::BurnHit:
  case Effect::BlazeKick:
  case Effect::ThawHit: return Secondary::Burn;
  case Effect::FreezeHit: return Secondary::Freeze;
  case Effect::ParalyzeHit:
  case Effect::Thunder:
  case Effect::SemiInvulnerable: // Bounce; Fly, Dig and Dive have chance 0
  case Effect::SecretPower: return Secondary::Paralysis;
  case Effect::ConfuseHit: return Secondary::Confusion;
  case Effect::FlinchHit:
  case Effect::FlinchMinimizeHit:
  case Effect::Twister:
  case Effect::FakeOut:
  case Effect::SkyAttack:
  case Effect::Snore: return Secondary::Flinch;
  case Effect::TriAttack: return Secondary::TriAttack;
  case Effect::AttackDownHit: return Secondary::AtkDown;
  case Effect::DefenseDownHit: return Secondary::DefDown;
  case Effect::SpeedDownHit: return Secondary::SpeDown;
  case Effect::SpecialAttackDownHit: return Secondary::SpaDown;
  case Effect::SpecialDefenseDownHit: return Secondary::SpdDown;
  case Effect::AccuracyDownHit: return Secondary::AccDown;
  case Effect::AttackUpHit: return Secondary::AtkUp;
  case Effect::DefenseUpHit: return Secondary::DefUp;
  case Effect::AllStatsUpHit: return Secondary::AllUp;
  default: return Secondary::None;
  }
}

// Every hit has a power.
static_assert([] {
  for (uint16_t i = 1; i < MOVES; i++) {
    const MoveData& data = get(Move(i));
    if (plain_hit(data.effect) && data.bp == 0) return false;
  }
  return true;
}());

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// The confusion self-hit, replacing the move (bs1:3796, aimed at the user,
// util:2158): Showdown's typeless 40-power hit (Hit::confusion), with Focus
// Band even behind a Substitute (the decomp's none there, bsc:1725). It counts
// for Bide (bsc:1905), not Counter. Its draws are hits[0].
template <typename O>
constexpr void hurt_itself(Battle& battle, Player player, MoveState& m, O& options) {
  Pokemon& mon = battle.side(player).get(1);
  m.self = true;
  m.unable = true;
  uint32_t d = apply_roll(damage(battle, player, player, Hit::confusion()),
                          rolls::damage(battle, player, 0, options));
  const ItemData item = item_data(mon.item);
  if (mon.hp <= d && item.effect == HoldEffect::FocusBand &&
      rolls::focus_band(battle, player, 0, item.param, options)) {
    protocol::activate(options.log, protocol::ident(battle, player), protocol::Activate::FocusBand);
    d = mon.hp - 1u;
  }
  add_bide(battle.side(player).active, d);
  mon.hp = mon.hp > d ? uint16_t(mon.hp - d) : 0;
  protocol::damage(options.log, protocol::ident(battle, player), mon, protocol::Damage::Confusion);
  abort_move(battle, player, options);
  if (mon.fainted()) faint(battle, player, options);
}

// Whether an Uproar is going on. Showdown's blocks every sleep
// (data/moves.ts:20253); the decomp spares Soundproof (bsc:6807), banned in OU.
constexpr bool uproar(const Battle& battle) {
  return battle.sides[0].active.volatiles.uproar != 0 ||
         battle.sides[1].active.volatiles.uproar != 0;
}

// The uproar's -fail for a sleep it stops on p ([msg] if p is causing it).
template <typename O>
constexpr void uproar_stops(const Battle& battle, Player p, O& options) {
  const bool own = battle.side(p).active.volatiles.uproar != 0;
  protocol::fail(options.log, protocol::ident(battle, p),
                 own ? protocol::Fail::UproarMsg : protocol::Fail::Uproar);
}

// Sleep Clause Mod (Showdown's sleepclausemod, data/rulesets.ts:1379): a sleep
// from the foe fails while a Pokémon on `target`'s side sleeps from the foe.
// Rest's doesn't count.
constexpr bool sleep_clause(const Battle& battle, Player target) {
  for (const Pokemon& p : battle.side(target).pokemon) {
    if (!p.fainted() && p.status.asleep() && !p.status.self_inflicted()) return true;
  }
  return false;
}

// Whether a major status `s` can take hold on `target`: SetMoveEffect's checks
// (bsc:2408-2560) without Safeguard: not on a fainted or statused Pokémon, the
// type and ability immunities, and for a freeze sun and Freeze Clause
// (Showdown's freezeclausemod).
constexpr bool can_status(const Battle& battle, Player target, Secondary s) {
  const Side& side = battle.side(target);
  const ActivePokemon& t = side.active;
  if (side.get(1).fainted() || side.get(1).status.any()) return false;
  switch (s) {
  case Secondary::Poison:
  case Secondary::Toxic:
    return !has_type(t, Type::Poison) && !has_type(t, Type::Steel) &&
           t.ability != Ability::Immunity;
  case Secondary::Burn:
    return !has_type(t, Type::Fire) && t.ability != Ability::WaterVeil;
  case Secondary::Freeze: {
    if (has_type(t, Type::Ice) || t.ability == Ability::MagmaArmor) return false;
    if (effective_weather(battle) == Weather::Sun) return false;
    for (const Pokemon& p : side.pokemon) {
      if (p.status.frozen()) return false;
    }
    return true;
  }
  case Secondary::Paralysis:
    return t.ability != Ability::Limber;
  case Secondary::Sleep:
    // SetMoveEffect's sleep case (bsc:2273).
    return t.ability != Ability::VitalSpirit && t.ability != Ability::Insomnia &&
           !uproar(battle);
  default:
    assert(false && "not a major status");
    return false;
  }
}

// Showdown's gen3 focuspunch volatile, from the start of the turn p chose Focus
// Punch: it stops a flinch (onTryAddVolatile), so Fake Out only breaks the
// focus. The decomp's flinch stops Focus Punch.
constexpr bool focusing(const Battle& battle, Player p) {
  const Request& r = battle.request;
  return r.choice[index(p)].type() == Choice::Move && r.move[index(p)] == Move::FocusPunch;
}

// Whether a move's major status `s` takes hold on p's foe: Safeguard,
// can_status, Sleep Clause.
constexpr bool status_takes(const Battle& battle, Player player, Secondary s) {
  return battle.side(foe(player)).conditions.safeguard == 0 &&
         can_status(battle, foe(player), s) &&
         !(s == Secondary::Sleep && sleep_clause(battle, foe(player)));
}

// Sets a major status on `target`, as SetMoveEffect once statusChanged
// (bsc:2476): poison, toxic, paralysis and burn arm Synchronize (bsc:2503), and
// the toxic counter restarts. The decomp's sleep and freeze also cancel a
// charging move (bsc:2296); Showdown's wait for the move to be aborted
// (cancel), so a Pokémon that wakes first still fires.
template <typename O>
constexpr void inflict(Battle& battle, Player target, Secondary s,
                       MoveState& m, O& options,
                       protocol::StatusSource src = protocol::StatusSource::none()) {
  Side& side = battle.side(target);
  Status& status = side.get(1).status;
  switch (s) {
  case Secondary::Sleep:
    // (Random() & 3) + 2 turns, hidden; the public count starts at 1.
    status = Status::slp(uint8_t(Status::MIN_SLEEP + battle.rng.range(4)));
    if constexpr (CHANCE<O>) options.chance.durations.get(target).sleep[0] = 1;
    break;
  case Secondary::Poison: status = Status::psn(); break;
  case Secondary::Toxic: status = Status::tox(); break;
  case Secondary::Burn: status = Status::brn(); break;
  case Secondary::Freeze: status = Status::frz(); break;
  case Secondary::Paralysis: status = Status::par(); break;
  default: assert(false && "not a major status");
  }
  side.active.toxic = 0;
  if (s != Secondary::Freeze && s != Secondary::Sleep) {
    m.synchronize = s;
    m.synchronized = target;
  }
  protocol::status(options.log, protocol::ident(battle, target), status, src);
  // Showdown's Lum Berry eats at once (onAfterSetStatus), except for a
  // Synchronize holder, which waits for moveend.
  if (item_data(side.get(1).item).effect == HoldEffect::CureStatus &&
      side.active.ability != Ability::Synchronize) {
    item_cure(battle, target, options);
  }
}

// MOVE_EFFECT_CONFUSION taking hold on `target` (bsc:2541): a hidden Random() %
// 4 + 2 turns; the public count starts at 1.
template <typename O>
constexpr void confuse(Battle& battle, Player target, O& options,
                       protocol::Start how = protocol::Start::Confusion) {
  ActivePokemon& t = battle.side(target).active;
  assert(t.volatiles.confusion == 0);
  protocol::start(options.log, protocol::ident(battle, target), how);
  t.volatiles.confusion = uint8_t(2 + battle.rng.range(4));
  if constexpr (CHANCE<O>) options.chance.durations.get(target).confusion = 1;
}

// seteffectwithchance (bsc:2923) and SetMoveEffect (bsc:2380) for a hit's
// secondary effect on hit `i`: the table's chance, doubled by Serene Grace,
// drawn only where the effect could take hold (CLAUDE.md, "Chance").
template <typename O>
constexpr void secondary(Battle& battle, Player player, MoveState& m,
                         O& options, uint8_t i = 0) {
  const MoveData& data = get(m.move);
  const Secondary s = secondary_of(data.effect);
  if (s == Secondary::None || (m.result & MoveResult::NO_EFFECT)) return;
  // Fake Out's flinch is certain (MOVE_EFFECT_CERTAIN).
  const bool serene = battle.side(player).active.ability == Ability::SereneGrace;
  const uint32_t chance = data.effect == Effect::FakeOut ? 100u : data.chance * (serene ? 2u : 1u);
  if (chance == 0) return;

  // A raise on the user (MOVE_EFFECT_AFFECTS_USER) fails only at +6 (bsc:6937).
  // Ancient Power and Silver Wind (bsc:2814) raise whichever of the five aren't
  // at +6, logged in Showdown's key order.
  if (s == Secondary::AllUp || s == Secondary::AtkUp || s == Secondary::DefUp) {
    constexpr uint8_t ALL[5] = {ATK, DEF, SPA, SPD, SPE};
    const std::span<const uint8_t> stats{ALL + (s == Secondary::DefUp),
                                         s == Secondary::AllUp ? 5u : 1u};
    Boosts& b = battle.side(player).active.boosts;
    bool any = false;
    for (const uint8_t stat : stats) any |= stage(b, stat) < 6;
    if (!any || (chance < 100 && !rolls::secondary(battle, player, i, chance, options))) return;
    for (const uint8_t stat : stats) {
      if (stage(b, stat) >= 6) continue;
      stage(b, stat)++;
      protocol::boost(options.log, protocol::ident(battle, player), stat, 1, false);
    }
    return;
  }

  // SetMoveEffect's preamble: not into a fainted target, a Substitute or Shield
  // Dust.
  Side& side = battle.side(foe(player));
  ActivePokemon& t = side.active;
  const Pokemon& target = side.get(1);
  if (target.fainted() || t.volatiles.substitute ||
      t.ability == Ability::ShieldDust) {
    return;
  }

  bool matters = false;
  switch (s) {
  case Secondary::AtkDown:
  case Secondary::DefDown:
  case Secondary::SpeDown:
  case Secondary::SpaDown:
  case Secondary::SpdDown:
  case Secondary::AccDown: {
    // ChangeStatBuffs (bsc:6937) for a secondary drop: Mist, Clear Body, White
    // Smoke, Hyper Cutter (Attack), Keen Eye (accuracy) and -6 stop it.
    const uint8_t stat = uint8_t(s) - uint8_t(Secondary::AtkDown);
    matters = side.conditions.mist == 0 &&
              t.ability != Ability::ClearBody &&
              t.ability != Ability::WhiteSmoke &&
              !(t.ability == Ability::HyperCutter && stat == ATK) &&
              !(t.ability == Ability::KeenEye && stat == ACC) &&
              stage(t.boosts, stat) > -6;
    break;
  }
  case Secondary::Confusion:
    // Safeguard (bsc:2257), Own Tempo and an existing confusion (bsc:2534) stop
    // it.
    matters = side.conditions.safeguard == 0 &&
              t.ability != Ability::OwnTempo && t.volatiles.confusion == 0;
    break;
  case Secondary::Flinch:
    // Only if the target hasn't moved yet this turn (bsc:2685).
    matters = m.first && !t.volatiles.flinched &&
              t.ability != Ability::InnerFocus && !focusing(battle, foe(player));
    break;
  case Secondary::TriAttack:
    // Any status stops it before the 1/3 draw.
    matters = !target.status.any();
    break;
  case Secondary::Toxic:
    // Into a Poison or Steel type it marks the move as not affecting the foe
    // (bsc:2552).
    matters = status_takes(battle, player, s) ||
              (!target.status.any() && side.conditions.safeguard == 0 &&
               (has_type(t, Type::Poison) || has_type(t, Type::Steel)));
    break;
  default:
    matters = status_takes(battle, player, s);
  }
  if (!matters) return;
  if (chance < 100 && !rolls::secondary(battle, player, i, chance, options)) {
    return;
  }

  switch (s) {
  case Secondary::AtkDown:
  case Secondary::DefDown:
  case Secondary::SpeDown:
  case Secondary::SpaDown:
  case Secondary::SpdDown:
  case Secondary::AccDown: {
    const uint8_t stat = uint8_t(uint8_t(s) - uint8_t(Secondary::AtkDown));
    stage(t.boosts, stat)--;
    protocol::boost(options.log, protocol::ident(battle, foe(player)), stat, 1, true);
    break;
  }
  case Secondary::Confusion:
    confuse(battle, foe(player), options);
    break;
  case Secondary::Flinch:
    t.volatiles.flinched = true;
    break;
  case Secondary::TriAttack: {
    constexpr Secondary TRI[3] = {Secondary::Burn, Secondary::Freeze,
                                  Secondary::Paralysis};
    const Secondary which = TRI[rolls::tri_attack(battle, player, options) - 1];
    if (status_takes(battle, player, which)) inflict(battle, foe(player), which, m, options);
    break;
  }
  case Secondary::Toxic:
    if (status_takes(battle, player, s)) {
      inflict(battle, foe(player), s, m, options);
    } else {
      m.result |= MoveResult::DOESNT_AFFECT_FOE;
    }
    break;
  default:
    inflict(battle, foe(player), s, m, options);
  }
}

// ---------------------------------------------------------------------------
// Hit effects
// ---------------------------------------------------------------------------

// ABILITYEFFECT_ABSORBING (util:2662), as Showdown's TryHit handlers (after the
// accuracy roll and Protect): Volt Absorb and Water Absorb take a damaging
// Electric or Water move and heal a quarter (bs1:4078), Flash Fire any Fire
// move unless frozen (bs1:4101). Showdown's Flash Fire lets Will-O-Wisp through
// to a Fire type, a statused holder or one behind a Substitute.
enum class Absorb : uint8_t {
  None,
  Idle,  // absorbed with no effect: -immune [from] the ability
  Takes, // absorbed, healing or starting Flash Fire
};

constexpr Absorb absorbs(const Battle& battle, Player player, const MoveState& m) {
  const Side& side = battle.side(foe(player));
  const ActivePokemon& t = side.active;
  const Pokemon& target = side.get(1);
  const MoveData& data = get(m.move);
  switch (t.ability) {
  case Ability::VoltAbsorb:
  case Ability::WaterAbsorb: {
    const Type type =
        t.ability == Ability::VoltAbsorb ? Type::Electric : Type::Water;
    if (m.type != type || data.bp == 0) return Absorb::None;
    return target.hp != target.stats.hp ? Absorb::Takes : Absorb::Idle;
  }
  case Ability::FlashFire:
    if (m.type != Type::Fire || target.status.frozen()) return Absorb::None;
    if (data.effect == Effect::WillOWisp &&
        (has_type(t, Type::Fire) || target.status.any() || t.volatiles.substitute)) {
      return Absorb::None;
    }
    return t.volatiles.flash_fire ? Absorb::Idle : Absorb::Takes;
  default:
    return Absorb::None;
  }
}

// The absorption absorbs() found.
template <typename O>
constexpr void absorbed(Battle& battle, Player player, MoveState& m, Absorb a,
                        O& options) {
  assert(a != Absorb::None);
  Side& side = battle.side(foe(player));
  ActivePokemon& t = side.active;
  Pokemon& target = side.get(1);
  const ID id = protocol::ident(battle, foe(player));
  if (t.ability != Ability::FlashFire) m.result |= MoveResult::DOESNT_AFFECT_FOE;
  if (a == Absorb::Idle) {
    protocol::immune(options.log, id, protocol::Immune::Ability, t.ability);
  } else if (t.ability == Ability::FlashFire) {
    t.volatiles.flash_fire = true;
    protocol::start(options.log, id, protocol::Start::FlashFire);
  } else {
    heal(target, fraction(target, 4));
    protocol::heal_ability(options.log, id, target, t.ability,
                           protocol::ident(battle, player));
  }
}

// Showdown's reflectable flag: FLAG_MAGIC_COAT_AFFECTED, and Kinesis.
constexpr bool reflectable(Move move) {
  return (get(move).flags & Flag::MagicCoatAffected) || move == Move::Kinesis;
}

// The target's Magic Coat bounces m, as Showdown's gen4 onTryHit: past the
// accuracy roll and Protect, back at the user with no PP, once. The decomp
// bounces in the canceler.
constexpr bool bounces(Player player, const MoveState& m) {
  return m.turn != nullptr && m.turn->magic_coat[index(foe(player))] &&
         m.chosen != Move::MagicCoat && reflectable(m.move);
}

// The move that runs for `user` on behalf of Snatch or Magic Coat (`by`).
template <typename O>
constexpr void call_back(Battle& battle, Player user, Move move, Move by,
                         const MoveState& m, O& options) {
  MoveState c{};
  c.move = move;
  c.chosen = by;
  c.from = by;
  c.slot = NO_SLOT;
  c.no_pp = true;
  c.called = true;
  c.turn = m.turn;
  execute(battle, user, c, options);
  assert(!c.drag && !c.pass);
}

// Snatch, as Showdown's gen4 onAnyPrepareHit: a snatchable move is stolen once
// its user has paid the PP (the decomp's, at the canceler), and the snatcher
// uses it with no PP; Pressure takes a PP from its Snatch. Returns whether the
// move was stolen.
template <typename O>
constexpr bool snatched(Battle& battle, Player player, MoveState& m, O& options) {
  const Player snatcher = foe(player);
  if (m.turn == nullptr || !m.turn->snatch[index(snatcher)] ||
      m.chosen == Move::Snatch || !(get(m.move).flags & Flag::SnatchAffected)) {
    return false;
  }
  m.turn->snatch[index(snatcher)] = false;
  protocol::activate_with(options.log, protocol::ident(battle, snatcher),
                          protocol::Activate::Snatch, Move::None,
                          protocol::ident(battle, player));
  if (const uint8_t s = slot_of(battle.side(snatcher).active, Move::Snatch);
      s != NO_SLOT && battle.side(player).active.ability == Ability::Pressure) {
    deduct_pp(battle, snatcher, s, 1);
  }
  call_back(battle, snatcher, m.move, Move::Snatch, m, options);
  m.result |= MoveResult::FAILED;
  return true;
}

// ABILITYEFFECT_SYNCHRONIZE and ATK_SYNCHRONIZE (util:2957, 2973): a status
// this move gave `holder`'s Synchronize passes back to the other Pokémon (toxic
// as poison), arming it again. Showdown's Safeguard blocks it
// (data/moves.ts:15598); the decomp's doesn't.
template <typename O>
constexpr void synchronize(Battle& battle, Player holder, MoveState& m,
                           O& options) {
  if (battle.side(holder).active.ability != Ability::Synchronize ||
      m.synchronize == Secondary::None || m.synchronized != holder) {
    return;
  }
  const Secondary s =
      m.synchronize == Secondary::Toxic ? Secondary::Poison : m.synchronize;
  m.synchronize = Secondary::None;
  const Player other = foe(holder);
  if (!can_status(battle, other, s)) return;
  if (battle.side(other).conditions.safeguard != 0) {
    protocol::activate(options.log, protocol::ident(battle, other), protocol::Activate::Safeguard);
    return;
  }
  inflict(battle, other, s, m, options,
          protocol::StatusSource::by_ability(Ability::Synchronize,
                                             protocol::ident(battle, holder)));
}

// MOVEEND_DEFROST: a Fire move's damage thaws the target, by the table's type
// as Showdown's gen3 frz (data/mods/gen3/conditions.ts:47), so neither Hidden
// Power Fire (bsc:1852) nor Weather Ball thaws.
template <typename O>
constexpr void defrost(Battle& battle, Player player, const MoveState& m, O& options) {
  Pokemon& target = battle.side(foe(player)).get(1);
  const MoveData& data = get(m.move);
  if (!m.self && target.status.frozen() && !target.fainted() && m.damaged &&
      !(m.result & MoveResult::NO_EFFECT) && data.type == Type::Fire &&
      data.effect != Effect::HiddenPower) {
    protocol::cure_status(options.log, protocol::ident(battle, foe(player)), target.status);
    target.status = Status::none();
  }
}

// ABILITYEFFECT_ON_DAMAGE (util:2735): the target's Color Change, or its
// contact ability on the attacker, after a hit that took the target's own HP,
// as Showdown's gen3 handlers. Showdown's Safeguard blocks the statuses; the
// decomp's doesn't.
template <typename O>
constexpr void ability_on_damage(Battle& battle, Player player, MoveState& m,
                                 uint8_t i, O& options) {
  if (m.self || !m.damaged || m.hp_dealt == 0 || (m.result & MoveResult::NO_EFFECT)) return;
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const Pokemon& target_mon = battle.side(target).get(1);
  const MoveData& data = get(m.move);

  if (t.ability == Ability::ColorChange) {
    if (m.move != Move::Struggle && data.bp != 0 && m.type != Type::Mystery &&
        !has_type(t, m.type) &&
        !target_mon.fainted()) {
      t.types[0] = m.type;
      t.types[1] = m.type;
      protocol::start_type(options.log, protocol::ident(battle, target), m.type, true);
    }
    return;
  }

  Side& side = battle.side(player);
  Pokemon& user = side.get(1);
  if (user.fainted() || !(data.flags & Flag::MakesContact)) return;
  const auto takes = [&](Secondary s) { return status_takes(battle, target, s); };
  const auto from_ability = [&] {
    return protocol::StatusSource::by_ability(t.ability, protocol::ident(battle, target));
  };
  Secondary s = Secondary::None;
  switch (t.ability) {
  case Ability::RoughSkin:
    passive_damage(battle, player, fraction(user, 16), options);
    protocol::damage_ability(options.log, protocol::ident(battle, player), user,
                             Ability::RoughSkin, protocol::ident(battle, target));
    return;
  case Ability::EffectSpore: {
    if (!takes(Secondary::Sleep) && !takes(Secondary::Poison) &&
        !takes(Secondary::Paralysis)) {
      return;
    }
    if (!rolls::contact(battle, player, i, 10, options)) return;
    const uint8_t which = rolls::spore(battle, player, i, options);
    s = which == 1   ? Secondary::Sleep
        : which == 2 ? Secondary::Poison
                     : Secondary::Paralysis;
    if (takes(s)) inflict(battle, player, s, m, options, from_ability());
    return;
  }
  case Ability::PoisonPoint: s = Secondary::Poison; break;
  case Ability::Static: s = Secondary::Paralysis; break;
  case Ability::FlameBody: s = Secondary::Burn; break;
  case Ability::CuteCharm: {
    ActivePokemon& a = side.active;
    const Gender g1 = user.personality.gender();
    const Gender g2 = target_mon.personality.gender();
    // A holder that faints to the hit still attracts, as Showdown.
    if (a.ability == Ability::Oblivious || a.volatiles.infatuated ||
        g1 == Gender::None || g2 == Gender::None || g1 == g2) {
      return;
    }
    if (rolls::contact(battle, player, i, 3, options)) {
      a.volatiles.infatuated = true;
      protocol::start(options.log, protocol::ident(battle, player),
                      protocol::Start::AttractCuteCharm, protocol::ident(battle, target));
    }
    return;
  }
  default:
    return;
  }
  if (takes(s) && rolls::contact(battle, player, i, 3, options)) {
    inflict(battle, player, s, m, options, from_ability());
  }
}

// ABILITYEFFECT_IMMUNITY (util:2850), for both Pokémon: an ability cures the
// status it prevents (one from before it: Baton Pass, Trace, Skill Swap), as
// Showdown's onUpdate.
template <typename O> constexpr void ability_immunity_held(Battle& battle, O& options) {
  for (const Player p : {Player::P1, Player::P2}) {
    Side& side = battle.side(p);
    Pokemon& mon = side.get(1);
    Volatiles& v = side.active.volatiles;
    const Status st = mon.status;
    const ID id = protocol::ident(battle, p);
    const auto cure = [&] {
      protocol::activate_ability(options.log, id, side.active.ability);
      protocol::cure_status(options.log, id, st);
      mon.status = Status::none();
    };
    switch (side.active.ability) {
    case Ability::Immunity:
      if (st.poisoned()) cure();
      break;
    case Ability::OwnTempo:
      if (v.confusion != 0) {
        protocol::activate_ability(options.log, id, Ability::OwnTempo);
        protocol::end_effect(options.log, id, protocol::End::Confusion);
        v.confusion = 0;
        if constexpr (CHANCE<O>) options.chance.durations.get(p).confusion = 0;
      }
      break;
    case Ability::Limber:
      if (st.paralyzed()) cure();
      break;
    case Ability::Insomnia:
    case Ability::VitalSpirit:
      if (st.asleep()) {
        cure();
        wake(battle, p, options);
      }
      break;
    case Ability::WaterVeil:
      if (st.burned()) cure();
      break;
    case Ability::MagmaArmor:
      if (st.frozen()) cure();
      break;
    case Ability::Oblivious:
      // Showdown's gen5 onUpdate (no -activate before gen 6).
      if (v.infatuated) {
        protocol::end_effect(options.log, id, protocol::End::Attract);
        protocol::end_effect(options.log, id, protocol::End::AttractOblivious);
      }
      v.infatuated = false;
      break;
    default:
      break;
    }
  }
}

// King's Rock (ITEMEFFECT_KINGSROCK_SHELLBELL, util:3763) for hit `i`, as
// Showdown's gen3: a 10% flinch secondary on the moves the decomp flags,
// doubled by Serene Grace (the decomp's isn't).
template <typename O>
constexpr void flinch_item(Battle& battle, Player player, const MoveState& m,
                           uint8_t i, O& options) {
  const Side& side = battle.side(player);
  if (m.release || !holds(side, HoldEffect::Flinch) || !m.damaged ||
      !(get(m.move).flags & Flag::KingsRockAffected)) {
    return;
  }
  Side& target_side = battle.side(foe(player));
  ActivePokemon& t = target_side.active;
  if (target_side.get(1).fainted() || !m.first || t.volatiles.flinched ||
      t.ability == Ability::ShieldDust || t.ability == Ability::InnerFocus ||
      focusing(battle, foe(player))) {
    return;
  }
  const uint32_t chance = item_data(side.get(1).item).param *
                          (side.active.ability == Ability::SereneGrace ? 2u : 1u);
  if (rolls::kings_rock(battle, player, i, chance, options)) t.volatiles.flinched = true;
}

// The Stockpile volatile's end.
template <typename O>
constexpr void stockpile_end(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.stockpile == 0) return;
  a.stockpile = 0;
  protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Stockpile);
}

// Showdown's AfterMove (runMove, sim/battle-actions.ts:311), for the last move
// the action ran: Charge ends after any move but Charge (gen8's condition), not
// after Pursuit on a switching target or for a fainted user; Spit Up's layers
// go.
template <typename O>
constexpr void after_move(Battle& battle, Player player, const MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.volatiles.charged_up && !m.pursuing && m.move != Move::Charge &&
      !battle.side(player).get(1).fainted()) {
    a.volatiles.charged_up = false;
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::Charge);
  }
  if (m.obeys && get(m.move).effect == Effect::SpitUp) stockpile_end(battle, player, options);
}

// adjustnormaldamage's and adjustsetdamage's survival (bsc:1677, 5878): a
// lethal hit `d` on the foe itself leaves it at 1 HP under Endure, False Swipe
// or Focus Band (hit `i`'s draw), in Showdown's onDamage priority order.
// Returns the damage to deal.
template <typename O>
constexpr uint32_t survive(Battle& battle, Player player, const MoveState& m,
                           uint32_t d, uint8_t i, O& options) {
  const Player defender = foe(player);
  const Side& foe_side = battle.side(defender);
  const Pokemon& target = foe_side.get(1);
  if (foe_side.active.volatiles.substitute || target.hp > d) return d;
  const ID id = protocol::ident(battle, defender);
  // A future attack's landing has no Turn, so Endure doesn't save the target
  // there, where Showdown's would (unchecked; not on teams/ou/).
  if (m.turn != nullptr && m.turn->endure[index(defender)]) {
    protocol::activate(options.log, id, protocol::Activate::Endure);
    return target.hp - 1u;
  }
  if (get(m.move).effect == Effect::FalseSwipe) return target.hp - 1u;
  const ItemData item = item_data(target.item);
  if (item.effect == HoldEffect::FocusBand &&
      rolls::focus_band(battle, player, i, item.param, options)) {
    protocol::activate(options.log, id, protocol::Activate::FocusBand);
    return target.hp - 1u;
  }
  return d;
}

// Jump Kick's crash (bs1:853), as Showdown's gen3 onMoveFail
// (data/mods/gen3/moves.ts:357): a failure against a target not immune to
// Fighting costs the user half the damage `h` would have dealt, at least 1 and
// at most half the target's max HP. Showdown's damage draws a crit and a roll,
// and the user's Focus Band can save it, all in hits[1]; the decomp's has no
// crit and no cap (bsc:6747).
template <typename O>
constexpr void crash(Battle& battle, Player player, const MoveState& m, Hit h,
                     O& options) {
  Pokemon& user = battle.side(player).get(1);
  const Pokemon& target = battle.side(foe(player)).get(1);
  if (user.fainted() || (matchup(battle, player, h) & MoveResult::DOESNT_AFFECT_FOE)) {
    return;
  }
  h.crit = can_crit(battle, player) &&
           rolls::crit(battle, player, 1, crit_stage(battle, player, m.move), options);
  uint32_t d = apply_roll(damage(battle, player, h), rolls::damage(battle, player, 1, options)) / 2;
  const uint32_t cap = target.stats.hp / 2u;
  if (d > cap) d = cap;
  if (d == 0) d = 1;
  const ItemData item = item_data(user.item);
  if (d >= user.hp && item.effect == HoldEffect::FocusBand &&
      rolls::focus_band(battle, player, 1, item.param, options)) {
    protocol::activate(options.log, protocol::ident(battle, player),
                       protocol::Activate::FocusBand);
    d = user.hp - 1u;
  }
  if (d > user.hp) d = user.hp;
  user.hp = uint16_t(user.hp - d);
  protocol::damage(options.log, protocol::ident(battle, player), user);
  if (user.fainted()) faint(battle, player, options);
}

// The target's Rage (Showdown's onHit, data/moves.ts:14570): each hit on the
// Pokémon itself from a damaging move raises its Attack. The decomp's
// MOVEEND_RAGE runs once.
template <typename O>
constexpr void rage_boost(Battle& battle, Player defender, O& options) {
  const int8_t n = raise(battle.side(defender).active, ATK, 1);
  protocol::boost(options.log, protocol::ident(battle, defender), ATK, n, false);
}

template <typename O>
constexpr void rage(Battle& battle, Player player, const MoveState& m, O& options) {
  const Player defender = foe(player);
  if (m.self || !m.damaged || !battle.side(defender).active.volatiles.rage ||
      get(m.move).bp == 0 || battle.side(defender).get(1).fainted()) {
    return;
  }
  rage_boost(battle, defender, options);
}

// The damage Bide's release, Counter, Mirror Coat and Endeavor deal, decided
// before the accuracy check, or 0 if they fail: twice the damage taken
// (bsc:7943: from the foe's physical or special moves this turn) or the HP
// difference (bsc:9366). Sleep Talk's Counter fails, as Showdown's. 0 for other
// moves.
constexpr uint32_t counter_damage(const Battle& battle, Player player, const MoveState& m) {
  if (m.release) {
    const uint32_t taken = battle.side(player).active.bide_damage;
    assert(taken != 0);
    return taken > UINT32_MAX / 2 ? UINT32_MAX : 2 * taken;
  }
  const Effect effect = get(m.move).effect;
  if (effect == Effect::Endeavor) {
    const uint16_t hp = battle.side(player).get(1).hp;
    const uint16_t foe_hp = battle.side(foe(player)).get(1).hp;
    return foe_hp > hp ? foe_hp - hp : 0u;
  }
  if ((effect != Effect::Counter && effect != Effect::MirrorCoat) || m.called) return 0;
  return 2u * (effect == Effect::Counter ? m.turn->physical : m.turn->special)[index(player)];
}

// Explosion (bs1:374): Damp stops it (Showdown names the holder); otherwise the
// user's HP goes to 0 before the hit (bsc:6577), and it faints after moveend.
// Returns whether it goes on.
template <typename O>
constexpr bool explodes(Battle& battle, Player player, MoveState& m, O& options) {
  const bool d1 = battle.sides[0].active.ability == Ability::Damp;
  const bool d2 = battle.sides[1].active.ability == Ability::Damp;
  if (d1 || d2) {
    m.attackstring = true;
    pp_reduce(battle, player, m);
    const Player holder = d1 && d2 ? log_order(battle) : d1 ? Player::P1 : Player::P2;
    protocol::last_still(options.log);
    protocol::cant(options.log, protocol::ident(battle, holder), protocol::Cant::Damp, m.move,
                   protocol::ident(battle, player));
    return false;
  }
  // Showdown faints the user here: its Faint comes before the target's.
  battle.side(player).get(1).hp = 0;
  m.faints = true;
  protocol::faint(options.log, protocol::ident(battle, player));
  return true;
}

// The hit's power before it lands, with Showdown's basePowerCallbacks, which
// double the power where the decomp doubles the damage (Pursuit, Weather Ball,
// Revenge).
constexpr Hit base_hit(const Battle& battle, Player player, const MoveState& m) {
  const Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  const MoveData& data = get(m.move);
  Hit h = Hit::of(m.move);
  h.type = m.type;
  if (m.pursuing) h.power = uint16_t(h.power * 2);
  switch (data.effect) {
  case Effect::HiddenPower: // hiddenpowercalc (bsc:8889)
    assert(m.move != Move::HiddenPower && side.active.hidden_power != 0);
    h.power = side.active.hidden_power;
    break;
  case Effect::Return: // friendshiptodamagecalculation: happiness * 10 / 25, at least 1
  case Effect::Frustration: {
    const uint16_t f = data.effect == Effect::Return ? mon.happiness : uint16_t(255 - mon.happiness);
    h.power = f * 10 / 25 == 0 ? uint16_t(1) : uint16_t(f * 10 / 25);
    break;
  }
  case Effect::Flail: { // remaininghptopower: by hp * 48 / max HP
    uint32_t ratio = uint32_t(mon.hp) * 48 / mon.stats.hp;
    if (ratio == 0) ratio = 1;
    h.power = ratio < 2 ? 200 : ratio < 5 ? 150 : ratio < 10 ? 100 : ratio < 17 ? 80 : ratio < 33 ? 40 : 20;
    break;
  }
  case Effect::Eruption: { // scaledamagebyhealthratio (bsc:9379): at least 1
    const uint32_t power = uint32_t(data.bp) * mon.hp / mon.stats.hp;
    h.power = power == 0 ? uint16_t(1) : uint16_t(power);
    break;
  }
  case Effect::WeatherBall: // setweatherballtype (bsc:9787); move_type has the type
    if (effective_weather(battle) != Weather::None) h.power = uint16_t(h.power * 2);
    break;
  case Effect::Revenge: // doubledamagedealtifdamaged (bsc:9339): the user itself took HP
    if (m.turn->physical[index(player)] != 0 || m.turn->special[index(player)] != 0) {
      h.power = uint16_t(h.power * 2);
    }
    break;
  case Effect::Smellingsalt: // bs1:2268; Showdown's doubles into a Substitute too
    if (battle.side(foe(player)).get(1).status.paralyzed()) h.power = uint16_t(h.power * 2);
    break;
  case Effect::SpitUp: // 100 per layer, as Showdown's (the decomp's has no roll)
    h.power = uint16_t(100 * side.active.stockpile);
    break;
  case Effect::LowKick: { // weightdamagecalculation (bsc:9467)
    const uint16_t weight = get(battle.side(foe(player)).active.species).weight;
    h.power = LOW_KICK_MAX_POWER;
    for (const WeightEntry& e : LOW_KICK_POWER) {
      if (weight < e.weight) {
        h.power = e.power;
        break;
      }
    }
    break;
  }
  default:
    break;
  }
  return h;
}

// What a hit on the target itself sets off, per hit: Defrost, the target's
// contact ability or Color Change, and Synchronize passing a status back
// (Showdown's DamagingHit; the decomp's moveend cases).
template <typename O>
constexpr void contact(Battle& battle, Player player, MoveState& m, uint8_t i, O& options) {
  defrost(battle, player, m, options);
  ability_on_damage(battle, player, m, i, options);
  synchronize(battle, player, m, options);
}

// Present's heal (bs1:1664), as Showdown's: a quarter of the target's max HP
// (-fail at full HP, or into a Substitute), which counts as a hit for Rage and
// Focus Punch. Returns whether it healed, which ends the move.
template <typename O>
constexpr bool present_heals(Battle& battle, Player player, MoveState& m, uint8_t variable,
                             O& options) {
  if (variable != 1) return false;
  const Player defender = foe(player);
  Side& foe_side = battle.side(defender);
  Pokemon& target = foe_side.get(1);
  const ID id = protocol::ident(battle, defender);
  if (foe_side.active.volatiles.substitute) {
    protocol::failed(options.log, protocol::ident(battle, player));
  } else if (target.hp >= target.stats.hp) {
    protocol::fail(options.log, id, protocol::Fail::Heal, true);
  } else {
    heal(target, target.stats.hp / 4u);
    protocol::heal(options.log, id, target);
    if (m.turn != nullptr) m.turn->hit[index(defender)] = true;
    if (foe_side.active.volatiles.rage) rage_boost(battle, defender, options);
  }
  return true;
}

// Showdown's onHit, for a hit on the target itself: binding for a hidden 3-6
// turns (bs1:1225), Pay Day, Fury Cutter's count, Smelling Salt's cure
// (bsc:2822), and the drain (bs1:323, 1290): half the HP dealt, at least 1, a
// Substitute's half rounded up as Showdown's, or Liquid Ooze's damage.
template <typename O>
constexpr void on_hit(Battle& battle, Player player, MoveState& m, O& options) {
  const Effect effect = get(m.move).effect;
  const Player defender = foe(player);
  Side& foe_side = battle.side(defender);
  Pokemon& target = foe_side.get(1);
  Pokemon& user = battle.side(player).get(1);
  if (effect == Effect::Trap && m.damaged && !target.fainted() &&
      foe_side.active.volatiles.wrapped == 0) {
    ActivePokemon& t = foe_side.active;
    t.volatiles.wrapped = uint8_t(3 + battle.rng.range(4));
    t.wrapped_move = m.move;
    if constexpr (CHANCE<O>) options.chance.durations.get(defender).binding = 1;
    protocol::activate_with(options.log, protocol::ident(battle, defender),
                            protocol::Activate::Bound, m.move, protocol::ident(battle, player));
  }
  if (effect == Effect::PayDay && m.damaged) {
    protocol::field_activate(options.log, protocol::FieldActivate::PayDay);
  }
  if (effect == Effect::FuryCutter && m.damaged) {
    ActivePokemon& a = battle.side(player).active;
    if (a.fury_cutter < 5) a.fury_cutter++;
    a.renewed |= ActivePokemon::RENEWED_FURY_CUTTER;
  }
  if (effect == Effect::Smellingsalt && m.damaged && !target.fainted() &&
      target.status.paralyzed()) {
    protocol::cure_status(options.log, protocol::ident(battle, defender), target.status);
    target.status = Status::none();
  }
  if ((effect == Effect::Absorb || effect == Effect::DreamEater) && m.hp_dealt != 0) {
    uint16_t drain = m.damaged ? uint16_t(m.hp_dealt / 2) : uint16_t((m.hp_dealt + 1) / 2);
    if (drain == 0) drain = 1;
    // Dream Eater skips Liquid Ooze (bs1:434); Ooze's faint is written at once.
    if (foe_side.active.ability == Ability::LiquidOoze && effect != Effect::DreamEater) {
      passive_damage(battle, player, drain, options);
      protocol::damage_ability(options.log, protocol::ident(battle, player), user,
                               Ability::LiquidOoze, protocol::ident(battle, defender));
      if (user.fainted()) protocol::faints_last_first(options.log);
    } else if (user.hp != user.stats.hp) {
      heal(user, drain);
      protocol::heal_drain(options.log, protocol::ident(battle, player), user,
                           protocol::ident(battle, defender));
    }
  }
}

// The effects on the user after a hit that affected the target (Showdown's
// self): Hyper Beam's recharge (bs1:1111), Thrash's lock (a hidden 2-3 turns,
// bs1:1101), Uproar's (2-5, bsc:2759), Rage, and Rapid Spin (bsc:8821).
template <typename O>
constexpr void self_effects(Battle& battle, Player player, MoveState& m, O& options) {
  const Effect effect = get(m.move).effect;
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  const ID id = protocol::ident(battle, player);
  if (effect == Effect::Recharge) {
    a.volatiles.recharge = true;
    a.recharge_timer = 2;
    a.locked_move = m.move;
    protocol::must_recharge(options.log, id);
  } else if (effect == Effect::Rampage) {
    if (a.volatiles.multiple_turns) {
      assert(a.locked_move == m.move);
      lock_renew(battle, player);
    } else {
      a.volatiles.multiple_turns = true;
      a.locked_move = m.move;
      a.volatiles.thrash = uint8_t(2 + battle.rng.range(2));
      a.renewed |= ActivePokemon::RENEWED_LOCK;
      if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 1;
    }
  } else if (effect == Effect::Uproar && a.volatiles.uproar == 0) {
    a.volatiles.uproar = uint8_t(2 + battle.rng.range(4));
    a.volatiles.multiple_turns = true;
    a.locked_move = m.move;
    protocol::start(options.log, id, protocol::Start::Uproar);
    if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 1;
  } else if (effect == Effect::Rage && !a.volatiles.rage) {
    a.volatiles.rage = true;
    protocol::single_move(options.log, id, protocol::SingleMove::Rage);
  }
  if (effect == Effect::RapidSpin && !side.get(1).fainted()) {
    if (a.volatiles.leech_seed) {
      protocol::end_effect(options.log, id, protocol::End::LeechSeedRapidSpin);
    }
    if (side.conditions.spikes != 0) {
      protocol::side_end(options.log, player, protocol::SideCondition::SpikesRapidSpin, id);
    }
    if (a.volatiles.wrapped != 0) {
      protocol::end_effect(options.log, id, protocol::End::Bound, a.wrapped_move);
    }
    unbind(a);
    a.volatiles.leech_seed = false;
    side.conditions.spikes = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).binding = 0;
  }
}

// Showdown's onAfterHit, for a hit on the target itself with the user in: Knock
// Off leaves Item::KnockedOff (bsc:2858; ENGINE.md decision 8), Thief and Covet
// take the item for an empty hand (bsc:2738), handed over at moveend. Sticky
// Hold keeps it. The target's Choice Band lock goes with the band.
template <typename O>
constexpr void after_hit(Battle& battle, Player player, MoveState& m, O& options) {
  const Effect effect = get(m.move).effect;
  const Player defender = foe(player);
  Side& foe_side = battle.side(defender);
  Pokemon& target = foe_side.get(1);
  Pokemon& user = battle.side(player).get(1);
  const bool takes = (effect == Effect::KnockOff || (effect == Effect::Thief && user.item == Item::None)) &&
                     m.damaged && !user.fainted() && target.item != Item::None &&
                     target.item != Item::KnockedOff;
  if (!takes) return;
  const ID thief = protocol::ident(battle, player);
  const ID robbed = protocol::ident(battle, defender);
  if (foe_side.active.ability == Ability::StickyHold) {
    return protocol::activate_ability(options.log, robbed, Ability::StickyHold);
  }
  const Item item = target.item;
  foe_side.active.choice_move = Move::None;
  if (effect == Effect::KnockOff) {
    protocol::end_item(options.log, robbed, item, protocol::EndItemFrom::KnockOff, thief);
    target.item = Item::KnockedOff;
    return;
  }
  target.item = Item::None;
  // setItem runs White Herb's onStart; an item it uses up isn't handed over.
  user.item = item;
  white_herb(battle, player, options);
  m.stolen = user.item;
  user.item = Item::None;
  if (m.move == Move::Thief) {
    protocol::end_item(options.log, robbed, item, protocol::EndItemFrom::Thief, thief);
    protocol::item(options.log, thief, item, protocol::ItemFrom::Thief, robbed);
  } else {
    protocol::item(options.log, thief, item, protocol::ItemFrom::Covet, robbed);
  }
}

// Recoil (bsc:2636, 2843): a quarter, or Double-Edge's third, of the HP dealt,
// at least 1, unless Rock Head (bs1:3938). As Showdown, none after a hit that
// took no HP.
template <typename O>
constexpr void recoil(Battle& battle, Player player, const MoveState& m, O& options) {
  const Effect effect = get(m.move).effect;
  Pokemon& user = battle.side(player).get(1);
  if ((effect != Effect::Recoil && effect != Effect::DoubleEdge) || m.hp_dealt == 0 ||
      user.fainted() ||
      (m.move != Move::Struggle && battle.side(player).active.ability == Ability::RockHead)) {
    return;
  }
  uint16_t n = uint16_t(m.hp_dealt / (effect == Effect::DoubleEdge ? 3 : 4));
  if (n == 0) n = 1;
  user.hp = user.hp > n ? uint16_t(user.hp - n) : 0;
  protocol::damage(options.log, protocol::ident(battle, player), user, protocol::Damage::Recoil);
  if (user.fainted()) faint(battle, player, options);
}

// ---------------------------------------------------------------------------
// Move scripts
// ---------------------------------------------------------------------------

// BattleScript_EffectConfuse (bs1:903), with Showdown's order of checks:
// try_hit, the Substitute, an existing confusion, Own Tempo, Safeguard. On the
// cartridge Safeguard skips moveend (bs1:3257); here it fails the move.
template <typename O>
constexpr void confuse_move(Battle& battle, Player player, MoveState& m,
                            O& options) {
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& t = foe_side.active;
  const ID target = protocol::ident(battle, foe(player));
  const bool takes = !t.volatiles.substitute && t.volatiles.confusion == 0 &&
                     t.ability != Ability::OwnTempo && foe_side.conditions.safeguard == 0;
  if (!try_hit(battle, player, m, {.matters = takes}, options)) {
    return;
  }
  // Own Tempo and an existing confusion set no result flag.
  if (t.volatiles.substitute) {
    but_it_failed(battle, player, m, options);
  } else if (t.volatiles.confusion != 0) {
    protocol::failed(options.log, protocol::ident(battle, player));
  } else if (t.ability == Ability::OwnTempo) {
    protocol::immune(options.log, target, protocol::Immune::Confusion, Ability::OwnTempo);
  } else if (foe_side.conditions.safeguard != 0) {
    protocol::activate(options.log, target, protocol::Activate::Safeguard);
    m.result |= MoveResult::FAILED;
  } else {
    confuse(battle, foe(player), options);
  }
}

// Why a primary status move that hits stops, in Showdown's order (setStatus,
// sim/pokemon.ts:1685): the Substitute, an existing status, the type
// immunities, then the ability, Safeguard and Sleep Clause.
enum class StatusStop : uint8_t {
  None,
  Substitute,
  Same,
  Other,
  Type,
  Ability,
  Safeguard,
  Clause, // Sleep Clause Mod: only a -message, which the log drops
  Uproar,
};

constexpr StatusStop status_stop(const Battle& battle, Player player, Secondary s) {
  const Side& side = battle.side(foe(player));
  const ActivePokemon& t = side.active;
  const Status status = side.get(1).status;
  if (t.volatiles.substitute) return StatusStop::Substitute;
  if (status.any()) {
    const bool same = (s == Secondary::Sleep && status.asleep()) ||
                      (s == Secondary::Poison && status.poisoned() && !status.badly_poisoned()) ||
                      (s == Secondary::Toxic && status.badly_poisoned()) ||
                      (s == Secondary::Burn && status.burned()) ||
                      (s == Secondary::Paralysis && status.paralyzed());
    return same ? StatusStop::Same : StatusStop::Other;
  }
  switch (s) {
  case Secondary::Poison:
  case Secondary::Toxic:
    if (has_type(t, Type::Poison) || has_type(t, Type::Steel)) return StatusStop::Type;
    if (t.ability == Ability::Immunity) return StatusStop::Ability;
    break;
  case Secondary::Burn:
    if (has_type(t, Type::Fire)) return StatusStop::Type;
    if (t.ability == Ability::WaterVeil) return StatusStop::Ability;
    break;
  case Secondary::Paralysis:
    if (t.ability == Ability::Limber) return StatusStop::Ability;
    break;
  case Secondary::Sleep:
    if (t.ability == Ability::Insomnia || t.ability == Ability::VitalSpirit) {
      // An uproar's onAnySetStatus (a Condition) goes first if its holder is
      // the target or at least as fast (Speed, then subOrder).
      const Player target = foe(player);
      const bool first = t.volatiles.uproar != 0 ||
                         (battle.side(player).active.volatiles.uproar != 0 &&
                          speed(battle, player) >= speed(battle, target));
      return first ? StatusStop::Uproar : StatusStop::Ability;
    }
    break;
  default:
    assert(false && "not a primary status");
  }
  if (side.conditions.safeguard != 0) return StatusStop::Safeguard;
  // Showdown's uproar onAnySetStatus comes before Sleep Clause Mod's
  // onSetStatus.
  if (s == Secondary::Sleep && uproar(battle)) return StatusStop::Uproar;
  if (s == Secondary::Sleep && sleep_clause(battle, foe(player))) return StatusStop::Clause;
  return StatusStop::None;
}

// -fail's form for the status a target already has.
constexpr protocol::Fail fail_status(Status status) {
  if (status.asleep()) return protocol::Fail::Sleep;
  if (status.badly_poisoned()) return protocol::Fail::Toxic;
  if (status.poisoned()) return protocol::Fail::Poison;
  if (status.burned()) return protocol::Fail::Burn;
  if (status.frozen()) return protocol::Fail::Freeze;
  return protocol::Fail::Paralysis;
}

// The primary status moves: Poison (bs1:986), Toxic (bs1:686), Paralyze
// (bs1:1007), Will-O-Wisp (bs1:2174) and Sleep (bs1:284). Every check fails the
// move the same way, so this takes Showdown's order: try_hit (Thunder Wave's
// Ground and Glare's Ghost immunity are natural ones), status_stop, then Sleep
// Clause after the accuracy check (onSetStatus). Safeguard runs moveend, as
// Showdown's (see confuse_move).
template <typename O>
constexpr void status_move(Battle& battle, Player player, MoveState& m,
                           Secondary s, O& options) {
  const Side& foe_side = battle.side(foe(player));
  const ActivePokemon& t = foe_side.active;
  bool immune = false;
  if (m.move == Move::ThunderWave || m.move == Move::Glare) {
    const Type type = get(m.move).type;
    for (const Type d : t.types) {
      const bool negated = t.volatiles.foresight && d == Type::Ghost &&
                           (type == Type::Normal || type == Type::Fighting);
      immune |= effectiveness(type, d) == Effectiveness::Immune && !negated;
    }
  }
  const StatusStop stop = status_stop(battle, player, s);
  if (!try_hit(battle, player, m, {.immune = immune, .matters = stop == StatusStop::None},
               options)) {
    return;
  }
  const ID user = protocol::ident(battle, player);
  const ID target = protocol::ident(battle, foe(player));
  switch (stop) {
  case StatusStop::None:
    inflict(battle, foe(player), s, m, options, protocol::StatusSource::by_move(m.move));
    return;
  case StatusStop::Substitute:
  case StatusStop::Other: protocol::failed(options.log, user); break;
  case StatusStop::Same:
    protocol::fail(options.log, target, fail_status(foe_side.get(1).status));
    break;
  case StatusStop::Type: protocol::immune(options.log, target); break;
  case StatusStop::Ability:
    protocol::immune(options.log, target, protocol::Immune::Ability, t.ability);
    break;
  case StatusStop::Safeguard:
    protocol::activate(options.log, target, protocol::Activate::Safeguard);
    break;
  case StatusStop::Clause: break;
  case StatusStop::Uproar: uproar_stops(battle, foe(player), options); break;
  }
  m.result |= MoveResult::FAILED;
}

// Rest (bs1:735, Cmd_trysetrest): it fails while asleep, under Uproar, with
// Vital Spirit or Insomnia, or at full HP; otherwise 3 turns of self-inflicted
// sleep over any status, and full HP. Showdown agrees; its onTry's messages, in
// its order.
template <typename O>
constexpr void rest(Battle& battle, Player player, MoveState& m, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  const ActivePokemon& a = side.active;
  if (mon.status.asleep() || uproar(battle) || a.ability == Ability::VitalSpirit ||
      a.ability == Ability::Insomnia || mon.hp == mon.stats.hp) {
    const ID id = protocol::ident(battle, player);
    if (!mon.status.asleep() && mon.hp == mon.stats.hp) {
      protocol::fail(options.log, id, protocol::Fail::Heal);
    } else if (!mon.status.asleep() &&
               (a.ability == Ability::VitalSpirit || a.ability == Ability::Insomnia)) {
      protocol::fail_ability(options.log, id, a.ability);
    } else if (!mon.status.asleep()) {
      // Rest's onHit: setStatus meets the uproar.
      uproar_stops(battle, player, options);
    }
    return mark_failed(m);
  }
  mon.status = Status::slf(Status::REST_SLEEP);
  side.active.toxic = 0;
  mon.hp = mon.stats.hp;
  const ID id = protocol::ident(battle, player);
  protocol::status(options.log, id, mon.status, protocol::StatusSource::by_move(Move::Rest));
  if constexpr (CHANCE<O>) options.chance.durations.get(player).sleep[0] = 1;
  // Showdown's Lum Berry eats before the heal (onAfterSetStatus, inflict()).
  if (item_data(mon.item).effect == HoldEffect::CureStatus) item_cure(battle, player, options);
  protocol::heal(options.log, id, mon, protocol::Heal::Silent);
}

// Yawn (bs1:2451, Cmd_setyawn): it fails against Vital Spirit or Insomnia, a
// Substitute, Safeguard, a semi-invulnerable target, a status or a pending
// Yawn. The cartridge also fails it during an Uproar; Showdown's onTryHit
// doesn't, and this follows it. The sleep comes at ENDTURN_YAWN
// (battler_stage).
template <typename O>
constexpr void yawn(Battle& battle, Player player, MoveState& m, O& options) {
  Side& foe_side = battle.side(foe(player));
  ActivePokemon& t = foe_side.active;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return;
  }
  const ID user = protocol::ident(battle, player);
  const ID target = protocol::ident(battle, foe(player));
  if (foe_side.get(1).status.any() || t.volatiles.substitute || t.volatiles.yawn != 0) {
    return but_it_failed(battle, player, m, options);
  }
  if (t.ability == Ability::VitalSpirit || t.ability == Ability::Insomnia) {
    protocol::immune(options.log, target, protocol::Immune::Ability, t.ability);
    return;
  }
  if (foe_side.conditions.safeguard != 0) {
    protocol::activate(options.log, target, protocol::Activate::Safeguard);
    return mark_failed(m);
  }
  t.volatiles.yawn = 2;
  protocol::start(options.log, target, protocol::Start::Yawn, user);
}

// A one-stat raise's stat and stages (setstatchanger, bs1:475-489, 927-945).
struct StatChange {
  uint8_t stat;
  int8_t n;
};

// BattleScript_EffectStatUp (bs1:489) and the scripts sharing its tail.
// Minimize (setminimize, bs1:1476) and Defense Curl (bs1:2014) set their flag
// first and keep it when the raise fails. At +6 Showdown prints -boost 0 where
// the cartridge says "won't go higher".
template <typename O>
constexpr void stat_up_move(Battle& battle, Player player, MoveState& m, StatChange c,
                            O& options) {
  ActivePokemon& a = battle.side(player).active;
  const Effect effect = get(m.move).effect;
  if (effect == Effect::Minimize) a.volatiles.minimized = true;
  if (effect == Effect::DefenseCurl) a.volatiles.defense_curl = true;
  protocol::boost(options.log, protocol::ident(battle, player), c.stat,
                  raise(a, c.stat, c.n, m), false);
}

// BattleScript_EffectStatDown (bs1:534), the one-stat drops. The failures leave
// the same state, so the checks come in Showdown's order: try_hit (Soundproof
// for the sound moves), the Substitute (TryPrimaryHit), then lower_stats().
template <typename O>
constexpr void stat_down_move(Battle& battle, Player player, MoveState& m,
                              StatChange c, O& options) {
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const uint8_t bit = uint8_t(1 << c.stat);
  const bool sub = t.volatiles.substitute;
  // A hit that Mist, an ability or -6 stops still lands (moveend's
  // last_landed_move and last_taken_move), so its roll matters.
  if (!try_hit(battle, player, m, {.matters = !sub}, options)) {
    return;
  }
  if (sub) return but_it_failed(battle, player, m, options);
  lower_stats(battle, target, bit, c.n, options.log);
}

// Tickle (bs1:2652): it fails with Attack and Defense at -6, then on a miss;
// then each drop that can happen. Neither the decomp nor Showdown's gen3
// (bypasssub, data/mods/gen3/moves.ts:639) checks a Substitute.
template <typename O>
constexpr void tickle(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  // As stat_down_move, a blocked drop still lands: the roll matters.
  if (!try_hit(battle, player, m, {}, options)) {
    return;
  }
  lower_stats(battle, target, 1 << ATK | 1 << DEF, 1, options.log);
}

// Memento, as Showdown's gen3 (accuracy true, selfdestruct "ifHit"): try_hit or
// a Substitute stops it and the user lives; otherwise the drops that can
// happen, then the user faints. The decomp (bs1:2204, bsc:9072) faints the user
// through Protect, a Substitute and Fly, and fails at -6/-6 with the user
// alive.
template <typename O>
constexpr void memento(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  if (!try_hit(battle, player, m, {.sure = true}, options)) return mark_failed(m);
  if (t.volatiles.substitute) return but_it_failed(battle, player, m, options);
  lower_stats(battle, target, 1 << ATK | 1 << SPA, 2, options.log);
  battle.side(player).get(1).hp = 0;
  m.faints = true;
}

// Imprison (bs1:2509, Cmd_tryimprison, bsc:9413): it fails if already on or no
// move is shared; then the foe can't select or use the user's moves while it
// stays in. Showdown's gen3 takes no extra PP under Pressure, unlike the
// decomp.
template <typename O>
constexpr void imprison(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  bool shared = false;
  for (const MoveSlot& ms : a.moves) shared |= ms.id != Move::None && knows(f, ms.id);
  if (a.volatiles.imprisoned_others || !shared) return but_it_failed(battle, player, m, options);
  a.volatiles.imprisoned_others = true;
  protocol::start(options.log, protocol::ident(battle, player), protocol::Start::Imprison);
}

// Nightmare (bs1:1459): it fails on a Substitute, a target already in one, or
// one awake. Showdown's gen3 also fails it into Protect and a semi-invulnerable
// target (try_hit); this follows it. The residual takes 1/4 while it sleeps.
template <typename O>
constexpr void nightmare(Battle& battle, Player player, MoveState& m, O& options) {
  Side& foe_side = battle.side(foe(player));
  Volatiles& v = foe_side.active.volatiles;
  if (!try_hit(battle, player, m, {.sure = true}, options)) return mark_failed(m);
  if (v.substitute || v.nightmare || !foe_side.get(1).status.asleep()) {
    return but_it_failed(battle, player, m, options);
  }
  v.nightmare = true;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Nightmare);
}

// Haze (bs1:562, Cmd_normalisebuffs): every active Pokémon's stages go to 0;
// Focus Energy stays (Showdown's gen3 too). Never fails.
template <typename O>
constexpr void haze(Battle& battle, O& options) {
  for (Side& side : battle.sides) side.active.boosts = Boosts{};
  protocol::clear_all_boost(options.log);
}

// Psych Up (bs1:1790, Cmd_copyfoestats, bsc:8809): the foe's stages, not Focus
// Energy (gen5 mod's onHit, data/mods/gen5/moves.ts:641), through a Substitute
// and Protect. Showdown's misses a semi-invulnerable target; this follows it.
template <typename O>
constexpr void psych_up(Battle& battle, Player player, MoveState& m,
                        O& options) {
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return;
  }
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& f = battle.side(foe(player)).active;
  a.boosts = f.boosts;
  protocol::copy_boost(options.log, protocol::ident(battle, player),
                       protocol::ident(battle, foe(player)));
}

// Cosmic Power, Bulk Up, Calm Mind, Dragon Dance (bs1:2687-2800): they fail
// only with both stats at +6; otherwise each is raised in turn, a capped one
// marking the move MISSED as raise() does.
template <typename O>
constexpr void stat_pair_move(Battle& battle, Player player, MoveState& m,
                              uint8_t first, uint8_t second, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const ID id = protocol::ident(battle, player);
  if (stage(a.boosts, first) >= 6 && stage(a.boosts, second) >= 6) {
    // Showdown prints both 0s and no -fail.
    protocol::boost(options.log, id, first, 0, false);
    protocol::boost(options.log, id, second, 0, false);
    return mark_failed(m);
  }
  protocol::boost(options.log, id, first, raise(a, first, 1, m), false);
  protocol::boost(options.log, id, second, raise(a, second, 1, m), false);
}

// Belly Drum (bs1:1776, Cmd_maxattackhalvehp, bsc:8785): it fails at +6 or at
// half HP or less; otherwise Attack goes to +6 and the user loses max HP / 2
// (at least 1), past its Substitute. Showdown agrees.
template <typename O>
constexpr void belly_drum(Battle& battle, Player player, MoveState& m,
                          O& options) {
  Side& side = battle.side(player);
  const Pokemon& mon = side.get(1);
  const uint32_t half = fraction(mon, 2);
  if (side.active.boosts.atk >= 6 || mon.hp <= half) {
    return but_it_failed(battle, player, m, options);
  }
  side.active.boosts.atk = 6;
  // The user can't faint: its HP was above the half it loses.
  passive_damage(battle, player, half, options);
  const ID id = protocol::ident(battle, player);
  protocol::damage(options.log, id, mon);
  protocol::set_boost(options.log, id);
}

// Curse (bs1:1482). Not a Ghost: it fails only at -6 Speed and +6 Attack and
// Defense; otherwise Speed -1 (Mist and Clear Body don't apply, bsc:6962),
// Attack and Defense +1, each skipped at its cap. Showdown gives the same
// boosts in that order.
//
// A Ghost (BattleScript_GhostCurse): it fails on a Substitute, a
// semi-invulnerable foe or one already cursed (bsc:8468); otherwise the foe is
// cursed and the user loses max HP / 2, past its own Substitute, and may faint.
// Protect doesn't stop it, in both (no protect flag).
template <typename O>
constexpr void curse(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (m.self) {
    Boosts& b = a.boosts;
    if (b.spe <= -6 && b.atk >= 6 && b.def >= 6) return mark_failed(m);
    const bool slower = b.spe > -6;
    if (slower) b.spe--;
    const int8_t atk = raise(a, ATK, 1, m);
    const int8_t def = raise(a, DEF, 1, m);
    const ID id = protocol::ident(battle, player);
    if (slower) protocol::boost(options.log, id, SPE, 1, true);
    if (atk != 0) protocol::boost(options.log, id, ATK, atk, false);
    if (def != 0) protocol::boost(options.log, id, DEF, def, false);
    return;
  }

  ActivePokemon& t = battle.side(foe(player)).active;
  // NO_ACC_CALC_CHECK_LOCK_ON (bsc:1099): only the semi-invulnerable check,
  // unless Locked-On. Showdown's -miss comes first, then -fail for an existing
  // curse or a Substitute.
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return;
  }
  if (t.volatiles.substitute || t.volatiles.cursed) {
    return but_it_failed(battle, player, m, options);
  }
  t.volatiles.cursed = true;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Curse,
                  protocol::ident(battle, player));
  // Damage, plain (Showdown's directDamage from the move).
  passive_damage(battle, player, fraction(battle.side(player).get(1), 2),
                 options);
  protocol::damage(options.log, protocol::ident(battle, player),
                   battle.side(player).get(1));
}

// Substitute (bs1:1085, Cmd_setsubstitute, bsc:7808): it fails with one up or
// at max HP / 4 (at least 1) or less, with no result flag; otherwise it gets
// that quarter, the user loses it past the new Substitute, and a binding on the
// user ends. Showdown agrees.
template <typename O>
constexpr void substitute(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const Pokemon& mon = battle.side(player).get(1);
  const uint32_t quarter = fraction(mon, 4);
  if (a.volatiles.substitute || mon.hp <= quarter) {
    protocol::fail(options.log, protocol::ident(battle, player),
                   a.volatiles.substitute ? protocol::Fail::Substitute
                                          : protocol::Fail::SubstituteWeak);
    return;
  }
  assert(quarter <= UINT8_MAX);
  a.volatiles.substitute = true;
  a.substitute_hp = uint8_t(quarter);
  const ID id = protocol::ident(battle, player);
  protocol::start(options.log, id, protocol::Start::Substitute);
  if (a.volatiles.wrapped != 0) {
    protocol::end_effect(options.log, id, protocol::End::BoundSilent, a.wrapped_move);
    unbind(a);
    if constexpr (CHANCE<O>) options.chance.durations.get(player).binding = 0;
  }
  // The user can't faint: its HP was above the quarter it loses.
  passive_damage(battle, player, quarter, options);
  protocol::damage(options.log, id, battle.side(player).get(1));
}

// Protect, Detect and Endure (Cmd_setprotectlike, bsc:6503): they fail when the
// user moves last (Showdown's willAct) or on the draw (rolls::protect), which
// restarts the count. The count restarts after any turn without a success, as
// Showdown's gen3 stall (gen5 conditions.ts:24, duration 2); the decomp keeps
// it through a turn the user doesn't act.
template <typename O>
constexpr void protect(Battle& battle, Player player, MoveState& m,
                       O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.stall == 0) a.protect_uses = 0;
  const uint8_t n = a.protect_uses < 3 ? a.protect_uses : 3;
  if (!m.first || (n != 0 && !rolls::protect(battle, player, n, options))) {
    a.protect_uses = 0;
    return but_it_failed(battle, player, m, options, MoveResult::MISSED);
  }
  const bool endure = get(m.move).effect != Effect::Protect;
  (endure ? m.turn->endure : m.turn->protect)[index(player)] = true;
  protocol::single_turn(options.log, protocol::ident(battle, player),
                        endure ? protocol::SingleTurn::Endure : protocol::SingleTurn::Protect);
  if (a.protect_uses != UINT8_MAX) a.protect_uses++;
  a.stall = 2;
}

// Spikes (bs1:1544, Cmd_trysetspikes): a layer on the foe's side, failing at 3.
// Pressure takes its extra PP (Showdown's mustpressure).
template <typename O>
constexpr void spikes_move(Battle& battle, Player player, MoveState& m,
                           O& options) {
  uint8_t& layers = battle.side(foe(player)).conditions.spikes;
  if (layers >= 3) return but_it_failed(battle, player, m, options);
  layers++;
  protocol::side_start(options.log, foe(player), protocol::SideCondition::Spikes);
}

// The healing moves: Recover and Slack Off (bs1:672), Soft-Boiled and Milk
// Drink (bs1:2027), half the max HP; Morning Sun, Synthesis and Moonlight
// (bs1:1731), 2/3 in sun, 1/4 in other weather, 1/2 without. At least 1; at
// full HP nothing, with no result flag. Showdown agrees, but its Morning Sun
// family prints no -fail at full HP (gen4 mod, data/mods/gen4/moves.ts:940).
template <typename O>
constexpr void heal_move(Battle& battle, Player player, bool weather, O& options) {
  Pokemon& mon = battle.side(player).get(1);
  if (mon.hp == mon.stats.hp) {
    if (!weather) {
      protocol::fail(options.log, protocol::ident(battle, player), protocol::Fail::Heal, true);
    }
    return;
  }
  uint32_t amount = mon.stats.hp / 2u;
  if (weather) {
    const Weather w = effective_weather(battle);
    if (w == Weather::Sun) {
      amount = 20u * mon.stats.hp / 30u;
    } else if (w != Weather::None) {
      amount = mon.stats.hp / 4u;
    }
  }
  heal(mon, amount == 0 ? 1 : amount);
  protocol::heal(options.log, protocol::ident(battle, player), mon);
}

// The SLOTs p's bench can send in, as a mask (bit s - 1 for SLOT s).
constexpr uint8_t bench(const Battle& battle, Player player) {
  const Side& side = battle.side(player);
  uint8_t mask = 0;
  for (uint8_t s = 2; s <= 6; s++) {
    const Pokemon& mon = side.get(s);
    if (mon.species != Species::None && !mon.fainted()) mask |= uint8_t(1 << (s - 1));
  }
  return mask;
}

// Roar and Whirlwind (bs1:593), as Showdown's gen3 (accuracy 100, protect
// flag). Suction Cups and Ingrain stop it with no result flag, then a
// semi-invulnerable target, the accuracy roll and an empty bench fail it, with
// the messages in Showdown's order. No level check (TryDoForceSwitchOut,
// bsc:7163; Showdown has none). The drag waits for the end of the action
// (m.drag), as Showdown's forceSwitchFlag; the decomp drags before moveend.
template <typename O>
constexpr void roar(Battle& battle, Player player, MoveState& m, O& options) {
  const ActivePokemon& t = battle.side(foe(player)).active;
  const bool anchored = t.ability == Ability::SuctionCups || t.volatiles.rooted;
  const bool bench_empty = bench(battle, foe(player)) == 0;
  if (!try_hit(battle, player, m, {.matters = !anchored && !bench_empty}, options)) {
    return;
  }
  if (bench_empty) return but_it_failed(battle, player, m, options);
  // With both, Suction Cups speaks (Showdown's onDragOutPriority 1,
  // data/abilities.ts:4694); the decomp checks Ingrain first.
  if (anchored) {
    protocol::activate(options.log, protocol::ident(battle, foe(player)),
                       t.ability == Ability::SuctionCups ? protocol::Activate::SuctionCups
                                                         : protocol::Activate::Ingrain);
    return;
  }
  m.drag = true;
}

// The drag (Showdown's dragIn, sim/battle-actions.ts:162): a random bench
// Pokémon replaces `target`'s as a switch. `user` made the draw.
template <typename O>
constexpr void drag(Battle& battle, Player user, O& options) {
  const Player target = foe(user);
  assert(!battle.side(target).get(1).fainted());
  const uint8_t slot = rolls::switch_target(battle, user, bench(battle, target), options);
  leave(battle, target, slot, options, Leave::Drag);
  arrive(battle, target, options);
}

// Baton Pass (bs1:1694): it fails with no bench, trapping or not; otherwise the
// side picks the recipient. Moveend runs on the passer here, on the recipient
// in the decomp, which is the same for Baton Pass: no last move, no Choice
// lock, no status to cure.
template <typename O>
constexpr void baton_pass(Battle& battle, Player player, MoveState& m,
                          O& options) {
  if (bench(battle, player) == 0) return but_it_failed(battle, player, m, options);
  m.pass = true;
}

// The moves Sleep Talk can't call, as Showdown's gen3 (nosleeptalk and charge
// flags): the decomp's list (IsInvalidForSleepTalkOrAssist, Focus Punch,
// Uproar, the two-turn moves) without Bide (data/mods/gen3/moves.ts) and Nature
// Power (data/mods/gen4/moves.ts:980).
constexpr bool sleep_talk_banned(Move move) {
  if (move == Move::Bide) return false;
  return get(move).extra & Extra::NoSleepTalk;
}

// A move that `m` calls (Showdown's useMove): no canceler and no PP, [from] the
// caller, and Pressure's PP from the caller's slot (gen3 scripts.ts:174; the
// cartridge takes none). `slot` is the called move's slot (Sleep Talk's), or
// NO_SLOT.
template <typename O>
constexpr void call_move(Battle& battle, Player player, MoveState& m, Move move,
                         uint8_t slot, O& options) {
  const ActivePokemon& a = battle.side(player).active;
  if (move == Move::HiddenPower || is_hidden_power(move)) {
    move = hidden_power(a.hidden_power_type);
  }
  if (pressure_applies(battle, player, move, self_aimed(a, move)) && m.slot != NO_SLOT) {
    deduct_pp(battle, player, m.slot, 1);
  }
  MoveState c{};
  c.move = move;
  c.chosen = m.chosen;
  c.from = m.move;
  c.slot = slot;
  c.no_pp = true;
  c.called = true;
  c.first = m.first;
  c.turn = m.turn;
  execute(battle, player, c, options);
  m.drag = c.drag;
  m.pass = c.pass;
  m.delegated = true;
}

// Sleep Talk (bs1:1311), as Showdown's gen3 (data/mods/gen3/moves.ts:557): only
// while asleep; it fails under a Choice Band lock or Encore; it picks uniformly
// among the callable slots and fails if the pick has no PP. The cartridge
// leaves out disabled, taunted, tormented and imprisoned moves and calls one
// with no PP (bsc:8241).
template <typename O>
constexpr void sleep_talk(Battle& battle, Player player, MoveState& m,
                          O& options) {
  const ActivePokemon& a = battle.side(player).active;
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const Move move = a.moves[i].id;
    if (move != Move::None && !sleep_talk_banned(move)) mask |= uint8_t(1 << i);
  }
  if (!battle.side(player).get(1).status.asleep() || a.choice_move != Move::None ||
      a.encore_timer != 0 || mask == 0) {
    // Showdown's onTry fails silently when awake; its onTryHit (the lock)
    // and onHit (nothing to call) print -fail.
    if (battle.side(player).get(1).status.asleep()) {
      protocol::failed(options.log, protocol::ident(battle, player));
    }
    return mark_failed(m);
  }
  const uint8_t slot = rolls::sleep_talk(battle, player, mask, options);
  if (a.moves[slot].pp == 0) {
    protocol::cant(options.log, protocol::ident(battle, player),
                   protocol::Cant::SleepTalkPP, a.moves[slot].id);
    return mark_failed(m);
  }
  assert(get(a.moves[slot].id).effect != Effect::SleepTalk);
  call_move(battle, player, m, a.moves[slot].id, slot, options);
}

// Wish (bs1:2358, Cmd_trywish, bsc:9292): the side's counter goes to 2, or it
// fails while one is pending. Showdown agrees. It lands in field_residuals.
template <typename O>
constexpr void wish(Battle& battle, Player player, MoveState& m, O& options) {
  Side& side = battle.side(player);
  if (side.conditions.wish != 0) return but_it_failed(battle, player, m, options);
  side.conditions.wish = 2;
  side.conditions.wisher = side.order[0];
}

// Leech Seed (bs1:1157, Cmd_setseeded, bsc:6718): a Substitute, a miss, a
// seeded target or a Grass one fails it, with the messages in Showdown's order
// (Grass is a natural immunity). The drain is in battler_stage.
template <typename O>
constexpr void leech_seed(Battle& battle, Player player, MoveState& m,
                          O& options) {
  ActivePokemon& t = battle.side(foe(player)).active;
  const bool takes = !t.volatiles.substitute && !t.volatiles.leech_seed;
  if (!try_hit(battle, player, m,
               {.immune = has_type(t, Type::Grass), .matters = takes}, options)) {
    return;
  }
  if (!takes) {
    return but_it_failed(battle, player, m, options,
                         t.volatiles.substitute ? MoveResult::FAILED : MoveResult::MISSED);
  }
  t.volatiles.leech_seed = true;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::LeechSeed);
}

// The weather moves (bs1:1744, 1757; Cmd_setrain, bsc:6681, setsunny,
// bsc:8767): they fail (MISSED) if that weather is up, an ability's included;
// otherwise it replaces any weather for 5 turns. Showdown's setWeather agrees.
template <typename O>
constexpr void weather_move(Battle& battle, Player player, MoveState& m,
                            Weather w, O& options) {
  Field& field = battle.field;
  if (field.weather == w) return but_it_failed(battle, player, m, options, MoveResult::MISSED);
  field.weather = w;
  field.weather_turns = 5;
  field.permanent = false;
  protocol::weather(options.log, w);
  weather_change(battle, options);
}

// Beat Up, as Showdown's gen3 (data/mods/gen3/moves.ts:31): one accuracy check,
// then one typeless special hit per party member not fainted or statused, in
// SLOT order, until the target faints. Each uses damage() with the member's
// base Atk against the target's base Def (Hit::ally), with its own crit, roll
// and Focus Band; the screens and Mirror Coat count it. The decomp (bs1:1940,
// bsc:8957) has its own formula with no roll, ignores the screens and goes in
// party order.
template <typename O>
constexpr void beat_up(Battle& battle, Player player, MoveState& m, O& options) {
  const Side& side = battle.side(player);
  const Player defender = foe(player);
  const Pokemon& target = battle.side(defender).get(1);
  Species allies[6] = {};
  uint8_t n = 0;
  for (uint8_t s = 1; s <= 6; s++) {
    const Pokemon& ally = side.get(s);
    if (ally.species != Species::None && !ally.fainted() && !ally.status.any()) {
      allies[n++] = ally.species;
    }
  }
  if (!try_hit(battle, player, m, {.matters = n != 0}, options)) {
    return;
  }
  if (n == 0) return but_it_failed(battle, player, m, options);
  uint8_t i = 0;
  for (; i < n && !target.fainted(); i++) {
    Hit h = Hit::of(m.move);
    h.ally = allies[i];
    const bool damaged = m.damaged;
    m.damaged = false;
    strike(battle, player, m, h, 0, i, options);
    rage(battle, player, m, options);
    flinch_item(battle, player, m, i, options);
    m.damaged |= damaged;
  }
  protocol::hit_count(options.log, protocol::ident(battle, defender), i);
  faint_target(battle, player, m.chosen, m.grudge, m.bonded_hp, options);
}

// Perish Song (bs1:1567), as Showdown's onHitField: every active Pokémon
// without a count gets 3, except a semi-invulnerable one and a Soundproof one,
// the user included (data/mods/gen7/abilities.ts:100). It fails only if nothing
// was missed, blocked or started. The decomp (bsc:8508) ignores
// semi-invulnerability. Pressure takes an extra PP.
template <typename O>
constexpr void perish_song(Battle& battle, Player player, MoveState& m,
                           O& options) {
  // Showdown's getAllActive order: P1 first.
  bool result = false;
  bool started = false;
  for (const Player p : {Player::P1, Player::P2}) {
    ActivePokemon& a = battle.side(p).active;
    const ID id = protocol::ident(battle, p);
    if (p != player && a.volatiles.semi_invulnerable()) {
      protocol::miss(options.log, protocol::ident(battle, player), id, false);
      result = true;
    } else if (a.ability == Ability::Soundproof) {
      protocol::immune(options.log, id, protocol::Immune::Ability, Ability::Soundproof);
      result = true;
    } else if (!a.volatiles.perish_song) {
      a.volatiles.perish_song = true;
      a.perish_song_timer = 3;
      protocol::start(options.log, id, protocol::Start::PerishSilent);
      result = true;
      started = true;
    }
  }
  if (!result) {
    but_it_failed(battle, player, m, options);
  } else if (started) {
    protocol::field_activate(options.log, protocol::FieldActivate::PerishSong);
  }
}

// Reflect (bs1:974), Light Screen, Mist (bs1:874) and Safeguard (bs1:1672):
// they fail while up (FAILED for Mist, MISSED for the others), else 5 turns on
// the user's side (bsc:6697, 7732, 8652). Showdown agrees.
template <typename O>
constexpr void screen(Battle& battle, Player player, MoveState& m, O& options) {
  SideConditions& c = battle.side(player).conditions;
  using SC = protocol::SideCondition;
  const Effect effect = get(m.move).effect;
  uint8_t& turns = effect == Effect::Reflect   ? c.reflect
                   : effect == Effect::Mist    ? c.mist
                   : effect == Effect::Safeguard ? c.safeguard
                                               : c.light_screen;
  if (turns != 0) {
    return but_it_failed(battle, player, m, options,
                         effect == Effect::Mist ? MoveResult::FAILED : MoveResult::MISSED);
  }
  turns = 5;
  protocol::side_start(options.log, player,
                       effect == Effect::Reflect     ? SC::Reflect
                       : effect == Effect::Mist      ? SC::Mist
                       : effect == Effect::Safeguard ? SC::Safeguard
                                                     : SC::LightScreen);
}

// Magic Coat (bs1:2392) and Snatch (bs1:2543), as Showdown's one-turn
// volatiles: the user bounces (try_hit) or steals (snatched) the next such move
// this turn. The decomp fails them when the user moves last (bsc:9537, 9553);
// Showdown's never fail.
template <typename O>
constexpr void coat_or_snatch(Battle& battle, Player player, MoveState& m, O& options) {
  assert(m.turn != nullptr);
  const bool coat = m.move == Move::MagicCoat;
  (coat ? m.turn->magic_coat : m.turn->snatch)[index(player)] = true;
  protocol::single_turn(options.log, protocol::ident(battle, player),
                        coat ? protocol::SingleTurn::MagicCoat : protocol::SingleTurn::Snatch);
}

// Mean Look (bs1:1444): Protect and a semi-invulnerable target stop it, and it
// fails on one already trapped or behind a Substitute; otherwise the target
// can't switch until the user leaves (ENGINE.md decision 9). Showdown agrees.
template <typename O>
constexpr void mean_look(Battle& battle, Player player, MoveState& m,
                         O& options) {
  Volatiles& v = battle.side(foe(player)).active.volatiles;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return;
  }
  if (v.escape_prevention || v.substitute) return but_it_failed(battle, player, m, options);
  v.escape_prevention = true;
  protocol::activate(options.log, protocol::ident(battle, foe(player)),
                     protocol::Activate::Trapped);
}

// Taunt (bs1:2308, Cmd_settaunt, bsc:9156): a miss or an existing Taunt fails
// it; else 2 turns. No Substitute check; Showdown's gen3 agrees. Power-0 moves,
// exactly Showdown's Status moves, are blocked.
template <typename O>
constexpr void taunt(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& t = battle.side(foe(player)).active;
  if (!try_hit(battle, player, m, {.matters = t.taunt == 0}, options)) {
    return;
  }
  if (t.taunt != 0) return but_it_failed(battle, player, m, options);
  t.taunt = 2;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Taunt);
}

// Encore (bs1:1227, Cmd_trysetencore, bsc:8017): it fails without a last move
// in the moveset with PP, on Showdown's gen3 failencore moves (Encore, Mimic,
// Mirror Move, Sketch, Struggle, Transform: a superset of the decomp's) or an
// existing Encore; then the accuracy check and a hidden 3-6 timer. No
// Substitute check. A target that hasn't moved yet uses the encored move
// (use_move; Showdown's onOverrideAction).
template <typename O>
constexpr void encore(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& t = battle.side(foe(player)).active;
  const uint8_t slot = slot_of(t, t.last_move);
  bool fails = slot == NO_SLOT || t.last_move == Move::None ||
               t.encored_move != Move::None;
  switch (t.last_move) {
  case Move::Encore:
  case Move::Mimic:
  case Move::MirrorMove:
  case Move::Sketch:
  case Move::Struggle:
  case Move::Transform: fails = true; break;
  default: break;
  }
  fails = fails || t.moves[slot].pp == 0;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return;
  }
  if (fails) return but_it_failed(battle, player, m, options);
  t.encored_move = t.last_move;
  t.encored_slot = slot;
  t.encore_timer = uint8_t(3 + battle.rng.range(4));
  if constexpr (CHANCE<O>) options.chance.durations.get(foe(player)).encore = 1;
  protocol::start(options.log, protocol::ident(battle, foe(player)), protocol::Start::Encore);
}

// Mimic (bs1:1134, Cmd_mimicattackcopy, bsc:7844): it fails on a Substitute, on
// Protect or a semi-invulnerable target unless Locked-On, a transformed user,
// no last move, a move it can't copy, or one already known. Otherwise the slot
// holds the move with min(5, base PP) until the user leaves (`mimicked`).
// Showdown's gen3 agrees (data/mods/gen4/moves.ts:882).
template <typename O>
constexpr void mimic(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& t = battle.side(foe(player)).active;
  // Hidden Power is one move on the cartridge: any typed one counts as known,
  // and a copy is the user's own type.
  const Move last = t.last_move;
  const bool known = knows(a, last);
  if (!try_hit(battle, player, m, {.sure = true}, options)) return mark_failed(m);
  if (t.volatiles.substitute || a.volatiles.transformed || last == Move::None ||
      (get(last).extra & Extra::NoMimic) || known) {
    return but_it_failed(battle, player, m, options);
  }
  assert(m.slot < 4);
  const uint8_t base = get(last).pp;
  a.moves[m.slot].id = is_hidden_power(last)
                           ? hidden_power(a.hidden_power_type)
                           : last;
  a.moves[m.slot].pp = base < 5 ? base : 5;
  a.mimicked = uint8_t(a.mimicked | 1 << m.slot);
  protocol::activate_with(options.log, protocol::ident(battle, player), protocol::Activate::Mimic,
                          a.moves[m.slot].id);
}

// Pain Split (bs1:1239, bsc:8049): it fails on Protect or a semi-invulnerable
// target unless Locked-On, or on a Substitute; else both go to their average
// HP. Showdown sets the HP directly where the decomp runs datahpupdate, so it
// isn't damage (Counter, Focus Punch, Bide).
template <typename O>
constexpr void pain_split(Battle& battle, Player player, MoveState& m,
                          O& options) {
  Side& foe_side = battle.side(foe(player));
  if (!try_hit(battle, player, m, {.sure = true}, options)) return mark_failed(m);
  if (foe_side.active.volatiles.substitute) return but_it_failed(battle, player, m, options);
  Pokemon& user = battle.side(player).get(1);
  Pokemon& target = foe_side.get(1);
  const uint16_t average = uint16_t((uint32_t(user.hp) + target.hp) / 2);
  user.hp = average < user.stats.hp ? average : user.stats.hp;
  target.hp = average < target.stats.hp ? average : target.stats.hp;
  protocol::set_hp(options.log, protocol::ident(battle, foe(player)), target, true);
  protocol::set_hp(options.log, protocol::ident(battle, player), user, false);
}

// Refresh (bs1:2520, bsc:9119): cures poison, burn or paralysis; otherwise it
// fails. Showdown agrees.
template <typename O>
constexpr void refresh(Battle& battle, Player player, MoveState& m, O& options) {
  Pokemon& mon = battle.side(player).get(1);
  if (!mon.status.poisoned() && !mon.status.burned() && !mon.status.paralyzed()) {
    return but_it_failed(battle, player, m, options);
  }
  protocol::cure_status(options.log, protocol::ident(battle, player), mon.status);
  mon.status = Status::none();
  battle.side(player).active.toxic = 0; // status1 holds the counter
}

// Heal Bell and Aromatherapy (bs1:1361, Cmd_healpartystatus, bsc:8370): the
// user's whole side is cured, the active one's Nightmare too; Heal Bell skips
// the Soundproof (the active one by its battle ability). Never fails.
// Showdown's gen4 versions agree. Cured sleeps are public, so their counts
// reset.
template <typename O>
constexpr void heal_bell(Battle& battle, Player player, MoveState& m,
                         O& options) {
  Side& side = battle.side(player);
  const bool bell = m.move == Move::HealBell;
  const ID user = protocol::ident(battle, player);
  if (bell) {
    protocol::activate(options.log, user, protocol::Activate::HealBell);
  } else {
    protocol::cure_team(options.log, user);
  }
  for (uint8_t s = 1; s <= 6; s++) {
    Pokemon& mon = side.get(s);
    if (mon.species == Species::None) continue;
    const Ability ability = s == 1 ? side.active.ability : mon.ability;
    if (bell && ability == Ability::Soundproof) {
      if (s == 1) protocol::immune(options.log, user, protocol::Immune::Ability, ability);
      continue;
    }
    if (bell && mon.status.any() && !mon.fainted()) {
      protocol::cure_status(options.log, ID::init(player, side.order[s - 1]), mon.status,
                            protocol::Cure::Silent, s != 1);
    }
    if (s == 1) {
      if (mon.status.asleep()) wake(battle, player, options);
      side.active.volatiles.nightmare = false;
      side.active.toxic = 0;
    } else if constexpr (CHANCE<O>) {
      options.chance.durations.get(player).sleep[s - 1] = 0;
    }
    mon.status = Status::none();
  }
}

// The accuracy check of a move aimed at the user (Bide, Nature Power, as
// Showdown's gen3): its own stages, ability and evasion items count. A miss
// is -miss at itself.
template <typename O>
constexpr bool hits_self(Battle& battle, Player player, MoveState& m, bool phys, O& options) {
  const ActivePokemon& a = battle.side(player).active;
  const uint32_t calc = staged_accuracy(battle, player, get(m.move).accuracy, a.boosts.acc,
                                        a.ability, phys);
  if (calc >= 100 || rolls::hit(battle, player, calc, options)) return true;
  const ID id = protocol::ident(battle, player);
  protocol::miss(options.log, id, id);
  m.result |= MoveResult::MISSED;
  return false;
}

// Bide (bs1:1149), as Showdown's gen3: the first turn starts it (duration 3,
// locked), the second prints -activate, the third releases twice the damage
// taken since (both in cancel()). The release is hit() with that damage at the
// foe, no move line or PP, as Showdown's moveData (accuracy 100, Protect,
// Normal, contact, Physical). The damage counted is the foe's, before the cap
// at the HP left (onDamage, priority -101). The decomp's release ignores
// immunities.
template <typename O>
constexpr void bide(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (m.release) {
    m.self = false;
    return hit(battle, player, m, options);
  }
  // Accuracy 100, so its own stages count, and Hustle's 0.8 (it's Physical).
  if (!hits_self(battle, player, m, true, options)) return;
  a.volatiles.bide = 3;
  a.bide_damage = 0;
  a.volatiles.multiple_turns = true;
  a.locked_move = m.move;
  protocol::start(options.log, protocol::ident(battle, player), protocol::Start::Bide);
}

// Stockpile (bs1:2147), as Showdown's gen3 (data/mods/gen3/moves.ts:591, no Def
// or SpD raise): up to 3 layers, then it fails silently. Baton Pass doesn't
// pass them.
template <typename O>
constexpr void stockpile(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.stockpile >= 3) return mark_failed(m);
  a.stockpile++;
  protocol::start_count(options.log, protocol::ident(battle, player), protocol::Start::Stockpile,
                        a.stockpile);
}

// Swallow (bs1:2167), as Showdown: with no Stockpile it fails silently;
// otherwise it heals max HP times 1/4, 1/2 or 1 by the layers (modify; the
// decomp divides), and the layers go either way.
template <typename O>
constexpr void swallow(Battle& battle, Player player, MoveState& m, O& options) {
  const ActivePokemon& a = battle.side(player).active;
  if (a.stockpile == 0) return mark_failed(m);
  Pokemon& mon = battle.side(player).get(1);
  const ID id = protocol::ident(battle, player);
  if (mon.hp == mon.stats.hp) {
    protocol::fail(options.log, id, protocol::Fail::Heal);
  } else {
    constexpr uint32_t SHARE[3] = {1024, 2048, 4096};
    heal(mon, uint32_t(modify(mon.stats.hp, SHARE[a.stockpile - 1])));
    protocol::heal(options.log, id, mon);
  }
  stockpile_end(battle, player, options);
}

// Future Sight and Doom Desire (bs1:2061, bsc:8357), as Showdown's gen3 onTry
// (data/mods/gen3/moves.ts:218): it fails silently while one is pending on the
// foe's side. Otherwise the damage is computed now: a typeless hit of power 80
// (special) or 120 (physical), no crit, with the roll. Protect and
// semi-invulnerability don't stop it. The decomp's has no roll and keeps the
// type. It lands in future_attack.
template <typename O>
constexpr void future_sight_move(Battle& battle, Player player, MoveState& m,
                                 O& options) {
  Side& foe_side = battle.side(foe(player));
  if (foe_side.conditions.future_sight != 0) return mark_failed(m);
  const Hit h = Hit::of(m.move);
  foe_side.future_sight_damage =
      apply_roll(damage(battle, player, h), rolls::damage(battle, player, 0, options));
  foe_side.future_sight_move = m.move;
  foe_side.conditions.future_sight = 3;
  foe_side.conditions.future_user = battle.side(player).order[0];
  protocol::start(options.log, protocol::ident(battle, player),
                  m.move == Move::DoomDesire ? protocol::Start::DoomDesire
                                             : protocol::Start::FutureSight);
}

// Confusion from a status move (Showdown's addVolatile): an existing one fails
// it silently, Own Tempo and Safeguard stop it. Returns 1 if it took hold, 0 if
// stopped, -1 if it failed.
template <typename O>
constexpr int8_t add_confusion(Battle& battle, Player player, O& options) {
  const Player target = foe(player);
  const Side& foe_side = battle.side(target);
  const ActivePokemon& t = foe_side.active;
  if (t.volatiles.confusion != 0) return -1;
  if (t.ability == Ability::OwnTempo) return 0;
  if (foe_side.conditions.safeguard != 0) {
    protocol::activate(options.log, protocol::ident(battle, target),
                       protocol::Activate::Safeguard);
    return 0;
  }
  confuse(battle, target, options);
  return 1;
}

// Swagger and Flatter, as Showdown: try_hit, a Substitute, the raise, then
// add_confusion, Own Tempo's -immune last. It fails only if the raise did
// nothing and the target was confused already. The decomp fails it first
// (jumpifconfusedandstatmaxed, bs1:1559); same state.
template <typename O>
constexpr void swagger(Battle& battle, Player player, MoveState& m, StatChange c,
                       O& options) {
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const int8_t b = stage(t.boosts, c.stat);
  const bool sub = t.volatiles.substitute;
  const bool confusable = t.volatiles.confusion == 0 && t.ability != Ability::OwnTempo &&
                          battle.side(target).conditions.safeguard == 0;
  if (!try_hit(battle, player, m, {.matters = !sub && (b < 6 || confusable)}, options)) {
    return;
  }
  const ID id = protocol::ident(battle, target);
  if (sub) return but_it_failed(battle, player, m, options);
  // At +6 the raise marks the move MISSED, as the decomp.
  const int8_t gained = raise(t, c.stat, c.n, m);
  protocol::boost(options.log, id, c.stat, gained, false);
  const int8_t confused = add_confusion(battle, player, options);
  if (t.ability == Ability::OwnTempo) {
    protocol::immune(options.log, id, protocol::Immune::Confusion, Ability::OwnTempo);
  }
  if (gained == 0 && confused < 0) but_it_failed(battle, player, m, options);
}

// Attract, as Showdown: a same-gender or genderless target is immune
// (onTryImmunity), an existing infatuation or Oblivious fails it. No Substitute
// check. It ends when the user leaves.
template <typename O>
constexpr void attract(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const Gender g1 = battle.side(player).get(1).personality.gender();
  const Gender g2 = battle.side(target).get(1).personality.gender();
  const bool immune = g1 == Gender::None || g2 == Gender::None || g1 == g2;
  const bool fails = t.volatiles.infatuated || t.ability == Ability::Oblivious;
  if (!try_hit(battle, player, m, {.immune = immune, .matters = !fails}, options)) {
    return;
  }
  if (fails) return but_it_failed(battle, player, m, options);
  t.volatiles.infatuated = true;
  protocol::start(options.log, protocol::ident(battle, target), protocol::Start::Attract);
}

// Disable, as Showdown's gen3: it fails without a last move or on Struggle, on
// a disabled target, or if the move isn't known or has no PP. No Substitute
// check. Its duration is random(2, 6), plus 1 if the target won't move after
// this (gen4's onStart), so it covers 2-5 of its moves; the decomp's (Random()
// & 3) + 2 has no +1. Durations: chance.hpp.
template <typename O>
constexpr void disable(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  ActivePokemon& t = battle.side(target).active;
  const uint8_t slot = slot_of(t, t.last_move);
  const bool fails = t.last_move == Move::None || t.last_move == Move::Struggle ||
                     t.disabled_move != Move::None || slot == NO_SLOT ||
                     t.moves[slot].pp == 0;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return;
  }
  if (fails) return but_it_failed(battle, player, m, options);
  const bool will_move =
      m.first && battle.request.choice[index(target)].type() == Choice::Move;
  const uint8_t count = will_move ? 2 : 1;
  t.disabled_move = t.last_move;
  t.disable_timer = uint8_t(3 + battle.rng.range(4) - (count - 1));
  if constexpr (CHANCE<O>) options.chance.durations.get(target).disable = count;
  protocol::start_disable(options.log, protocol::ident(battle, target), t.last_move);
}

// Torment, as Showdown: it fails on a tormented target; the selection then
// leaves out the last move. It ends when the target leaves.
template <typename O>
constexpr void torment(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  Volatiles& v = battle.side(target).active.volatiles;
  if (!try_hit(battle, player, m, {.matters = !v.torment}, options)) {
    return;
  }
  if (v.torment) return but_it_failed(battle, player, m, options);
  v.torment = true;
  protocol::start(options.log, protocol::ident(battle, target), protocol::Start::Torment);
}

// Spite, as Showdown's gen3: the last move loses 2-5 PP (at most what it has);
// it fails without a known last move with PP. The roll is recorded only where
// it changes the PP (rolls::spite).
template <typename O>
constexpr void spite(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  const ActivePokemon& t = battle.side(target).active;
  const uint8_t slot = slot_of(t, t.last_move);
  const bool fails = t.last_move == Move::None || slot == NO_SLOT ||
                     t.moves[slot].pp == 0;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return;
  }
  if (fails) return but_it_failed(battle, player, m, options);
  const uint8_t n = t.moves[slot].pp >= 3 ? rolls::spite(battle, player, options)
                                          : uint8_t(2 + battle.rng.range(4));
  deduct_pp(battle, target, slot, n);
  protocol::activate_spite(options.log, protocol::ident(battle, target), t.last_move, n);
}

// Foresight and Odor Sleuth, as Showdown's gen3 (accuracy 100, bypasssub): they
// fail on a target already identified; until it leaves, its positive evasion
// counts as 0 and a Ghost loses its Normal and Fighting immunity.
template <typename O>
constexpr void foresight(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  Volatiles& v = battle.side(target).active.volatiles;
  if (!try_hit(battle, player, m, {.matters = !v.foresight}, options)) {
    return;
  }
  if (v.foresight) return but_it_failed(battle, player, m, options);
  v.foresight = true;
  protocol::start(options.log, protocol::ident(battle, target), protocol::Start::Foresight);
}

// Lock-On and Mind Reader, as Showdown's gen3: a lock already on (even on a
// target that has left) or a Substitute fails it; otherwise the user's moves
// hit the target through the next turn (locked_on()).
template <typename O>
constexpr void lock_on(Battle& battle, Player player, MoveState& m, O& options) {
  const Player target = foe(player);
  Volatiles& v = battle.side(player).active.volatiles;
  const bool fails = v.lock_on != 0 || battle.side(target).active.volatiles.substitute;
  if (!try_hit(battle, player, m, {.matters = !fails}, options)) {
    return;
  }
  if (fails) return but_it_failed(battle, player, m, options);
  v.lock_on = uint8_t(2 | battle.side(target).order[0] << 2);
  protocol::activate_with(options.log, protocol::ident(battle, player),
                          m.move == Move::MindReader ? protocol::Activate::MindReader
                                                     : protocol::Activate::LockOn,
                          Move::None, protocol::ident(battle, target));
}

// Focus Energy, Ingrain, Charge and the Sports: a volatile on the user, failing
// if already on (the Sports: bsc:9758), except Charge, which restarts and
// raises no Special Defense. The Sports are Showdown's gen4 volatiles.
// Showdown's Charge (gen8 mod) ends after the user's next other move
// (use_move); the decomp's at the next turn's end.
template <typename O>
constexpr void self_volatile(Battle& battle, Player player, MoveState& m, O& options) {
  Volatiles& v = battle.side(player).active.volatiles;
  const Effect effect = get(m.move).effect;
  bool& on = effect == Effect::FocusEnergy ? v.focus_energy
             : effect == Effect::Ingrain   ? v.rooted
             : effect == Effect::MudSport  ? v.mud_sport
             : effect == Effect::WaterSport ? v.water_sport
                                           : v.charged_up;
  if (on && effect != Effect::Charge) return but_it_failed(battle, player, m, options);
  on = true;
  protocol::start(options.log, protocol::ident(battle, player),
                  effect == Effect::FocusEnergy  ? protocol::Start::FocusEnergy
                  : effect == Effect::Ingrain    ? protocol::Start::Ingrain
                  : effect == Effect::MudSport   ? protocol::Start::MudSport
                  : effect == Effect::WaterSport ? protocol::Start::WaterSport
                                                 : protocol::Start::Charge);
}

// ---------------------------------------------------------------------------
// Copying, calling and swapping, as Showdown's gen3
// ---------------------------------------------------------------------------

// p's ability becomes `ability`, as Showdown's setAbility: the old one's End
// runs (Flash Fire, Cloud Nine, Air Lock), and gen 3 runs no Start for the new
// one.
template <typename O>
constexpr void set_ability(Battle& battle, Player player, Ability ability, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const Ability old = a.ability;
  if (a.volatiles.flash_fire) {
    a.volatiles.flash_fire = false;
    protocol::end_effect(options.log, protocol::ident(battle, player), protocol::End::FlashFire);
  }
  a.ability = ability;
  if (old == Ability::CloudNine || old == Ability::AirLock) weather_change(battle, options);
}

// Transform, as Showdown's transformInto (sim/pokemon.ts:1270): through a
// Substitute and Protect, but it misses a semi-invulnerable target and fails on
// a transformed one. The user takes everything but HP, and each move gets
// min(5, base PP).
template <typename O>
constexpr void transform(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  const ActivePokemon& t = battle.side(foe(player)).active;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return;
  }
  if (t.volatiles.transformed) return but_it_failed(battle, player, m, options);
  a.species = t.species;
  a.types[0] = t.types[0];
  a.types[1] = t.types[1];
  const uint16_t hp = a.stats.hp;
  a.stats = t.stats;
  a.stats.hp = hp;
  for (uint8_t i = 0; i < 4; i++) {
    const Move move = t.moves[i].id;
    const uint8_t base = move == Move::None ? 0 : get(move).pp;
    a.moves[i] = {move, uint8_t(base < 5 ? base : 5)};
  }
  a.mimicked = 0;
  a.boosts = t.boosts;
  a.hidden_power = t.hidden_power;
  a.hidden_power_type = t.hidden_power_type;
  a.volatiles.transformed = true;
  protocol::transform(options.log, protocol::ident(battle, player),
                      protocol::ident(battle, foe(player)));
  set_ability(battle, player, t.ability, options);
}

// Sketch, as Showdown's gen4 mod: it misses a semi-invulnerable target, and
// fails for a transformed user, no last move, a Substitute, Sketch or Struggle,
// or a move already known. Otherwise the slot keeps the move for good, with
// base PP. Showdown prints Mimic's -activate.
template <typename O>
constexpr void sketch(Battle& battle, Player player, MoveState& m, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Move last = battle.side(foe(player)).active.last_move;
  if (is_hidden_power(last)) last = hidden_power(side.get(1).hidden_power_type);
  const bool known = knows(a, last);
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return;
  }
  if (a.volatiles.transformed || last == Move::None ||
      battle.side(foe(player)).active.volatiles.substitute || last == Move::Sketch ||
      last == Move::Struggle || known) {
    return but_it_failed(battle, player, m, options);
  }
  assert(m.slot < 4);
  a.moves[m.slot] = {last, get(last).pp};
  side.get(1).moves[m.slot] = a.moves[m.slot];
  protocol::activate_with(options.log, protocol::ident(battle, player),
                          protocol::Activate::Mimic, last);
}

// Showdown's gen3 Mirror Move noMirror list (data/mods/gen3/moves.ts).
constexpr bool no_mirror(Move move) {
  switch (move) {
  case Move::Assist:
  case Move::Curse:
  case Move::DoomDesire:
  case Move::FocusPunch:
  case Move::FutureSight:
  case Move::MagicCoat:
  case Move::Metronome:
  case Move::Mimic:
  case Move::MirrorMove:
  case Move::NaturePower:
  case Move::PsychUp:
  case Move::RolePlay:
  case Move::Sketch:
  case Move::SleepTalk:
  case Move::Spikes:
  case Move::SpitUp:
  case Move::Taunt:
  case Move::TeeterDance:
  case Move::Transform: return true;
  default: return false;
  }
}

// Mirror Move, as Showdown's gen3 (data/mods/gen3/moves.ts:432): the last move
// that got past accuracy and TryHit on the user (attackedBy), if the same foe
// is in, has moved since it came in, still knows it and it isn't on noMirror.
// The decomp uses FLAG_MIRROR_MOVE_AFFECTED.
template <typename O>
constexpr void mirror_move(Battle& battle, Player player, MoveState& m, O& options) {
  const ActivePokemon& a = battle.side(player).active;
  const Move move = a.last_taken_move;
  const Side& source = battle.side(foe(player));
  const bool available = a.last_taken_from == source.order[0] &&
                         source.active.last_move != Move::None && knows(source.active, move);
  if (move == Move::None || no_mirror(move) || !available) {
    return but_it_failed(battle, player, m, options);
  }
  call_move(battle, player, m, move, NO_SLOT, options);
}

// Showdown's gen3 noassist flag. The decomp also leaves out the two-turn moves.
constexpr bool assist_banned(Move move) {
  return (get(move).extra & Extra::NoMetronome) || move == Move::Assist ||
         move == Move::MirrorMove;
}

// Assist, as Showdown's gen4: a pick among the moves of the user's party but
// itself, fainted ones included, repeats counted (rolls::assist); it fails if
// there are none.
template <typename O>
constexpr void assist(Battle& battle, Player player, MoveState& m, O& options) {
  const Side& side = battle.side(player);
  Move list[20] = {};
  uint8_t n = 0;
  for (uint8_t i = 0; i < 6; i++) {
    if (i + 1 == side.order[0] || side.pokemon[i].species == Species::None) continue;
    for (const MoveSlot& ms : side.pokemon[i].moves) {
      if (ms.id == Move::None || assist_banned(ms.id)) continue;
      list[n++] = is_hidden_power(ms.id) ? Move::HiddenPower : ms.id;
    }
  }
  if (n == 0) return but_it_failed(battle, player, m, options);
  call_move(battle, player, m, rolls::assist(battle, player, list, n, options),
            NO_SLOT, options);
}

// Nature Power, as Showdown's gen3: aimed at the user with accuracy 95, so its
// own stages, Sand Veil and BrightPowder count; a hit calls Swift. The decomp
// calls Swift with no accuracy check.
template <typename O>
constexpr void nature_power(Battle& battle, Player player, MoveState& m, O& options) {
  if (!hits_self(battle, player, m, false, options)) return;
  call_move(battle, player, m, Move::Swift, NO_SLOT, options);
}

// Camouflage, as Showdown's gen4: Normal, failing if the user is Normal
// already.
template <typename O>
constexpr void camouflage(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (has_type(a, Type::Normal)) return but_it_failed(battle, player, m, options);
  a.types[0] = a.types[1] = Type::Normal;
  protocol::start_type(options.log, protocol::ident(battle, player), Type::Normal, false);
}

// Conversion, as Showdown's gen3: a pick among the user's move types but
// Curse's and its own (repeats count; Hidden Power and Weather Ball are
// Normal). Conversion 2, as Showdown's gen3: a pick among the types that resist
// the foe's last move used since it came in (last_used_move), its own type
// included; the decomp's reads the last move to hit the user. Either fails with
// nothing to pick.
template <typename O>
constexpr void conversion(Battle& battle, Player player, MoveState& m, O& options) {
  ActivePokemon& a = battle.side(player).active;
  Type list[TYPES] = {};
  uint8_t n = 0;
  if (m.move == Move::Conversion) {
    for (const MoveSlot& ms : a.moves) {
      if (ms.id == Move::None || ms.id == Move::Curse) continue;
      const Type t = is_hidden_power(ms.id) ? Type::Normal : get(ms.id).type;
      if (!has_type(a, t)) list[n++] = t;
    }
  } else {
    const ActivePokemon& f = battle.side(foe(player)).active;
    if (!try_hit(battle, player, m, {.sure = true}, options)) {
      return;
    }
    if (f.last_used_move != Move::None) {
      for (uint8_t i = 0; i < TYPES; i++) {
        const Type t = Type(i);
        if (t == Type::Mystery) continue;
        const Effectiveness e = effectiveness(f.last_used_type, t);
        if (e == Effectiveness::Resisted || e == Effectiveness::Immune) list[n++] = t;
      }
    }
  }
  if (n == 0) return but_it_failed(battle, player, m, options);
  const Type t = rolls::conversion(battle, player, list, n, options);
  a.types[0] = a.types[1] = t;
  protocol::start_type(options.log, protocol::ident(battle, player), t, false);
}

// Role Play, as Showdown's: it misses a semi-invulnerable target, and fails on
// the same ability or Wonder Guard. Skill Swap, as Showdown's skillSwap
// (sim/battle.ts:1315): Protect stops it; Wonder Guard on either side or the
// same ability fails it.
template <typename O>
constexpr void role_play(Battle& battle, Player player, MoveState& m, O& options) {
  const Ability mine = battle.side(player).active.ability;
  const Ability theirs = battle.side(foe(player)).active.ability;
  const bool swap = m.move == Move::SkillSwap;
  if (!try_hit(battle, player, m, {.sure = true}, options)) {
    return;
  }
  if (mine == theirs || theirs == Ability::WonderGuard ||
      (swap && mine == Ability::WonderGuard)) {
    return but_it_failed(battle, player, m, options);
  }
  const ID user = protocol::ident(battle, player);
  const ID target = protocol::ident(battle, foe(player));
  if (swap) {
    protocol::activate_with(options.log, user, protocol::Activate::SkillSwap, Move::None, target);
    set_ability(battle, player, theirs, options);
    set_ability(battle, foe(player), mine, options);
  } else {
    set_ability(battle, player, theirs, options);
    protocol::ability_copy(options.log, user, theirs, mine, protocol::AbilityFrom::RolePlay,
                           target);
  }
}

// Trick, as Showdown's: Sticky Hold makes the target immune, then a Substitute
// stops it; it fails if either item was knocked off, neither has one, or either
// holds Mail. A Choice Band lock outlives its band until the holder's next move
// or the turn's end (cancel()), and a new band locks the user into Trick at
// moveend.
template <typename O>
constexpr void trick(Battle& battle, Player player, MoveState& m, O& options) {
  Side& side = battle.side(player);
  Side& foe_side = battle.side(foe(player));
  Pokemon& user = side.get(1);
  Pokemon& target = foe_side.get(1);
  const Item mine = user.item;
  const Item theirs = target.item;
  const bool swaps = !foe_side.active.volatiles.substitute &&
                     mine != Item::KnockedOff && theirs != Item::KnockedOff &&
                     (mine != Item::None || theirs != Item::None) &&
                     mine != Item::Mail && theirs != Item::Mail;
  if (!try_hit(battle, player, m,
               {.immune = foe_side.active.ability == Ability::StickyHold, .matters = swaps},
               options)) {
    return;
  }
  if (!swaps) return but_it_failed(battle, player, m, options);
  const ID id = protocol::ident(battle, player);
  const ID of = protocol::ident(battle, foe(player));
  protocol::activate_with(options.log, id, protocol::Activate::Trick, Move::None, of);
  // Each setItem runs the new item's onStart (White Herb).
  user.item = Item::None;
  target.item = mine;
  if (mine != Item::None) {
    white_herb(battle, foe(player), options);
    protocol::item(options.log, of, mine, protocol::ItemFrom::Trick);
  } else {
    protocol::end_item(options.log, of, theirs, protocol::EndItemFrom::Trick);
  }
  user.item = theirs;
  if (theirs != Item::None) {
    white_herb(battle, player, options);
    protocol::item(options.log, id, theirs, protocol::ItemFrom::Trick);
  } else {
    protocol::end_item(options.log, id, mine, protocol::EndItemFrom::Trick);
  }
}

// Recycle, as Showdown's: the item last used up in this position
// (Side::recycle_item, lastItem) comes back to an empty or knocked-off hand.
// Showdown's itemKnockedOff would still stop Trick and Thief from taking it;
// here they can.
template <typename O>
constexpr void recycle(Battle& battle, Player player, MoveState& m, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  if ((mon.item != Item::None && mon.item != Item::KnockedOff) ||
      side.recycle_item == Item::None) {
    return but_it_failed(battle, player, m, options);
  }
  mon.item = side.recycle_item;
  side.recycle_item = Item::None;
  protocol::item(options.log, protocol::ident(battle, player), mon.item,
                 protocol::ItemFrom::Recycle);
}

// ---------------------------------------------------------------------------
// Residual handlers
// ---------------------------------------------------------------------------

// ITEMEFFECT_NORMAL (util:3317) at ENDTURN_ITEMS1. Leftovers and the HP and
// pinch berries act only here, in both (data/mods/gen3/items.ts onResidual),
// not right after a hit.
template <typename O>
constexpr void item_end_turn(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  Pokemon& mon = side.get(1);
  ActivePokemon& a = side.active;
  if (mon.fainted()) return;
  const Item held = mon.item;
  const ItemData item = item_data(held);
  switch (item.effect) {
  case HoldEffect::RestoreHp:
    // Oran, Sitrus (30 HP in Gen III) and Berry Juice: a fixed amount.
    if (mon.hp > mon.stats.hp / 2) return;
    use_item(battle, player, options);
    heal(mon, item.param);
    protocol::heal_item(options.log, protocol::ident(battle, player), mon, held);
    return;
  case HoldEffect::ConfuseSpicy:
  case HoldEffect::ConfuseDry:
  case HoldEffect::ConfuseSweet:
  case HoldEffect::ConfuseBitter:
  case HoldEffect::ConfuseSour: {
    // The confusion berries (util:3213): at half HP or less, an eighth of the
    // max HP, then confusion if the nature dislikes the flavor. As Showdown's
    // gen3, Own Tempo and an existing confusion stop it silently; Safeguard
    // doesn't, the holder being the source.
    if (mon.hp > mon.stats.hp / 2) return;
    use_item(battle, player, options);
    heal(mon, fraction(mon, item.param));
    protocol::heal_item(options.log, protocol::ident(battle, player), mon, held);
    const Flavor f = Flavor(uint8_t(item.effect) - uint8_t(HoldEffect::ConfuseSpicy));
    if (dislikes(mon.personality.nature(), f) && a.ability != Ability::OwnTempo &&
        a.volatiles.confusion == 0) {
      confuse(battle, player, options);
    }
    return;
  }
  case HoldEffect::Leftovers:
    if (mon.hp != mon.stats.hp) {
      heal(mon, fraction(mon, 16));
      protocol::heal_item(options.log, protocol::ident(battle, player), mon, mon.item);
    }
    return;
  case HoldEffect::AttackUp:
  case HoldEffect::DefenseUp:
  case HoldEffect::SpeedUp:
  case HoldEffect::SpAttackUp:
  case HoldEffect::SpDefenseUp: {
    // The stat berries (util:3230): +1 at max HP / param or less. The decomp
    // keeps the berry at +6; Showdown's gen3 eats it (data/items.ts:3392), as
    // here.
    if (mon.hp > mon.stats.hp / item.param) return;
    const uint8_t stat = uint8_t(item.effect) - uint8_t(HoldEffect::AttackUp);
    const int8_t n = raise(a, stat, 1);
    use_item(battle, player, options);
    protocol::boost(options.log, protocol::ident(battle, player), stat, n, false, held);
    return;
  }
  case HoldEffect::CriticalUp:
    // Lansat (util:3441): Focus Energy. The decomp keeps it under Focus Energy;
    // Showdown eats it (data/items.ts:3267), as here.
    if (mon.hp > mon.stats.hp / item.param) return;
    use_item(battle, player, options);
    if (!a.volatiles.focus_energy) {
      protocol::start(options.log, protocol::ident(battle, player), protocol::Start::FocusEnergy);
    }
    a.volatiles.focus_energy = true;
    return;
  case HoldEffect::RandomStatUp: {
    // Starf (util:3450): +2 to a random stat below +6. With none the decomp
    // keeps it; Showdown eats it with no draw (data/items.ts:6003), as here.
    if (mon.hp > mon.stats.hp / item.param) return;
    const Boosts& b = a.boosts;
    use_item(battle, player, options);
    if (b.atk >= 6 && b.def >= 6 && b.spe >= 6 && b.spa >= 6 && b.spd >= 6) return;
    const uint8_t stat = uint8_t(rolls::starf(battle, player, options) - 1);
    const int8_t n = raise(a, stat, 2);
    protocol::boost(options.log, protocol::ident(battle, player), stat, n, false, held);
    return;
  }
  default:
    item_cure(battle, player, options);
    return;
  }
}

// DoFieldEndTurnEffects (util:1181): the side timers, Wish, then the weather.
// Sand and hail hurt each Pokémon in the residual order (bs1:3205) and stop
// once the battle is over (bs1:3219).
template <typename O>
constexpr Flow field_residuals(Battle& battle, uint32_t s1, uint32_t s2,
                               O& options) {
  // ENDTURN_REFLECT to SAFEGUARD (util:1218-1311), each over both sides;
  // Showdown's gen4 orders 1-4 agree. A side condition has no Speed, so two
  // ending together print in an unrecorded random order.
  using SC = protocol::SideCondition;
  const auto count_down = [&](uint8_t SideConditions::*turns, SC c) {
    bool ends[2];
    for (const Player p : {Player::P1, Player::P2}) {
      uint8_t& t = battle.side(p).conditions.*turns;
      ends[uint8_t(p)] = t != 0 && --t == 0;
    }
    Player first = Player::P1;
    if (ends[0] && ends[1] && battle.rng.chance(1, 2)) first = Player::P2;
    for (const Player p : {first, foe(first)}) {
      if (ends[uint8_t(p)]) protocol::side_end(options.log, p, c);
    }
  };
  count_down(&SideConditions::reflect, SC::Reflect);
  count_down(&SideConditions::light_screen, SC::LightScreen);
  count_down(&SideConditions::mist, SC::Mist);
  count_down(&SideConditions::safeguard, SC::Safeguard);

  // ENDTURN_WISH (util:1312): half the max HP to whoever is active, unless
  // fainted (bsc:9311). `s1` and `s2` are the residual Speeds
  // (residual_speeds).
  const Player first = speed_order(battle, s1, s2);
  for (const Player p : {first, foe(first)}) {
    Side& side = battle.side(p);
    SideConditions& c = side.conditions;
    if (c.wish == 0 || --c.wish != 0) continue;
    const uint8_t wisher = c.wisher;
    c.wisher = 0;
    Pokemon& mon = side.get(1);
    // Showdown's Wish logs only a heal that restores HP.
    if (!mon.fainted() && mon.hp != mon.stats.hp) {
      heal(mon, mon.stats.hp / 2);
      protocol::heal_wish(options.log, protocol::ident(battle, p), mon, wisher);
    }
  }

  // ENDTURN_RAIN to HAIL (util:1331-1427): the timer, unless an ability set the
  // weather. The turn it runs out, sand and hail deal no damage
  // (sim/battle.ts:519); otherwise they hurt each Pokémon if not suppressed
  // (bsc:7602). The decomp's loop stops once a side is out (bs1:3219), so the
  // faster of two last Pokémon loses; Showdown hurts both before checkWin
  // (sim/battle.ts:2539, 2610), a tie, and this follows it.
  Field& field = battle.field;
  if (field.weather == Weather::None) return std::nullopt;
  if (!field.permanent && --field.weather_turns == 0) {
    field.weather = Weather::None;
    protocol::weather(options.log, Weather::None);
    weather_change(battle, options);
    return std::nullopt;
  }
  const Weather weather = field.weather;
  protocol::weather(options.log, weather, protocol::WeatherFrom::Upkeep);
  if ((weather != Weather::Sandstorm && weather != Weather::Hail) ||
      effective_weather(battle) == Weather::None) {
    return std::nullopt;
  }
  // Showdown's weather hurts in its own Speed order (eachEvent,
  // sim/battle.ts:470), with Quick Claw and a tie coin of its own.
  const Player order = speed_order(battle, s1, s2);
  for (const Player p : {order, foe(order)}) {
    Side& side = battle.side(p);
    const ActivePokemon& a = side.active;
    if (side.get(1).fainted()) continue;
    const bool immune =
        a.volatiles.underground || a.volatiles.underwater ||
        (weather == Weather::Sandstorm
             ? has_type(a, Type::Rock) || has_type(a, Type::Steel) ||
                   has_type(a, Type::Ground) || a.ability == Ability::SandVeil
             : has_type(a, Type::Ice));
    if (immune) continue;
    passive_damage(battle, p, fraction(side.get(1), 16), options);
    protocol::damage(options.log, protocol::ident(battle, p), side.get(1),
                     weather == Weather::Sandstorm ? protocol::Damage::Sandstorm
                                                   : protocol::Damage::Hail);
  }
  protocol::faints(options.log);
  return outcome(battle);
}

// The HP p takes in a damage stage, or 0, before the handler runs: toxic's
// counter steps first, and a bind whose counter runs out now or whose binder
// has gone deals none.
constexpr uint32_t stage_damage(const Battle& battle, Player player, Stage stage) {
  const Side& side = battle.side(player);
  const ActivePokemon& a = side.active;
  const Volatiles& v = a.volatiles;
  const Pokemon& mon = side.get(1);
  if (mon.fainted()) return 0;
  switch (stage) {
  case Stage::Status:
    if (mon.status.badly_poisoned()) {
      const uint32_t toxic = a.toxic != ActivePokemon::MAX_TOXIC ? a.toxic + 1u : a.toxic;
      return fraction(mon, 16) * toxic;
    }
    return mon.status.poisoned() || mon.status.burned() ? fraction(mon, 8) : 0;
  case Stage::Nightmare:
    return v.nightmare && mon.status.asleep() ? fraction(mon, 4) : 0;
  case Stage::Curse:
    return v.cursed ? fraction(mon, 4) : 0;
  case Stage::Bind:
    return v.wrapped >= 2 && !v.wrapper_gone ? fraction(mon, 16) : 0;
  default:
    assert(false && "not a damage stage");
    return 0;
  }
}

// One stage of DoBattlerEndTurnEffects for p's active Pokémon; a fainted one
// skips it. Returns the end of the battle if a residual KO ends it. Uproar and
// the Thrash family follow Showdown (ENGINE.md decision 2). Always inlined,
// with a constant stage, so the switch folds away: as a call it costs more than
// the stages' work.
template <typename O>
[[gnu::always_inline]] constexpr Flow battler_stage(Battle& battle, Player player,
                                                    Stage stage, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  Pokemon& mon = side.get(1);
  const auto alive = [&] { return !mon.fainted(); };
  const ID id = protocol::ident(battle, player);
  // Each damaging residual is its own Showdown handler, with faintMessages
  // after it (sim/battle.ts:569).
  const auto hurt = [&](uint32_t n, protocol::Damage reason) {
    passive_damage(battle, player, n, options);
    protocol::damage(options.log, id, mon, reason);
    protocol::faints(options.log);
    return outcome(battle);
  };

  switch (stage) {
  case Stage::Ingrain:
    // ENDTURN_INGRAIN (util:1476)
    if (v.rooted && alive() && mon.hp != mon.stats.hp) {
      heal(mon, fraction(mon, 16));
      protocol::heal(options.log, id, mon, protocol::Heal::Ingrain);
    }
    return std::nullopt;

  case Stage::Ability:
    // ENDTURN_ABILITIES (util:2587).
    if (!alive()) return std::nullopt;
    switch (a.ability) {
    case Ability::RainDish:
      if (effective_weather(battle) == Weather::Rain && mon.hp != mon.stats.hp) {
        heal(mon, fraction(mon, 16));
        protocol::heal_ability(options.log, id, mon, Ability::RainDish);
      }
      break;
    case Ability::ShedSkin:
      // Shed Skin: Random() % 3 == 0, drawn only with a status to cure
      // (util:2607). A cured sleep is public, so its count resets.
      if (mon.status.any() &&
          rolls::coin(battle, player, &Action::shed_skin, 1, 3, options)) {
        const bool asleep = mon.status.asleep();
        protocol::activate_ability(options.log, id, Ability::ShedSkin);
        protocol::cure_status(options.log, id, mon.status);
        mon.status = Status::none();
        if (asleep) wake(battle, player, options);
      }
      break;
    case Ability::SpeedBoost:
      // Not on the turn it came in (decision 14). Showdown prints neither the
      // -ability nor a boost at +6 (sim/battle.ts:2073).
      if (a.boosts.spe < 6 && a.first_turn != 2) {
        a.boosts.spe++;
        protocol::ability(options.log, id, Ability::SpeedBoost, protocol::AbilityFrom::Boost);
        protocol::boost(options.log, id, SPE, 1, false);
      }
      break;
    case Ability::Truant:
      a.truant = !a.truant;
      break;
    default:
      break;
    }
    return std::nullopt;

  case Stage::Item:
    // ENDTURN_ITEMS1 (util:1496)
    item_end_turn(battle, player, options);
    return std::nullopt;

  case Stage::LeechSeed: {
    // ENDTURN_LEECH_SEED (util:1501, bs1:3265): max HP / 8 from the seeded
    // Pokémon to the seeder, or from the seeder by Liquid Ooze. The seeded
    // Pokémon faints first (bs1:3283).
    const Player seeder = foe(player);
    Pokemon& seeder_mon = battle.side(seeder).get(1);
    if (!v.leech_seed || !alive() || seeder_mon.fainted()) return std::nullopt;
    const bool ooze = a.ability == Ability::LiquidOoze;
    const uint16_t drained = passive_damage(battle, player, fraction(mon, 8), options);
    const ID seeder_id = protocol::ident(battle, seeder);
    protocol::damage_seeded(options.log, id, mon, seeder_id);
    if (ooze) {
      // Showdown's gen4 Liquid Ooze has instafaint
      // (data/mods/gen4/abilities.ts:256): a seeder it KOs faints first.
      passive_damage(battle, seeder, drained, options);
      protocol::damage_ability(options.log, seeder_id, seeder_mon,
                               Ability::LiquidOoze, id);
      if (seeder_mon.fainted()) protocol::faints_last_first(options.log);
    } else if (seeder_mon.hp != seeder_mon.stats.hp) {
      heal(seeder_mon, drained);
      protocol::heal(options.log, seeder_id, seeder_mon, protocol::Heal::Silent);
    }
    protocol::faints(options.log);
    return outcome(battle);
  }

  case Stage::Status: {
    // ENDTURN_POISON, BAD_POISON, BURN (util:1517-1552): 1/8; toxic's counter
    // (at most 15) / 16; 1/8.
    const uint32_t n = stage_damage(battle, player, stage);
    if (n == 0) return std::nullopt;
    if (mon.status.badly_poisoned() && a.toxic != ActivePokemon::MAX_TOXIC) a.toxic++;
    return hurt(n, mon.status.burned() ? protocol::Damage::Burn : protocol::Damage::Poison);
  }

  case Stage::Nightmare:
    // ENDTURN_NIGHTMARES (util:1553): 1/4 while asleep, else it ends.
    if (!v.nightmare || !alive()) return std::nullopt;
    if (mon.status.asleep()) {
      return hurt(stage_damage(battle, player, stage), protocol::Damage::Nightmare);
    }
    v.nightmare = false;
    return std::nullopt;

  case Stage::Curse:
    // ENDTURN_CURSE (util:1574): 1/4.
    if (!v.cursed || !alive()) return std::nullopt;
    return hurt(stage_damage(battle, player, stage), protocol::Damage::Curse);

  case Stage::Bind: {
    // ENDTURN_WRAP (util:1585), as Showdown's gen4 partiallytrapped: the hidden
    // counter ticks first and at 0 frees the Pokémon; a binding whose binder
    // has left ends silently, else 1/16. The decomp frees it as the binder
    // leaves. With the binder gone the tick changes only the log, so it isn't
    // recorded.
    if (v.wrapped == 0 || !alive()) return std::nullopt;
    const uint32_t n = stage_damage(battle, player, stage);
    v.wrapped--;
    const bool freed = v.wrapped == 0;
    if constexpr (CHANCE<O>) {
      if (v.wrapper_gone) {
        options.chance.durations.get(player).binding = 0;
      } else {
        rolls::tick(options.chance.actions.get(player).binding,
                    options.chance.durations.get(player).binding, freed, 3, 6,
                    1, options);
      }
    }
    if (freed || v.wrapper_gone) {
      protocol::end_effect(options.log, id,
                           freed ? protocol::End::Bound : protocol::End::BoundSilent,
                           a.wrapped_move);
      unbind(a);
      return std::nullopt;
    }
    // A faint clears the volatiles, so the move is read first.
    const Move by = a.wrapped_move;
    assert(n != 0);
    passive_damage(battle, player, n, options);
    protocol::damage_bound(options.log, id, mon, by);
    protocol::faints(options.log);
    return outcome(battle);
  }

  case Stage::Uproar: {
    // Showdown's gen4 uproar (subOrder 11): the hidden duration ticks, ending
    // at 0. The decomp also ends it if the user couldn't move (util:1647).
    if (v.uproar == 0 || !alive()) return std::nullopt;
    const bool ended = --v.uproar == 0;
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).uproar,
                  options.chance.durations.get(player).attacking, ended, 2, 5, 1,
                  options);
    }
    if (ended) {
      v.multiple_turns = false;
      protocol::end_effect(options.log, id, protocol::End::Uproar);
    } else {
      protocol::start(options.log, id, protocol::Start::UproarUpkeep);
    }
    return std::nullopt;
  }

  case Stage::Timers:
    break;
  }

  // ENDTURN_ENCORE (util:1713): it ends if the move was overwritten or out of
  // PP, else the hidden timer ticks.
  if (a.encore_timer != 0) {
    const MoveSlot& ms = a.moves[a.encored_slot];
    const bool lost = ms.id != a.encored_move || ms.pp == 0;
    const bool ended = lost || --a.encore_timer == 0;
    if constexpr (CHANCE<O>) {
      if (lost) {
        options.chance.durations.get(player).encore = 0;
      } else {
        rolls::tick(options.chance.actions.get(player).encore,
                    options.chance.durations.get(player).encore, ended, 3, 6,
                    1, options);
      }
    }
    if (ended) {
      a.encored_move = Move::None;
      a.encore_timer = 0;
      protocol::end_effect(options.log, id, protocol::End::Encore);
    }
  }

  // ENDTURN_LOCK_ON, TAUNT (util:1734-1748). Lock-On is on its user, as
  // Showdown's.
  if (v.lock_on != 0 && --v.lock_on % 4 == 0) v.lock_on = 0;
  if (a.taunt != 0 && --a.taunt == 0) {
    protocol::end_effect(options.log, id, protocol::End::Taunt);
  }

  // ENDTURN_YAWN (util:1752): sleep the turn after, unless the Pokémon has a
  // status, Vital Spirit or Insomnia, or an Uproar is on. Safeguard doesn't
  // stop it; Sleep Clause does (Showdown's trySetStatus from the user).
  if (v.yawn != 0 && --v.yawn == 0 && alive()) {
    protocol::end_effect(options.log, id, protocol::End::Yawn);
    if (can_status(battle, player, Secondary::Sleep) && !sleep_clause(battle, player)) {
      MoveState none{};
      inflict(battle, player, Secondary::Sleep, none, options);
    } else if (uproar(battle) && !mon.status.any() &&
               a.ability != Ability::VitalSpirit && a.ability != Ability::Insomnia) {
      uproar_stops(battle, player, options);
    }
  }

  // The decomp's ENDTURN_ITEMS2 (util:1501) cures here; Showdown's berries wait
  // for the Update after all the residuals (end_of_turn), so one Lum Berry
  // takes both Yawn's sleep and Thrash's confusion.
  return std::nullopt;
}

// Perish Song's count, as Showdown's gen4 condition (onResidualOrder 12,
// data/mods/gen4/moves.ts:1017): after the per-Pokémon residuals and in their
// order, its faints replaced with theirs. The decomp (util:1825) runs it after
// the replacements, with its own request. Both counts run before the battle can
// end (sim/battle.ts:519), so two last Pokémon at 0 tie.
template <typename O>
constexpr void perish_count(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  Pokemon& mon = battle.side(player).get(1);
  if (!a.volatiles.perish_song || mon.fainted()) return;
  // Showdown's duration runs 4 to 0, so it prints the timer before this step.
  protocol::start_count(options.log, protocol::ident(battle, player), protocol::Start::Perish,
                        a.perish_song_timer);
  if (a.perish_song_timer != 0) {
    a.perish_song_timer--;
    return;
  }
  a.volatiles.perish_song = false;
  passive_damage(battle, player, mon.hp, options);
}

// A future attack on p's side, as Showdown's futuremove
// (data/conditions.ts:379; order 11): at 0 it lands on whoever is in, unless
// fainted. A semi-invulnerable target is missed unless locked on; the accuracy
// uses the user's stage only while it is in, its ability wherever it is, and
// the target's evasion. No Protect; Wonder Guard lets it through. The stored
// damage goes through a Substitute, with Focus Band but no Endure
// (data/mods/gen4/moves.ts:434), and no Destiny Bond. The decomp (util:1781)
// lands it after the replacements, with its own request.
template <typename O>
constexpr Flow future_attack(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  SideConditions& c = side.conditions;
  if (c.future_sight == 0 || --c.future_sight != 0) return std::nullopt;
  const Move move = side.future_sight_move;
  const uint32_t stored = side.future_sight_damage;
  const uint8_t position = c.future_user;
  side.future_sight_move = Move::None;
  side.future_sight_damage = 0;
  c.future_user = 0;
  Pokemon& target = side.get(1);
  if (target.fainted()) return std::nullopt;

  const Player user = foe(player);
  const Side& user_side = battle.side(user);
  const bool in = user_side.order[0] == position && !user_side.get(1).fainted();
  const ID id = protocol::ident(battle, player);
  const ID source = in ? protocol::ident(battle, user)
                       : protocol::benched(ID::init(user, position));
  protocol::end_effect(options.log, id, protocol::End::FutureMove, move);
  const ActivePokemon& t = side.active;
  // The user's Lock-On, while it's in, reaches a semi-invulnerable target too.
  const bool locked = in && locked_on(battle, user);
  bool lands = !t.volatiles.semi_invulnerable() || locked;
  if (lands && !locked) {
    const uint32_t calc = staged_accuracy(
        battle, player, get(move).accuracy, in ? user_side.active.boosts.acc : int8_t(0),
        user_side.pokemon[position - 1].ability, move == Move::DoomDesire);
    lands = calc >= 100 || rolls::future_hit(battle, user, calc, options);
  }
  if (!lands) {
    protocol::miss(options.log, source, id);
    return std::nullopt;
  }
  MoveState m{};
  m.move = move;
  m.chosen = move;
  m.type = Type::Mystery;
  m.slot = NO_SLOT;
  const uint32_t d = survive(battle, user, m, stored, rolls::FUTURE, options);
  m.hp_dealt = hp_update(battle, player, d, m, options);
  // It enters attackedBy, and both moves are noMirror, so Mirror Move fails
  // until the next attack; unless its user is out, since the next turn drops
  // absent sources (sim/battle.ts:1707).
  if (in) {
    side.active.last_taken_move = Move::None;
    side.active.last_taken_from = 0;
  }
  ActivePokemon& a = side.active;
  if (a.substitute_hp == 0) a.volatiles.substitute = false;
  if (m.damaged) {
    protocol::damage(options.log, id, target);
    if (target.fainted()) faint(battle, player, options);
    rage(battle, user, m, options);
  }
  protocol::faints(options.log);
  // The hit loop's Update after faintMessages (sim/battle-actions.ts:996) ends
  // the foe's Attract once its source has fainted.
  if (ActivePokemon& f = battle.side(user).active; target.fainted() && f.volatiles.infatuated) {
    protocol::end_effect(options.log, protocol::ident(battle, user), protocol::End::Attract);
    f.volatiles.infatuated = false;
  }
  return outcome(battle);
}

// The durations with no residual order, which Showdown runs after every other
// residual, Perish Song's included. Each lasts while its move renews it
// (ActivePokemon::renewed: Showdown's duration 2).
// - lockedmove (data/conditions.ts:253): ending with trueDuration 1, the user
//   is confused (-start confusion [fatigue]) unless confused already or Own
//   Tempo. A sleeping user loses the lock silently. The decomp (util:1673)
//   counts down only on turns the move ran.
// - Rollout, Ice Ball and Fury Cutter end silently.
// - Bide (gen3 mod) ends silently if its third turn's action never came.
template <typename O>
constexpr void lock_residuals(Battle& battle, Player player, O& options) {
  Side& side = battle.side(player);
  ActivePokemon& a = side.active;
  Volatiles& v = a.volatiles;
  if (side.get(1).fainted()) return;
  if (v.thrash != 0) {
    // trueDuration is public at the second residual: the lock goes on only at
    // 2, and ends in confusion only at 1. Recorded once, with 1/2; the count is
    // then untracked.
    if constexpr (CHANCE<O>) {
      uint8_t& c = options.chance.durations.get(player).attacking;
      if (c == 2) {
        rolls::tick(options.chance.actions.get(player).thrash, c, v.thrash <= 1, 2, 3,
                    1, options);
        c = 0;
      }
    }
    if ((a.renewed & ActivePokemon::RENEWED_LOCK) && v.thrash >= 2) {
      if (side.get(1).status.asleep()) {
        v.multiple_turns = false;
        v.thrash = 0;
        if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 0;
      } else {
        v.thrash--;
        if constexpr (CHANCE<O>) {
          uint8_t& c = options.chance.durations.get(player).attacking;
          if (c != 0) c++;
        }
      }
    } else {
      const bool fatigue = v.thrash <= 1;
      v.multiple_turns = false;
      v.thrash = 0;
      if constexpr (CHANCE<O>) options.chance.durations.get(player).attacking = 0;
      if (fatigue && v.confusion == 0 && a.ability != Ability::OwnTempo) {
        confuse(battle, player, options, protocol::Start::ConfusionFatigue);
      }
    }
  }
  if (a.rollout != 0 && !(a.renewed & ActivePokemon::RENEWED_LOCK)) {
    a.rollout = 0;
    v.multiple_turns = false;
  }
  if (a.fury_cutter != 0 && !(a.renewed & ActivePokemon::RENEWED_FURY_CUTTER)) {
    a.fury_cutter = 0;
  }
  a.renewed = 0;
  if (v.bide != 0 && --v.bide == 0) end_bide(battle, player, options);
}

// Disable's end, as Showdown's gen3, with no residual order
// (data/mods/gen3/moves.ts:210), so after Perish Song; the decomp's runs with
// the other timers (util:1692). It ends silently if the move is no longer
// known, else the hidden timer ticks.
template <typename O>
constexpr void disable_residual(Battle& battle, Player player, O& options) {
  ActivePokemon& a = battle.side(player).active;
  if (a.disable_timer == 0 || battle.side(player).get(1).fainted()) return;
  const ID id = protocol::ident(battle, player);
  if (slot_of(a, a.disabled_move) == NO_SLOT) {
    a.disabled_move = Move::None;
    a.disable_timer = 0;
    if constexpr (CHANCE<O>) options.chance.durations.get(player).disable = 0;
  } else {
    const bool ended = --a.disable_timer == 0;
    if (ended) {
      a.disabled_move = Move::None;
      protocol::end_effect(options.log, id, protocol::End::Disable);
    }
    if constexpr (CHANCE<O>) {
      rolls::tick(options.chance.actions.get(player).disable,
                  options.chance.durations.get(player).disable, ended, 3, 6,
                  1, options);
    }
  }
}

} // namespace detail

} // namespace hoenn
